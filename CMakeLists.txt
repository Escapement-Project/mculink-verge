cmake_minimum_required(VERSION 3.20)

# -O3, not the board support's -Os. What this firmware sells is the speed of a bit-banged clock,
# and the image is a small fraction of the part's flash. family_support.cmake forces MinSizeRel on
# an unset build type, so the choice has to be made before it is included.
if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# TinyUSB's board support carries the toolchain file, startup code, linker script and clock setup
# for the MCU-Link, so the only board code here is the debug interface pin map.
include(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/bsp/family_support.cmake)

project(mculink_cjtag C CXX ASM)

# Link-time optimisation. family_support.cmake asks for it as well, but it gates on
# check_ipo_supported, which runs before project() has enabled a language and so always answers
# no. The probe cannot be repeated from here either: it builds a throwaway project against the
# same toolchain file, whose includes are relative paths out of the real one. So set it outright,
# behind a switch for bisecting a miscompilation.
option(LTO "Build with link-time optimisation" ON)
if(LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Run the unrolled bit paths out of SRAM rather than flash. See src/hot.h for what the bench
# measured; behind a switch so the two can be compared on one unit without editing a source file.
#
# Not available in a Debug build: unrolled at -O0 the four functions come to about 300 KB, and
# m_data is 204 KB and also holds the stack. Refuse it here rather than leave the linker to report
# a 119 KB overflow, since Debug exists only to bisect a miscompilation.
option(RAMFUNC "Run the bit paths from SRAM" ON)

# Run the core at 150 MHz on PLL0 rather than the 144 MHz on PLL1 the board support leaves.
# See src/core_clock.h for why the 144 does not bind this firmware.
option(CORE_150MHZ "Run the core at 150 MHz" ON)

# Drive J8's four pins high while a host command runs, so an analyser sees the gap between
# commands as well as the clock. src/marker.h says why that is worth a pin.
option(MARKER "Mark command processing on the J8 header" ON)
# Sample SWDIO just after the rising edge on the delayed SWD path, with each transfer masked. Off
# until the bench has settled it; BENCH.md has the measurements behind it.
option(SWD_SAMPLE_AFTER_EDGE "Sample SWDIO after the rising edge on the delayed SWD path" OFF)
# Generate the SWD transfer paths at run time into SRAMX, for the clock the host asks for. See
# src/swd_gen.c. Off builds ARM's paths alone, the wire tools/equiv.py holds the generated one to.
option(SWD_CODEGEN "Generate the SWD transfer paths at run time" ON)
# Generate the four-wire and two-pin (OScan1) JTAG paths at run time into SRAMX. See
# src/jtag_gen.c. Off builds ARM's paths alone.
option(JTAG_CODEGEN "Generate the JTAG transfer paths at run time" ON)
# Run TinyUSB's device stack from SRAM. Flash runs at 11 wait states, and the stack's interrupt
# handler and task are branchy code the prefetcher cannot cover; they are paid once per command.
option(USB_RAM "Run TinyUSB from SRAM" ON)
# Vendor 0x88, which runs code the host sends. A bench instrument for the emulator's cost model;
# src/code_lab.c says what it measures.
option(CODE_LAB "Build the vendor 0x88 code lab" OFF)
# Add shared-timer counters for the USB and two-core command path. Bench-only; off in shipped images.
option(DAP_TIMING_DIAGNOSTICS "Measure command handoff stages" OFF)
if(RAMFUNC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR
    "RAMFUNC does not fit a Debug build: the unrolled bit paths are about 300 KB at -O0 and SRAM "
    "is 204 KB. Configure with -DRAMFUNC=OFF to bisect in Debug.")
endif()

family_initialize_project(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR})

add_executable(${PROJECT_NAME})

