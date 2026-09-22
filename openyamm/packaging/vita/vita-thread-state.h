#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

// Explicit native pthread storage: this toolchain's GCC TLS probe reports single-threaded.
struct OyNativeThreadState
{
    int renderPinned;
    uint64_t houseRecoveryBegin;
};
static pthread_key_t oy_native_state_key;
static pthread_once_t oy_native_state_once = PTHREAD_ONCE_INIT;
static int oy_native_state_key_result = -1;
static void oy_native_state_create_key(void)
{
    oy_native_state_key_result = pthread_key_create(&oy_native_state_key, free);
}
static struct OyNativeThreadState *oy_native_thread_state(void)
{
    if (pthread_once(&oy_native_state_once, oy_native_state_create_key) != 0
        || oy_native_state_key_result != 0) { return NULL; }
    struct OyNativeThreadState *state = pthread_getspecific(oy_native_state_key);
    if (!state)
    {
        state = calloc(1, sizeof(*state));
        if (!state) { return NULL; }
        if (pthread_setspecific(oy_native_state_key, state) != 0)
        {
            free(state);
            return NULL;
        }
    }
    return state;
}
