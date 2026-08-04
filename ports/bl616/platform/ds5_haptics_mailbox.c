#include "ds5_haptics_mailbox.h"

#include <FreeRTOS.h>
#include "queue.h"

#include "ds5_protocol.h"

#define DS5_HAPTICS_MAILBOX_LENGTH 4U

static StaticQueue_t haptics_queue_storage;
static uint8_t haptics_queue_buffer[DS5_HAPTICS_MAILBOX_LENGTH *
                                    DS5_HAPTICS_DATA_SIZE];
static QueueHandle_t haptics_queue;
static uint32_t dropped_haptics_blocks;

int ds5_haptics_mailbox_init(void)
{
    if (haptics_queue != NULL) {
        return 0;
    }

    haptics_queue = xQueueCreateStatic(DS5_HAPTICS_MAILBOX_LENGTH,
                                       DS5_HAPTICS_DATA_SIZE,
                                       haptics_queue_buffer,
                                       &haptics_queue_storage);
    return haptics_queue != NULL ? 0 : -1;
}

bool ds5_haptics_mailbox_publish(const uint8_t *data, size_t length)
{
    uint8_t discarded[DS5_HAPTICS_DATA_SIZE];

    if ((haptics_queue == NULL) || (data == NULL) ||
        (length != DS5_HAPTICS_DATA_SIZE)) {
        return false;
    }

    if (xQueueSend(haptics_queue, data, 0U) == pdPASS) {
        return true;
    }

    if (xQueueReceive(haptics_queue, discarded, 0U) != pdPASS) {
        return false;
    }

    ++dropped_haptics_blocks;
    return xQueueSend(haptics_queue, data, 0U) == pdPASS;
}

bool ds5_haptics_mailbox_try_receive(uint8_t *data, size_t capacity)
{
    if ((haptics_queue == NULL) || (data == NULL) ||
        (capacity < DS5_HAPTICS_DATA_SIZE)) {
        return false;
    }

    return xQueueReceive(haptics_queue, data, 0U) == pdPASS;
}

uint32_t ds5_haptics_mailbox_dropped_count(void)
{
    return dropped_haptics_blocks;
}
