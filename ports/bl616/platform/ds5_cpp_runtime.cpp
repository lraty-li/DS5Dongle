#include <stddef.h>
#include <stdlib.h>

/*
 * BouffaloSDK's bare-metal link does not provide libstdc++ allocation
 * operators.  WDL uses them only for optional resampler filters; the native
 * haptics configuration disables those filters and preallocates its working
 * buffer before the USB stream starts.
 */
void *operator new(size_t size) noexcept
{
    return malloc(size);
}

void operator delete(void *pointer) noexcept
{
    free(pointer);
}
