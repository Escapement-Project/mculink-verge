# Probes and jumpers

Verge runs on NXP's standalone MCU-Link and MCU-Link Pro. One image runs on both, and verge installs
it on nothing else: an MCU-Link built into an evaluation board is not supported.

## What each probe has

| Feature | Standalone MCU-Link | MCU-Link Pro | Conditions |
| --- | --- | --- | --- |
| CMSIS-DAP over USB | Yes | Yes | The SWD jumper turns the debug interface off ([below](#jumpers)). |
| SWD and JTAG | Yes | Yes | JTAG's clock depends on the target's TDO delay ([Wires and clocks](wires.md#jtag)). |
| cJTAG | OScan1, OScan2, OScan5, OScan6 | OScan1, OScan2, OScan5, OScan6 | The host chooses a format the probe and the target share. |
| SWO capture | UART encoding | UART encoding | No Manchester encoding and no streaming endpoint; up to 30 Mbaud ([Trace and serial ports](trace-and-serial.md#swo)). |
| Target serial port | One | Two | The VCOM jumper turns the first off. The Pro's second is on J26, 3.3 V only. |
| DIO pins | Four, on J8 | Four, on J19 | The standalone MCU-Link's are 3.3 V only ([below](#the-standalone-mcu-links-dio-pins)). On the Pro, J15 turns them off, leaving them inputs ([The Pro](pro.md#dio-pins)). |
| SPI and I2C bridges | No | SPI on DIO0 to DIO3; I2C on Flexcomm 3 | J15 turns both off. SPI shares the DIO pins; I2C shares Flexcomm 3 with SWO ([The Pro](pro.md#spi-and-i2c-bridges)). |
| Target supply | No | 1.8 V or 3.3 V, up to 400 mA | J5 picks the voltage and J6 connects it. Off whenever the Pro starts ([The Pro](pro.md#target-supply)). |
| Energy capture | No | Voltage, current and energy | J16 to J18 pick the 350 mA or 50 mA setting ([Energy capture](https://github.com/Escapement-Project/verge/blob/HEAD/docs/energy.md)). |
| Verge Aux | A second vendor interface | A second vendor interface | Every probe has it, whatever its jumpers ([The Pro](pro.md#verge-aux)). On the standalone MCU-Link it carries the DIO pins alone. |
| Firmware update | With verge | With verge | No ISP jumper needed for a probe already running Verge. The ISP jumper stays the way back for an image that does not start ([Updating and recovery](https://github.com/Escapement-Project/verge/blob/HEAD/docs/updating.md#when-an-update-needs-your-hands)). |

Verge does not support the Pro's analog trace input.

## Jumpers

The function jumpers work as they do with NXP's firmware. Each turns a function off. The probe reads
them once, as it starts, so fit or remove one with the probe unplugged.

| Function | Standalone MCU-Link | MCU-Link Pro | With the jumper fitted |
| --- | --- | --- | --- |
| VCOM | J4 | J14 | No target serial port. The Pro's second, on J26, stays. |
| SWD | J5 | J13 | No CMSIS-DAP interface, so no debugger can use the probe. Verge Aux stays. |
| USB-SIO | | J15 | No DIO, SPI or I2C bridges; the DIO pins on J19 stay inputs. |

A function that is off is left out of the USB configuration, rather than shown as an interface that
does nothing. The interfaces after it move down a number. CMSIS-DAP v2 is always interface 0 when it
is there, so the probe selector `probe-rs list` prints still works. A selector for the CMSIS-DAP v1
interface, such as `1fc9:0143-3:<serial>`, stops matching once the VCOM jumper moves that interface
to 1.

With the SWD jumper fitted, the probe still restarts into its bootloader on a USB request, so verge
can still update it ([The SWD
jumper](https://github.com/Escapement-Project/verge/blob/HEAD/docs/updating.md#the-swd-jumper)).

The Pro's other jumpers set its hardware. J5 picks the target supply's voltage and J6 connects the
supply to the target. J16 to J18 pick the energy capture's setting: 350 mA at 1-2, 50 mA at 2-3.

## The standalone MCU-Link's DIO pins

J8 is four through-hole pads beside the MCU. With the USB connector to the left, they are DIO0 to
DIO3 from left to right. They are wired straight to the MCU, with no level shifter, no series
resistor and no protection, so they work at 3.3 V only. A 5 V signal can damage the probe, and a
3.3 V output can damage a target that runs at a lower voltage. J8 has no ground pin, so take ground
from the debug connector.

The pins start as inputs, pulled down inside the MCU, and go back to that whenever the probe
restarts. Each one turns between input and output on its own. `verge dio` drives and reads them.
It goes through Verge Aux, so it works while a debugger holds the probe ([Host
interface](host-interface.md#dio-0x8e)). `verge dio record` records their edges to a tickwave
recording ([DIO streams](host-interface.md#dio-streams)).

## How the probe shows up

In a listing, a probe running Verge reads `Escapement Project Verge CMSIS-DAP (EVF2551AD5)`, or
`Verge Pro CMSIS-DAP` on a Pro. It keeps the USB vendor and product IDs NXP's firmware uses,
`0x1FC9:0x0143`. Its serial number is `EV` and eight hex digits worked out from the chip's unique
ID. [Host interface](host-interface.md#how-the-probe-identifies-itself) has every field.

## The Pro's LEDs

The LEDs mean what NXP's manual for the Pro gives them, where the function exists:

- LED1 is lit while USB is configured.
- The status, SWO and VCOM LEDs flicker with traffic.
- LED5 (FUNC) shows the wire by the shape of its ramp: dark to light for SWD, light to dark for
  cJTAG, light to dark to light for JTAG. It is solid with the SWD jumper fitted, and dark with no
  port connected.
- LED6 (SIO) is lit while the host drives a DIO pin or runs SPI.
- LED7 (energy) stays dark.

The fades follow CIE lightness, so they look even to the eye.
