// Aggregate transport only. No GL interception, allocation timing, pool scans or per-call clocks.
#include <psp2/ctrl.h>
static unsigned oy_profile_requested;
static unsigned oy_aggregate_api_done;
#include "bridge-profile-output.c"

void *oy_profile_wrap(const char *name, void *raw)
{
    (void)name;
    return raw;
}

void oy_profile_memory_init(void)
{
    fprintf(stderr, "PROFILE POLICY aggregate=1 gl_wrappers=0 allocation_timers=0 overhead_verified=0\n");
}

void oy_profile_event(uint32_t event, uint64_t value)
{
    switch (event)
    {
    case 1:
        __atomic_store_n(&oy_profile_requested, (unsigned)value, __ATOMIC_RELEASE);
        break;
    case 4:
        if (value >= 20) oy_aggregate_api_done = 1;
        break;
    case 5:
    {
        struct Packet { const char *data; uint32_t size; };
        const struct Packet *packet = (const void *)(uintptr_t)value;
        oy_profile_append(packet->data, packet->size);
        break;
    }
    case 7:
        __atomic_store_n(&oy_render_cycle_done, 1, __ATOMIC_RELEASE);
        break;
    case 8:
        oy_profile_try_flush(oy_aggregate_api_done);
        break;
    case 9:
    {
        uint32_t *ready = (uint32_t *)(uintptr_t)value;
        *ready = 0;
        if (!__atomic_load_n(&oy_capture_flushed, __ATOMIC_ACQUIRE)) break;
        // Both producers completed before the flush. The guest rearms its render epoch after this reply.
        oy_capture_size = oy_capture_dropped = 0;
        ++oy_capture_phase;
        oy_aggregate_api_done = 0;
        __atomic_store_n(&oy_render_cycle_done, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&oy_capture_flushed, 0, __ATOMIC_RELEASE);
        *ready = 1;
        break;
    }
    case 10:
        *(uint32_t **)(uintptr_t)value = NULL;
        break;
    case 11:
        oy_profile_lto = (unsigned)value;
        break;
    case 12:
    {
        SceCtrlData pad;
        const unsigned chord = SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT;
        *(uint32_t *)(uintptr_t)value = sceCtrlPeekBufferPositive(0, &pad, 1) > 0
            && (pad.buttons & chord) == chord;
        break;
    }
    default:
        break;
    }
}
