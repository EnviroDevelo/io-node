/* board_nucleo.c - see board_nucleo.h. Register level (CMSIS) plus the CubeMX pin labels (main.h). */
#include "board_nucleo.h"
#include "main.h"

#define ADDR_MAGIC 0xA0D50000u           /* BKP0R = magic | ~address << 8 | address */
#define CNT_MAGIC  0x52530000u           /* BKP5R = magic | reset count */

static inline void pin_set(GPIO_TypeDef *p, uint16_t pin, bool v) { p->BSRR = v ? pin : (uint32_t)pin << 16; }

static void backup_open(void)            /* the TAMP backup registers are in the backup domain */
{
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;
    (void)RCC->APB3ENR;
    PWR->DBPCR |= PWR_DBPCR_DBP;
}

static node_reset_t reset_cause(void)
{
    uint32_t rsr = RCC->RSR;
    RCC->RSR |= RCC_RSR_RMVF;                                       /* FW-03: flags cleared */
    if (rsr & RCC_RSR_IWDGRSTF) return NODE_RST_IWDG;
    if (rsr & RCC_RSR_SFTRSTF) return NODE_RST_SOFTWARE;
    if (rsr & RCC_RSR_BORRSTF) return NODE_RST_POWER_ON;
    if (rsr & RCC_RSR_PINRSTF) return NODE_RST_PIN;
    return NODE_RST_OTHER;
}

void board_boot_info(node_boot_t *b)
{
    backup_open();
    b->reset_cause = reset_cause();
    uint32_t c = TAMP->BKP5R;
    uint16_t resets = (b->reset_cause == NODE_RST_POWER_ON || (c & 0xFFFF0000u) != CNT_MAGIC) ? 0u : (uint16_t)(c + 1u);
    TAMP->BKP5R = CNT_MAGIC | resets;
    b->resets = resets;
    uint32_t a = TAMP->BKP0R;
    bool set = (a & 0xFFFF0000u) == ADDR_MAGIC && (uint8_t)(a >> 8) == (uint8_t)~a;
    b->address = set ? (uint8_t)a : (uint8_t)NODE_BENCH_ADDRESS;
    b->nv_ok = true;
    const volatile uint32_t *s = &TAMP->BKP1R;                     /* BKP1R..BKP4R: serial, 16 bytes */
    for (unsigned i = 0; i < 16; i++) b->serial[i] = (uint8_t)(s[i / 4u] >> (8u * (i % 4u)));
    b->hw_rev = (uint8_t)NODE_BENCH_HW_REV;
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    for (unsigned i = 0; i < 3; i++) {
        b->uid[2 * i] = (uint16_t)uid[i];
        b->uid[2 * i + 1] = (uint16_t)(uid[i] >> 16);
    }
}

bool bench_store_address(uint8_t a)
{
    backup_open();
    uint32_t v = ADDR_MAGIC | (uint32_t)(uint8_t)~a << 8 | a;
    TAMP->BKP0R = v;
    return TAMP->BKP0R == v;
}

bool bench_store_serial(const uint8_t s[16])
{
    backup_open();
    volatile uint32_t *r = &TAMP->BKP1R;
    bool ok = true;
    for (unsigned w = 0; w < 4; w++) {
        uint32_t v = (uint32_t)s[4 * w] | (uint32_t)s[4 * w + 1] << 8 | (uint32_t)s[4 * w + 2] << 16 | (uint32_t)s[4 * w + 3] << 24;
        r[w] = v;
        ok = ok && r[w] == v;
    }
    return ok;
}

void board_iwdg_start(void)
{
    /* LSI 32 kHz / 4 = 8 kHz; 800 counts = 100 ms (FW-23) */
    IWDG->KR = 0xCCCCu;
    IWDG->KR = 0x5555u;
    IWDG->PR = 0u;
    IWDG->RLR = 800u - 1u;
    while (IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) {}
    IWDG->KR = 0xAAAAu;
}

void board_iwdg_refresh(void) { IWDG->KR = 0xAAAAu; }

void board_healthy_toggle(void) { HEALTHY_GPIO_Port->ODR ^= HEALTHY_Pin; }

void board_leds(bool comms, bool fault, bool heartbeat)
{
    pin_set(LD1_GPIO_Port, LD1_Pin, heartbeat);
    pin_set(LD2_GPIO_Port, LD2_Pin, comms);
    pin_set(LD3_GPIO_Port, LD3_Pin, fault);
}
