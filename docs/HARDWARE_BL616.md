# BL616 Hardware Record

This document separates observations made on the current board from settings
inherited from the `bl616dk` SDK baseline.

## Confirmed board properties

- Chip marking: `BL616C50`
- Chip family: BL616C; BootROM reports revision A0
- Build settings: `CHIP=bl616`, `BOARD=bl616dk`
- USB connector: Type-C with a working native USB data path
- Flash JEDEC ID: `c86016`
- Flash capacity: `0x00400000` bytes (4 MiB)
- Board controls and indicators: BOOT button, onboard antenna, red power LED,
  and blue user LED

The `bl616dk` configuration is no longer merely a compile-time assumption:
the current board has completed BootROM communication, erase/write/verification,
normal firmware boot, Bluetooth operation, and native USB enumeration with this
configuration. It remains a compatibility mapping rather than a claim that the
board is electrically identical to the official BL616DK.

## USB modes

### Download mode

- Holding BOOT while reconnecting Type-C enumerates
  `USB\VID_349B&PID_6160` as a Windows COM port.
- The COM number is host-assigned. COM3 was observed during the first probe on
  2026-08-04; later setups commonly assign COM4.
- The pinned `BLFlashCommand.exe` communicates reliably at 2,000,000 baud.
- Successful flashes have completed both device writing and SHA256 readback
  verification.

### Normal firmware mode

- The same Type-C data path successfully enumerates CherryUSB as
  `DualSense Wireless Controller` (`VID_054C&PID_0CE6`).
- The firmware forces Full Speed operation to match the wired DualSense.
- The runtime configuration contains HID and full-duplex UAC1 Audio
  interfaces. It intentionally contains no CDC ACM interface.
- USB is deliberately attached only after the Bluetooth state reaches
  `READY`. With no connected controller, the absence of a host USB device is
  expected.

These observations confirm the functional USB D+/D- route and supersede the
initial 2026-08-04 pre-flash observation that native USB was unconfirmed.

## Flash layout

- Boot2: `0x000000`
- Partition table: `0x00E000`
- Application firmware: partition-defined offset `0x010000`
- Partition source: the pinned BL616DK 4 MiB configuration

The RF manufacturing image produced by the SDK is not part of
`flash_prog_cfg.ini`; the board's factory RF calibration is retained.

## SDK console baseline

The pinned `bl616dk` BSP configures UART0 TX on GPIO21 and RX on GPIO22,
8-N-1 at 2,000,000 baud. Firmware logging uses this SDK console, but the board
layout has not yet confirmed that those signals are exposed on accessible
pads or through a USB-to-UART bridge.

## Markings not decoded

- `6N7PJ9`
- `2328 F2`

They appear to be manufacturing trace markings and are not used by the
firmware configuration.

## Still to verify electrically

- RESET circuit
- Blue LED GPIO and active level
- Debug UART pad routing
- Header pinout
- Power-supply regulator and exposed voltage rails
