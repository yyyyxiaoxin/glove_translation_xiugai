#include "glove_link.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "glove_identity.h"
#include "glove_recording.h"

/*
 * 左右手 ESP-NOW 帧格式（v1，约 174 字节，远小于 250 字节上限）：
 *   magic/version/hand/session/sequence/timestamp_ms/valid_mask/mag_mask
 *   + 11 路原始 int16 数据。
 *
 * 数据排列与云端 frames 数组完全一致：
 *   CH0..CH9 : ax,ay,az,gx,gy,gz
 *   CH10     : ax,ay,az,gx,gy,gz,mx,my,mz
 */
#define GL_LINK_MAGIC    0x314C5647u   /* "GVL1" */
#define GL_LINK_VERSION  1u
#define GL_VALUE_COUNT   (IMU_MPU6050_COUNT * 6 + 9)

/* 控制包：左手 -> 右手，开始/停止录制；录制期间每秒一次心跳。 */
#define GL_CONTROL_MAGIC   0x324C5647u   /* "GVL2" */
#define GL_CONTROL_VERSION 1u
#define GL_CONTROL_START   1u
#define GL_CONTROL_STOP    2u
#define GL_CONTROL_TIMEOUT_MS 5000u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint8_t  hand;       /* 发送方手别，右手只接受 left */
    uint8_t  reserved;
    uint64_t recording;
    uint32_t heartbeat;
    uint32_t reserved2;
} glove_link_control_t;

_Static_assert(offsetof(glove_link_control_t, recording) == 8,
               "unexpected control packet layout");
_Static_assert(sizeof(glove_link_control_t) == 24,
               "unexpected control packet size");

/*
 * 不强制 packed：字段顺序已按自然对齐排列（无内部填充），左右手固件使用
 * 同一编译器和目标平台，布局一致；这样也能安全地把 values 的地址传给
 * 解包函数（GCC 12+ 对 packed 成员取地址会报 -Werror=address-of-packed-member）。
 */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  hand;
    uint16_t reserved;
    uint64_t session;
    uint32_t sequence;
    uint32_t valid_mask;
    int64_t  timestamp_ms;
    uint32_t mag_mask;
    int16_t  values[GL_VALUE_COUNT];
} glove_link_packet_t;

_Static_assert(offsetof(glove_link_packet_t, values) == 36,
               "unexpected ESP-NOW packet layout");
_Static_assert(sizeof(glove_link_packet_t) == 176,
               "unexpected ESP-NOW packet size");
_Static_assert(sizeof(glove_link_packet_t) <= ESP_NOW_MAX_DATA_LEN,
               "ESP-NOW frame exceeds v1 payload limit");

#if CONFIG_GLOVE_HAND_LEFT
#define GL_EXPECTED_HAND      GLOVE_HAND_ID_RIGHT
#define GL_EXPECTED_HAND_NAME "right"
#else
#define GL_EXPECTED_HAND      GLOVE_HAND_ID_LEFT
#define GL_EXPECTED_HAND_NAME "left"
#endif

static const char *TAG = "glove_link";

