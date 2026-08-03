#include "bflb_mtimer.h"
#include "board.h"

#define DBG_TAG "DS5"
#include "log.h"

int main(void)
{
    board_init();

    LOG_I("DS5Dongle BL616 port started\r\n");

    while (1) {
        bflb_mtimer_delay_ms(1000);
    }
}