# The build's identity, regenerated every build so it cannot go stale between a commit and a
# flash. Two units running different builds of ours are otherwise indistinguishable over USB.
# A build option that moves what goes on the wire away from the default is named in it too, or
# two images built from one commit answer alike.
set(FW_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/fw_version.h)
set(FW_VARIANT "")
if(SWD_SAMPLE_AFTER_EDGE)
  string(APPEND FW_VARIANT "+after-edge")
endif()
if(NOT SWD_CODEGEN AND NOT JTAG_CODEGEN)
  string(APPEND FW_VARIANT "+armwire")
elseif(NOT SWD_CODEGEN)
  string(APPEND FW_VARIANT "+armswd")
elseif(NOT JTAG_CODEGEN)
  string(APPEND FW_VARIANT "+armjtag")
endif()
if(CODE_LAB)
  string(APPEND FW_VARIANT "+lab")
endif()
if(DAP_TIMING_DIAGNOSTICS)
  string(APPEND FW_VARIANT "+timing")
endif()

add_custom_target(fw_version ALL
  BYPRODUCTS ${FW_VERSION_HEADER}
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUTPUT=${FW_VERSION_HEADER}
    -DVARIANT=${FW_VARIANT}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fw_version.cmake
  COMMENT "Resolving the firmware version"
  VERBATIM
  )
add_dependencies(${PROJECT_NAME} fw_version)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(DAP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Source)

# CPU 0's image: USB, the console and the way back to the bootloader. Every DAP command runs in
# CPU 1's image, which is built below and embedded here by src/core1_image.S.
target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core1_image.S
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mem.S
  ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ser_num.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_straps.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_clocks.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/reboot.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/led.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/vcom.c
  )

