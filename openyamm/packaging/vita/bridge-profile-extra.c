// Diagnostic state. API and render producers have separate slots; pool counters
// in vitaGL are atomic because its collector frees on another thread.
#include <malloc.h>
static void oy_profile_report_textures(unsigned);
extern void oy_vgl_pool_stats(int, uint32_t *, int32_t *, int32_t *, uint32_t *, uint32_t *);
static SceUID oy_api_thread;
static unsigned oy_api_finished;
static uint64_t oy_api_gl_us;
static unsigned oy_api_gl_calls;
static uint64_t oy_guest_alloc_us, oy_guest_alloc_bytes;
static unsigned oy_guest_alloc_calls, oy_guest_alloc_failures;
static uint32_t oy_guest_alloc_total;
static void oy_profile_guest_alloc(size_t bytes, void *result, uint64_t begin)
{
    if (sceKernelGetThreadId()!=oy_api_thread || oy_api_finished ||
        !__atomic_load_n(&oy_profile_requested,__ATOMIC_ACQUIRE)) return;
    oy_guest_alloc_us+=oy_profile_now()-begin; oy_guest_alloc_bytes+=bytes;
    ++oy_guest_alloc_total;
    ++oy_guest_alloc_calls; oy_guest_alloc_failures+=!result && bytes!=0;
}
typedef struct OyContext
{
    const char *operation;
    unsigned target, object, transient;
} OyContext;
static OyContext oy_context[2] = {{"none",0,0,0},{"none",0,0,0}};
static unsigned oy_bound_buffers[2][2];
static unsigned oy_generated_buffers, oy_deleted_buffers, oy_generated_textures, oy_deleted_textures;
static unsigned oy_pool_last_allocations[VGL_MEM_ALL], oy_pool_last_failures[VGL_MEM_ALL];
static unsigned oy_profile_slot(void) { return sceKernelGetThreadId() == oy_api_thread ? 0 : 1; }
typedef struct OyRecovery
{
    OyContext context;
    uint64_t begin, us;
    unsigned size, alignment, gpu, attempts, success;
    void *caller;
    int result_pool;
} OyRecovery;
static OyRecovery oy_recovery[2][64], oy_current_recovery[2];
static unsigned oy_recovery_count[2], oy_recovery_dropped[2];
static uint64_t oy_recovery_us[2];
static unsigned oy_recovery_attempts[2], oy_recovery_failed[2];
static void oy_profile_report_recovery(unsigned slot, unsigned n)
{
    oy_profile_fprintf(stderr,
        "PROFILE RECOVERY n=%u producer=%s events=%u attempts=%u failed=%u ms=%.3f detail_dropped=%u\n",
        n, slot ? "render" : "api", oy_recovery_count[slot], oy_recovery_attempts[slot],
        oy_recovery_failed[slot], oy_recovery_us[slot]/1000.0, oy_recovery_dropped[slot]);
    unsigned count = oy_recovery_count[slot] < 64 ? oy_recovery_count[slot] : 64;
    for (unsigned i=0; i<count; ++i)
    {
        OyRecovery *v=&oy_recovery[slot][i];
        oy_profile_fprintf(stderr,
            "PROFILE RECOVERY_EVENT n=%u producer=%s op=%s target=0x%x object=%u transient=%u "
            "bytes=%u alignment=%u preferred=%s attempts=%u success=%u result_pool=%d ms=%.3f caller=%p\n",
            n, slot ? "render" : "api", v->context.operation, v->context.target, v->context.object,
            v->context.transient, v->size, v->alignment, v->gpu ? "VRAM" : "RAM", v->attempts,
            v->success, v->result_pool, v->us/1000.0, v->caller);
    }
    oy_recovery_count[slot]=oy_recovery_dropped[slot]=oy_recovery_attempts[slot]=oy_recovery_failed[slot]=0;
    oy_recovery_us[slot]=0;
}
// Cheap rolling snapshots after the measured capture. Persist only on allocation failure.
static char oy_memory_ring[32][192];
static unsigned oy_memory_ring_next, oy_memory_ring_count;
static uint64_t oy_memory_ring_due;
static void oy_profile_memory_ring_sample(void)
{
    uint64_t now=oy_profile_now();
    if (!oy_profile_complete || now<oy_memory_ring_due) return;
    oy_memory_ring_due=now+2000000;
    snprintf(oy_memory_ring[oy_memory_ring_next],192,
        "PROFILE MEMORY_TAIL t_us=%llu cdram_free=%u ram_free=%u phycont_free=%u\n",
        (unsigned long long)now,(unsigned)vglMemFree(VGL_MEM_VRAM),
        (unsigned)vglMemFree(VGL_MEM_RAM),(unsigned)vglMemFree(VGL_MEM_PHYCONT));
    oy_memory_ring_next=(oy_memory_ring_next+1)%32;
    if (oy_memory_ring_count<32) ++oy_memory_ring_count;
}
static void oy_profile_memory_ring_flush(void)
{
    for (unsigned i=0;i<oy_memory_ring_count;++i)
        fputs(oy_memory_ring[(oy_memory_ring_next+32-oy_memory_ring_count+i)%32],stderr);
}
void oy_profile_buffer_failure(unsigned object, unsigned size, unsigned offset)
{
    // Stop before any draw can consume invalid storage. This is terminal, not a per-frame log path.
    oy_profile_memory_ring_flush();
    fprintf(stderr,"PROFILE BUFFER_NULL_FATAL object=%u bytes=%u offset=%u\n",object,size,offset);
    if (!oy_capture_flushed) {
        while (__atomic_exchange_n(&oy_capture_lock,1,__ATOMIC_ACQUIRE)) {}
        __atomic_store_n(&oy_capture_flushed,1,__ATOMIC_RELEASE);
        unsigned bytes=oy_capture_size;
        __atomic_store_n(&oy_capture_lock,0,__ATOMIC_RELEASE);
        FILE *partial=fopen("ux0:data/openyamm/profile-00.33-partial.log","wb");
        if (partial) {
            fwrite(oy_capture_text,1,bytes,partial);
            fprintf(partial,"PROFILE CAPTURE_ABORTED reason=null_buffer object=%u bytes=%u\n",object,size);
            fclose(partial);
        }
    }
    fflush(stderr);
    sceKernelExitProcess(1);
}

