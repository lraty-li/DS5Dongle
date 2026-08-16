#include "ds5_usb_log.h"

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"
#include "usbd_hid.h"

#include "ds5_usb_audio.h"
#include "ds5_usb_hid.h"

#define DS5_USB_BUS_ID          0U
#define DS5_USB_VID             0x054cU
#define DS5_USB_PID             0x0ce6U
#define DS5_USB_MAX_POWER_MA    100U
#define DS5_USB_CONFIG_SIZE 227U

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00,
                               DS5_USB_VID, DS5_USB_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    /* Native DualSense UAC1 function, transcribed from src/usb_descriptors.cpp. */
    0x09, 0x02, 0xe3, 0x00, 0x04, 0x01, 0x00, 0xc0, 0xfa,

    /* AudioControl interface 0: 4-channel host output and stereo host input. */
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0a, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00, 0x02, 0x01, 0x02,
    0x0c, 0x24, 0x02, 0x01, 0x01, 0x01, 0x06, 0x04, 0x33, 0x00,
    0x00, 0x00,
    0x0c, 0x24, 0x06, 0x02, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x04, 0x02, 0x00,
    0x0c, 0x24, 0x02, 0x04, 0x02, 0x04, 0x03, 0x02, 0x03, 0x00,
    0x00, 0x00,
    0x09, 0x24, 0x06, 0x05, 0x04, 0x01, 0x03, 0x00, 0x00,
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x01, 0x05, 0x00,

    /* AudioStreaming interface 1: 48 kHz, four-channel PCM OUT on EP 1. */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x04, 0x02, 0x10, 0x01, 0x80, 0xbb,
    0x00,
    0x09, 0x05, 0x01, 0x09, 0x88, 0x01, 0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* AudioStreaming interface 2: 48 kHz stereo PCM IN on EP 2. */
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xbb,
    0x00,
    0x09, 0x05, 0x82, 0x05, 0xc4, 0x00, 0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* HID interface 3: EP 4 IN and EP 3 OUT, matching the native DS5. */
    0x09,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    DS5_USB_HID_INTERFACE_NUMBER,
    0x00,
    0x02,
    0x03,
    0x00,
    0x00,
    0x00,
    0x09,
    HID_DESCRIPTOR_TYPE_HID,
    0x11, 0x01,
    0x00,
    0x01,
    HID_DESCRIPTOR_TYPE_HID_REPORT,
    (uint8_t)(DS5_USB_HID_REPORT_DESCRIPTOR_SIZE & 0xffU),
    (uint8_t)(DS5_USB_HID_REPORT_DESCRIPTOR_SIZE >> 8U),
    0x07,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    DS5_USB_HID_IN_EP,
    0x03,
    (uint8_t)(DS5_USB_HID_ENDPOINT_MPS & 0xffU),
    (uint8_t)(DS5_USB_HID_ENDPOINT_MPS >> 8U),
    DS5_USB_HID_POLL_INTERVAL,
    0x07,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    DS5_USB_HID_OUT_EP,
    0x03,
    (uint8_t)(DS5_USB_HID_ENDPOINT_MPS & 0xffU),
    (uint8_t)(DS5_USB_HID_ENDPOINT_MPS >> 8U),
    DS5_USB_HID_POLL_INTERVAL,
};

_Static_assert(sizeof(config_descriptor) == DS5_USB_CONFIG_SIZE,
               "DualSense HID/UAC configuration descriptor size mismatch");

static const uint8_t device_qualifier_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01)
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    "Sony Interactive Entertainment",
    "DualSense Wireless Controller",
    /* Bump the serial with the original project's descriptor change rule. */
    "BL616-0002",
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const uint8_t *device_qualifier_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_qualifier_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;

    if (index >= (sizeof(string_descriptors) / sizeof(string_descriptors[0]))) {
        return NULL;
    }
    return string_descriptors[index];
}

static const struct usb_descriptor descriptors = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback =
        device_qualifier_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static bool usb_classes_initialized;
static bool usb_active;

static void ds5_usb_event_handler(uint8_t busid, uint8_t event)
{
    ds5_usb_hid_handle_event(busid, event);
    ds5_usb_audio_handle_event(busid, event);
}

int ds5_usb_log_init(void)
{
    int err;

    if (usb_classes_initialized) {
        return 0;
    }

    usbd_desc_register(DS5_USB_BUS_ID, &descriptors);

    /*
     * CherryUSB assigns each class interface number from registration order.
     * Keep that order identical to config_descriptor: UAC 0..2, then HID 3.
     */
    err = ds5_usb_audio_init(DS5_USB_BUS_ID);
    if (err != 0) {
        return err;
    }

    err = ds5_usb_hid_init(DS5_USB_BUS_ID);
    if (err != 0) {
        ds5_usb_audio_deinit();
        return err;
    }

    usb_classes_initialized = true;
    usb_active = false;

    return 0;
}

int ds5_usb_log_set_active(bool active)
{
    int err;

    if (!usb_classes_initialized || (usb_active == active)) {
        return 0;
    }

    if (active) {
        err = usbd_initialize(DS5_USB_BUS_ID, 0U, ds5_usb_event_handler);
    } else {
        err = usbd_deinitialize(DS5_USB_BUS_ID);
    }

    if (err == 0) {
        usb_active = active;
    }

    return err;
}