# CPU 1's image. CPU 1 has no FPU, no DSP extension and no MPU (LPC55S6x data sheet, section 7.4),
# so it is compiled for that core rather than CPU 0's.
set(CORE1 ${PROJECT_NAME}_core1)
add_executable(${CORE1})
add_dependencies(${CORE1} fw_version)
target_include_directories(${CORE1} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(CORE1_CPU_FLAGS -mcpu=cortex-m33+nodsp+nofp -mfpu=auto -mfloat-abi=soft)
else()
  set(CORE1_CPU_FLAGS -mcpu=cortex-m33+nodsp+nofp -mfpu=none -mfloat-abi=soft)
endif()
# After the toolchain's CPU 0 flags on every command line, so these are the ones that count.
target_compile_options(${CORE1} PRIVATE ${CORE1_CPU_FLAGS} ${WARN_FLAGS_${CMAKE_C_COMPILER_ID}})
target_compile_definitions(${CORE1} PRIVATE CPU_LPC55S69JBD64_cm33_core1 SPI_RETRY_TIMES=100000U
                                             I2C_RETRY_TIMES=100000U I2C_MASTER_TRANSMIT_IGNORE_LAST_NACK=0U)

set(MCUX_DRIVERS ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/mcu/nxp/mcuxsdk-core/drivers)
set(LPC55_DEVICE ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/mcu/nxp/mcux-devices-lpc/LPC5500)

target_sources(${CORE1} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core1_start.S
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core1_main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mem.S
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_pins.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ser_num.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/target_sense.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/energy_monitor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/target_power.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/pro_bus.c
  ${MCUX_DRIVERS}/lpadc/fsl_lpadc.c
  ${MCUX_DRIVERS}/flexcomm/spi/fsl_spi.c
  ${MCUX_DRIVERS}/flexcomm/fsl_flexcomm.c
  ${MCUX_DRIVERS}/flexcomm/i2c/fsl_i2c.c
  ${LPC55_DEVICE}/LPC55S69/drivers/fsl_clock.c
  ${LPC55_DEVICE}/LPC55S69/drivers/fsl_reset.c
  ${DAP_SRC}/DAP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_vendor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sample_probe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp_fast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp_tracked.c
  ${DAP_SRC}/SW_DP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swd_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_dispatch.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_dp_block.c
  ${DAP_SRC}/JTAG_DP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swo.c
  )

target_include_directories(${CORE1} PRIVATE
  ${MCUX_DRIVERS}/common
  ${MCUX_DRIVERS}/lpc_iocon
  ${MCUX_DRIVERS}/lpc_gpio
  ${MCUX_DRIVERS}/lpadc
  ${MCUX_DRIVERS}/flexcomm
  ${MCUX_DRIVERS}/flexcomm/spi
  ${MCUX_DRIVERS}/flexcomm/i2c
  ${LPC55_DEVICE}/LPC55S69
  ${LPC55_DEVICE}/LPC55S69/drivers
  ${LPC55_DEVICE}/periph
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/lib/CMSIS_6/CMSIS/Core/Include
  )

# Upstream sources compile as they ship. TinyUSB's example flags are far stricter than either ARM
# or NXP builds their code with, and carrying patches to make third-party files warning-clean would
# mean re-applying them at every version bump. Our own sources keep the full set.
set(UPSTREAM_RELAX
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-missing-braces
  -Wno-redundant-decls
  -Wno-sign-compare
  -Wno-cast-qual
  -Wno-cast-align
  )

set_source_files_properties(
  ${DAP_SRC}/DAP.c ${DAP_SRC}/SW_DP.c ${DAP_SRC}/JTAG_DP.c
  ${MCUX_DRIVERS}/flexcomm/spi/fsl_spi.c
  ${MCUX_DRIVERS}/flexcomm/fsl_flexcomm.c
  ${MCUX_DRIVERS}/flexcomm/i2c/fsl_i2c.c
  ${LPC55_DEVICE}/LPC55S69/drivers/fsl_clock.c
  ${LPC55_DEVICE}/LPC55S69/drivers/fsl_reset.c
  PROPERTIES COMPILE_OPTIONS "${UPSTREAM_RELAX}"
  )

# ARM's SWD_Transfer is renamed out of the way so src/swd_dp.c can own the name with a cheaper
# data phase. The rest of SW_DP.c -- SWJ_Sequence and SWD_Sequence -- is still ARM's and still
# linked, so this is a replaced function rather than a replaced file.
set_source_files_properties(${DAP_SRC}/SW_DP.c PROPERTIES COMPILE_DEFINITIONS
  "SWD_Transfer=arm_SWD_Transfer"
  )

# The two command entry points are renamed the same way, so src/dap_dispatch.c can own them and
# send a SWD transfer block down the shorter route in src/swd_dp.c. Everything else reaches ARM's
# processing unchanged.
set_source_files_properties(${DAP_SRC}/DAP.c PROPERTIES COMPILE_DEFINITIONS
  "DAP_ProcessCommand=arm_DAP_ProcessCommand;DAP_ExecuteCommand=arm_DAP_ExecuteCommand"
  )
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swd_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_dispatch.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_dp_block.c
  PROPERTIES COMPILE_OPTIONS "-funroll-loops"
  )

if(RAMFUNC)
  target_compile_definitions(${CORE1} PRIVATE RAMFUNC_BIT_PATHS=1)
endif()

