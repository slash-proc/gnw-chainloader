#include "input.h"
#include "board.h"
#include "main.h"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
} button_map_t;

static const button_map_t BUTTON_MAP[INPUT_COUNT] = {
    [INPUT_UP]     = {BTN_Up_GPIO_Port,     BTN_Up_Pin},
    [INPUT_DOWN]   = {BTN_Down_GPIO_Port,   BTN_Down_Pin},
    [INPUT_LEFT]   = {BTN_Left_GPIO_Port,   BTN_Left_Pin},
    [INPUT_RIGHT]  = {BTN_Right_GPIO_Port,  BTN_Right_Pin},
    [INPUT_A]      = {BTN_A_GPIO_Port,      BTN_A_Pin},
    [INPUT_B]      = {BTN_B_GPIO_Port,      BTN_B_Pin},
    [INPUT_START]  = {BTN_START_GPIO_Port,  BTN_START_Pin},
    [INPUT_SELECT] = {BTN_SELECT_GPIO_Port, BTN_SELECT_Pin},
    [INPUT_PAUSE]  = {BTN_PAUSE_GPIO_Port,  BTN_PAUSE_Pin},
    [INPUT_GAME]   = {BTN_GAME_GPIO_Port,   BTN_GAME_Pin},
    [INPUT_TIME]   = {BTN_TIME_GPIO_Port,   BTN_TIME_Pin},
    [INPUT_PWR]    = {BTN_PWR_GPIO_Port,    BTN_PWR_Pin},
};

static uint16_t g_current_state = 0;
static uint16_t g_previous_state = 0;
static uint16_t g_repeat_state = 0;
static uint32_t g_next_repeat_time[INPUT_COUNT] = {0};

void input_init(void) {
    g_current_state = 0;
    g_previous_state = 0;
    g_repeat_state = 0;
    for (int i = 0; i < INPUT_COUNT; i++) {
        g_next_repeat_time[i] = 0;
    }
}

void input_update(void) {
    g_previous_state = g_current_state;
    g_current_state = 0;
    g_repeat_state = 0;

    uint32_t ticks = HAL_GetTick();

    for (int i = 0; i < INPUT_COUNT; i++) {
        if (board_check_button(BUTTON_MAP[i].port, BUTTON_MAP[i].pin)) {
            g_current_state |= (1 << i);
            
            // Auto-repeat logic
            if ((g_previous_state & (1 << i)) == 0) {
                // Just pressed
                g_repeat_state |= (1 << i);
                g_next_repeat_time[i] = ticks + INPUT_REPEAT_DELAY_MS;
            } else if (ticks >= g_next_repeat_time[i]) {
                // Repeating
                g_repeat_state |= (1 << i);
                g_next_repeat_time[i] = ticks + INPUT_REPEAT_RATE_MS;
            }
        }
    }
}

uint16_t input_get_state(void) {
    return g_current_state;
}

bool input_is_pressed(input_button_t button) {
    return (g_current_state & (1 << button)) != 0;
}

bool input_just_pressed(input_button_t button) {
    return ((g_current_state & (1 << button)) != 0) && ((g_previous_state & (1 << button)) == 0);
}

bool input_just_released(input_button_t button) {
    return ((g_current_state & (1 << button)) == 0) && ((g_previous_state & (1 << button)) != 0);
}

bool input_is_repeating(input_button_t button) {
    return (g_repeat_state & (1 << button)) != 0;
}

void input_clear_all(void) {
    g_current_state = 0;
    g_previous_state = 0;
    g_repeat_state = 0;
}
