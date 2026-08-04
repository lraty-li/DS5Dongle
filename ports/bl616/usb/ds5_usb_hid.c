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

#include "ds5_input_mailbox.h"
#include "ds5_log.h"
#include "ds5_protocol.h"

#define DS5_USB_HID_STACK_DEPTH  (configMINIMAL_STACK_SIZE * 4U)
#define DS5_USB_HID_PRIORITY     (configMAX_PRIORITIES - 4U)
#define DS5_USB_HID_WAIT_MS      10U
#define DS5_USB_HID_LOG_INTERVAL 1024U

/*
 * Input-only subset of the MIT-licensed DualSense report descriptor already
 * present in src/usb_descriptors.cpp. Output and Feature items are deferred.
 */
static const uint8_t hid_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x06,
    0x00, 0xFF, 0x09, 0x21, 0x95, 0x0D, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x22, 0x15, 0x00, 0x26,
    0xFF, 0x00, 0x75, 0x08, 0x95, 0x34, 0x81, 0x02,
    0xC0,
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

static struct usbd_interface hid_interface;
static uint8_t hid_bus_id;
static volatile bool hid_configured;
static volatile bool hid_suspended;
static volatile bool hid_busy;
static volatile uint32_t hid_completed_reports;
static volatile uint32_t hid_short_completions;

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

    if (transferred_bytes == DS5_USB_HID_IN_REPORT_SIZE) {
        ++hid_completed_reports;
    } else {
        ++hid_short_completions;
    }
    hid_busy = false;
    ds5_usb_hid_notify();
}

static struct usbd_endpoint hid_in_endpoint = {
    .ep_addr = DS5_USB_HID_IN_EP,
    .ep_cb = ds5_usb_hid_in,
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
        (interface != DS5_USB_HID_INTERFACE_NUMBER) ||
        (report_id != DS5_USB_INPUT_REPORT_ID) ||
        (report_type != HID_REPORT_INPUT)) {
        return;
    }

    *data = hid_last_report;
    *length = sizeof(hid_last_report);
}

static void ds5_usb_hid_task(void *parameter)
{
    uint8_t payload[DS5_USB_INPUT_PAYLOAD_SIZE];
    uint32_t observed_completions = 0U;
    uint32_t observed_short_completions = 0U;
    uint32_t start_failures = 0U;
    bool configured_announced = false;

    (void)parameter;

    while (1) {
        uint32_t completions = hid_completed_reports;
        uint32_t short_completions = hid_short_completions;

        if (completions != observed_completions) {
            if (observed_completions == 0U) {
                ds5_log_printf("DS5 USB: first 64-byte HID IN report "
                               "completed\r\n");
            }
            if ((completions / DS5_USB_HID_LOG_INTERVAL) !=
                (observed_completions / DS5_USB_HID_LOG_INTERVAL)) {
                ds5_log_printf("DS5 USB: HID IN reports completed %lu, "
                               "short %lu\r\n",
                               (unsigned long)completions,
                               (unsigned long)short_completions);
            }
            observed_completions = completions;
        }

        if (short_completions != observed_short_completions) {
            ds5_log_printf("DS5 USB: HID IN short completion count %lu\r\n",
                           (unsigned long)short_completions);
            observed_short_completions = short_completions;
        }

        if (!hid_configured) {
            configured_announced = false;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
            continue;
        }

        if (!configured_announced) {
            configured_announced = true;
            ds5_log_printf("DS5 USB: HID gamepad configured\r\n");
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
            ++start_failures;
            if (start_failures <= 4U) {
                ds5_log_printf("DS5 USB: HID IN transfer failed to start "
                               "(%lu)\r\n",
                               (unsigned long)start_failures);
            }
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
    hid_completed_reports = 0U;
    hid_short_completions = 0U;
    memset(hid_transmit_report, 0, sizeof(hid_transmit_report));
    memset(hid_last_report, 0, sizeof(hid_last_report));
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
        break;
    case USBD_EVENT_CONFIGURED:
        hid_configured = true;
        hid_suspended = false;
        hid_busy = false;
        break;
    case USBD_EVENT_SUSPEND:
        hid_suspended = true;
        break;
    case USBD_EVENT_RESUME:
        hid_suspended = false;
        break;
    default:
        return;
    }

    ds5_usb_hid_notify();
}
