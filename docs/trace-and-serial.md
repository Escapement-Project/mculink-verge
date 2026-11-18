# Trace and serial ports

Besides the debug wires, Verge captures SWO trace from the target and runs the probe's USB serial
ports: one on the standalone MCU-Link, two on the Pro.

## SWO

The probe captures SWO in UART (NRZ) encoding, and a host reads it with CMSIS-DAP's `DAP_SWO_Data`.
It captures up to 30 Mbaud, which is 3 MB/s. That is the fastest rate SWO's UART clock makes, and a
faster request gets it. The probe reports 30 Mbaud as its ceiling.

There is no Manchester encoding, and no streaming endpoint: a host polls `DAP_SWO_Data`. The probe's
buffer holds 7 KiB.

The standalone MCU-Link captures 30 Mbaud without a break. On the firmware of 2026-10-21 it did so
with its console running at 19.2 Mbaud too, and the Pro captured 24 Mbaud, the fastest SWO rate it
has been run at, with both of its consoles at 19.2 Mbaud. Rates above 10 Mbaud are past NXP's
specification for the UART, and above 20.1 Mbaud past its clock divider's too ([Past NXP's
limits](wires.md#past-nxps-limits)). The [measurement log](../MEASUREMENTS.md) has the captures.

On the Pro, SWO shares Flexcomm 3 with the I2C bridge ([The Pro](pro.md#spi-and-i2c-bridges)).

## The serial ports

Each probe has one USB serial port to the target, the console (VCOM). The Pro has a second, on J26,
at 3.3 V only.

The VCOM jumper turns the first port off: J4 on the standalone MCU-Link, J14 on the Pro. The Pro's
second port stays ([Probes and jumpers](probes.md#jumpers)).

Each port takes rates up to 30 Mbaud, and the Pro's two run at 30 Mbaud at once. The console UART
runs from a 96 MHz clock for any rate that clock makes within 3%, up to 19.2 Mbaud. For any other
rate, such as 10 Mbaud, it runs from the 150 MHz main clock, up to 30 Mbaud.

The probe stalls a request for a rate neither clock makes, and the port keeps its last rate. A port
takes 7 or 8 data bits, 1 or 2 stop bits, and no, odd or even parity. The probe stalls any other
framing the same way, and the port keeps its last one. On Linux the program's call succeeds anyway:
the cdc-acm driver does not pass the stall on.

Rates above 10 Mbaud are past NXP's specification for the UART ([Past NXP's
limits](wires.md#past-nxps-limits)). NXP's firmware V3.174 runs both of the Pro's ports at once
without loss up to 4 Mbaud.
