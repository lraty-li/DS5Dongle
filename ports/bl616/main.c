#include <FreeRTOS.h>
#include "task.h"

#include "board.h"
#include "rfparam_adapter.h"

#include "ds5_bt.h"
#include "ds5_log.h"
#include "ds5_usb_log.h"

static void app_start_task(void *parameter)
{
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
        ds5_log_printf("DS5 USB: CDC log initialization failed (err %d)\r\n",
                       err);
    } else {
        ds5_log_printf("DS5 USB: CDC diagnostic interface initialized\r\n");
    }

    vTaskDelete(NULL);
}

int main(void)
{
    BaseType_t task_result;
    int rf_err;

    board_init();
    ds5_log_init();

    ds5_log_printf("DS5Dongle BL616 BR/EDR connection bring-up\r\n");

    configASSERT(configMAX_PRIORITIES > 5U);

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
