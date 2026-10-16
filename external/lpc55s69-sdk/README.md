# Vendored NXP SDK files

`fsl_usart_cmsis.{c,h}` from `NXP.LPC55S69_DFP` version 26.09.00, fetched from
`https://mcuxpresso.nxp.com/cmsis_pack/repo/`.

These are here rather than in a submodule because the pack is a zip rather than a repository.
Everything else the build needs from NXP -- `fsl_usart.c`, `fsl_flexcomm.c`, `fsl_clock.c`, the
startup code and the linker script -- comes from TinyUSB's board support, which tracks the same
upstream.

The file implements the CMSIS-Driver USART interface that ARM's `SWO.c` expects. Nothing in it is
modified; its configuration comes from `src/RTE_Device.h`.