void oy_profile_recovery(unsigned phase, unsigned gpu, size_t alignment, size_t size,
    unsigned attempts, void *result, void *caller)
{
    if (!__atomic_load_n(&oy_profile_requested,__ATOMIC_ACQUIRE)) return;
    unsigned slot=oy_profile_slot();
    if ((!slot && oy_api_finished) || (slot && oy_profile_complete)) {
        if (phase && !result) {
            oy_profile_memory_ring_flush();
            fprintf(stderr,"PROFILE ALLOCATION_FAILED op=%s object=%u transient=%u bytes=%u "
                "alignment=%u attempts=%u caller=%p\n",oy_context[slot].operation,
                oy_context[slot].object,oy_context[slot].transient,(unsigned)size,
                (unsigned)alignment,attempts,caller);
            fflush(stderr);
        }
        return;
    }
    OyRecovery *v=&oy_current_recovery[slot];
    if (phase==0)
    {
        *v=(OyRecovery){.context=oy_context[slot],.begin=oy_profile_now(),.size=size,
            .alignment=alignment,.gpu=gpu,.caller=caller,.result_pool=-1};
    }
    else
    {
        extern vglMemType vgl_mem_get_type_by_addr(void *);
        v->us=oy_profile_now()-v->begin; v->attempts=attempts; v->success=result!=NULL;
        if (result) v->result_pool=vgl_mem_get_type_by_addr(result);
        unsigned index=oy_recovery_count[slot]++;
        if (index<64) oy_recovery[slot][index]=*v; else ++oy_recovery_dropped[slot];
        oy_recovery_attempts[slot]+=attempts; oy_recovery_failed[slot]+=!result;
        oy_recovery_us[slot]+=v->us;
    }
}
static void oy_profile_heap(unsigned n)
{
    extern unsigned int _newlib_heap_size_user;
    uint64_t begin=oy_profile_now();
    struct mallinfo v=mallinfo();
    oy_profile_fprintf(stderr,
        "PROFILE HEAP n=%u capacity=%u committed=%d used=%d free_committed=%d sample_ms=%.3f\n",
        n,_newlib_heap_size_user,v.arena,v.uordblks,v.fordblks,(oy_profile_now()-begin)/1000.0);
}
void oy_profile_memory_init(void)
{
    const char *names[]={"CDRAM","RAM","PHYCONT","BUDGET","NEWLIB"};
    for (int type=0; type<VGL_MEM_EXTERNAL; ++type)
    {
        uint32_t capacity,allocations,failures;int32_t live,blocks;
        oy_vgl_pool_stats(type,&capacity,&live,&blocks,&allocations,&failures);
        uint64_t begin=oy_profile_now();
        size_t free_bytes=vglMemFree(type); // Exact scan once at initialization, outside measured frames.
        oy_profile_fprintf(stderr,
            "PROFILE POOL_INIT type=%s capacity=%u free=%u live_payload=%d blocks=%d scan_ms=%.3f\n",
            names[type],capacity,(unsigned)free_bytes,live,blocks,(oy_profile_now()-begin)/1000.0);
    }
    oy_profile_heap(0);
}
static void oy_profile_report_extra(unsigned n)
{
    oy_profile_report_textures(n);
    const char *names[]={"CDRAM","RAM","PHYCONT","BUDGET","NEWLIB"};
    oy_profile_fprintf(stderr,"PROFILE LIFETIME n=%u buffers_generated=%u buffers_deleted=%u textures_generated=%u textures_deleted=%u\n",
        n,oy_generated_buffers,oy_deleted_buffers,oy_generated_textures,oy_deleted_textures);
    oy_generated_buffers=oy_deleted_buffers=oy_generated_textures=oy_deleted_textures=0;
    oy_profile_report_recovery(1,n);
    for (int type=0; type<VGL_MEM_EXTERNAL; ++type)
    {
        uint32_t capacity,allocations,failures;int32_t live,blocks;
        oy_vgl_pool_stats(type,&capacity,&live,&blocks,&allocations,&failures);
        uint64_t scan_begin=oy_profile_now();
        size_t free_bytes=vglMemFree(type);
        uint64_t scan_us=oy_profile_now()-scan_begin;
        oy_profile_fprintf(stderr,
            "PROFILE POOL n=%u type=%s capacity=%u live_payload=%d free=%u scan_ms=%.3f "
            "blocks=%d allocations=%u pool_attempt_failures=%u\n",
            n,names[type],capacity,live,(unsigned)free_bytes,scan_us/1000.0,blocks,
            allocations-oy_pool_last_allocations[type],failures-oy_pool_last_failures[type]);
        oy_pool_last_allocations[type]=allocations; oy_pool_last_failures[type]=failures;
    }
}
#include "bridge-profile-texture.c"
