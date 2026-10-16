cmake_minimum_required(VERSION 3.20)

# TinyUSB's board support carries the toolchain file, startup code, linker script and clock setup
# for the MCU-Link, so the only board code here is the debug interface pin map.
include(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/bsp/family_support.cmake)

project(mculink_cjtag C CXX ASM)

family_initialize_project(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR})

add_executable(${PROJECT_NAME})

set(DAP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Source)

target_sources(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ser_num.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/board_pins.c
  ${DAP_SRC}/DAP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dap_vendor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_dp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cjtag_jtag_dp.c
  ${DAP_SRC}/SW_DP.c
  ${DAP_SRC}/JTAG_DP.c
  )

# ARM's sources are compiled as they ship. TinyUSB's example flags include -Werror, and DAP.c
# leaves a variable unused when SWO is configured out, which is upstream's code and not a defect
# worth carrying a patch for.
set_source_files_properties(
  ${DAP_SRC}/DAP.c ${DAP_SRC}/SW_DP.c ${DAP_SRC}/JTAG_DP.c
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-but-set-variable"
  )

# ARM's JTAG entry points are renamed here so src/cjtag_dp.c can own the real names and dispatch
# between four-wire and OScan1. Renaming at compile time rather than patching keeps the submodule
# at a released tag.
set_source_files_properties(${DAP_SRC}/JTAG_DP.c PROPERTIES COMPILE_DEFINITIONS
  "JTAG_Sequence=jtag4_Sequence;JTAG_IR=jtag4_IR;JTAG_ReadIDCode=jtag4_ReadIDCode;JTAG_WriteAbort=jtag4_WriteAbort;JTAG_Transfer=jtag4_Transfer"
  )

target_include_directories(${PROJECT_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/external/CMSIS-DAP/Firmware/Include
  ${DAP_SRC}
  )

family_configure_device_example(${PROJECT_NAME} noos)