# Clang reaches for its own compiler-rt start files and runtime, which the distribution only
# builds for the host. This firmware brings its own reset handler and linker script, so there are
# no start files to want, and libgcc from the arm-none-eabi toolchain supplies the helpers.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  # Clang's driver for a bare-metal arm target reaches for compiler-rt start files and libgloss,
  # neither of which the distribution builds for this triple. Everything needed is in the
  # arm-none-eabi gcc install instead, so take the libraries from there explicitly. gcc's
  # multilib for these flags is soft-float v8-m.main, which is what picks the directory.
  execute_process(COMMAND ${CMAKE_C_COMPILER} --target=arm-none-eabi -print-resource-dir
                  OUTPUT_VARIABLE _unused ERROR_QUIET)
  execute_process(COMMAND arm-none-eabi-gcc -mcpu=cortex-m33 -print-libgcc-file-name
                  OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
  get_filename_component(_libgcc_dir "${_libgcc}" DIRECTORY)
  execute_process(COMMAND arm-none-eabi-gcc -mcpu=cortex-m33 -print-multi-directory
                  OUTPUT_VARIABLE _multi OUTPUT_STRIP_TRAILING_WHITESPACE)
  target_link_options(${PROJECT_NAME} PRIVATE
    -nostdlib -nostartfiles -L${_libgcc_dir} -L/usr/arm-none-eabi/lib/${_multi})
  # As libraries rather than link options, so CMake puts them after the objects that need them.
  target_link_libraries(${PROJECT_NAME} PRIVATE -Wl,--start-group c_nano nosys gcc -Wl,--end-group)
endif()

# The toolchain's CPU 0 flags stay on CPU 1's command lines, ahead of CPU 1's own. They lose for
# code generation, but gcc still lets them steer its library choice, to the ARMv4T default. So
# CPU 1 names its libraries itself, under either compiler.
execute_process(COMMAND arm-none-eabi-gcc -mthumb -mcpu=cortex-m33+nodsp+nofp -mfloat-abi=soft
                        -print-libgcc-file-name
                OUTPUT_VARIABLE _core1_libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND arm-none-eabi-gcc -mthumb -mcpu=cortex-m33+nodsp+nofp -mfloat-abi=soft
                        -print-file-name=libc_nano.a
                OUTPUT_VARIABLE _core1_libc OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_core1_libgcc_dir "${_core1_libgcc}" DIRECTORY)
get_filename_component(_core1_libc_dir "${_core1_libc}" DIRECTORY)
if(NOT _core1_libgcc_dir MATCHES "v8-m.main/nofp$" OR NOT _core1_libc_dir MATCHES "v8-m.main/nofp$")
  message(FATAL_ERROR "No v8-M Mainline soft-float libraries for CPU 1: ${_core1_libgcc_dir}, ${_core1_libc_dir}")
endif()
target_link_options(${CORE1} PRIVATE -nostdlib -nostartfiles -L${_core1_libgcc_dir} -L${_core1_libc_dir})
target_link_libraries(${CORE1} PRIVATE -Wl,--start-group c_nano nosys gcc -Wl,--end-group)
if(NO_WARN_RWX_SEGMENTS_SUPPORTED)
  target_link_options(${CORE1} PRIVATE "LINKER:--no-warn-rwx-segments")
endif()

# How many host commands the firmware will hold. DAP_PACKET_COUNT in src/DAP_config.h has to
# match, because it is what the host is told.
# Three, because a host keeps its pipeline one packet short of what the probe says it holds.
# That spare buffer is for DAP_TransferAbort to arrive in, so a probe advertising two gets a
# pipeline of one and no overlap at all. Two commands in flight is what saturates the wire --
# measured 669 KiB/s at depths 2, 4 and 8 with the slots to match -- so three slots is what lets
# a conforming host reach it. Through probe-rs it is worth 4.5% on reads and 4.8% on writes.
set(REQUEST_SLOTS 3 CACHE STRING "How many host commands the firmware will hold")
if(NOT REQUEST_SLOTS)
  set(REQUEST_SLOTS 3 CACHE STRING "How many host commands the firmware will hold" FORCE)
endif()
set(DAP_PACKET_COUNT ${REQUEST_SLOTS})

if(MARKER)
  target_compile_definitions(${CORE1} PRIVATE MARKER_PINS=1)
endif()

if(SWD_SAMPLE_AFTER_EDGE)
  target_compile_definitions(${CORE1} PRIVATE SWD_SAMPLE_AFTER_EDGE=1)
endif()

if(SWD_CODEGEN OR JTAG_CODEGEN)
  target_sources(${CORE1} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/gen.c)
endif()
if(SWD_CODEGEN)
  target_sources(${CORE1} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/swd_gen.c
                                  ${CMAKE_CURRENT_SOURCE_DIR}/src/swd_sweep.c)
  target_compile_definitions(${CORE1} PRIVATE SWD_CODEGEN=1)
endif()
if(JTAG_CODEGEN)
  target_sources(${CORE1} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_gen.c
                                  ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_gen_dp.c
                                  ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_sweep.c)
  target_compile_definitions(${CORE1} PRIVATE JTAG_CODEGEN=1)
endif()

if(CODE_LAB)
  target_sources(${CORE1} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/code_lab.c)
  target_compile_definitions(${CORE1} PRIVATE CODE_LAB=1)
  target_compile_definitions(${PROJECT_NAME} PRIVATE CODE_LAB=1)
endif()

if(DAP_TIMING_DIAGNOSTICS)
  target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_timing.c)
  target_compile_definitions(${CORE1} PRIVATE DAP_TIMING_DIAGNOSTICS=1)
  target_compile_definitions(${PROJECT_NAME} PRIVATE DAP_TIMING_DIAGNOSTICS=1)
