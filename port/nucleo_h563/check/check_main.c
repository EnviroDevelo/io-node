/* check_main.c - stand-in for CubeMX's main.c, compile and link check only. */
#include "main.h"

void node_main(void);

void SysTick_Handler(void);
void SysTick_Handler(void) { HAL_IncTick(); }

int main(void)
{
    HAL_Init();
    /* SystemClock_Config(); MX_GPIO_Init(); MX_USART2_UART_Init(); MX_USART3_UART_Init(); */
    node_main();                       /* USER CODE BEGIN 2 */
    for (;;) {}
}
