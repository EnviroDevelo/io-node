/* board_g0.c - see board_g0.h and node_port.h. Pin names are CubeMX user labels (main.h);
 * the list is in port/stm32g0/CUBEMX_SETUP.md. Register level (CMSIS), no HAL calls except
 * HAL_GetTick in the delay, so the timing is plain to see. */
#include "board_g0.h"
#include "main.h"
#include "node_port.h"

#ifndef NODE_SPI
#define NODE_SPI SPI1
#endif
#define SYSMEM_BASE 0x1FFF0000u          /* STM32G0B1 system memory (ST bootloader), AN2606 */
#define BOOT_MAGIC  0xB007B007u
#define CNT_MAGIC   0x52535443u           /* "RSTC" */

static inline void pin_set(GPIO_TypeDef *p, uint16_t pin, bool v) { p->BSRR = v ? pin : (uint32_t)pin << 16; }
static inline bool pin_get(const GPIO_TypeDef *p, uint16_t pin) { return (p->IDR & pin) != 0; }

/* Kept over a reset, lost at power-off (linker section .noinit, CUBEMX_SETUP.md step 7). */
static struct { uint32_t boot_magic, cnt_magic, resets, resets_inv; } noinit __attribute__((section(".noinit")));

/* ---------------- start-up ---------------- */

void board_early_boot_check(void)
{
    if (noinit.boot_magic != BOOT_MAGIC) return;
    noinit.boot_magic = 0;
    /* Straight after a software reset: clocks at reset values, no interrupts, watchdog stopped. */
    RCC->APBENR2 |= RCC_APBENR2_SYSCFGEN;
    (void)RCC->APBENR2;
    SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) | SYSCFG_CFGR1_MEM_MODE_0;   /* system flash at 0 */
    uint32_t sp = *(volatile uint32_t *)SYSMEM_BASE, pc = *(volatile uint32_t *)(SYSMEM_BASE + 4u);
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) {}
}

void board_enter_bootloader(void)
{
    noinit.boot_magic = BOOT_MAGIC;
    NVIC_SystemReset();
}

static node_reset_t reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;                                       /* FW-03: flags cleared */
    if (csr & RCC_CSR_IWDGRSTF) return NODE_RST_IWDG;               /* checked first: NRST is also pulsed */
    if (csr & RCC_CSR_SFTRSTF) return NODE_RST_SOFTWARE;
    if (csr & RCC_CSR_PWRRSTF) return NODE_RST_POWER_ON;            /* power-on or brown-out */
    if (csr & RCC_CSR_PINRSTF) return NODE_RST_PIN;
    return NODE_RST_OTHER;
}

void board_boot_info(node_boot_t *b)
{
    b->reset_cause = reset_cause();
    if (b->reset_cause == NODE_RST_POWER_ON || noinit.cnt_magic != CNT_MAGIC || noinit.resets != ~noinit.resets_inv) {
        noinit.cnt_magic = CNT_MAGIC;
        noinit.resets = 0;
    } else
        noinit.resets = (noinit.resets + 1u) & 0xFFFFu;             /* RESETS wraps */
    noinit.resets_inv = ~noinit.resets;
    b->resets = (uint16_t)noinit.resets;
    /* DIP switch to 0 V when ON, pull-ups: ON = 1 (FW-02, HW-03). DIP1 is bit 0. */
    b->dip_address = (uint8_t)((pin_get(DIP1_GPIO_Port, DIP1_Pin) ? 0u : 1u) | (pin_get(DIP2_GPIO_Port, DIP2_Pin) ? 0u : 2u) |
                               (pin_get(DIP3_GPIO_Port, DIP3_Pin) ? 0u : 4u) | (pin_get(DIP4_GPIO_Port, DIP4_Pin) ? 0u : 8u));
    /* board revision straps: 0..3 -> HW_REV 1..4 (HW-11) */
    b->hw_rev = (uint8_t)(1u + (pin_get(HWREV0_GPIO_Port, HWREV0_Pin) ? 1u : 0u) + (pin_get(HWREV1_GPIO_Port, HWREV1_Pin) ? 2u : 0u));
    /* 96-bit unique ID: register 69 = bits 0-15 of the first word, 70 = bits 16-31, ... */
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    for (unsigned i = 0; i < 3; i++) {
        b->uid[2 * i] = (uint16_t)uid[i];
        b->uid[2 * i + 1] = (uint16_t)(uid[i] >> 16);
    }
}

/* ---------------- watchdog, HEALTHY, LEDs ---------------- */

void board_iwdg_start(void)
{
    /* LSI 32 kHz / 4 = 8 kHz; 800 counts = 100 ms (FW-23). The LSI spread (about +-10 %)
     * gives 90 to 110 ms. */
    IWDG->KR = 0xCCCCu;
    IWDG->KR = 0x5555u;
    IWDG->PR = 0u;            /* /4 */
    IWDG->RLR = 800u - 1u;
    while (IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) {}
    IWDG->KR = 0xAAAAu;
}

