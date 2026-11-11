#pragma once

#include "sdkconfig.h"

typedef enum {
    GLOVE_HAND_ID_LEFT = 0,
    GLOVE_HAND_ID_RIGHT = 1,
} glove_hand_id_t;

#if CONFIG_GLOVE_HAND_LEFT
#define GLOVE_HAND_ID          GLOVE_HAND_ID_LEFT
#define GLOVE_HAND_NAME        "left"
#define GLOVE_HAND_NAME_UPPER  "LEFT"
#define GLOVE_HAND_NAME_CN     "左"
#define GLOVE_BUILD_DIRECTORY  "build_left"
#define GLOVE_FIRMWARE_ID      "GLOVE_LEFT"
#define GLOVE_ROCK_DATA_PORT   9101
#elif CONFIG_GLOVE_HAND_RIGHT
#define GLOVE_HAND_ID          GLOVE_HAND_ID_RIGHT
#define GLOVE_HAND_NAME        "right"
#define GLOVE_HAND_NAME_UPPER  "RIGHT"
#define GLOVE_HAND_NAME_CN     "右"
#define GLOVE_BUILD_DIRECTORY  "build_right"
#define GLOVE_FIRMWARE_ID      "GLOVE_RIGHT"
#define GLOVE_ROCK_DATA_PORT   9102
#else
#error "必须在menuconfig中选择左手或右手固件"
#endif
