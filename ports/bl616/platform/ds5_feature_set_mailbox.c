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
static volatile uint32_t received_feature_sets;
static volatile uint32_t dropped_feature_sets;
static volatile uint32_t forwarded_feature_sets;
static volatile uint32_t failed_feature_set_forwards;
static volatile uint8_t last_feature_set_report_id;
static volatile uint8_t last_feature_set_payload_length;

int ds5_feature_set_mailbox_init(void)
{
    if (feature_set_queue != NULL) {
        return 0;
    }

    received_feature_sets = 0U;
    dropped_feature_sets = 0U;
    forwarded_feature_sets = 0U;
    failed_feature_set_forwards = 0U;
    last_feature_set_report_id = 0U;
    last_feature_set_payload_length = 0U;
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
        ++dropped_feature_sets;
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
        ++dropped_feature_sets;
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

void ds5_feature_set_mailbox_note_received(uint8_t report_id, size_t length)
{
    last_feature_set_report_id = report_id;
    last_feature_set_payload_length =
        (length > UINT8_MAX) ? UINT8_MAX : (uint8_t)length;
    ++received_feature_sets;
}

void ds5_feature_set_mailbox_note_forwarded(void)
{
    ++forwarded_feature_sets;
}

void ds5_feature_set_mailbox_note_forward_failed(void)
{
    ++failed_feature_set_forwards;
}

uint32_t ds5_feature_set_mailbox_received_count(void)
{
    return received_feature_sets;
}

uint32_t ds5_feature_set_mailbox_dropped_count(void)
{
    return dropped_feature_sets;
}

uint32_t ds5_feature_set_mailbox_forwarded_count(void)
{
    return forwarded_feature_sets;
}

uint32_t ds5_feature_set_mailbox_forward_failed_count(void)
{
    return failed_feature_set_forwards;
}

uint8_t ds5_feature_set_mailbox_last_report_id(void)
{
    return last_feature_set_report_id;
}

uint8_t ds5_feature_set_mailbox_last_payload_length(void)
{
    return last_feature_set_payload_length;
}
