#include "ds5_audio_mailbox.h"

#include <FreeRTOS.h>
#include "portmacro.h"
#include "queue.h"

#include "ds5_protocol.h"

#define DS5_AUDIO_SPEAKER_MAILBOX_LENGTH    2U
#define DS5_AUDIO_MICROPHONE_MAILBOX_LENGTH 8U
#define DS5_AUDIO_STATE_MAILBOX_LENGTH      1U

static StaticQueue_t speaker_queue_storage;
static uint8_t speaker_queue_buffer[DS5_AUDIO_SPEAKER_MAILBOX_LENGTH *
                                    DS5_AUDIO_SPEAKER_OPUS_SIZE];
static QueueHandle_t speaker_queue;

static StaticQueue_t microphone_queue_storage;
static uint8_t microphone_queue_buffer[DS5_AUDIO_MICROPHONE_MAILBOX_LENGTH *
                                       DS5_AUDIO_MIC_OPUS_SIZE];
static QueueHandle_t microphone_queue;

static StaticQueue_t microphone_state_queue_storage;
static bool microphone_state_queue_buffer[DS5_AUDIO_STATE_MAILBOX_LENGTH];
static QueueHandle_t microphone_state_queue;

static volatile bool speaker_stream_active;
static uint32_t dropped_speaker_frames;
static uint32_t dropped_microphone_frames;

static bool ds5_audio_mailbox_publish_latest(QueueHandle_t queue,
                                             const void *data,
                                             size_t length,
                                             size_t expected_length,
                                             uint32_t *dropped_count)
{
    uint8_t discarded[DS5_AUDIO_SPEAKER_OPUS_SIZE];

    if ((queue == NULL) || (data == NULL) || (length != expected_length)) {
        return false;
    }

    if (xQueueSend(queue, data, 0U) == pdPASS) {
        return true;
    }

    if ((expected_length > sizeof(discarded)) ||
        (xQueueReceive(queue, discarded, 0U) != pdPASS)) {
        return false;
    }

    ++(*dropped_count);
    return xQueueSend(queue, data, 0U) == pdPASS;
}

int ds5_audio_mailbox_init(void)
{
    if ((speaker_queue != NULL) && (microphone_queue != NULL) &&
        (microphone_state_queue != NULL)) {
        return 0;
    }

    speaker_queue = xQueueCreateStatic(DS5_AUDIO_SPEAKER_MAILBOX_LENGTH,
                                       DS5_AUDIO_SPEAKER_OPUS_SIZE,
                                       speaker_queue_buffer,
                                       &speaker_queue_storage);
    microphone_queue = xQueueCreateStatic(DS5_AUDIO_MICROPHONE_MAILBOX_LENGTH,
                                          DS5_AUDIO_MIC_OPUS_SIZE,
                                          microphone_queue_buffer,
                                          &microphone_queue_storage);
    microphone_state_queue = xQueueCreateStatic(
        DS5_AUDIO_STATE_MAILBOX_LENGTH, sizeof(bool),
        (uint8_t *)microphone_state_queue_buffer,
        &microphone_state_queue_storage);

    if ((speaker_queue == NULL) || (microphone_queue == NULL) ||
        (microphone_state_queue == NULL)) {
        return -1;
    }

    speaker_stream_active = false;
    dropped_speaker_frames = 0U;
    dropped_microphone_frames = 0U;
    return 0;
}

bool ds5_audio_mailbox_publish_speaker_opus(const uint8_t *data,
                                            size_t length)
{
    return ds5_audio_mailbox_publish_latest(
        speaker_queue, data, length, DS5_AUDIO_SPEAKER_OPUS_SIZE,
        &dropped_speaker_frames);
}

bool ds5_audio_mailbox_try_receive_speaker_opus(uint8_t *data,
                                                size_t capacity)
{
    if ((speaker_queue == NULL) || (data == NULL) ||
        (capacity < DS5_AUDIO_SPEAKER_OPUS_SIZE)) {
        return false;
    }

    return xQueueReceive(speaker_queue, data, 0U) == pdPASS;
}

bool ds5_audio_mailbox_publish_microphone_opus(const uint8_t *data,
                                               size_t length)
{
    return ds5_audio_mailbox_publish_latest(
        microphone_queue, data, length, DS5_AUDIO_MIC_OPUS_SIZE,
        &dropped_microphone_frames);
}

bool ds5_audio_mailbox_try_receive_microphone_opus(uint8_t *data,
                                                   size_t capacity)
{
    if ((microphone_queue == NULL) || (data == NULL) ||
        (capacity < DS5_AUDIO_MIC_OPUS_SIZE)) {
        return false;
    }

    return xQueueReceive(microphone_queue, data, 0U) == pdPASS;
}

void ds5_audio_mailbox_set_speaker_stream_active(bool active)
{
    speaker_stream_active = active;
}

bool ds5_audio_mailbox_speaker_stream_active(void)
{
    return speaker_stream_active;
}

bool ds5_audio_mailbox_publish_microphone_stream_active(bool active)
{
    if (microphone_state_queue == NULL) {
        return false;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t task_woken = pdFALSE;
        BaseType_t result = xQueueOverwriteFromISR(microphone_state_queue,
                                                   &active, &task_woken);

        portYIELD_FROM_ISR(task_woken);
        return result == pdPASS;
    }

    return xQueueOverwrite(microphone_state_queue, &active) == pdPASS;
}

bool ds5_audio_mailbox_try_receive_microphone_stream_active(bool *active)
{
    if ((microphone_state_queue == NULL) || (active == NULL)) {
        return false;
    }

    return xQueueReceive(microphone_state_queue, active, 0U) == pdPASS;
}

uint32_t ds5_audio_mailbox_dropped_speaker_frames(void)
{
    return dropped_speaker_frames;
}

uint32_t ds5_audio_mailbox_dropped_microphone_frames(void)
{
    return dropped_microphone_frames;
}
