/* Experiment: present an Intel-Mac clock (mach_timebase_info 1/1, ticks in
 * nanoseconds) to everything in the process. Load with DYLD_INSERT_LIBRARIES. */
#include <mach/mach_time.h>

#define DYLD_INTERPOSE(repl, orig)                                                   \
    __attribute__((used)) static struct {                                            \
        const void *replacement;                                                     \
        const void *replacee;                                                        \
    } interpose_##orig __attribute__((section("__DATA,__interpose"))) = {             \
        (const void *)(unsigned long)&repl, (const void *)(unsigned long)&orig}

static uint64_t my_mach_absolute_time(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0)
        mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
DYLD_INTERPOSE(my_mach_absolute_time, mach_absolute_time);

static kern_return_t my_mach_timebase_info(mach_timebase_info_t info)
{
    info->numer = 1;
    info->denom = 1;
    return KERN_SUCCESS;
}
DYLD_INTERPOSE(my_mach_timebase_info, mach_timebase_info);
