#ifndef DS5_LOG_H
#define DS5_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

int ds5_log_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
