# The Pro

The MCU-Link Pro adds to what the standalone MCU-Link does: a switched supply for the target, four
general-purpose pins, SPI and I2C bridges, a second serial port, and energy capture. On Verge, a
second USB interface, Verge Aux, lets a tool use the pins, the supply and energy capture while a
debugger holds the probe.

[Probes and jumpers](probes.md) compares the two probes, and lists the jumpers.

## Target supply

The Pro can power the target, at 3.3 V, or at 1.8 V with J5 fitted, up to 400 mA. J6 connects the
supply to the target. The supply is off whenever the Pro starts.

```
verge power on     # switch it on
verge power        # read it
verge power off
```

verge reports the state the firmware set, not a measured voltage. A tool of your own switches it
with `0x8F` ([Host interface](host-interface.md#pro-target-supply-0x8f)). After switching the
supply on, give the regulator and the target's rail time to settle before you talk to the target.

## DIO pins

Four general-purpose pins, DIO0 to DIO3, on J19 (pins 2 to 5). They run at the voltage on J19's pin
14, or at 3.3 V with nothing there.

```
verge dio                # read them
verge dio --high 0,1,2   # make DIO0 to DIO2 outputs, driven high
verge dio --low all
verge dio --input all    # make them inputs again
```

DIO0, DIO1 and DIO2 change direction together, and DIO3 on its own, so a change has to turn a whole
group round: `--high 0` alone is refused while DIO1 and DIO2 are inputs. Inputs have pull-downs, and
an output reads back its own drive.

The pins keep their state between debug sessions, and across hosts. So a pin can hold a target's
boot mode through a reset and a reconnect. They go back to inputs when the probe restarts. LED6
(SIO) is lit while any pin is driven.

J15, fitted when the probe starts, turns the pins off and leaves them inputs.

## SPI and I2C bridges

The Pro's USB-SIO functions are bridges a host runs through CMSIS-DAP vendor commands: SPI with
`0x91`, I2C with `0x92` ([Host interface](host-interface.md#pro-spi-0x91)). There is no separate
USB-SIO endpoint. J15, fitted when the probe starts, turns both off.

- **SPI** runs on the DIO pins, so the probe refuses it, answering busy, while any DIO pin is an
  output. It leaves the pins as inputs afterwards. Its clock goes from about 23.4 kHz to 48 MHz.
- **I2C** runs on Flexcomm 3, which SWO also uses, so the probe refuses it, answering busy, while
  SWO is set up: turn SWO off first. The bus runs from 10 kHz to 1 MHz. 1 MHz is past NXP's
  limit for these pins: the LPC55S6x specifies Fast-mode Plus only on its open-drain I2C pins,
  PIO0_13 and PIO0_14 (UM11126, 32.2 and 15.2). The probe enables no pull-ups on the bus; fit
  your own, to the bus voltage.

verge has no command for the bridges.

## The second serial port

The Pro has a second USB serial port, on J26. It is 3.3 V only: J26 has no level shifter. The VCOM
jumper leaves it on. Wire the target's TXD to J26 pin 4, its RXD to pin 5, and its ground to pin 1.
NXP's manual for the Pro gives pins 4 and 5 the other way round, but the probe transmits on pin 5.
[Trace and serial ports](trace-and-serial.md#the-serial-ports).

## Verge Aux

On the Pro, Verge adds a second vendor interface, Verge Aux. It carries the DIO, target supply,
energy and marker commands, so a tool can use them while a debugger holds the CMSIS-DAP interface.
Debuggers pass it by: its name does not contain `CMSIS-DAP`.

Every Pro has it, whatever its jumpers, and so does the standalone MCU-Link, for its DIO pins
([Probes and jumpers](probes.md#the-mcu-links-dio-pins)). Both interfaces share one set of DIO pins, one target supply
and one energy capture. `verge power`, `verge dio` and `verge energy` use Verge Aux wherever the
probe has it, so they work beside a debugger, as the desktop app's supply, DIO and energy panels do.
[Host interface](host-interface.md#the-auxiliary-interface-verge-aux) describes it for tool authors.

## Energy capture

The Pro measures the current a target draws, the voltage it sees and the energy it uses: live, to a
file, or as a running total. [Energy
capture](https://github.com/Escapement-Project/verge/blob/HEAD/docs/energy.md) covers it with verge,
and [The desktop
app](https://github.com/Escapement-Project/verge/blob/HEAD/docs/desktop-app.md#recording-energy)
with verge-gui.
