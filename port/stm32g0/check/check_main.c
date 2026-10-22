/* check_main.c - stand-in for CubeMX's main.c, compile and link check only: the calls the real
 * main.c makes (CUBEMX_SETUP.md), the SysTick handler, and a USB transmit stub. */
#include "main.h"
#include "board_g0.h"
#include "usbd_cdc_if.h"

void node_main(void);

uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len) { (void)Buf; (void)Len; return 0; }

void SysTick_Handler(void);
void SysTick_Handler(void) { HAL_IncTick(); }

int main(void)
{
    board_early_boot_check();          /* USER CODE BEGIN 1 */
    HAL_Init();
    /* SystemClock_Config(); MX_GPIO_Init(); MX_SPI1_Init(); MX_USART1_UART_Init(); MX_USB_Device_Init(); */
    node_main();                       /* USER CODE BEGIN 2 */
    for (;;) {}
}
