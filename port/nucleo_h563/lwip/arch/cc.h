/* cc.h - lwIP compiler/platform glue for arm-none-eabi-gcc + newlib (bench board, NO_SYS). */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stdlib.h>

#define LWIP_ERRNO_STDINCLUDE   1
#define BYTE_ORDER              LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { } while (0)   /* a failed assert: carry on, the IWDG guards the loop */

uint32_t net_bench_rand(void);
#define LWIP_RAND()             net_bench_rand()

#endif
