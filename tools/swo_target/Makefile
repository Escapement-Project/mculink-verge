# The ITM counter for a CC13x2/CC26x2. `make`, then load it with probe-rs over cJTAG
# (README.md). TI_SDK is the SimpleLink CC13xx/CC26xx SDK, for driverlib.
TI_SDK ?= $(HOME)/ti/simplelink_cc13xx_cc26xx_sdk_8_33_00_16
DRIVERLIB := $(TI_SDK)/source/ti/devices/cc13x2_cc26x2/driverlib/bin/gcc/driverlib.lib

ARCH := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS := $(ARCH) -std=c99 -Os -g -Wall -Wextra -ffunction-sections -fdata-sections \
	-I$(TI_SDK)/source
LDFLAGS := $(ARCH) -nostartfiles -nostdlib -static -Wl,--gc-sections -Wl,-T,cc13x2.ld \
	-Wl,-Map,build/itm_counter.map

build/itm_counter.elf: itm_counter.c cc13x2.ld | build
	arm-none-eabi-gcc $(CFLAGS) $(LDFLAGS) -o $@ itm_counter.c $(DRIVERLIB) -lgcc

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