void board_iwdg_refresh(void) { IWDG->KR = 0xAAAAu; }

void board_healthy_toggle(void) { HEALTHY_GPIO_Port->ODR ^= HEALTHY_Pin; }

void board_leds(bool activity, bool fault)
{
    pin_set(LED_ACT_GPIO_Port, LED_ACT_Pin, activity);
    pin_set(LED_FAULT_GPIO_Port, LED_FAULT_Pin, fault);
}

bool board_vbus(void) { return pin_get(VBUS_SENSE_GPIO_Port, VBUS_SENSE_Pin); }

/* ---------------- node_port.h ---------------- */

int port_spi_xfer(port_cs_t cs, uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    SPI_TypeDef *s = NODE_SPI;
    uint32_t cpol_cpha = ((mode & 2u) ? SPI_CR1_CPOL : 0u) | ((mode & 1u) ? SPI_CR1_CPHA : 0u);
    if ((s->CR1 & (SPI_CR1_CPOL | SPI_CR1_CPHA)) != cpol_cpha) {
        s->CR1 &= ~SPI_CR1_SPE;
        s->CR1 = (s->CR1 & ~(SPI_CR1_CPOL | SPI_CR1_CPHA)) | cpol_cpha;
    }
    s->CR2 |= SPI_CR2_FRXTH;                                        /* RXNE on every byte */
    s->CR1 |= SPI_CR1_SPE;
    GPIO_TypeDef *port = cs == PORT_CS_BANK_A ? CS_A_GPIO_Port : CS_IN_GPIO_Port;
    uint16_t pin = cs == PORT_CS_BANK_A ? CS_A_Pin : CS_IN_Pin;
    int rc = 0;
    pin_set(port, pin, false);
    for (uint16_t i = 0; i < n && rc == 0; i++) {
        uint32_t guard = 10000u;
        while (!(s->SR & SPI_SR_TXE)) if (!--guard) { rc = -1; break; }
        if (rc) break;
        *(volatile uint8_t *)&s->DR = tx ? tx[i] : 0u;
        guard = 10000u;
        while (!(s->SR & SPI_SR_RXNE)) if (!--guard) { rc = -1; break; }
        if (rc) break;
        uint8_t v = *(volatile uint8_t *)&s->DR;
        if (rx) rx[i] = v;
    }
    while (s->SR & SPI_SR_BSY) {}
    pin_set(port, pin, true);
    return rc;
}

void port_bank_b_inputs(uint8_t o)
{
    pin_set(B_IN1_GPIO_Port, B_IN1_Pin, o & 0x01u); pin_set(B_IN2_GPIO_Port, B_IN2_Pin, o & 0x02u);
    pin_set(B_IN3_GPIO_Port, B_IN3_Pin, o & 0x04u); pin_set(B_IN4_GPIO_Port, B_IN4_Pin, o & 0x08u);
    pin_set(B_IN5_GPIO_Port, B_IN5_Pin, o & 0x10u); pin_set(B_IN6_GPIO_Port, B_IN6_Pin, o & 0x20u);
    pin_set(B_IN7_GPIO_Port, B_IN7_Pin, o & 0x40u); pin_set(B_IN8_GPIO_Port, B_IN8_Pin, o & 0x80u);
}

void port_bank_b_pin(port_bpin_t p, bool v)
{
    switch (p) {
    case PORT_B_SYNC: pin_set(B_SYNC_GPIO_Port, B_SYNC_Pin, v); break;
    case PORT_B_LOAD: pin_set(B_LOAD_GPIO_Port, B_LOAD_Pin, v); break;
    case PORT_B_WD:   pin_set(B_WD_GPIO_Port, B_WD_Pin, v); break;
    }
}

bool port_bank_b_fault(void) { return !pin_get(B_FAULT_GPIO_Port, B_FAULT_Pin); }    /* open drain, active low */

bool port_bank_b_supply(void)
{
#ifdef B_SUPPLY_Pin
    return pin_get(B_SUPPLY_GPIO_Port, B_SUPPLY_Pin);
#else
    return true;                     /* no opto sense fitted: bank B's supply comes from its chip's status */
#endif
}

void port_relays(uint8_t r)
{
    pin_set(RELAY1_GPIO_Port, RELAY1_Pin, r & 1u);
    pin_set(RELAY2_GPIO_Port, RELAY2_Pin, r & 2u);
}

void port_delay_us(uint32_t us)
{
    /* SysTick counts down from LOAD at the core clock: busy-wait on its value. */
    uint32_t per_us = SystemCoreClock / 1000000u, load = SysTick->LOAD + 1u;
    uint32_t left = us * per_us, last = SysTick->VAL;
    while (left) {
        uint32_t v = SysTick->VAL, d = last >= v ? last - v : last + load - v;
        last = v;
        left = d >= left ? 0u : left - d;
    }
}
