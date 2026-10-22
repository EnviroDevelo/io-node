/* main.h - stand-in for CubeMX's main.h, for the compile check only (port/stm32g0/check.mk).
 * The pin labels are the ones CUBEMX_SETUP.md asks for; the ports and pins here are placeholders,
 * not the board's pinout (that comes from the schematic and lives in the real .ioc). */
#ifndef MAIN_H
#define MAIN_H
#include "stm32g0xx_hal.h"

#define NODE_USB 1

#define HEALTHY_GPIO_Port   GPIOA
#define HEALTHY_Pin         GPIO_PIN_0
#define LED_ACT_GPIO_Port   GPIOA
#define LED_ACT_Pin         GPIO_PIN_1
#define LED_FAULT_GPIO_Port GPIOA
#define LED_FAULT_Pin       GPIO_PIN_4
#define CS_A_GPIO_Port      GPIOA
#define CS_A_Pin            GPIO_PIN_8
#define CS_IN_GPIO_Port     GPIOA
#define CS_IN_Pin           GPIO_PIN_15
#define DIP1_GPIO_Port      GPIOC
#define DIP1_Pin            GPIO_PIN_13
#define DIP2_GPIO_Port      GPIOC
#define DIP2_Pin            GPIO_PIN_14
#define DIP3_GPIO_Port      GPIOC
#define DIP3_Pin            GPIO_PIN_15
#define DIP4_GPIO_Port      GPIOD
#define DIP4_Pin            GPIO_PIN_0
#define HWREV0_GPIO_Port    GPIOD
#define HWREV0_Pin          GPIO_PIN_1
#define HWREV1_GPIO_Port    GPIOD
#define HWREV1_Pin          GPIO_PIN_2
#define B_IN1_GPIO_Port     GPIOB
#define B_IN1_Pin           GPIO_PIN_0
#define B_IN2_GPIO_Port     GPIOB
#define B_IN2_Pin           GPIO_PIN_1
#define B_IN3_GPIO_Port     GPIOB
#define B_IN3_Pin           GPIO_PIN_2
#define B_IN4_GPIO_Port     GPIOB
#define B_IN4_Pin           GPIO_PIN_10
#define B_IN5_GPIO_Port     GPIOB
#define B_IN5_Pin           GPIO_PIN_11
#define B_IN6_GPIO_Port     GPIOB
#define B_IN6_Pin           GPIO_PIN_12
#define B_IN7_GPIO_Port     GPIOB
#define B_IN7_Pin           GPIO_PIN_13
#define B_IN8_GPIO_Port     GPIOB
#define B_IN8_Pin           GPIO_PIN_14
#define B_SYNC_GPIO_Port    GPIOB
#define B_SYNC_Pin          GPIO_PIN_15
#define B_LOAD_GPIO_Port    GPIOC
#define B_LOAD_Pin          GPIO_PIN_6
#define B_WD_GPIO_Port      GPIOC
#define B_WD_Pin            GPIO_PIN_7
#define B_FAULT_GPIO_Port   GPIOD
#define B_FAULT_Pin         GPIO_PIN_3
#define B_SUPPLY_GPIO_Port  GPIOB
#define B_SUPPLY_Pin        GPIO_PIN_3
#define RELAY1_GPIO_Port    GPIOB
#define RELAY1_Pin          GPIO_PIN_4
#define RELAY2_GPIO_Port    GPIOB
#define RELAY2_Pin          GPIO_PIN_5
#define VBUS_SENSE_GPIO_Port GPIOB
#define VBUS_SENSE_Pin      GPIO_PIN_8

#endif
