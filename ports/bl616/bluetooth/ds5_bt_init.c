#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"
#include "btble_lib_api.h"
#include "hci_driver.h"

#include "ds5_bt_internal.h"
#include "ds5_l2cap.h"
#include "ds5_log.h"

#define printf ds5_log_printf

static int ds5_bt_start_worker(void)
{
    if ((worker_task != NULL) && (tx_worker_task != NULL) &&
        (policy_task != NULL)) {
        return 0;
    }

    if (worker_task == NULL) {
        worker_task = xTaskCreateStatic(ds5_bt_worker, "ds5_bt_worker",
                                        DS5_BT_WORKER_STACK_DEPTH, NULL,
                                        configMAX_PRIORITIES - 5U,
                                        worker_task_stack,
                                        &worker_task_storage);
        if (worker_task == NULL) {
            return -ENOMEM;
        }
    }

    if (tx_worker_task == NULL) {
        tx_worker_task = xTaskCreateStatic(ds5_bt_tx_worker, "ds5_bt_tx",
                                           DS5_BT_TX_WORKER_STACK_DEPTH, NULL,
                                           configMAX_PRIORITIES - 5U,
                                           tx_worker_task_stack,
                                           &tx_worker_task_storage);
        if (tx_worker_task == NULL) {
            return -ENOMEM;
        }
    }

    if (policy_task == NULL) {
        policy_task = xTaskCreateStatic(
            ds5_bt_policy_worker, "ds5_bt_policy", DS5_BT_POLICY_STACK_DEPTH,
            NULL, configMAX_PRIORITIES - 5U, policy_task_stack,
            &policy_task_storage);
    }
    return policy_task != NULL ? 0 : -ENOMEM;
}
static void ds5_bt_ready(int err)
{
    size_t bond_count = 0U;

    if (err != 0) {
        printf("DS5 BT: host initialization failed (err %d)\r\n", err);
        ds5_bt_set_state(DS5_BT_STATE_OFF);
        return;
    }

    printf("DS5 BT: host ready\r\n");

    err = ds5_bt_register_callbacks();
    if (err != 0) {
        printf("DS5 BT: auth callback registration failed (err %d)\r\n", err);
        return;
    }

    err = ds5_l2cap_init();
    if (err != 0) {
        printf("DS5 BT: HID L2CAP registration failed (err %d)\r\n", err);
        return;
    }

    printf("DS5 BT: HID L2CAP servers ready\r\n");

    /* bt_enable() has already restored BR link keys before this callback. */
    memset(&candidate, 0, sizeof(candidate));
    bonded_peer_valid = false;
    bond_recovery_pending = false;
    bond_recovery_retry_at = 0U;
    page_scan_restore_pending = false;
    page_scan_retry_at = 0U;
    bt_br_foreach_bond(ds5_bt_restore_bond, &bond_count);
    if (bonded_peer_valid) {
        char address[BT_ADDR_STR_LEN];

        bt_addr_to_str(&bonded_peer_address, address, sizeof(address));
        printf("DS5 BT: restored saved BR/EDR peer %s "
               "(%u bond(s) stored)\r\n",
               address, (unsigned int)bond_count);
        if (bond_count > 1U) {
            printf("DS5 BT: multiple bonds present; using the first saved "
                   "peer\r\n");
        }

        /*
         * Page scan is an external connection entry point.  Defer enabling
         * it until the policy task owns the fully initialized state machine;
         * otherwise a saved controller can connect in the middle of this
         * callback and have its state overwritten below.
         */
        page_scan_restore_pending = true;
        page_scan_retry_at = xTaskGetTickCount();
    } else {
        printf("DS5 BT: no saved BR/EDR peer\r\n");
    }

    /*
     * Passive page scan for the bonded peer and active inquiry for a new
     * controller are independent procedures.  Start the pairing window
     * immediately; it must not delay reconnection of the bonded peer.
     */
    ds5_bt_set_state(DS5_BT_STATE_IDLE);
    ds5_bt_open_pairing_window();

    /* Start policy only after all initial bond/window state is consistent. */
    err = ds5_bt_start_worker();
    if (err != 0) {
        printf("DS5 BT: worker creation failed (err %d)\r\n", err);
        return;
    }
}

int ds5_bt_init(void)
{
    int err;

    if (lifecycle_mutex == NULL) {
        lifecycle_mutex = xSemaphoreCreateRecursiveMutexStatic(
            &lifecycle_mutex_storage);
        if (lifecycle_mutex == NULL) {
            return -ENOMEM;
        }
    }

    printf("DS5 BT: initializing controller\r\n");
    btble_controller_init(configMAX_PRIORITIES - 1U);
    printf("DS5 BT: controller initialized\r\n");

    printf("DS5 BT: initializing HCI driver\r\n");
    err = hci_driver_init();
    if (err != 0) {
        printf("DS5 BT: HCI driver initialization failed (err %d)\r\n", err);
        return err;
    }
    printf("DS5 BT: HCI driver initialized\r\n");

    printf("DS5 BT: enabling host\r\n");
    err = bt_enable(ds5_bt_ready);
    if (err != 0) {
        printf("DS5 BT: bt_enable failed (err %d)\r\n", err);
        return err;
    }
    printf("DS5 BT: host enable submitted\r\n");

    return 0;
}
