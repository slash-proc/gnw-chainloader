#include <stdint.h>

const uint8_t ASSET_MARIO_BRICK_FLOOR[] = { 0x11, 0x00, 0x01, 0x5a };
const uint8_t ASSET_MARIO_CLOUDS_LARGE[] = { 0x32, 0x00, 0x06, 0x09, 0x0a, 0x0b, 0x19, 0x1a, 0x1b };
const uint8_t ASSET_MARIO_COIN_FRAME_1[] = { 0x11, 0x00, 0x01, 0x70 };
const uint8_t ASSET_MARIO_COIN_FRAME_2[] = { 0x11, 0x00, 0x01, 0x71 };
const uint8_t ASSET_MARIO_COIN_FRAME_3[] = { 0x11, 0x00, 0x01, 0x72 };
const uint8_t ASSET_MARIO_COIN_FRAME_4[] = { 0x11, 0x00, 0x01, 0x73 };
const uint8_t ASSET_MARIO_YOSHI_GREEN_WALKING_1[] = { 0x22, 0x00, 0x04, 0x84, 0x85, 0x94, 0x95 };
const uint8_t ASSET_MARIO_YOSHI_GREEN_WALKING_2[] = { 0x22, 0x00, 0x04, 0x86, 0x87, 0x96, 0x97 };
const uint8_t ASSET_MARIO_YOSHI_GREEN_WALKING_3[] = { 0x22, 0x00, 0x04, 0x88, 0x89, 0x98, 0x99 };
const uint8_t ASSET_ZELDA_ENTITIES_FAIRY_LEFT[] = { 0x11, 0x03, 0x01, 0xbe };
const uint8_t ASSET_ZELDA_ENTITIES_FAIRY_RIGHT[] = { 0x11, 0x0b, 0x01, 0xbe, 0x10 };

const uint8_t * const asset_list[] = {
    ASSET_MARIO_BRICK_FLOOR,
    ASSET_MARIO_CLOUDS_LARGE,
    ASSET_MARIO_COIN_FRAME_1,
    ASSET_MARIO_COIN_FRAME_2,
    ASSET_MARIO_COIN_FRAME_3,
    ASSET_MARIO_COIN_FRAME_4,
    ASSET_MARIO_YOSHI_GREEN_WALKING_1,
    ASSET_MARIO_YOSHI_GREEN_WALKING_2,
    ASSET_MARIO_YOSHI_GREEN_WALKING_3,
    ASSET_ZELDA_ENTITIES_FAIRY_LEFT,
    ASSET_ZELDA_ENTITIES_FAIRY_RIGHT
};
const uint16_t asset_list_count = sizeof(asset_list) / sizeof(asset_list[0]);
