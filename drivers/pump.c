/* pump.c - the pump connector (map 2): PWM duty and frequency, DIR, and the tacho period, straight
 * through to the port (TIM3 on the H563 board). The node core turns them off with the outputs. */
#include "drivers.h"
#include "node_port.h"

void pump_pwm(void *ctx, uint16_t duty, uint16_t freq_hz, bool dir)
{
    (void)ctx;
    port_pwm(duty > 1000u ? 1000u : duty, freq_hz, dir);
}

uint32_t pump_tach_period_us(void *ctx) { (void)ctx; return port_tach_period_us(); }