static const uint8_t s_broadcast[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static bool s_ready;
static glove_link_rx_cb_t s_rx_cb;

#if CONFIG_GLOVE_HAND_RIGHT
static uint32_t s_tx_frames;
static uint32_t s_tx_errors;
#endif
static uint32_t s_rx_frames;
static uint32_t s_rx_gaps;
static uint32_t s_rx_invalid;
static uint32_t s_last_sequence;
static bool s_have_last_sequence;
static uint64_t s_last_frame_session;

#if CONFIG_GLOVE_HAND_LEFT
static uint32_t s_control_sequence;
static bool s_last_control_active;
static uint64_t s_last_control_id;
#else
static volatile uint32_t s_last_control_ms;
static volatile bool s_have_control;
#endif

#if CONFIG_GLOVE_HAND_RIGHT
static uint32_t channel_mask(const imu_frame_t *frame, bool mag)
{
    uint32_t mask = 0;
    for (int ch = 0; ch < IMU_TOTAL_CHANNELS; ++ch) {
        bool set = mag ? frame->channel[ch].has_mag : frame->channel[ch].valid;
        if (set) {
            mask |= 1u << ch;
        }
    }
    return mask;
}

static void fill_values(const imu_frame_t *frame, int16_t *values)
{
    unsigned index = 0;
    for (int ch = 0; ch < IMU_MPU6050_COUNT; ++ch) {
        const imu_channel_data_t *data = &frame->channel[ch];
        values[index++] = data->ax;
        values[index++] = data->ay;
        values[index++] = data->az;
        values[index++] = data->gx;
        values[index++] = data->gy;
        values[index++] = data->gz;
    }
    const imu_channel_data_t *data = &frame->channel[IMU_ICM20948_INDEX];
    values[index++] = data->ax;
    values[index++] = data->ay;
    values[index++] = data->az;
    values[index++] = data->gx;
    values[index++] = data->gy;
    values[index++] = data->gz;
    values[index++] = data->mx;
    values[index++] = data->my;
    values[index++] = data->mz;
}
#endif /* CONFIG_GLOVE_HAND_RIGHT */

static void unpack_values(const int16_t *values, imu_frame_t *frame)
{
    unsigned index = 0;
    for (int ch = 0; ch < IMU_MPU6050_COUNT; ++ch) {
        imu_channel_data_t *data = &frame->channel[ch];
        data->ax = values[index++];
        data->ay = values[index++];
        data->az = values[index++];
        data->gx = values[index++];
        data->gy = values[index++];
        data->gz = values[index++];
    }
    imu_channel_data_t *data = &frame->channel[IMU_ICM20948_INDEX];
    data->ax = values[index++];
    data->ay = values[index++];
    data->az = values[index++];
    data->gx = values[index++];
    data->gy = values[index++];
    data->gz = values[index++];
    data->mx = values[index++];
    data->my = values[index++];
    data->mz = values[index++];
}

static void handle_frame(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != (int)sizeof(glove_link_packet_t)) {
        ++s_rx_invalid;
        return;
    }

    const glove_link_packet_t *packet = (const glove_link_packet_t *)data;
    if (packet->magic != GL_LINK_MAGIC || packet->version != GL_LINK_VERSION) {
        ++s_rx_invalid;
        return;
    }
    /* 只接收对侧手的数据；自己的广播不会被回环，但防止拿错固件时混流。 */
    if (packet->hand != (uint8_t)GL_EXPECTED_HAND) {
        return;
    }

    imu_frame_t frame = {0};
    frame.sequence = packet->sequence;
    frame.timestamp_ms = packet->timestamp_ms;
    unpack_values(packet->values, &frame);
    for (int ch = 0; ch < IMU_TOTAL_CHANNELS; ++ch) {
        frame.channel[ch].valid = (packet->valid_mask >> ch) & 1u;
    }
    frame.channel[IMU_ICM20948_INDEX].has_mag =
        (packet->mag_mask >> IMU_ICM20948_INDEX) & 1u;

    /*
     * 新录制 session 的第一帧不要把上一段录制的序号差算作丢帧；
     * 右手只在上传时发送，非录制期间的采集序号本来就会跳。
     */
    if (packet->session != s_last_frame_session) {
        s_last_frame_session = packet->session;
        s_have_last_sequence = false;
    }

    ++s_rx_frames;
    if (s_have_last_sequence && packet->sequence > s_last_sequence + 1) {
        s_rx_gaps += packet->sequence - s_last_sequence - 1;
    }
    s_last_sequence = packet->sequence;
    s_have_last_sequence = true;

    if (s_rx_frames == 1 || (s_rx_frames % 500) == 0) {
        ESP_LOGI(TAG, "接收远端帧 %" PRIu32 "，累计丢帧 %" PRIu32 "，无效 %" PRIu32
                 "，session=%016" PRIx64,
                 s_rx_frames, s_rx_gaps, s_rx_invalid, packet->session);
    }

    if (s_rx_cb != NULL) {
        s_rx_cb(info->src_addr, packet->session, &frame);
    }
}

static void handle_control(const uint8_t *data, int len)
{
    if (len != (int)sizeof(glove_link_control_t)) {
        ++s_rx_invalid;
        return;
    }

    const glove_link_control_t *control = (const glove_link_control_t *)data;
    if (control->magic != GL_CONTROL_MAGIC ||
        control->version != GL_CONTROL_VERSION) {
        ++s_rx_invalid;
        return;
    }
    if (control->hand != (uint8_t)GLOVE_HAND_ID_LEFT) {
        return; /* 右手不接受右手/未知来源的控制包 */
    }
    if (control->type != GL_CONTROL_START && control->type != GL_CONTROL_STOP) {
        return;
    }

#if CONFIG_GLOVE_HAND_RIGHT
    s_last_control_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_have_control = true;
    if (control->type == GL_CONTROL_START) {
        glove_recording_set_remote(control->recording);
    } else {
        glove_recording_set_remote(0);
    }
#else
    (void)control; /* 左手不处理控制包 */
#endif
}

