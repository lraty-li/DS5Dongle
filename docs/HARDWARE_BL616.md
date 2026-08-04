# BL616 Hardware Record

## Confirmed

- Chip marking: `BL616C50`
- Chip family: BL616C
- Build chip setting: `CHIP=bl616`
- USB connector: Type-C
- Board has BOOT button
- Board has onboard antenna
- Board has red power LED
- Board has blue user LED

## Markings not yet decoded

- `6N7PJ9`
- `2328 F2`

These appear to be manufacturing trace markings and are not currently used
for the firmware configuration.

## Temporary build configuration

- `BOARD=bl616dk`

This is currently used only because the SDK example builds successfully.
Board-level compatibility has not yet been confirmed.

## First host enumeration (2026-08-04)

- The board is physically available and connected to the Windows host by its
  Type-C connector.
- Windows currently exposes no new COM port and no present unknown/error USB
  device. The only serial port is the motherboard ACPI `COM1`, so it must not
  be selected as the board's flashing port.
- Native USB data connectivity is therefore still unconfirmed. Repeat the
  enumeration while the board is explicitly placed in download mode before
  drawing conclusions about the connector or cable.

## Download-mode verification (2026-08-04)

- Holding BOOT while reconnecting Type-C exposes `USB Serial Device (COM3)` as
  `USB\VID_349B&PID_6160` on the current Windows host.
- The pinned SDK `BLFlashCommand.exe` completed a read-only BootROM handshake
  on COM3 at 2,000,000 baud and identified BL616 chip revision A0.
- Flash JEDEC ID: `c86016`.
- Detected flash capacity: `0x00400000` bytes (4 MiB), matching the temporary
  `bl616dk` partition baseline.
- A 256-byte read at flash offset zero succeeded. No flash erase or write was
  performed during this probe.
- This confirms that the Type-C data path exposes a UART-compatible download
  port in BOOT mode. It does not yet prove that BL616 native USB D+/D- is wired
  to the connector.

## SDK baseline pending board verification

The pinned `bl616dk` BSP configures its console as UART0 TX on GPIO21 and RX on
GPIO22, 8-N-1 at 2,000,000 baud. These are SDK baseline values only; the board
layout has not yet confirmed that either signal reaches the Type-C connector or
an onboard USB-to-UART bridge.

## Still to verify

- USB D+ and D- connection
- Whether Type-C is connected directly to BL616 USB
- Debug UART TX/RX pins
- UART baud rate
- BOOT pin
- RESET circuit
- Blue LED GPIO and active level
- Header pinout
- Power supply voltage and regulator
