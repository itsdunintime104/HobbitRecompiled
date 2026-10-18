/* Drives slowframe.c: a 5 ms frame must print nothing, a 60 ms frame must
 * print a SLOWFRAME report naming busy_wait_marker. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "slowframe.h"

__attribute__((noinline)) void busy_wait_marker(int ms)
{
    uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)ms * 1000000ull;
    while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < end)
        ;
}

int main(void)
{
    slowframe_mark();
    busy_wait_marker(5);
    slowframe_mark();
    busy_wait_marker(60);
    slowframe_mark();
    usleep(50000);
    slowframe_mark();
    usleep(20000);
    return 0;
}
