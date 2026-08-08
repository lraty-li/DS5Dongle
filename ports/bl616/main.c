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
#define DS5_BT_DISCOVERY_RETRY_MS  750U
#define DS5_BT_CONNECTION_RETRY_MS 1000U
#define DS5_BT_RECONNECT_TIMEOUT_MS 20000U

static void app_start_task(void *parameter)
{
    ds5_bt_state_t bluetooth_state;
    ds5_bt_state_t previous_state = DS5_BT_STATE_OFF;
    TickType_t reconnect_wait_since = 0U;
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
        ds5_log_printf("DS5 USB: HID/UAC composite initialized\r\n");
    }

    while (1) {
        bluetooth_state = ds5_bt_get_state();

        /* Restart the passive-reconnect timer every time we enter wait. */
        if (bluetooth_state != previous_state) {
            if (bluetooth_state == DS5_BT_STATE_RECONNECT_WAIT) {
                reconnect_wait_since = xTaskGetTickCount();
            }
            previous_state = bluetooth_state;
        }

        /*
         * RECONNECT_WAIT is passive: it waits for the controller to page
         * this dongle (PS button).  If the controller's saved host is gone
         * or invalid, nothing will ever page us, so time out and fall back
         * to active discovery + connection (keeps the existing bond).
         */
        if ((bluetooth_state == DS5_BT_STATE_RECONNECT_WAIT) &&
            (xTaskGetTickCount() - reconnect_wait_since) >=
                pdMS_TO_TICKS(DS5_BT_RECONNECT_TIMEOUT_MS)) {
            ds5_log_printf("DS5: reconnect wait timed out; "
                           "falling back to discovery\r\n");
            (void)ds5_bt_fallback_to_discovery();
        }

        if (bluetooth_state == DS5_BT_STATE_CANDIDATE_READY) {
            err = ds5_bt_connect_candidate();
            if (err != 0) {
                ds5_log_printf(
                    "DS5: candidate ACL connection failed to start "
                    "(err %d)\r\n",
                    err);
                vTaskDelay(pdMS_TO_TICKS(DS5_BT_CONNECTION_RETRY_MS));
            } else {
                ds5_log_printf("DS5: candidate ACL connection started\r\n");
            }
            continue;
        }

        if (bluetooth_state == DS5_BT_STATE_RECONNECT_WAIT) {
            vTaskDelay(pdMS_TO_TICKS(DS5_BT_STATE_POLL_MS));
            continue;
        }

        if (bluetooth_state == DS5_BT_STATE_IDLE) {
            ds5_log_printf("DS5: no candidate; discovery will retry\r\n");

            /*
             * The pinned SDK clears its discovery callback/result pointers
             * immediately after the completion callback returns. Delay the
             * restart so it cannot overwrite that cleanup in hci_core.c.
             */
            vTaskDelay(pdMS_TO_TICKS(DS5_BT_DISCOVERY_RETRY_MS));
            if (ds5_bt_get_state() != DS5_BT_STATE_IDLE) {
                continue;
            }

            err = ds5_bt_start_discovery();
            if (err != 0) {
                ds5_log_printf("DS5: discovery retry failed (err %d)\r\n",
                               err);
            }
            continue;
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
