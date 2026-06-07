#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INPUT_UP,
    INPUT_DOWN,
    INPUT_LEFT,
    INPUT_RIGHT,
    INPUT_A,
    INPUT_B,
    INPUT_START,
    INPUT_SELECT,
    INPUT_PAUSE,
    INPUT_GAME,
    INPUT_TIME,
    INPUT_PWR,
    INPUT_COUNT
} input_button_t;

#define INPUT_REPEAT_DELAY_MS 350
#define INPUT_REPEAT_RATE_MS 50

void input_init(void);
void input_update(void);
uint16_t input_get_state(void);

bool input_is_pressed(input_button_t button);
bool input_just_pressed(input_button_t button);
bool input_just_released(input_button_t button);
bool input_is_repeating(input_button_t button);
void input_clear_all(void);

#endif // INPUT_H
