#include "ds5_usb_hid.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "task.h"

#include "usbd_core.h"
#include "usbd_hid.h"

#include "ds5_feature_cache.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_input_mailbox.h"
#include "ds5_output_mailbox.h"
#include "ds5_protocol.h"

#define DS5_USB_HID_STACK_DEPTH  (configMINIMAL_STACK_SIZE * 4U)
#define DS5_USB_HID_PRIORITY     (configMAX_PRIORITIES - 4U)
#define DS5_USB_HID_WAIT_MS      10U

/* Exact DualSense (USB PID 0x0ce6) HID report model from this repository. */
static const uint8_t hid_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x20,
    0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81,
    0x42, 0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x21,
    0x95, 0x0D, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02, 0x85, 0x02, 0x09, 0x23,
    0x95, 0x2F, 0x91, 0x02, 0x85, 0x05, 0x09, 0x33, 0x95, 0x28, 0xB1, 0x02,
    0x85, 0x08, 0x09, 0x34, 0x95, 0x2F, 0xB1, 0x02, 0x85, 0x09, 0x09, 0x24,
    0x95, 0x13, 0xB1, 0x02, 0x85, 0x0A, 0x09, 0x25, 0x95, 0x1A, 0xB1, 0x02,
    0x85, 0x0B, 0x09, 0x41, 0x95, 0x29, 0xB1, 0x02, 0x85, 0x0C, 0x09, 0x42,
    0x95, 0x29, 0xB1, 0x02, 0x85, 0x20, 0x09, 0x26, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x21, 0x09, 0x27, 0x95, 0x04, 0xB1, 0x02, 0x85, 0x22, 0x09, 0x40,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0x80, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x81, 0x09, 0x29, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x82, 0x09, 0x2A,
    0x95, 0x09, 0xB1, 0x02, 0x85, 0x83, 0x09, 0x2B, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0x84, 0x09, 0x2C, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x85, 0x09, 0x2D,
    0x95, 0x02, 0xB1, 0x02, 0x85, 0xA0, 0x09, 0x2E, 0x95, 0x01, 0xB1, 0x02,
    0x85, 0xE0, 0x09, 0x2F, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF0, 0x09, 0x30,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF1, 0x09, 0x31, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF2, 0x09, 0x32, 0x95, 0x0F, 0xB1, 0x02, 0x85, 0xF4, 0x09, 0x35,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF5, 0x09, 0x36, 0x95, 0x03, 0xB1, 0x02,
    0x85, 0xF6, 0x09, 0x37, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF7, 0x09, 0x38,
    0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF8, 0x09, 0x39, 0x95, 0x3F, 0xB1, 0x02,
    0x85, 0xF9, 0x09, 0x3A, 0x95, 0x3F, 0xB1, 0x02, 0xC0,
};

_Static_assert(sizeof(hid_report_descriptor) ==
                   DS5_USB_HID_REPORT_DESCRIPTOR_SIZE,
               "DualSense HID report descriptor size mismatch");
