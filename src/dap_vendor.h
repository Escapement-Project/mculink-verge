/** The vendor commands' ids. DAP_ProcessVendorCommand (src/dap_vendor.c) sends each to its handler,
 * and dap_packet.h states its request's and reply's lengths. docs/host-interface.md has their bytes. */
#ifndef DAP_VENDOR_H
#define DAP_VENDOR_H

#include "DAP.h"

/// Identifies this firmware and says what it can do. Read-only, so a host may send it blindly.
#define ID_VENDOR_CJTAG_INFO ID_DAP_Vendor2
/// Gives J8's pins to the bench marker (marker.h), and back to DIO. A bench command.
#define ID_VENDOR_J8 ID_DAP_Vendor4
/// Reads the target's supply and J6 pin 9's detect line (target_sense.c).
#define ID_VENDOR_TARGET ID_DAP_Vendor6
/// DPIDR reads with the SWDIO sample placed by the host. An instrument; see sample_probe.c.
#define ID_VENDOR_SAMPLE ID_DAP_Vendor7
/// Runs host-supplied code from RAM, masked. An instrument, built only with CODE_LAB; see code_lab.c.
#define ID_VENDOR_LAB ID_DAP_Vendor8
/// Reports the generated SWD paths and sets the read latency they are generated for (swd_sweep.c).
#define ID_VENDOR_SWD_GEN ID_DAP_Vendor9
/// The same for the generated cJTAG frames (jtag_sweep.c).
#define ID_VENDOR_JTAG_GEN ID_DAP_Vendor10
/// Switches the generated JTAG transfers' learned pacing (jtag_sweep.c).
#define ID_VENDOR_JTAG_PACING ID_DAP_Vendor11
/// Switches whether CPU 0 stages commands for CPU 1 (dap_stage in dap_shared.h).
#define ID_VENDOR_STAGE ID_DAP_Vendor12
/// Reads back the JTAG generator's log of what each generation was for (jtag_sweep.c).
#define ID_VENDOR_JTAG_LOG ID_DAP_Vendor13
/// Drives and reads the DIO pins: the Pro's on J19, the plain board's on J8 (dio.h, dio.c).
#define ID_VENDOR_DIO ID_DAP_Vendor14
/// Switches and reads the Pro's target supply (target_power.h).
#define ID_VENDOR_TARGET_POWER ID_DAP_Vendor15
/// Starts and stops voltage/current capture on the Pro.
#define ID_VENDOR_ENERGY ID_DAP_Vendor16
/// Executes one full-duplex transaction on the Pro's hardware SPI pins.
#define ID_VENDOR_SPI ID_DAP_Vendor17
/// Executes one Pro I2C write, read, or combined transaction.
#define ID_VENDOR_I2C ID_DAP_Vendor18
/// Marks a moment in Verge Aux's energy stream, stamped as the command runs
/// (docs/host-interface.md, "Markers: 0x94").
#define ID_VENDOR_MARK ID_DAP_Vendor20
/// DAP_JTAG_Format and DAP_JTAG_Escape as proposed for CMSIS-DAP (0x24 and 0x25 there), until ARM
/// assigns them: `ID Format` and `ID EdgeCount Level`, each answered `ID Status` (src/cjtag.h).
#define ID_VENDOR_JTAG_FORMAT ID_DAP_Vendor21
#define ID_VENDOR_JTAG_ESCAPE ID_DAP_Vendor22

#endif
