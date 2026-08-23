#include "ds5_log.h"

#include <stdarg.h>
#include <stdio.h>

int ds5_log_printf(const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = vprintf(format, arguments);
    va_end(arguments);

    return result;
}
