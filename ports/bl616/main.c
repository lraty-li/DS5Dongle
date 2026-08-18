#include <FreeRTOS.h>
#include "task.h"

#include "board.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "rfparam_adapter.h"

#include "ds5_audio_mailbox.h"
#include "ds5_bt.h"
#include "ds5_feature_set_mailbox.h"
#include "ds5_haptics_mailbox.h"
#include "ds5_input_mailbox.h"
#include "ds5_output_mailbox.h"
#include "ds5_log.h"
#include "ds5_usb_log.h"

#define DS5_BT_STATE_POLL_MS       50U

/* Stop the MCU core while the scheduler has no ready application task. */
void vApplicationIdleHook(void)
{
    __asm volatile("wfi" ::: "memory");
}

static void app_start_task(void *parameter)
{
    ds5_bt_state_t bluetooth_state;
    bool usb_initialized = false;
    bool usb_active = false;
    int err;

    (void)parameter;

    /*
     * The pinned BL616 controller library resets the PDS block from
     * btble_controller_init(). PDS owns USB power/PLL/reset controls, so USB
     * must not be initialized until the controller reset has completed.
     */
    err = ds5_bt_init();
    if (err != 0) {
        ds5_log_printf("DS5: Bluetooth startup failed (err %d)\r\n", err);
    }

    err = ds5_usb_log_init();
    if (err != 0) {
        ds5_log_printf("DS5 USB: HID/UAC initialization failed (err %d)\r\n",
                       err);
    } else {
        usb_initialized = true;
        ds5_log_printf("DS5 USB: HID/UAC classes registered\r\n");
    }

    while (1) {
        bluetooth_state = ds5_bt_get_state();

        if (usb_initialized) {
            bool should_usb_active =
                bluetooth_state == DS5_BT_STATE_READY;

            if (should_usb_active != usb_active) {
                err = ds5_usb_log_set_active(should_usb_active);
                if (err != 0) {
                    ds5_log_printf(
                        "DS5 USB: failed to %s USB device (err %d)\r\n",
                        should_usb_active ? "connect" : "disconnect", err);
                } else {
                    usb_active = should_usb_active;
                    ds5_log_printf("DS5 USB: device %s\r\n",
                                   usb_active ? "connected" : "disconnected");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DS5_BT_STATE_POLL_MS));
    }
}

int main(void)
{
    BaseType_t task_result;
    EfErrCode storage_err;
    int mailbox_err;
    int rf_err;

    board_init();
    ds5_log_init();

    ds5_log_printf("DS5Dongle BL616 HID bridge bring-up\r\n");

    configASSERT(configMAX_PRIORITIES > 5U);

    mailbox_err = ds5_input_mailbox_init();
    if (mailbox_err != 0) {
        ds5_log_printf("DS5: input mailbox initialization failed "
                       "(err %d)\r\n",
                       mailbox_err);
        return 0;
    }

    mailbox_err = ds5_output_mailbox_init();
    if (mailbox_err != 0) {
        ds5_log_printf("DS5: output mailbox initialization failed "
                       "(err %d)\r\n",
                       mailbox_err);
        return 0;
    }

    mailbox_err = ds5_feature_set_mailbox_init();
    if (mailbox_err != 0) {
        ds5_log_printf("DS5: Feature SET mailbox initialization failed "
                       "(err %d)\r\n",
                       mailbox_err);
        return 0;
    }

    mailbox_err = ds5_haptics_mailbox_init();
    if (mailbox_err != 0) {
        ds5_log_printf("DS5: haptics mailbox initialization failed "
                       "(err %d)\r\n",
                       mailbox_err);
        return 0;
    }

    mailbox_err = ds5_audio_mailbox_init();
    if (mailbox_err != 0) {
        ds5_log_printf("DS5: audio mailbox initialization failed "
                       "(err %d)\r\n",
                       mailbox_err);
        return 0;
    }

    /* Match the pinned SDK btble_cli persistent-settings initialization. */
    bflb_mtd_init();
    storage_err = easyflash_init();
    if (storage_err != EF_NO_ERR) {
        ds5_log_printf("DS5: pairing storage initialization failed "
                       "(err %d)\r\n",
                       (int)storage_err);
        return 0;
    }
    ds5_log_printf("DS5: pairing storage ready\r\n");

    /* Match the pinned SDK btble_cli and wifi_http initialization order. */
    rf_err = rfparam_init(0U, NULL, 0U);
    if (rf_err != 0) {
        ds5_log_printf("DS5: PHY RF initialization failed (err %d)\r\n",
                       rf_err);
        return 0;
    }
    ds5_log_printf("DS5: PHY RF initialization complete\r\n");

    task_result = xTaskCreate(app_start_task, "app_start", 1024U, NULL,
                              configMAX_PRIORITIES - 2U, NULL);
    if (task_result != pdPASS) {
        ds5_log_printf("DS5: failed to create startup task\r\n");
        return 0;
    }

    vTaskStartScheduler();

    while (1) {
    }
}