endif()

if(CORE_150MHZ)
  set(CORE_CLOCK_MHZ 150)
else()
  set(CORE_CLOCK_MHZ 144)
endif()

# How the two images share the part. CPU 1 runs the bit paths from RAM, and their load copies are
# most of its flash, so it gets the larger share of both. CPU 0's RAM is SRAM0 alone. CPU 1's is
# SRAM1 up to the top 8 KB of SRAM3, with its stack in SRAM4. The cores meet in that top of
# SRAM3, which holds dap_stage, the commands CPU 0 stages for CPU 1, and in USB SRAM, split between
# TinyUSB's buffers and dap_shared (both in src/dap_shared.h). Not SRAM4 for dap_stage: a load
# next to a store costs a cycle more there than in SRAM1 to SRAM3, and the generated transfers
# make those against their buffers.
set(CORE1_FLASH_ORIGIN 0x00040000)
set(CORE1_FLASH_END    0x0009D800)  # where the SDK's own reservation for CPU 1 ends
set(CORE0_RAM_LENGTH   0x00010000)
set(CORE1_RAM_ORIGIN   0x20010000)
set(CORE1_RAM_LENGTH   0x0002E000)
set(DAP_STAGE_ORIGIN   0x2003E000)
set(DAP_STAGE_LENGTH   0x00002000)
set(CORE1_STACK_ORIGIN 0x20040000)
set(CORE1_STACK_LENGTH 0x00004000)
set(CORE0_USB_LENGTH   0x00001000)
set(DAP_SHARED_ORIGIN  0x40101000)
set(DAP_SHARED_LENGTH  0x00003000)
math(EXPR CORE1_FLASH_LENGTH "${CORE1_FLASH_END} - ${CORE1_FLASH_ORIGIN}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR CORE0_TEXT_LENGTH "${CORE1_FLASH_ORIGIN} - 0x200" OUTPUT_FORMAT HEXADECIMAL)

foreach(_target ${PROJECT_NAME} ${CORE1})
  target_compile_definitions(${_target} PRIVATE
    REQUEST_SLOTS=${REQUEST_SLOTS}U
    DAP_PACKET_COUNT_OVERRIDE=${REQUEST_SLOTS}U
    CORE1_RAM_ORIGIN=${CORE1_RAM_ORIGIN}U
    CORE1_RAM_LENGTH=${CORE1_RAM_LENGTH}U
    $<$<BOOL:${CORE_150MHZ}>:CORE_150MHZ=1>
    )
  target_include_directories(${_target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Include
    ${DAP_SRC}
    )
endforeach()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/core1.ld.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core1.ld @ONLY)
target_link_options(${CORE1} PRIVATE
  ${CORE1_CPU_FLAGS}
  "LINKER:--script=${CMAKE_CURRENT_BINARY_DIR}/generated/core1.ld"
  "LINKER:-Map=$<TARGET_FILE:${CORE1}>.map"
  )
set_property(TARGET ${CORE1} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/core1.ld)

# CPU 1's binary, which src/core1_image.S embeds in CPU 0's image.
set(CORE1_BIN ${CMAKE_CURRENT_BINARY_DIR}/${CORE1}.bin)
add_custom_command(OUTPUT ${CORE1_BIN}
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CORE1}> ${CORE1_BIN}
  DEPENDS ${CORE1}
  COMMENT "Extracting CPU 1's image"
  VERBATIM
  )
