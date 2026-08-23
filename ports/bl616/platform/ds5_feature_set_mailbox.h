#ifndef DS5_FEATURE_SET_MAILBOX_H
#define DS5_FEATURE_SET_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ds5_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t report_id;
    uint8_t payload_length;
    uint8_t payload[DS5_FEATURE_SET_MAX_PAYLOAD];
} ds5_feature_set_request_t;

int ds5_feature_set_mailbox_init(void);
bool ds5_feature_set_mailbox_publish(uint8_t report_id,
                                     const uint8_t *payload,
                                     size_t length);
bool ds5_feature_set_mailbox_try_receive(ds5_feature_set_request_t *request);
/* Task-context API used when a Bluetooth link generation changes. */
void ds5_feature_set_mailbox_clear(void);

#ifdef __cplusplus
}
#endif

#endif
