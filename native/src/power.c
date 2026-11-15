#include "power.h"

#include <stdlib.h>

static SRWLOCK lock = SRWLOCK_INIT;
static const void **holders;
static int holder_count;
static int holder_capacity;
static volatile bool high;

static bool set_process(ULONG control, ULONG state)
{
    PROCESS_POWER_THROTTLING_STATE value = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, control, state };
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &value, sizeof(value)) != 0;
}

static void set_thread(ULONG control, ULONG state)
{
    THREAD_POWER_THROTTLING_STATE value = { THREAD_POWER_THROTTLING_CURRENT_VERSION, control, state };
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &value, sizeof(value));
}

void power_hold(const void *holder, bool active)
{
    AcquireSRWLockExclusive(&lock);
    int found = -1;
    for (int index = 0; index < holder_count; index++)
        if (holders[index] == holder) found = index;
    bool changed = false;
    if (active && found < 0)
    {
        if (holder_count == holder_capacity)
        {
            holder_capacity = holder_capacity ? holder_capacity * 2 : 8;
            holders = xrealloc((void *)holders, (size_t)holder_capacity * sizeof(*holders));
        }
        holders[holder_count++] = holder;
        changed = true;
    }
    else if (!active && found >= 0)
    {
        holders[found] = holders[--holder_count];
        changed = true;
    }

    bool now_high = holder_count > 0;
    if (changed && now_high != high)
    {
        high = now_high;
        // 明确不节能，也不忽略计时器精度请求；Windows 10 不认识后一项，失败时只设前一项。交还时两项都不再指定。
        ULONG control = now_high ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
        if (!set_process(control, 0) && now_high) set_process(PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0);
    }
    ReleaseSRWLockExclusive(&lock);
}

bool power_is_high(void)
{
    return high;
}

void power_background_thread(void)
{
    set_thread(THREAD_POWER_THROTTLING_EXECUTION_SPEED, THREAD_POWER_THROTTLING_EXECUTION_SPEED);
}

void power_follow_process(void)
{
    set_thread(0, 0);
}