add_custom_target(core1_bin DEPENDS ${CORE1_BIN})
add_dependencies(${PROJECT_NAME} core1_bin)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/core1_image.S PROPERTIES
  COMPILE_DEFINITIONS "CORE1_BIN=\"${CORE1_BIN}\""
  OBJECT_DEPENDS ${CORE1_BIN}
  )

# ARM's JTAG entry points are renamed here so src/cjtag_dp.c can own the real names and dispatch
# between four-wire and OScan1. Renaming at compile time rather than patching keeps the submodule
# at a released tag.
set_source_files_properties(${DAP_SRC}/JTAG_DP.c PROPERTIES COMPILE_DEFINITIONS
  "JTAG_Sequence=jtag4_Sequence;JTAG_IR=jtag4_IR;JTAG_ReadIDCode=jtag4_ReadIDCode;JTAG_WriteAbort=jtag4_WriteAbort;JTAG_Transfer=arm_jtag4_Transfer"
  )

# Unroll the bit paths. ARM unrolls the packet phases of a transfer by hand but leaves the 32-bit
# data phases as loops, so the data bits carry a loop branch and an induction variable that the
# packet bits do not: measured on the emulator, 22 cycles a bit against 16 at the same setting.
# Only these three files, because unrolling anything else is flash spent for nothing.
set_source_files_properties(
  ${DAP_SRC}/SW_DP.c ${DAP_SRC}/JTAG_DP.c
  PROPERTIES COMPILE_OPTIONS "${UPSTREAM_RELAX};-funroll-loops"
  )
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp_fast.c
  PROPERTIES COMPILE_OPTIONS "-funroll-loops"
  )

