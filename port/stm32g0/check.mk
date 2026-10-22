# check.mk - compile and link check of the node firmware for the STM32G0B1CET6 with
# arm-none-eabi-gcc against ST's real G0 HAL and CMSIS headers. Not the product build: that is
# the STM32CubeIDE project (port/stm32g0/CUBEMX_SETUP.md).
#   make target EXT=/path/to/ext   (default ../ext next to this repository)
# EXT holds: xpack-arm-none-eabi-gcc-13.3.1-1.1, stm32g0xx_hal_driver, cmsis_device_g0, cmsis-core
# (the GitHub repositories STMicroelectronics/stm32g0xx_hal_driver, cmsis_device_g0, cmsis-core).
PLAT   ?= ../cems-platform
EXT    ?= $(abspath ../ext)
ARMCC  ?= $(EXT)/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc
HAL     = $(EXT)/stm32g0xx_hal_driver
DEV     = -DSTM32G0B1xx -DUSE_HAL_DRIVER
CFLAGS  = -mcpu=cortex-m0plus -mthumb -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion \
          -Werror -ffunction-sections -fdata-sections $(EXTRA)
INC     = -Iport/stm32g0/check -Iport/stm32g0 -Inode -Idrivers -I$(PLAT)/platform/include \
          -I$(HAL)/Inc -I$(EXT)/cmsis_device_g0/Include -I$(EXT)/cmsis-core/CMSIS/Core/Include
SRCS    = node/node.c $(PLAT)/platform/core/mb_rtu.c drivers/drivers.c drivers/drv_iso2h823.c drivers/drv_iso1i813t.c \
          drivers/drv_iso808.c drivers/isoface_crc5.c port/stm32g0/node_main.c port/stm32g0/board_g0.c port/stm32g0/rs485_g0.c \
          port/stm32g0/usb_g0.c port/stm32g0/check/check_main.c
HALSRC  = $(EXT)/cmsis_device_g0/Source/Templates/system_stm32g0xx.c \
          $(addprefix $(HAL)/Src/stm32g0xx_hal, .c _cortex.c _rcc.c _rcc_ex.c _gpio.c _pwr.c _pwr_ex.c _flash.c _flash_ex.c _dma.c _dma_ex.c _exti.c)
START   = $(EXT)/cmsis_device_g0/Source/Templates/gcc/startup_stm32g0b1xx.s
OUT    ?= build/target
OBJS    = $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
HOBJS   = $(addprefix $(OUT)/hal/,$(notdir $(HALSRC:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

$(OUT)/node.elf: $(OBJS) $(HOBJS) $(OUT)/startup.o port/stm32g0/check/stm32g0b1ce_check.ld
	$(ARMCC) -mcpu=cortex-m0plus -mthumb --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections \
	    -Tport/stm32g0/check/stm32g0b1ce_check.ld -Wl,-Map=$(OUT)/node.map $(OBJS) $(HOBJS) $(OUT)/startup.o -o $@
	@echo "target compile and link check OK: $(words $(OBJS)) node files"
	@$(dir $(ARMCC))arm-none-eabi-size $(OBJS)
	@$(dir $(ARMCC))arm-none-eabi-size $@
$(OUT)/%.o: %.c | $(OUT)
	$(ARMCC) $(CFLAGS) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/%.o: $(EXT)/stm32g0xx_hal_driver/Src/%.c | $(OUT)
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/hal/system_stm32g0xx.o: $(EXT)/cmsis_device_g0/Source/Templates/system_stm32g0xx.c | $(OUT)
	$(ARMCC) $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS)) $(DEV) $(INC) -c $< -o $@
$(OUT)/startup.o: $(START) | $(OUT)
	$(ARMCC) -mcpu=cortex-m0plus -mthumb -c $< -o $@
$(OUT):
	mkdir -p $(OUT)/hal
