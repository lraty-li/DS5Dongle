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

## To verify after delivery

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
