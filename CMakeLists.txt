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
if(RAMFUNC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR
    "RAMFUNC does not fit a Debug build: the unrolled bit paths are about 300 KB at -O0 and SRAM "
    "is 204 KB. Configure with -DRAMFUNC=OFF to bisect in Debug.")
endif()

family_initialize_project(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR})

add_executable(${PROJECT_NAME})

# The build's identity, regenerated every build so it cannot go stale between a commit and a
# flash. Two units running different builds of ours are otherwise indistinguishable over USB.
set(FW_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/fw_version.h)
add_custom_target(fw_version ALL
  BYPRODUCTS ${FW_VERSION_HEADER}
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUTPUT=${FW_VERSION_HEADER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fw_version.cmake
  COMMENT "Resolving the firmware version"
  VERBATIM
  )
add_dependencies(${PROJECT_NAME} fw_version)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(DAP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Source)

target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ser_num.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_pins.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/reboot.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swo_board.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/vcom.c
  ${DAP_SRC}/DAP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_vendor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp_fast.c
  ${DAP_SRC}/SW_DP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/swd_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_dispatch.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/jtag_dp_block.c
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
  target_compile_definitions(${PROJECT_NAME} PRIVATE RAMFUNC_BIT_PATHS=1)
endif()

if(MARKER)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MARKER_PINS=1)
endif()


if(CORE_150MHZ)
  target_compile_definitions(${PROJECT_NAME} PRIVATE CORE_150MHZ=1)
  set(CORE_CLOCK_MHZ 150)
else()
  set(CORE_CLOCK_MHZ 144)
endif()

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
  ${CMAKE_CURRENT_SOURCE_DIR}/external/lpc55s69-sdk
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/lib/CMSIS_6/CMSIS/Driver/Include
  # fsl_usart_cmsis.c reaches fsl_usart_dma.h through the usart header whether or not DMA is
  # enabled, and that wants fsl_dma.h. Headers only: no DMA source is compiled.
  ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/mcu/nxp/mcuxsdk-core/drivers/lpc_dma
  )

family_configure_device_example(${PROJECT_NAME} noos)

# `ninja -C build check` runs the gate against what was just built. It needs uv, which fetches its
# own Python and the emulator, so it is a target rather than part of the build.
find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
  add_custom_target(check
    COMMAND ${UV_EXECUTABLE} run ${CMAKE_CURRENT_SOURCE_DIR}/tools/cycles.py
            --check $<TARGET_FILE:${PROJECT_NAME}>
            --cpu ${CORE_CLOCK_MHZ}
            --config ${CMAKE_CURRENT_SOURCE_DIR}/src/DAP_config.h
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND ${UV_EXECUTABLE} run ${CMAKE_CURRENT_SOURCE_DIR}/tools/image.py
            $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Checking the wire against tools/baseline, and the descriptors against themselves"
    USES_TERMINAL
    )
endif()
