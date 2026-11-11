#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max98357a_audio.h"

void app_main(void)
{
    printf("\nFIRMWARE_ID=GLOVE_SPEAKER_TEST\n");
    printf("TONE_HZ=1000 DURATION_MS=500 PAUSE_MS=2500\n");
    printf("TONE_AMPLITUDE=%d/32767\n", TEST_TONE_AMPLITUDE);
    fflush(stdout);

    ESP_ERROR_CHECK(max98357a_audio_init());
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t count = 0;
    while (true) {
        ESP_LOGI("speaker_test", "Playing tone # %" PRIu32, ++count);
        esp_err_t ret = max98357a_play_test_tone();
        if (ret != ESP_OK) {
            ESP_LOGE("speaker_test", "Tone failed: %s", esp_err_to_name(ret));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}
