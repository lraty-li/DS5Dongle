#include "ds5_usb_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "task.h"

#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usbd_hid.h"

#include "ds5_log.h"
#include "ds5_usb_hid.h"

#define DS5_USB_BUS_ID          0U
#define DS5_USB_CDC_IN_EP       0x83U
#define DS5_USB_CDC_OUT_EP      0x04U
#define DS5_USB_CDC_INT_EP      0x85U
#define DS5_USB_VID             0xffffU
#define DS5_USB_PID             0xffffU
#define DS5_USB_MAX_POWER_MA    100U
#define DS5_USB_CDC_MPS         512U
#define DS5_USB_CONFIG_SIZE     \
    (9U + CDC_ACM_DESCRIPTOR_LEN + DS5_USB_HID_CONFIG_DESCRIPTOR_SIZE)
#define DS5_USB_LOG_STACK_DEPTH (configMINIMAL_STACK_SIZE * 4U)
#define DS5_USB_LOG_PRIORITY    (configMAX_PRIORITIES - 4U)

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xef, 0x02, 0x01,
                               DS5_USB_VID, DS5_USB_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(DS5_USB_CONFIG_SIZE, 0x03, 0x01,
                               USB_CONFIG_BUS_POWERED,
                               DS5_USB_MAX_POWER_MA),
    CDC_ACM_DESCRIPTOR_INIT(0x00, DS5_USB_CDC_INT_EP,
                            DS5_USB_CDC_OUT_EP, DS5_USB_CDC_IN_EP,
                            DS5_USB_CDC_MPS, 0x02),
    0x09,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    DS5_USB_HID_INTERFACE_NUMBER,
    0x00,
    0x01,
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
    (uint8_t)(DS5_USB_HID_IN_REPORT_SIZE & 0xffU),
    (uint8_t)(DS5_USB_HID_IN_REPORT_SIZE >> 8U),
    DS5_USB_HID_POLL_INTERVAL,
};

_Static_assert(sizeof(config_descriptor) == DS5_USB_CONFIG_SIZE,
               "CDC/HID configuration descriptor size mismatch");

static const uint8_t device_qualifier_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0xef, 0x02, 0x01, 0x01)
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    "DS5Dongle",
    "DS5Dongle BL616 HID Bridge",
    "BL616-LOG-0001",
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t receive_buffer[DS5_USB_CDC_MPS];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
    uint8_t transmit_buffer[DS5_USB_CDC_MPS];

static struct usbd_interface control_interface;
static struct usbd_interface data_interface;

static volatile bool usb_configured;
static volatile bool dtr_enabled;
static volatile bool receive_armed;
static volatile bool transmit_active;
static volatile bool transmit_complete;
static volatile bool transmit_aborted;
static size_t transmit_length;

static StaticTask_t log_task_storage;
static StackType_t log_task_stack[DS5_USB_LOG_STACK_DEPTH];
static TaskHandle_t log_task;

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

static void ds5_usb_log_notify(void)
{
    if (log_task == NULL) {
        return;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        vTaskNotifyGiveFromISR(log_task, &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        xTaskNotifyGive(log_task);
    }
}

static void ds5_usb_event_handler(uint8_t busid, uint8_t event)
{
    ds5_usb_hid_handle_event(busid, event);

    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
    case USBD_EVENT_SUSPEND:
        usb_configured = false;
        dtr_enabled = false;
        receive_armed = false;
        if (transmit_active) {
            transmit_aborted = true;
        }
        ds5_usb_log_notify();
        break;
    case USBD_EVENT_CONFIGURED:
    case USBD_EVENT_RESUME:
        usb_configured = true;
        ds5_usb_log_notify();
        break;
    default:
        break;
    }
}

static void ds5_usb_cdc_out(uint8_t busid, uint8_t endpoint,
                            uint32_t transferred_bytes)
{
    (void)endpoint;
    (void)transferred_bytes;

    receive_armed = false;
    if (usb_configured &&
        (usbd_ep_start_read(busid, DS5_USB_CDC_OUT_EP,
                            receive_buffer, sizeof(receive_buffer)) == 0)) {
        receive_armed = true;
    }
}