_Static_assert(DS5_USB_INPUT_PAYLOAD_SIZE + 1U ==
                   DS5_USB_HID_IN_REPORT_SIZE,
               "DualSense HID IN report size mismatch");

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t hid_transmit_report[DS5_USB_HID_IN_REPORT_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t hid_last_report[DS5_USB_HID_IN_REPORT_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t hid_receive_report[DS5_USB_OUTPUT_REPORT_SIZE];

static struct usbd_interface hid_interface;
static uint8_t hid_bus_id;
static volatile bool hid_configured;
static volatile bool hid_suspended;
static volatile bool hid_busy;
static volatile bool hid_out_read_pending;

static StaticTask_t hid_task_storage;
static StackType_t hid_task_stack[DS5_USB_HID_STACK_DEPTH];
static TaskHandle_t hid_task;

static void ds5_usb_hid_notify(void)
{
    if (hid_task == NULL) {
        return;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        vTaskNotifyGiveFromISR(hid_task, &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        xTaskNotifyGive(hid_task);
    }
}

static void ds5_usb_hid_in(uint8_t busid, uint8_t endpoint,
                           uint32_t transferred_bytes)
{
    (void)busid;
    (void)endpoint;

    if (!hid_busy) {
        return;
    }

    (void)transferred_bytes;
    hid_busy = false;
    ds5_usb_hid_notify();
}

static struct usbd_endpoint hid_in_endpoint = {
    .ep_addr = DS5_USB_HID_IN_EP,
    .ep_cb = ds5_usb_hid_in,
};

static bool ds5_usb_hid_publish_output(const uint8_t *report, size_t length)
{
    if ((report == NULL) || (length != DS5_USB_OUTPUT_REPORT_SIZE) ||
        (report[0] != DS5_USB_OUTPUT_REPORT_ID) ||
        !ds5_output_mailbox_publish(report, length)) {
        return false;
    }

    ds5_usb_hid_notify();
    return true;
}

static bool ds5_usb_hid_publish_feature_set(uint8_t report_id,
                                             const uint8_t *report,
                                             size_t length)
{
    const uint8_t *payload = report;
    size_t payload_length = length;

    if ((report != NULL) &&
        (length == (DS5_FEATURE_SET_MAX_PAYLOAD + 1U)) &&
        (report[0] == report_id)) {
        payload = &report[1];
        payload_length = DS5_FEATURE_SET_MAX_PAYLOAD;
    }

    if ((payload == NULL) || (payload_length > DS5_FEATURE_SET_MAX_PAYLOAD) ||
        !ds5_feature_set_mailbox_publish(report_id, payload, payload_length)) {
        return false;
    }

    ds5_usb_hid_notify();
    return true;
}

static void ds5_usb_hid_arm_out(void)
{
    if (!hid_configured || hid_suspended || hid_out_read_pending) {
        return;
    }

    hid_out_read_pending = true;
    if (usbd_ep_start_read(hid_bus_id, DS5_USB_HID_OUT_EP,
                           hid_receive_report,
                           sizeof(hid_receive_report)) != 0) {
        hid_out_read_pending = false;
    }
}

static void ds5_usb_hid_out(uint8_t busid, uint8_t endpoint,
                            uint32_t transferred_bytes)
{

    (void)busid;
    (void)endpoint;

    hid_out_read_pending = false;
    /*
     * The local HID descriptor specifies 47 output bytes for report 0x02.
     * USB interrupt transport includes the report ID, so arm VDMA for the
     * exact 48-byte wire report.  Asking the BL616 USB v2 VDMA for the
     * 64-byte endpoint MPS merges adjacent short reports into one buffer.
     */
    (void)ds5_usb_hid_publish_output(hid_receive_report,
                                     (size_t)transferred_bytes);
    ds5_usb_hid_arm_out();
}

static struct usbd_endpoint hid_out_endpoint = {
    .ep_addr = DS5_USB_HID_OUT_EP,
    .ep_cb = ds5_usb_hid_out,
};

void usbd_hid_get_report(uint8_t busid, uint8_t interface,
                         uint8_t report_id, uint8_t report_type,
                         uint8_t **data, uint32_t *length)
{
    if ((data == NULL) || (length == NULL)) {
        return;
    }

    *length = 0U;
    if ((busid != hid_bus_id) ||
        (interface != DS5_USB_HID_INTERFACE_NUMBER)) {
        return;
    }

    if ((report_id == DS5_USB_INPUT_REPORT_ID) &&
        (report_type == HID_REPORT_INPUT)) {
        *data = hid_last_report;
        *length = sizeof(hid_last_report);
        return;
    }

    if (report_type == HID_REPORT_FEATURE) {
        size_t cached_length;

        if (ds5_feature_cache_get(report_id, data, &cached_length)) {
            *length = (uint32_t)cached_length;
        }
    }
}

void usbd_hid_set_report(uint8_t busid, uint8_t interface,
                         uint8_t report_id, uint8_t report_type,
                         uint8_t *report, uint32_t report_length)
{
    uint8_t normalized_report[DS5_USB_OUTPUT_REPORT_SIZE];

    if ((busid != hid_bus_id) ||
        (interface != DS5_USB_HID_INTERFACE_NUMBER)) {
        return;
    }

    if (report_type == HID_REPORT_FEATURE) {
        (void)ds5_usb_hid_publish_feature_set(report_id, report,
                                               (size_t)report_length);
        return;
    }

    if ((report_type != HID_REPORT_OUTPUT) || (report == NULL)) {
        return;
    }

    if ((report_id == DS5_USB_OUTPUT_REPORT_ID) &&
        (report_length == DS5_USB_OUTPUT_STATE_SIZE)) {
        normalized_report[0] = DS5_USB_OUTPUT_REPORT_ID;
        memcpy(&normalized_report[1], report, report_length);
        (void)ds5_usb_hid_publish_output(normalized_report,
                                          sizeof(normalized_report));
        return;
    }

    if (((report_id == 0U) ||
         (report_id == DS5_USB_OUTPUT_REPORT_ID)) &&
        (report_length == DS5_USB_OUTPUT_REPORT_SIZE)) {
        (void)ds5_usb_hid_publish_output(report, report_length);
        return;
    }
}

static void ds5_usb_hid_task(void *parameter)
{
    uint8_t payload[DS5_USB_INPUT_PAYLOAD_SIZE];

    (void)parameter;

    while (1) {
        if (!hid_configured) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
            continue;
        }

        if (hid_suspended || hid_busy ||
            !usb_device_is_configured(hid_bus_id)) {
            (void)ulTaskNotifyTake(pdTRUE,
                                   pdMS_TO_TICKS(DS5_USB_HID_WAIT_MS));
            continue;
        }

        if (!ds5_input_mailbox_receive(payload, sizeof(payload),
                                       DS5_USB_HID_WAIT_MS)) {
            continue;
        }

        hid_transmit_report[0] = DS5_USB_INPUT_REPORT_ID;
        memcpy(&hid_transmit_report[1], payload, sizeof(payload));
        memcpy(hid_last_report, hid_transmit_report,
               sizeof(hid_last_report));

        hid_busy = true;
        if (usbd_ep_start_write(hid_bus_id, DS5_USB_HID_IN_EP,
                                hid_transmit_report,
                                sizeof(hid_transmit_report)) != 0) {
            hid_busy = false;
        }
    }
}

int ds5_usb_hid_init(uint8_t busid)
{

    if (hid_task != NULL) {
        return -EALREADY;
    }

    hid_bus_id = busid;
    hid_configured = false;
    hid_suspended = false;
    hid_busy = false;
    hid_out_read_pending = false;
    memset(hid_transmit_report, 0, sizeof(hid_transmit_report));
    memset(hid_last_report, 0, sizeof(hid_last_report));
    memset(hid_receive_report, 0, sizeof(hid_receive_report));
    hid_last_report[0] = DS5_USB_INPUT_REPORT_ID;

    hid_task = xTaskCreateStatic(ds5_usb_hid_task, "usb_hid",
                                 DS5_USB_HID_STACK_DEPTH, NULL,
                                 DS5_USB_HID_PRIORITY, hid_task_stack,
                                 &hid_task_storage);
    if (hid_task == NULL) {
        return -ENOMEM;
    }

    usbd_add_interface(
        busid,
        usbd_hid_init_intf(busid, &hid_interface,
                           hid_report_descriptor,
                           sizeof(hid_report_descriptor)));
    usbd_add_endpoint(busid, &hid_in_endpoint);
    usbd_add_endpoint(busid, &hid_out_endpoint);
    return 0;
}

void ds5_usb_hid_deinit(void)
{
    if (hid_task != NULL) {
        vTaskDelete(hid_task);
        hid_task = NULL;
    }
}

void ds5_usb_hid_handle_event(uint8_t busid, uint8_t event)
{
    if (busid != hid_bus_id) {
        return;
    }

    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        hid_configured = false;
        hid_suspended = false;
        hid_busy = false;
        hid_out_read_pending = false;
        break;
    case USBD_EVENT_CONFIGURED:
        hid_configured = true;
        hid_suspended = false;
        hid_busy = false;
        hid_out_read_pending = false;
        ds5_usb_hid_arm_out();
        break;
    case USBD_EVENT_SUSPEND:
        hid_suspended = true;
        break;
    case USBD_EVENT_RESUME:
        hid_suspended = false;
        ds5_usb_hid_arm_out();
        break;
    default:
        return;
    }

    ds5_usb_hid_notify();
}
