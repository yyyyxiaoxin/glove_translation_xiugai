#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 双手录制状态。
 *
 * 左手：GPIO4 开始、GPIO5 停止，开始时为本次录制生成一个 64 位 session。
 * 右手：不直接操作按键，由 ESP-NOW 控制包设置同一个 session。
 *
 * 录制期间左右手的所有上传帧都使用该 session；服务端按 session
 * 导出的 CSV 即包含左右手。未录制时不上传。
 */

bool glove_recording_start(void);
bool glove_recording_stop(void);

/* 右手收到左手控制包时调用；recording_id=0 表示停止。 */
void glove_recording_set_remote(uint64_t recording_id);

/* 原子读取 active/id，避免开始与停止之间读到不一致的组合。 */
bool glove_recording_get(uint64_t *recording_id);

bool glove_recording_active(void);
uint64_t glove_recording_id(void);

/* 每次实际开始/停止时递增，供主循环检测并通知右手。 */
uint32_t glove_recording_generation(void);

#ifdef __cplusplus
}
#endif
