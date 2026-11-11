idf_component_register(
    SRCS "speaker_test.c" "../../../main/max98357a_audio.c"
    INCLUDE_DIRS "../../../main"
    REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_i2c
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_TONE_AMPLITUDE=6000)
