#ifndef DS5_USB_LOG_H
#define DS5_USB_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the native DualSense HID + full-duplex UAC1 USB device. */
int ds5_usb_log_init(void);

#ifdef __cplusplus
}
#endif

#endif
