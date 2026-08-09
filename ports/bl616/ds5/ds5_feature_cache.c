#include "ds5_feature_cache.h"

#include <string.h>

#define DS5_FEATURE_CACHE_ENTRY_COUNT 4U
#define DS5_FEATURE_CACHE_BANK_COUNT  2U

typedef struct {
    uint8_t report_id;
    uint8_t data[DS5_FEATURE_CACHE_BANK_COUNT]
                [DS5_FEATURE_CACHE_MAX_REPORT_SIZE];
    size_t length[DS5_FEATURE_CACHE_BANK_COUNT];
    volatile uint8_t active_bank;
    volatile bool valid;
} ds5_feature_cache_entry_t;

_Static_assert(DS5_FEATURE_CACHE_MAX_REPORT_SIZE ==
                   (DS5_FEATURE_CACHE_MAX_PAYLOAD + 1U),
               "Feature report must include its report ID");

static ds5_feature_cache_entry_t feature_entries[] = {
    { .report_id = 0x05U },
    { .report_id = 0x09U },
    { .report_id = 0x20U },
    { .report_id = 0x22U },
};

_Static_assert(sizeof(feature_entries) / sizeof(feature_entries[0]) ==
                   DS5_FEATURE_CACHE_ENTRY_COUNT,
               "DualSense Feature cache entry count mismatch");

static ds5_feature_cache_entry_t *ds5_feature_cache_find(uint8_t report_id)
{
    size_t index;

    for (index = 0U; index < DS5_FEATURE_CACHE_ENTRY_COUNT; ++index) {
        if (feature_entries[index].report_id == report_id) {
            return &feature_entries[index];
        }
    }

    return NULL;
}

void ds5_feature_cache_clear(void)
{
    size_t index;

    for (index = 0U; index < DS5_FEATURE_CACHE_ENTRY_COUNT; ++index) {
        feature_entries[index].valid = false;
    }
    __sync_synchronize();
}

bool ds5_feature_cache_store(uint8_t report_id, const uint8_t *payload,
                             size_t length)
{
    ds5_feature_cache_entry_t *entry = ds5_feature_cache_find(report_id);
    uint8_t bank;

    if ((entry == NULL) || (payload == NULL) || (length == 0U) ||
        (length > DS5_FEATURE_CACHE_MAX_PAYLOAD)) {
        return false;
    }

    bank = (uint8_t)(entry->active_bank ^ 1U);
    /*
     * The Bluetooth HID DATA transaction carries A3, report ID, then the
     * report payload. CherryUSB sends the GET_REPORT callback buffer
     * verbatim, so its USB response must restore the non-zero report ID at
     * byte zero. Returning only the Bluetooth payload shifts every Feature
     * field by one byte (most visibly the 0x05 IMU calibration data).
     */
    entry->data[bank][0] = report_id;
    memcpy(&entry->data[bank][1], payload, length);
    entry->length[bank] = length + 1U;
    __sync_synchronize();
    entry->active_bank = bank;
    entry->valid = true;
    return true;
}

bool ds5_feature_cache_get(uint8_t report_id, uint8_t **report,
                           size_t *length)
{
    ds5_feature_cache_entry_t *entry = ds5_feature_cache_find(report_id);
    uint8_t bank;

    if ((entry == NULL) || (report == NULL) || (length == NULL) ||
        !entry->valid) {
        return false;
    }

    __sync_synchronize();
    bank = entry->active_bank;
    *report = entry->data[bank];
    *length = entry->length[bank];
    return true;
}
