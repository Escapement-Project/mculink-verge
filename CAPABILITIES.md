# Capabilities

This firmware runs on a standalone full MCU-Link or MCU-Link Pro. It does not run on an MCU-Link
built into an evaluation board. The firmware reads the function jumpers at startup. Unplug the
probe before fitting or removing one.

| Feature | Full MCU-Link | MCU-Link Pro | Conditions |
| --- | --- | --- | --- |
| CMSIS-DAP over USB | Yes | Yes | J5 on the full MCU-Link or J13 on the Pro disables the debug interface. |
| SWD and four-wire JTAG | Yes | Yes | CMSIS-DAP. JTAG speed depends on the target's TDO delay. |
| Two-pin cJTAG | OScan1, OScan2, OScan5, OScan6 | OScan1, OScan2, OScan5, OScan6 | The host and target must use a shared scan format. The host chooses the format. |
| SWO capture | UART encoding | UART encoding | No Manchester encoding or streaming endpoint. The firmware reports a 30 Mbaud ceiling. |
| Target serial port | One USB serial port | Two USB serial ports | J4 on the full MCU-Link or J14 on the Pro disables the first port. The Pro's second port is on J26 and is 3.3 V only. |
| DIO pins | Not available | Four pins on J19 | J15 disables USB-SIO and leaves the pins as inputs. DIO0–2 change direction as a group; DIO3 changes on its own. They run at the voltage on J19 pin 14, or 3.3 V with nothing there. `verge dio` reads and drives them, as does `tools/dap.py dio` from a checkout. |
| SPI and I2C bridges | Not available | SPI on DIO0–DIO3; I2C on Flexcomm 3 | CMSIS-DAP vendor commands `0x91` and `0x92`. J15 disables both. SPI shares the DIO pins; I2C shares Flexcomm 3 with SWO. |
| Target supply | Not available | Switchable 1.8 V or 3.3 V, up to 400 mA | `verge power` controls it, as does `tools/dap.py target-power` from a checkout. J5 selects voltage and J6 connects it to the target. Firmware disables it during startup. |
| Energy capture | Not available | Calibrated voltage/current readings and accumulated energy | Capture is available when HW_VER_5 is fitted. Calibration is verified in both 350 mA and 50 mA jumper settings. |
| Verge Aux | Not available | A second vendor interface | Carries the DIO, target supply, energy and marker commands, so a tool can use them while a debugger holds CMSIS-DAP. Every Pro has it, whatever its jumpers. |
| Firmware update | Supported by `verge` | Supported by `verge` | A USB restart can enter the bootloader without the ISP jumper. The ISP jumper remains the recovery path for an image that does not start. |

The Pro also has LEDs for board, SWO, serial, debug-wire and DIO activity. Energy capture retains
raw ADC codes and adds calibrated voltage/current fields in both current range settings. Analog
trace is not implemented.

## Performance figures

The [limits table](README.md#limits) says where each figure comes from. Its SWD figures were
measured on 2026-10-20, on an older build than this source.

mculink-bench's [measurements page](https://github.com/Escapement-Project/mculink-bench) compares
this firmware with stock and with a SEGGER J-Link Ultra, through the same probe-rs workload on the
same target.

## Related guides

- [Limits and build instructions](README.md)
- [Supported host extensions](HOST-INTERFACE.md)
- [`verge` update and recovery guide](https://github.com/Escapement-Project/verge)
