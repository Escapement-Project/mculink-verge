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
| DIO pins | Not available | Four pins on J19 | J15 disables USB-SIO and leaves the pins as inputs. DIO0–2 change direction as a group; DIO3 changes on its own. |
| SPI and I2C bridges | Not available | SPI on DIO0–DIO3; I2C on Flexcomm 3 | CMSIS-DAP vendor commands `0x91` and `0x92`. J15 disables both. SPI shares the DIO pins; I2C shares Flexcomm 3 with SWO. |
| Target supply | Not available | Switchable 1.8 V or 3.3 V | `tools/dap.py target-power` controls it. J5 selects voltage and J6 connects it to the target. Firmware disables it during startup. |
| Energy capture | Not available | Calibrated voltage/current readings and accumulated energy | Capture is available when HW_VER_5 is fitted. Calibration is verified in both 350 mA and 50 mA jumper settings. |
| Firmware update | Supported by `verge` | Supported by `verge` | A USB restart can enter the bootloader without the ISP jumper. The ISP jumper remains the recovery path for an image that does not start. |

The Pro also has LEDs for board, SWO, serial, debug-wire and DIO activity. Energy capture retains
raw ADC codes and adds calibrated voltage/current fields in both current range settings. Analog
trace is not implemented.

## Performance figures

The [limits table](README.md#limits) labels each figure by its evidence. Its SWD values come from
the 2026-10-20 normal-path comparison; they do not measure the current source revision. A separate
timer-driven read experiment on 2026-10-21 reached 50 MHz, then was removed. The J-Link results are
a separate probe and target comparison and do not replace an MCU-Link run.

The firmware reports its maximum SWO baud as 30 Mbaud. Bench captures reached 30 Mbaud on the full
MCU-Link and 24 Mbaud on the Pro. The Pro result is limited by the available trace source, which
did not reach a higher rate.

## Related guides

- [Limits and build instructions](README.md)
- [Supported host extensions](HOST-INTERFACE.md)
- [`verge` update and recovery guide](https://github.com/Escapement-Project/verge)
