#include "ds5_output_mailbox.h"

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"

#include "ds5_protocol.h"

#define DS5_OUTPUT_MAILBOX_LENGTH 1U

static StaticQueue_t output_queue_storage;
static uint8_t output_queue_buffer[DS5_USB_OUTPUT_REPORT_SIZE];
static QueueHandle_t output_queue;

int ds5_output_mailbox_init(void)
{
    if (output_queue != NULL) {
        return 0;
    }

    output_queue = xQueueCreateStatic(DS5_OUTPUT_MAILBOX_LENGTH,
                                      DS5_USB_OUTPUT_REPORT_SIZE,
                                      output_queue_buffer,
                                      &output_queue_storage);
    return output_queue != NULL ? 0 : -1;
}

bool ds5_output_mailbox_publish(const uint8_t *report, size_t length)
{
    if ((output_queue == NULL) || (report == NULL) ||
        (length != DS5_USB_OUTPUT_REPORT_SIZE)) {
        return false;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;
        BaseType_t result = xQueueOverwriteFromISR(output_queue, report,
                                                   &task_woken);

        portYIELD_FROM_ISR(task_woken);
        return result == pdPASS;
    }

    return xQueueOverwrite(output_queue, report) == pdPASS;
}

bool ds5_output_mailbox_receive(uint8_t *report, size_t capacity)
{
    if ((output_queue == NULL) || (report == NULL) ||
        (capacity < DS5_USB_OUTPUT_REPORT_SIZE)) {
        return false;
    }

    return xQueueReceive(output_queue, report, portMAX_DELAY) == pdPASS;
}

bool ds5_output_mailbox_try_receive(uint8_t *report, size_t capacity)
{
    if ((output_queue == NULL) || (report == NULL) ||
        (capacity < DS5_USB_OUTPUT_REPORT_SIZE)) {
        return false;
    }

    return xQueueReceive(output_queue, report, 0U) == pdPASS;
}
