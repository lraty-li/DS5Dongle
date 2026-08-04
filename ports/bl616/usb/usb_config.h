#ifndef DS5_CHERRYUSB_CONFIG_H
#define DS5_CHERRYUSB_CONFIG_H

#include <stdio.h>

#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)
#define CONFIG_USB_DBG_LEVEL   0

#define CONFIG_USB_ALIGN_SIZE 4
#define USB_NOCACHE_RAM_SECTION __attribute__((section(".noncacheable")))

#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#define CONFIG_USBDEV_ADVANCE_DESC
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USB_EHCI_HCOR_RESERVED_DISABLE

/* The BL616 USB v2 driver includes usbh_core.h even in device-only builds. */
#define CONFIG_USBHOST_MAX_EHPORTS          4
#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 8
#define CONFIG_USBHOST_MAX_ENDPOINTS        4
#define CONFIG_USBHOST_DEV_NAMELEN          16

/* BL616's USB v2 controller and the local SDK example both use HS mode. */
#define CONFIG_USB_HS

#ifndef usb_phyaddr2ramaddr
#define usb_phyaddr2ramaddr(address) (address)
#endif

#ifndef usb_ramaddr2phyaddr
#define usb_ramaddr2phyaddr(address) (address)
#endif

#endif
