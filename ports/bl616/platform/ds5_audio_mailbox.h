#ifndef DS5_AUDIO_MAILBOX_H
#define DS5_AUDIO_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ds5_audio_mailbox_notify_t)(void);

int ds5_audio_mailbox_init(void);

bool ds5_audio_mailbox_publish_speaker_opus(const uint8_t *data,
                                            size_t length);
bool ds5_audio_mailbox_try_receive_speaker_opus(uint8_t *data,
                                                size_t capacity);

bool ds5_audio_mailbox_publish_microphone_opus(const uint8_t *data,
                                               size_t length);
bool ds5_audio_mailbox_try_receive_microphone_opus(uint8_t *data,
                                                   size_t capacity);
/*
 * Register the task-context wakeup used by the single microphone consumer.
 * Passing NULL unregisters it before that consumer is destroyed.
 */
void ds5_audio_mailbox_set_microphone_notify(
    ds5_audio_mailbox_notify_t notify);

void ds5_audio_mailbox_set_speaker_stream_active(bool active);
bool ds5_audio_mailbox_speaker_stream_active(void);

bool ds5_audio_mailbox_publish_microphone_stream_active(bool active);
bool ds5_audio_mailbox_try_receive_microphone_stream_active(bool *active);

uint32_t ds5_audio_mailbox_dropped_speaker_frames(void);
uint32_t ds5_audio_mailbox_dropped_microphone_frames(void);

#ifdef __cplusplus
}
#endif

#endif