target_include_directories(${PROJECT_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Include
  ${DAP_SRC}
  )

# The ADC driver's calibration waits spin on a status bit for ever unless these are set. A probe
# that hangs because a host asked for the target's voltage is a worse failure than a reading that
# says it failed, so every wait is bounded. Counts of poll iterations: tens of milliseconds.
target_compile_definitions(${CORE1} PRIVATE
  CONFIG_LPADC_CONVERSION_COMPLETE_TIMEOUT=1000000U
  CONFIG_LPADC_CALIBRATION_READY_TIMEOUT=1000000U
  CONFIG_LPADC_GAIN_CAL_READY_TIMEOUT=1000000U
  )

# CPU 0's link script is the board support's, rewritten rather than copied, so a submodule bump
# carries its changes over. Each rewrite checks that it found what it replaces.
file(READ ${LD_FILE_GNU} _ld)
function(rewrite_ld pattern replacement what)
  string(REGEX MATCH "${pattern}" _found "${_ld}")
  if(NOT _found)
    message(FATAL_ERROR "${LD_FILE_GNU} no longer has ${what}")
  endif()
  string(REGEX REPLACE "${pattern}" "${replacement}" _ld "${_ld}")
  set(_ld "${_ld}" PARENT_SCOPE)
endfunction()
rewrite_ld("(m_text +\\(RX\\) +: ORIGIN = 0x00000200, LENGTH = )0x[0-9A-Fa-f]+"
           "\\1${CORE0_TEXT_LENGTH}" "the m_text region")
rewrite_ld("(m_core1_image +\\(RX\\) +: )ORIGIN = 0x[0-9A-Fa-f]+, LENGTH = 0x[0-9A-Fa-f]+"
           "\\1ORIGIN = ${CORE1_FLASH_ORIGIN}, LENGTH = ${CORE1_FLASH_LENGTH}" "the m_core1_image region")
rewrite_ld("(m_data +\\(RW\\) +: ORIGIN = 0x20000000, LENGTH = )0x[0-9A-Fa-f]+ - RPMSG_SHMEM_SIZE"
           "\\1${CORE0_RAM_LENGTH}" "the m_data region")
rewrite_ld("(m_usb_sram +\\(RW\\) +: ORIGIN = 0x40100000, LENGTH = )0x[0-9A-Fa-f]+"
           "\\1${CORE0_USB_LENGTH}\n  m_dap_shared (RW) : ORIGIN = ${DAP_SHARED_ORIGIN}, LENGTH = ${DAP_SHARED_LENGTH}\n  m_dap_stage (RW) : ORIGIN = ${DAP_STAGE_ORIGIN}, LENGTH = ${DAP_STAGE_LENGTH}"
           "the m_usb_sram region")
# The boards this image runs on, for verge to read from the file (src/board_straps.c), right after
# the vector table.
rewrite_ld("(KEEP\\(\\*\\(\\.isr_vector\\)\\)[^\n]*\n)"
           "\\1    KEEP(*(.image_boards))\n" "the vector table's KEEP")
rewrite_ld("(\n  m_usb_global \\(NOLOAD\\) :)"
           "\n  .dap_shared (NOLOAD) :\n  {\n    KEEP(*(.dap_shared))\n  } > m_dap_shared\n  .dap_stage (NOLOAD) :\n  {\n    KEEP(*(.dap_stage))\n  } > m_dap_stage\n\\1"
           "the m_usb_global section")

# memcpy goes into the RAM copy of .data, as on CPU 1: it stages every command. USB_RAM moves
# TinyUSB's code there too. The linker places input sections by file, which LTO erases, so those
# sources compile without it.
set(_ram_files "*mem.S.obj")
set(_ram_inputs "\n    *mem.S.obj(.text .text*)")
if(USB_RAM)
  string(APPEND _ram_files " */tinyusb/src/*")
  string(APPEND _ram_inputs "\n    */tinyusb/src/*(.text .text*)")
endif()
rewrite_ld("\\*\\(\\.text\\)" "*(EXCLUDE_FILE(${_ram_files}) .text)" "*(.text)")
rewrite_ld("\\*\\(\\.text\\*\\)" "*(EXCLUDE_FILE(${_ram_files}) .text*)" "*(.text*)")
rewrite_ld("\\*\\(\\.ramfunc\\*\\)" "*(.ramfunc*)${_ram_inputs}" "the .ramfunc line to place code in RAM after")
set(LD_FILE_GNU ${CMAKE_CURRENT_BINARY_DIR}/generated/core0.ld)
set(LD_FILE_Clang ${LD_FILE_GNU})
file(WRITE ${LD_FILE_GNU} "${_ld}")

family_configure_device_example(${PROJECT_NAME} noos)

# The firmware targets mix project C with upstream C. Apply C23 only to project-owned
# translation units so upstream sources keep the dialect selected by their own toolchain.
file(GLOB FIRST_PARTY_C_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set_property(SOURCE ${FIRST_PARTY_C_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-std=gnu23")

if(USB_RAM)
  get_target_property(_sources ${PROJECT_NAME} SOURCES)
  list(FILTER _sources INCLUDE REGEX "/tinyusb/src/")
  set_property(SOURCE ${_sources} APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
endif()

# TinyUSB's hw/bsp/board.c is stdio retargeting and weak board stubs, and this firmware uses
# none of it: the console is its own CDC interface and nothing here calls printf. It also assumes
# clang means picolibc and redefines FILE, stdin, stdout and stderr on that basis, which does not
# compile against the newlib that an arm-none-eabi clang finds here. Dropping the file is what
# makes the two toolchains build the same sources rather than one of them needing another libc.
#
# The board's peripherals.c goes too. Inside board_init it starts Flexcomm 0 as a log console on
# P0_29 and P0_30, and on the Pro P0_30 is an input from the energy measurement circuit, which the
# console's idle TXD would drive against until vcom_init took the pin back. src/vcom.c has the
# empty BOARD_InitBootPeripherals that board_init calls instead.
#
# Both arrive as PUBLIC, so they are on both properties and both have to lose them.
foreach(_property SOURCES INTERFACE_SOURCES)
  get_target_property(_sources ${PROJECT_NAME} ${_property})
  if(_sources)
    list(FILTER _sources EXCLUDE REGEX "/(board|peripherals)\\.c$")
    set_property(TARGET ${PROJECT_NAME} PROPERTY ${_property} ${_sources})
  endif()
endforeach()


# `ninja -C build check` runs the gate against what was just built. It needs uv, which fetches its
# own Python and the emulator, so it is a target rather than part of the build.
find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
  set(CYCLES_CHECK_DEFAULT_JOBS 1)
  set(CYCLES_AVAILABLE_CPUS "")
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/host_resources.py"
      OUTPUT_VARIABLE CYCLES_AVAILABLE_CPUS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE CPU_COUNT_RESULT
    )
    if(NOT CPU_COUNT_RESULT EQUAL 0 OR NOT CYCLES_AVAILABLE_CPUS MATCHES "^[0-9]+$")
      set(CYCLES_AVAILABLE_CPUS "")
    endif()
  endif()
  if(NOT CYCLES_AVAILABLE_CPUS)
    find_program(NPROC_EXECUTABLE nproc)
    if(NPROC_EXECUTABLE)
      execute_process(
        COMMAND ${NPROC_EXECUTABLE}
        OUTPUT_VARIABLE CYCLES_AVAILABLE_CPUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE NPROC_RESULT
      )
      if(NOT NPROC_RESULT EQUAL 0 OR NOT CYCLES_AVAILABLE_CPUS MATCHES "^[0-9]+$")
        set(CYCLES_AVAILABLE_CPUS "")
      endif()
    endif()
  endif()
  if(CYCLES_AVAILABLE_CPUS GREATER 1)
    if(CYCLES_AVAILABLE_CPUS GREATER 4)
      set(CYCLES_CHECK_DEFAULT_JOBS 4)
    else()
      set(CYCLES_CHECK_DEFAULT_JOBS ${CYCLES_AVAILABLE_CPUS})
    endif()
  endif()
  set(CYCLES_CHECK_JOBS ${CYCLES_CHECK_DEFAULT_JOBS} CACHE STRING
      "Independent wire checks to run in parallel")
  set(IMAGE_CHECK_ARGS --packet-count ${DAP_PACKET_COUNT} --core-clock-mhz ${CORE_CLOCK_MHZ})
  if(CODE_LAB)
    list(APPEND IMAGE_CHECK_ARGS --code-lab)
  endif()
  if(DAP_TIMING_DIAGNOSTICS)
    list(APPEND IMAGE_CHECK_ARGS --timing-diagnostics)
  endif()
  add_custom_target(check_wire
    COMMAND ${UV_EXECUTABLE} run ${CMAKE_CURRENT_SOURCE_DIR}/tools/cycles.py
            --check $<TARGET_FILE:${CORE1}>
            --jobs ${CYCLES_CHECK_JOBS}
            --cpu ${CORE_CLOCK_MHZ}
            --config ${CMAKE_CURRENT_SOURCE_DIR}/src/DAP_config.h
    DEPENDS ${CORE1}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking wire events against tools/baseline"
  )
  add_custom_target(check_image
    COMMAND ${UV_EXECUTABLE} run ${CMAKE_CURRENT_SOURCE_DIR}/tools/image.py
            $<TARGET_FILE:${PROJECT_NAME}> --core1 $<TARGET_FILE:${CORE1}> ${IMAGE_CHECK_ARGS}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking image descriptors and internal layout"
  )
  add_custom_target(check
    DEPENDS check_wire check_image
    COMMENT "Checking the wire against tools/baseline, and the descriptors against themselves"
  )
endif()
