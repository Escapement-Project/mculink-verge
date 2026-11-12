# Trace and serial ports

Besides the debug wires, Verge captures SWO trace from the target and runs the probe's USB serial
ports: one on the standalone MCU-Link, two on the Pro.

## SWO

The probe captures SWO in UART (NRZ) encoding, and a host reads it with CMSIS-DAP's `DAP_SWO_Data`.
It captures up to 30 Mbaud, which is 3 MB/s. That is the fastest rate SWO's UART clock makes, and a
faster request gets it. The probe reports 30 Mbaud as its ceiling.

There is no Manchester encoding, and no streaming endpoint: a host polls `DAP_SWO_Data`. The probe's
buffer holds 7 KiB.

What was tested: the standalone MCU-Link captured 30 Mbaud with its console running at 19.2 Mbaud.
The Pro captured up to 24 Mbaud with both consoles at 19.2 Mbaud; no trace source on the bench sends
faster. Rates above 10 Mbaud are past NXP's specification for the UART, and above 20.1 Mbaud past
its clock divider's too ([Past NXP's limits](wires.md#past-nxps-limits)).

On the Pro, SWO shares Flexcomm 3 with the I2C bridge ([The Pro](pro.md#spi-and-i2c-bridges)).

## The serial ports

Each probe has one USB serial port to the target, the console (VCOM). The Pro has a second, on J26,
at 3.3 V only.

The VCOM jumper turns the first port off: J4 on the standalone MCU-Link, J14 on the Pro. The Pro's
second port stays ([Probes and jumpers](probes.md#jumpers)).

A loopback passed at 19.2 Mbaud, both Pro ports running together included. That is the fastest rate
the console UART's 96 MHz clock makes. For a rate that clock cannot make within 3%, such as 10
Mbaud, the UART runs from the 150 MHz main clock instead, up to 30 Mbaud. Those rates have not run
on hardware yet. The probe stalls a request for a rate neither clock makes, and the port keeps its
last rate. Rates above 10 Mbaud are past NXP's specification for the UART
([Past NXP's limits](wires.md#past-nxps-limits)). NXP's firmware ran its console at 3 Mbaud with
both Pro ports running, the fastest tested; its limit has not been measured.
