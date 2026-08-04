#ifndef DS5_USB_HID_H
#define DS5_USB_HID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_USB_HID_INTERFACE_NUMBER       0x02U
#define DS5_USB_HID_IN_EP                  0x81U
#define DS5_USB_HID_OUT_EP                 0x02U
#define DS5_USB_HID_IN_REPORT_SIZE         64U
#define DS5_USB_HID_ENDPOINT_MPS           64U
#define DS5_USB_HID_REPORT_DESCRIPTOR_SIZE 321U
#define DS5_USB_HID_CONFIG_DESCRIPTOR_SIZE 32U
#define DS5_USB_HID_POLL_INTERVAL          0x04U

int ds5_usb_hid_init(uint8_t busid);
void ds5_usb_hid_deinit(void);
void ds5_usb_hid_handle_event(uint8_t busid, uint8_t event);

#ifdef __cplusplus
}
#endif

#endif
