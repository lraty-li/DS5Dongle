#ifndef DS5_FEATURE_CACHE_H
#define DS5_FEATURE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS5_FEATURE_CACHE_MAX_PAYLOAD     63U
#define DS5_FEATURE_CACHE_MAX_REPORT_SIZE 64U

void ds5_feature_cache_clear(void);
bool ds5_feature_cache_store(uint8_t report_id, const uint8_t *payload,
                             size_t length);
bool ds5_feature_cache_get(uint8_t report_id, uint8_t **report,
                           size_t *length);

#ifdef __cplusplus
}
#endif

#endif
