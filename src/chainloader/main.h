#pragma once
#ifndef MAIN_H
#define MAIN_H

#include "stm32h7xx_hal.h"

#define BTN_PAUSE_Pin GPIO_PIN_13
#define BTN_PAUSE_GPIO_Port GPIOC
#define BTN_GAME_Pin GPIO_PIN_1
#define BTN_GAME_GPIO_Port GPIOC
#define BTN_TIME_Pin GPIO_PIN_5
#define BTN_TIME_GPIO_Port GPIOC

#define BTN_A_Pin GPIO_PIN_9
#define BTN_A_GPIO_Port GPIOD
#define BTN_B_Pin GPIO_PIN_5
#define BTN_B_GPIO_Port GPIOD

#define BTN_Left_Pin GPIO_PIN_11
#define BTN_Left_GPIO_Port GPIOD
#define BTN_Down_Pin GPIO_PIN_14
#define BTN_Down_GPIO_Port GPIOD
#define BTN_Right_Pin GPIO_PIN_15
#define BTN_Right_GPIO_Port GPIOD
#define BTN_Up_Pin GPIO_PIN_0
#define BTN_Up_GPIO_Port GPIOD

#define BTN_PWR_Pin GPIO_PIN_0
#define BTN_PWR_GPIO_Port GPIOA

#define BTN_START_Pin GPIO_PIN_11
#define BTN_START_GPIO_Port GPIOC
#define BTN_SELECT_Pin GPIO_PIN_12
#define BTN_SELECT_GPIO_Port GPIOC

extern OSPI_HandleTypeDef hospi1;

void Error_Handler(void);
void wdog_refresh(void);

#endif // MAIN_H
