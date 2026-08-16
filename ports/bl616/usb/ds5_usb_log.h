#ifndef DS5_USB_LOG_H
#define DS5_USB_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the native DualSense HID + full-duplex UAC1 USB classes. */
int ds5_usb_log_init(void);

/* Connects or disconnects the USB device pull-up on the host bus. */
int ds5_usb_log_set_active(bool active);

#ifdef __cplusplus
}
#endif

#endif
