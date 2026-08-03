#include <stdio.h>

#include <FreeRTOS.h>
#include "task.h"

#include "board.h"
#include "rfparam_adapter.h"

#include "ds5_bt.h"

static void app_start_task(void *parameter)
{
    int err;

    (void)parameter;

    err = ds5_bt_init();
    if (err != 0) {
        printf("DS5: Bluetooth startup failed (err %d)\r\n", err);
    }

    vTaskDelete(NULL);
}

int main(void)
{
    BaseType_t task_result;

    board_init();

    printf("DS5Dongle BL616 BR/EDR discovery bring-up\r\n");

    configASSERT(configMAX_PRIORITIES > 4U);

    if (rfparam_init(0U, NULL, 0U) != 0) {
        printf("DS5: PHY RF initialization failed\r\n");
        return 0;
    }

    task_result = xTaskCreate(app_start_task, "app_start", 1024U, NULL,
                              configMAX_PRIORITIES - 2U, NULL);
    if (task_result != pdPASS) {
        printf("DS5: failed to create Bluetooth startup task\r\n");
        return 0;
    }

    vTaskStartScheduler();

    while (1) {
    }
}