static void ds5_usb_cdc_in(uint8_t busid, uint8_t endpoint,
                           uint32_t transferred_bytes)
{
    (void)endpoint;

    if (!transmit_active) {
        return;
    }

    if ((transferred_bytes != 0U) &&
        ((transferred_bytes % DS5_USB_CDC_MPS) == 0U)) {
        if (usbd_ep_start_write(busid, DS5_USB_CDC_IN_EP, NULL, 0U) == 0) {
            return;
        }
    }

    transmit_complete = true;
    ds5_usb_log_notify();
}

static struct usbd_endpoint cdc_out_endpoint = {
    .ep_addr = DS5_USB_CDC_OUT_EP,
    .ep_cb = ds5_usb_cdc_out,
};

static struct usbd_endpoint cdc_in_endpoint = {
    .ep_addr = DS5_USB_CDC_IN_EP,
    .ep_cb = ds5_usb_cdc_in,
};

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t interface, bool dtr)
{
    (void)busid;
    (void)interface;

    dtr_enabled = dtr;
    ds5_usb_log_notify();
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t interface, bool rts)
{
    (void)busid;
    (void)interface;
    (void)rts;
}

static void ds5_usb_log_task(void *parameter)
{
    bool terminal_announced = false;

    (void)parameter;

    while (1) {
        if (transmit_active && transmit_complete) {
            ds5_log_discard(transmit_length);
            transmit_length = 0U;
            transmit_active = false;
            transmit_complete = false;
            transmit_aborted = false;
        } else if (transmit_active && transmit_aborted) {
            transmit_length = 0U;
            transmit_active = false;
            transmit_complete = false;
            transmit_aborted = false;
        }

        if (!usb_configured) {
            terminal_announced = false;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
            continue;
        }

        if (!receive_armed &&
            (usbd_ep_start_read(DS5_USB_BUS_ID, DS5_USB_CDC_OUT_EP,
                                receive_buffer,
                                sizeof(receive_buffer)) == 0)) {
            receive_armed = true;
        }

        if (!dtr_enabled) {
            terminal_announced = false;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
            continue;
        }

        if (!terminal_announced) {
            uint32_t dropped = ds5_log_dropped_bytes();

            terminal_announced = true;
            ds5_log_printf("DS5 USB: CDC terminal connected");
            if (dropped != 0U) {
                ds5_log_printf(", %lu buffered byte(s) dropped",
                               (unsigned long)dropped);
            }
            ds5_log_printf("\r\n");
        }

        if (!transmit_active) {
            int err;

            transmit_length = ds5_log_peek(transmit_buffer,
                                           sizeof(transmit_buffer));
            if (transmit_length != 0U) {
                transmit_complete = false;
                transmit_aborted = false;
                transmit_active = true;
                err = usbd_ep_start_write(DS5_USB_BUS_ID,
                                          DS5_USB_CDC_IN_EP,
                                          transmit_buffer,
                                          (uint32_t)transmit_length);
                if (err != 0) {
                    transmit_length = 0U;
                    transmit_active = false;
                }
            }
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50U));
    }
}

int ds5_usb_log_init(void)
{
    int err;

    usb_configured = false;
    dtr_enabled = false;
    receive_armed = false;
    transmit_active = false;
    transmit_complete = false;
    transmit_aborted = false;
    transmit_length = 0U;

    usbd_desc_register(DS5_USB_BUS_ID, &descriptors);
    usbd_add_interface(DS5_USB_BUS_ID,
                       usbd_cdc_acm_init_intf(DS5_USB_BUS_ID,
                                              &control_interface));
    usbd_add_interface(DS5_USB_BUS_ID,
                       usbd_cdc_acm_init_intf(DS5_USB_BUS_ID,
                                              &data_interface));
    usbd_add_endpoint(DS5_USB_BUS_ID, &cdc_out_endpoint);
    usbd_add_endpoint(DS5_USB_BUS_ID, &cdc_in_endpoint);

    err = ds5_usb_hid_init(DS5_USB_BUS_ID);
    if (err != 0) {
        return err;
    }

    log_task = xTaskCreateStatic(ds5_usb_log_task, "usb_log",
                                 DS5_USB_LOG_STACK_DEPTH, NULL,
                                 DS5_USB_LOG_PRIORITY, log_task_stack,
                                 &log_task_storage);
    if (log_task == NULL) {
        ds5_usb_hid_deinit();
        return -1;
    }

    err = usbd_initialize(DS5_USB_BUS_ID, 0U, ds5_usb_event_handler);
    if (err != 0) {
        ds5_usb_hid_deinit();
        vTaskDelete(log_task);
        log_task = NULL;
        return err;
    }

    return 0;
}
