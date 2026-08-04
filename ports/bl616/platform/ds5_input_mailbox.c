#include "ds5_input_mailbox.h"

#include <FreeRTOS.h>
#include "queue.h"

#include "ds5_protocol.h"

#define DS5_INPUT_MAILBOX_LENGTH 1U

static StaticQueue_t input_queue_storage;
static uint8_t input_queue_buffer[DS5_USB_INPUT_PAYLOAD_SIZE];
static QueueHandle_t input_queue;

int ds5_input_mailbox_init(void)
{
    if (input_queue != NULL) {
        return 0;
    }

    input_queue = xQueueCreateStatic(DS5_INPUT_MAILBOX_LENGTH,
                                     DS5_USB_INPUT_PAYLOAD_SIZE,
                                     input_queue_buffer,
                                     &input_queue_storage);
    return input_queue != NULL ? 0 : -1;
}

bool ds5_input_mailbox_publish(const uint8_t *payload, size_t length)
{
    if ((input_queue == NULL) || (payload == NULL) ||
        (length != DS5_USB_INPUT_PAYLOAD_SIZE)) {
        return false;
    }

    /* A gamepad is stateful; when USB lags, retain the newest state. */
    return xQueueOverwrite(input_queue, payload) == pdPASS;
}

bool ds5_input_mailbox_receive(uint8_t *payload, size_t capacity,
                               uint32_t timeout_ms)
{
    TickType_t timeout_ticks;

    if ((input_queue == NULL) || (payload == NULL) ||
        (capacity < DS5_USB_INPUT_PAYLOAD_SIZE)) {
        return false;
    }

    timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if ((timeout_ms != 0U) && (timeout_ticks == 0U)) {
        timeout_ticks = 1U;
    }

    return xQueueReceive(input_queue, payload, timeout_ticks) == pdPASS;
}
