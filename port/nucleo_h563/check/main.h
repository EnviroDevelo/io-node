/* main.h - stand-in for CubeMX's main.h, for the compile check only (port/nucleo_h563/check.mk).
 * LD1-LD3 and B1 are the NUCLEO-H563ZI's own (UM3115); the other labels are placeholders: the real
 * pins are chosen in CubeMX (CUBEMX_SETUP.md). */
#ifndef MAIN_H
#define MAIN_H
#include "stm32h5xx_hal.h"

#define LD1_GPIO_Port    GPIOB
#define LD1_Pin          GPIO_PIN_0
#define LD2_GPIO_Port    GPIOF
#define LD2_Pin          GPIO_PIN_4
#define LD3_GPIO_Port    GPIOG
#define LD3_Pin          GPIO_PIN_4
#define B1_GPIO_Port     GPIOC
#define B1_Pin           GPIO_PIN_13
#define HEALTHY_GPIO_Port GPIOG
#define HEALTHY_Pin      GPIO_PIN_0
#define OUT1_GPIO_Port   GPIOE
#define OUT1_Pin         GPIO_PIN_0
#define OUT2_GPIO_Port   GPIOE
#define OUT2_Pin         GPIO_PIN_1
#define OUT3_GPIO_Port   GPIOE
#define OUT3_Pin         GPIO_PIN_2
#define OUT4_GPIO_Port   GPIOE
#define OUT4_Pin         GPIO_PIN_3
#define OUT5_GPIO_Port   GPIOE
#define OUT5_Pin         GPIO_PIN_4
#define OUT6_GPIO_Port   GPIOE
#define OUT6_Pin         GPIO_PIN_5
#define OUT7_GPIO_Port   GPIOE
#define OUT7_Pin         GPIO_PIN_6
#define OUT8_GPIO_Port   GPIOE
#define OUT8_Pin         GPIO_PIN_7
#define OUT9_GPIO_Port   GPIOE
#define OUT9_Pin         GPIO_PIN_8
#define OUT10_GPIO_Port  GPIOE
#define OUT10_Pin        GPIO_PIN_9
#define OUT11_GPIO_Port  GPIOE
#define OUT11_Pin        GPIO_PIN_10
#define OUT12_GPIO_Port  GPIOE
#define OUT12_Pin        GPIO_PIN_11
#define OUT13_GPIO_Port  GPIOE
#define OUT13_Pin        GPIO_PIN_12
#define OUT14_GPIO_Port  GPIOE
#define OUT14_Pin        GPIO_PIN_13
#define OUT15_GPIO_Port  GPIOE
#define OUT15_Pin        GPIO_PIN_14
#define OUT16_GPIO_Port  GPIOE
#define OUT16_Pin        GPIO_PIN_15
#define REL1_GPIO_Port   GPIOG
#define REL1_Pin         GPIO_PIN_2
#define REL2_GPIO_Port   GPIOG
#define REL2_Pin         GPIO_PIN_3
#define IN1_GPIO_Port    GPIOF
#define IN1_Pin          GPIO_PIN_8
#define IN2_GPIO_Port    GPIOF
#define IN2_Pin          GPIO_PIN_9
#define IN3_GPIO_Port    GPIOF
#define IN3_Pin          GPIO_PIN_10
#define IN4_GPIO_Port    GPIOF
#define IN4_Pin          GPIO_PIN_11
#define IN5_GPIO_Port    GPIOF
#define IN5_Pin          GPIO_PIN_12
#define IN6_GPIO_Port    GPIOF
#define IN6_Pin          GPIO_PIN_13
#define IN7_GPIO_Port    GPIOF
#define IN7_Pin          GPIO_PIN_14
#define IN8_GPIO_Port    GPIOF
#define IN8_Pin          GPIO_PIN_15

#endif
