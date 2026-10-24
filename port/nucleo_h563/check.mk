# check.mk - compile and link check of the NUCLEO-H563ZI bench board build (port/nucleo_h563)
# with arm-none-eabi-gcc against ST's H5 HAL and CMSIS headers. Not the bench board's real build:
# that is the STM32CubeIDE project (port/nucleo_h563/CUBEMX_SETUP.md).
#   make bench EXT=/path/to/ext   (default ../ext next to this repository)
# EXT holds: xpack-arm-none-eabi-gcc-13.3.1-1.1, stm32h5xx_hal_driver, cmsis_device_h5, cmsis-core,
# lwip, lan8742 (the GitHub repositories STMicroelectronics/stm32h5xx_hal_driver, cmsis_device_h5,
# cmsis-core, stm32-lan8742 and lwip-tcpip/lwip).
PLAT   ?= ../cems-platform
EXT    ?= ../ext
ARMCC  ?= $(EXT)/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc
HAL     = $(EXT)/stm32h5xx_hal_driver
DEV     = -DSTM32H563xx -DUSE_HAL_DRIVER -DNODE_RS485_USART=USART2 -DNODE_RS485_IRQHandler=USART2_IRQHandler
CFLAGS  = -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -std=c11 -O2 -g -Wall -Wextra -Wshadow \
          -Wconversion -Wno-sign-conversion -Werror -ffunction-sections -fdata-sections $(EXTRA)
LW      = $(EXT)/lwip
INC     = -Iport/nucleo_h563/check -Iport/nucleo_h563 -Iport/nucleo_h563/lwip -Iport/stm32g0 -Inode -I$(PLAT)/platform/include \
          -I$(HAL)/Inc -I$(EXT)/cmsis_device_h5/Include -I$(EXT)/cmsis-core/CMSIS/Core/Include -I$(LW)/src/include -I$(EXT)/lan8742
SRCS    = node/node.c node/node_net.c $(PLAT)/platform/core/mb_rtu.c $(PLAT)/platform/core/iob_net.c port/stm32g0/rs485_g0.c \
          port/nucleo_h563/bench_main.c port/nucleo_h563/bench_hw.c port/nucleo_h563/board_nucleo.c \
          port/nucleo_h563/vcp_h5.c port/nucleo_h563/net_bench.c port/nucleo_h563/lwip/eth_h5.c port/nucleo_h563/check/check_main.c
# lwIP (NO_SYS, raw API: no api/ folder) and the PHY driver: third-party, built without -Werror
LWSRC   = $(wildcard $(LW)/src/core/*.c) $(wildcard $(LW)/src/core/ipv4/*.c) $(LW)/src/netif/ethernet.c $(EXT)/lan8742/lan8742.c
HALSRC  = $(EXT)/cmsis_device_h5/Source/Templates/system_stm32h5xx.c \
          $(addprefix $(HAL)/Src/stm32h5xx_hal, .c _cortex.c _rcc.c _rcc_ex.c _gpio.c _pwr.c _pwr_ex.c _eth.c _eth_ex.c)
START   = $(EXT)/cmsis_device_h5/Source/Templates/gcc/startup_stm32h563xx.s
LD      = $(EXT)/cmsis_device_h5/Source/Templates/gcc/linker/STM32H563xx_FLASH.ld
OUT    ?= build/bench
OBJS    = $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
HOBJS   = $(addprefix $(OUT)/hal/,$(notdir $(HALSRC:.c=.o)))
LOBJS   = $(addprefix $(OUT)/lwip/,$(notdir $(LWSRC:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

$(OUT)/bench.elf: $(OBJS) $(HOBJS) $(LOBJS) $(OUT)/startup.o
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard --specs=nano.specs --specs=nosys.specs \
	    -Wl,--gc-sections -T$(LD) -Wl,-Map=$(OUT)/bench.map $(OBJS) $(HOBJS) $(LOBJS) $(OUT)/startup.o -o $@
	@echo "bench board compile and link check OK: $(words $(OBJS)) node files"
	@$(dir $(ARMCC))arm-none-eabi-size $@

$(OUT)/%.o: %.c | $(OUT)
	$(ARMCC) $(CFLAGS) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/%.o: $(HAL)/Src/%.c | $(OUT)/hal
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/system_stm32h5xx.o: $(EXT)/cmsis_device_h5/Source/Templates/system_stm32h5xx.c | $(OUT)/hal
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/lwip/%.o: | $(OUT)/lwip
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $(filter %/$*.c,$(LWSRC)) -o $@
$(OUT)/startup.o: $(START) | $(OUT)
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -c $< -o $@
$(OUT) $(OUT)/hal $(OUT)/lwip:
	mkdir -p $@
