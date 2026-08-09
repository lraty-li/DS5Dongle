#ifndef DS5_OUTPUT_MAILBOX_H
#define DS5_OUTPUT_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ds5_output_mailbox_init(void);
bool ds5_output_mailbox_publish(const uint8_t *report, size_t length);
bool ds5_output_mailbox_receive(uint8_t *report, size_t capacity);
bool ds5_output_mailbox_try_receive(uint8_t *report, size_t capacity);

/* Physical presses are ordered events; do not collapse two presses into one. */
bool ds5_output_mailbox_publish_microphone_button_press(void);
bool ds5_output_mailbox_try_receive_microphone_button_press(void);

#ifdef __cplusplus
}
#endif

#endif
