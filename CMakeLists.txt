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

family_initialize_project(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR})

add_executable(${PROJECT_NAME})

set(DAP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Source)

target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ser_num.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_pins.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swo_board.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/vcom.c
  ${DAP_SRC}/DAP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_vendor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp.c
  ${DAP_SRC}/SW_DP.c
  ${DAP_SRC}/SWO.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/lpc55s69-sdk/fsl_usart_cmsis.c
  ${DAP_SRC}/JTAG_DP.c
  )

# Upstream sources compile as they ship. TinyUSB's example flags are far stricter than either ARM
# or NXP builds their code with, and carrying patches to make third-party files warning-clean would
# mean re-applying them at every version bump. Our own sources keep the full set.
set(UPSTREAM_RELAX
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-redundant-decls
  -Wno-sign-compare
  -Wno-cast-qual
  -Wno-cast-align
  )

set_source_files_properties(
  ${DAP_SRC}/DAP.c ${DAP_SRC}/SW_DP.c ${DAP_SRC}/JTAG_DP.c ${DAP_SRC}/SWO.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/lpc55s69-sdk/fsl_usart_cmsis.c
  PROPERTIES COMPILE_OPTIONS "${UPSTREAM_RELAX}"
  )

# ARM's JTAG entry points are renamed here so src/cjtag_dp.c can own the real names and dispatch
# between four-wire and OScan1. Renaming at compile time rather than patching keeps the submodule
# at a released tag.
set_source_files_properties(${DAP_SRC}/JTAG_DP.c PROPERTIES COMPILE_DEFINITIONS
  "JTAG_Sequence=jtag4_Sequence;JTAG_IR=jtag4_IR;JTAG_ReadIDCode=jtag4_ReadIDCode;JTAG_WriteAbort=jtag4_WriteAbort;JTAG_Transfer=jtag4_Transfer"
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
  PROPERTIES COMPILE_OPTIONS "-funroll-loops"
  )

target_include_directories(${PROJECT_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Include
  ${DAP_SRC}
  ${CMAKE_CURRENT_SOURCE_DIR}/external/lpc55s69-sdk
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/lib/CMSIS_6/CMSIS/Driver/Include
  # fsl_usart_cmsis.c reaches fsl_usart_dma.h through the usart header whether or not DMA is
  # enabled, and that wants fsl_dma.h. Headers only: no DMA source is compiled.
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/mcu/nxp/mcuxsdk-core/drivers/lpc_dma
  )

family_configure_device_example(${PROJECT_NAME} noos)
