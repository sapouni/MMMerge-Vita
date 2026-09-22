// Bounded capture storage. No file writes while measured producers are active.
#include <stdarg.h>
static char oy_capture_text[1024 * 1024];
static unsigned oy_capture_size, oy_capture_dropped, oy_capture_lock, oy_capture_flushed;
static unsigned oy_render_cycle_done;
static unsigned oy_capture_phase = 1;
static unsigned oy_capture_reset_requested, oy_capture_reset_ack;
static unsigned oy_profile_lto;
static const char *oy_profile_capture_path(void)
{
    static unsigned long long runId;
    static char path[192];
    if (!runId) runId = (unsigned long long)time(NULL);
    snprintf(path, sizeof(path), "ux0:data/openyamm/profile-00.70-%llu-%s-%s.log", runId,
        oy_profile_lto ? "lto" : "standard", oy_capture_phase == 1 ? "initial" : (oy_capture_phase == 2 ? "steady" : (oy_capture_phase == 3 ? "rotation" : (oy_capture_phase == 4 ? "near-npc" : "far-npc"))));
    return path;
}

static void oy_profile_append(const void *data, size_t size)
{
    while (__atomic_exchange_n(&oy_capture_lock,1,__ATOMIC_ACQUIRE)) {}
    if (!oy_capture_flushed) {
        if (size <= sizeof(oy_capture_text)-oy_capture_size) {
            memcpy(oy_capture_text+oy_capture_size,data,size);
            oy_capture_size+=(unsigned)size;
        } else __atomic_fetch_add(&oy_capture_dropped,1,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&oy_capture_lock,0,__ATOMIC_RELEASE);
}
int oy_profile_fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    char text[2048];
    va_list arguments;
    va_start(arguments,format);
    int size=vsnprintf(text,sizeof(text),format,arguments);
    va_end(arguments);
    if (size>0) oy_profile_append(text,(size_t)size<sizeof(text)?(size_t)size:sizeof(text)-1);
    if (size>=(int)sizeof(text)) __atomic_fetch_add(&oy_capture_dropped,1,__ATOMIC_RELAXED);
    return size;
}
int oy_profile_capture_write(FILE *stream, const void *data, size_t size)
{
    if ((stream!=stderr && stream!=stdout) ||
        !__atomic_load_n(&oy_profile_requested,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&oy_capture_flushed,__ATOMIC_ACQUIRE)) return 0;
    oy_profile_append(data,size);
    return 1;
}
int oy_profile_capture_vprintf(FILE *stream, const char *format, va_list arguments, int *result)
{
    if ((stream!=stderr && stream!=stdout) ||
        !__atomic_load_n(&oy_profile_requested,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&oy_capture_flushed,__ATOMIC_ACQUIRE)) return 0;
    char text[4096]; va_list copy;
    va_copy(copy,arguments);
    *result=vsnprintf(text,sizeof(text),format,copy);
    va_end(copy);
    if (*result>0) oy_profile_append(text,(size_t)*result<sizeof(text)?(size_t)*result:sizeof(text)-1);
    if (*result>=(int)sizeof(text)) __atomic_fetch_add(&oy_capture_dropped,1,__ATOMIC_RELAXED);
    return 1;
}
static void oy_profile_try_flush(unsigned producers_done)
{
    if (!producers_done || !__atomic_load_n(&oy_render_cycle_done,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&oy_capture_flushed,__ATOMIC_ACQUIRE)) return;
    while (__atomic_exchange_n(&oy_capture_lock,1,__ATOMIC_ACQUIRE)) {}
    __atomic_store_n(&oy_capture_flushed,1,__ATOMIC_RELEASE);
    __atomic_store_n(&oy_capture_lock,0,__ATOMIC_RELEASE);
    FILE *file=fopen(oy_profile_capture_path(),"wb");
    if (file) {
        fprintf(file,"PROFILE CAPTURE version=00.70 frames=20 bytes=%u dropped_records=%u\n",
            oy_capture_size,oy_capture_dropped);
        fprintf(file,"PROFILE WINDOW phase=%u second_window_seconds=90\n",oy_capture_phase);
#if OY_PROFILE_AGGREGATE
        fprintf(file,"PROFILE POLICY aggregate=1 hot_operation_timers=0 gl_wrappers=0 "
            "allocation_counts_available=0 overhead_verified=0\n");
#endif
        FILE *configFile = fopen("ux0:data/openyamm/vita-performance.ini", "r");
        if (configFile)
        {
            char configLine[160];
            while (fgets(configLine, sizeof(configLine), configFile))
            {
                if (configLine[0] != '#' && configLine[0] != '\n') fprintf(file, "CONFIG %s", configLine);
            }
            fclose(configFile);
        }
        fwrite(oy_capture_text,1,oy_capture_size,file);
        fprintf(file,"PROFILE CAPTURE_COMPLETE frames=20 dropped_records=%u\n",oy_capture_dropped);
        fclose(file);
    }
    fprintf(stderr,"PROFILE CAPTURE_COMPLETE version=00.70 frames=20 bytes=%u dropped_records=%u file=%s\n",
        oy_capture_size,oy_capture_dropped,file?oy_profile_capture_path():"WRITE_FAILED");
    fflush(stderr);
}