static void link_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len < (int)sizeof(uint32_t)) {
        ++s_rx_invalid;
        return;
    }

    uint32_t magic;
    memcpy(&magic, data, sizeof(magic));
    if (magic == GL_LINK_MAGIC) {
        handle_frame(info, data, len);
    } else if (magic == GL_CONTROL_MAGIC) {
        handle_control(data, len);
    } else {
        ++s_rx_invalid;
    }
}

esp_err_t glove_link_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "ESP-NOW 初始化失败");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(link_recv_cb), TAG,
                        "注册 ESP-NOW 接收回调失败");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast, ESP_NOW_ETH_ALEN);
    peer.channel = 0;              /* 跟随当前 Wi-Fi 信道 */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "添加 ESP-NOW 广播 peer 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ESP-NOW 就绪：本机=%s，等待=%s，帧长=%u 字节；录制 session 由左手分配",
             GLOVE_HAND_NAME, GL_EXPECTED_HAND_NAME,
             (unsigned)sizeof(glove_link_packet_t));
    return ESP_OK;
}

esp_err_t glove_link_send_frame(const imu_frame_t *frame)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_GLOVE_HAND_LEFT
    /* 左手是网关，不向右手回传采集数据。 */
    (void)frame;
    return ESP_OK;
#else
    uint64_t session;
    if (!glove_recording_get(&session)) {
        return ESP_OK; /* 非录制期间不发送，避免空耗和旧数据混流 */
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_have_control ||
        (uint32_t)(now_ms - s_last_control_ms) > GL_CONTROL_TIMEOUT_MS) {
        if (s_have_control) {
            s_have_control = false;
            glove_recording_set_remote(0);
            ESP_LOGW(TAG, "超过%u ms未收到左手录制心跳，右手停止发送",
                     GL_CONTROL_TIMEOUT_MS);
        }
        return ESP_OK;
    }

    glove_link_packet_t packet = {0};
    packet.magic = GL_LINK_MAGIC;
    packet.version = GL_LINK_VERSION;
    packet.hand = (uint8_t)GLOVE_HAND_ID;
    packet.session = session;
    packet.sequence = frame->sequence;
    packet.timestamp_ms = frame->timestamp_ms;
    packet.valid_mask = channel_mask(frame, false);
    packet.mag_mask = channel_mask(frame, true);
    fill_values(frame, packet.values);

    esp_err_t err = esp_now_send(s_broadcast, (const uint8_t *)&packet,
                                 sizeof(packet));
    if (err == ESP_OK) {
        ++s_tx_frames;
        if ((s_tx_frames % 500) == 0) {
            ESP_LOGI(TAG, "已发送 %" PRIu32 " 帧，发送错误 %" PRIu32,
                     s_tx_frames, s_tx_errors);
        }
    } else {
        ++s_tx_errors;
        if ((s_tx_errors % 50) == 1) {
            ESP_LOGW(TAG, "ESP-NOW 发送失败: %s（累计 %" PRIu32 " 次）",
                     esp_err_to_name(err), s_tx_errors);
        }
    }
    return err;
#endif
}

esp_err_t glove_link_send_recording(bool active, uint64_t recording_id)
{
#if CONFIG_GLOVE_HAND_LEFT
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (active && recording_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    glove_link_control_t control = {
        .magic = GL_CONTROL_MAGIC,
        .version = GL_CONTROL_VERSION,
        .type = active ? GL_CONTROL_START : GL_CONTROL_STOP,
        .hand = (uint8_t)GLOVE_HAND_ID,
        .recording = active ? recording_id : 0,
        .heartbeat = ++s_control_sequence,
    };
    esp_err_t err = esp_now_send(s_broadcast, (const uint8_t *)&control,
                                 sizeof(control));
    if (err != ESP_OK) {
        if ((s_control_sequence % 50) == 1) {
            ESP_LOGW(TAG, "录制控制包发送失败: %s（累计序号 %" PRIu32 "）",
                     esp_err_to_name(err), s_control_sequence);
        }
        return err;
    }

    if (active != s_last_control_active ||
        recording_id != s_last_control_id) {
        if (active) {
            ESP_LOGI(TAG, "已通知右手开始录制：session=%016" PRIx64,
                     recording_id);
        } else {
            ESP_LOGI(TAG, "已通知右手停止录制");
        }
        s_last_control_active = active;
        s_last_control_id = recording_id;
    }
    return ESP_OK;
#else
    (void)active;
    (void)recording_id;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void glove_link_register_rx_cb(glove_link_rx_cb_t cb)
{
    s_rx_cb = cb;
}

bool glove_link_is_ready(void)
{
    return s_ready;
}
