#include "ds5_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "bflb_irq.h"

#define DS5_LOG_RING_SIZE   4096U
#define DS5_LOG_FORMAT_SIZE 256U

static uint8_t log_ring[DS5_LOG_RING_SIZE];
static size_t log_head;
static size_t log_length;
static uint32_t dropped_bytes;

void ds5_log_init(void)
{
    uintptr_t flags = bflb_irq_save();

    log_head = 0U;
    log_length = 0U;
    dropped_bytes = 0U;

    bflb_irq_restore(flags);
}

int ds5_log_printf(const char *format, ...)
{
    char formatted[DS5_LOG_FORMAT_SIZE];
    va_list arguments;
    uintptr_t flags;
    size_t write_length;
    size_t tail;
    size_t first_length;
    int result;

    va_start(arguments, format);
    result = vsnprintf(formatted, sizeof(formatted), format, arguments);
    va_end(arguments);

    if (result < 0) {
        return result;
    }

    write_length = (size_t)result;
    if (write_length >= sizeof(formatted)) {
        write_length = sizeof(formatted) - 1U;
    }

    /* Preserve the existing UART output while also retaining it for USB. */
    printf("%.*s", (int)write_length, formatted);

    flags = bflb_irq_save();

    if (write_length > (sizeof(log_ring) - log_length)) {
        dropped_bytes += (uint32_t)write_length;
        bflb_irq_restore(flags);
        return result;
    }

    tail = (log_head + log_length) % sizeof(log_ring);
    first_length = sizeof(log_ring) - tail;
    if (first_length > write_length) {
        first_length = write_length;
    }

    for (size_t index = 0U; index < first_length; ++index) {
        log_ring[tail + index] = (uint8_t)formatted[index];
    }
    for (size_t index = first_length; index < write_length; ++index) {
        log_ring[index - first_length] = (uint8_t)formatted[index];
    }
    log_length += write_length;

    bflb_irq_restore(flags);
    return result;
}

size_t ds5_log_peek(uint8_t *buffer, size_t capacity)
{
    uintptr_t flags;
    size_t read_length;
    size_t first_length;

    if ((buffer == NULL) || (capacity == 0U)) {
        return 0U;
    }

    flags = bflb_irq_save();

    read_length = log_length;
    if (read_length > capacity) {
        read_length = capacity;
    }

    first_length = sizeof(log_ring) - log_head;
    if (first_length > read_length) {
        first_length = read_length;
    }

    for (size_t index = 0U; index < first_length; ++index) {
        buffer[index] = log_ring[log_head + index];
    }
    for (size_t index = first_length; index < read_length; ++index) {
        buffer[index] = log_ring[index - first_length];
    }

    bflb_irq_restore(flags);
    return read_length;
}

void ds5_log_discard(size_t length)
{
    uintptr_t flags = bflb_irq_save();

    if (length > log_length) {
        length = log_length;
    }

    log_head = (log_head + length) % sizeof(log_ring);
    log_length -= length;

    bflb_irq_restore(flags);
}

uint32_t ds5_log_dropped_bytes(void)
{
    uintptr_t flags = bflb_irq_save();
    uint32_t result = dropped_bytes;

    bflb_irq_restore(flags);
    return result;
}
