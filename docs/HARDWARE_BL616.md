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

## SDK baseline pending board verification

The pinned `bl616dk` BSP configures its console as UART0 TX on GPIO21 and RX on
GPIO22, 8-N-1 at 2,000,000 baud. These are SDK baseline values only; the board
layout has not yet confirmed that either signal reaches the Type-C connector or
an onboard USB-to-UART bridge.

## Still to verify

- Flash capacity
- USB D+ and D- connection
- Whether Type-C is connected directly to BL616 USB
- Debug UART TX/RX pins
- UART baud rate
- BOOT pin
- RESET circuit
- Blue LED GPIO and active level
- Header pinout
- Power supply voltage and regulator
