#include "ds5_feature_set_mailbox.h"

#include <string.h>

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"

#define DS5_FEATURE_SET_MAILBOX_LENGTH 4U

static StaticQueue_t feature_set_queue_storage;
static uint8_t feature_set_queue_buffer[
    DS5_FEATURE_SET_MAILBOX_LENGTH * sizeof(ds5_feature_set_request_t)];
static QueueHandle_t feature_set_queue;
int ds5_feature_set_mailbox_init(void)
{
    if (feature_set_queue != NULL) {
        return 0;
    }

    feature_set_queue = xQueueCreateStatic(
        DS5_FEATURE_SET_MAILBOX_LENGTH, sizeof(ds5_feature_set_request_t),
        feature_set_queue_buffer, &feature_set_queue_storage);
    return feature_set_queue != NULL ? 0 : -1;
}

bool ds5_feature_set_mailbox_publish(uint8_t report_id,
                                     const uint8_t *payload,
                                     size_t length)
{
    ds5_feature_set_request_t request = { 0 };
    BaseType_t result;

    if ((feature_set_queue == NULL) ||
        ((payload == NULL) && (length != 0U)) ||
        (length > DS5_FEATURE_SET_MAX_PAYLOAD)) {
        return false;
    }

    request.report_id = report_id;
    request.payload_length = (uint8_t)length;
    if (length != 0U) {
        memcpy(request.payload, payload, length);
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;

        result = xQueueSendFromISR(feature_set_queue, &request,
                                   &task_woken);
        portYIELD_FROM_ISR(task_woken);
    } else {
        result = xQueueSend(feature_set_queue, &request, 0U);
    }

    if (result != pdPASS) {
        return false;
    }

    return true;
}

bool ds5_feature_set_mailbox_try_receive(ds5_feature_set_request_t *request)
{
    if ((feature_set_queue == NULL) || (request == NULL)) {
        return false;
    }

    return xQueueReceive(feature_set_queue, request, 0U) == pdPASS;
}

void ds5_feature_set_mailbox_clear(void)
{
    if (feature_set_queue == NULL) {
        return;
    }

    configASSERT(!xPortIsInsideInterrupt());
    (void)xQueueReset(feature_set_queue);
}
