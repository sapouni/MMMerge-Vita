// Temporary diagnostics generated from the pinned vitaGL public declarations.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <inttypes.h>

typedef struct OyProfileCounter {
    uint64_t us, maximum, bytes;
    unsigned calls, null_data;
} OyProfileCounter;
static unsigned oy_profile_requested;
static unsigned oy_attrib_large_offsets, oy_attrib_max_offset;
static unsigned oy_profile_active, oy_profile_complete, oy_profile_number, oy_profile_render_id;
static uint64_t oy_profile_interval, oy_profile_last_log_us;
static uint64_t oy_profile_swap_us, oy_profile_queue_us, oy_profile_cleanup_us;
static uint64_t oy_profile_shader_creates, oy_profile_shader_compiles, oy_profile_program_links;
static uint64_t oy_profile_finish_us;
static unsigned oy_profile_finish_calls;
static SceUID oy_profile_owner;
static GLenum oy_profile_slowest_integer_query;
static uint64_t oy_profile_slowest_integer_us;
static uint64_t oy_profile_now(void) { return sceKernelGetProcessTimeWide(); }
#include "bridge-profile-output.c"
#include "bridge-profile-extra.c"
void oy_profile_event(uint32_t event, uint64_t value)
{
    if (event == 11) { oy_profile_lto = (unsigned)value; return; }
    if (event == 10) { *(uint32_t **)(uintptr_t)value = &oy_guest_alloc_total; return; }
    if (event == 9) {
        uint32_t *ready=(uint32_t *)(uintptr_t)value;
        *ready=0;
        if (__atomic_load_n(&oy_capture_reset_ack,__ATOMIC_ACQUIRE)) {
            oy_api_finished=0;
            oy_guest_alloc_calls=oy_guest_alloc_failures=0; oy_guest_alloc_us=oy_guest_alloc_bytes=0;
            oy_api_gl_calls=0; oy_api_gl_us=0;
            memset(oy_recovery_count,0,sizeof(oy_recovery_count));
            memset(oy_recovery_attempts,0,sizeof(oy_recovery_attempts));
            memset(oy_recovery_failed,0,sizeof(oy_recovery_failed));
            memset(oy_recovery_us,0,sizeof(oy_recovery_us));
            memset(oy_recovery_dropped,0,sizeof(oy_recovery_dropped));
            *ready=1;
        } else if (__atomic_load_n(&oy_capture_flushed,__ATOMIC_ACQUIRE)) {
            __atomic_store_n(&oy_capture_reset_requested,1,__ATOMIC_RELEASE);
        }
    }
    if (event == 1) __atomic_store_n(&oy_profile_requested, (unsigned)value, __ATOMIC_RELEASE);
    if (event == 2) {
        static int reported;
        oy_profile_render_id = (unsigned)value;
        if (!reported) { oy_profile_fprintf(stderr,"PROFILE THREAD_IDS api=%d render=%d\n",
            oy_api_thread,sceKernelGetThreadId()); reported=1; }
    }
    if (event == 3) oy_api_thread=sceKernelGetThreadId();
    if (event == 4) {
        oy_profile_fprintf(stderr,"PROFILE GUEST_ALLOC n=%u calls=%u bytes=%llu ms=%.3f failures=%u\n",
            (unsigned)value,oy_guest_alloc_calls,(unsigned long long)oy_guest_alloc_bytes,
            oy_guest_alloc_us/1000.0,oy_guest_alloc_failures);
        oy_guest_alloc_calls=oy_guest_alloc_failures=0; oy_guest_alloc_us=oy_guest_alloc_bytes=0;
        oy_profile_fprintf(stderr,"PROFILE API_GL n=%u calls=%u ms=%.3f\n",
            (unsigned)value,oy_api_gl_calls,oy_api_gl_us/1000.0);
        oy_api_gl_calls=0; oy_api_gl_us=0;
        oy_profile_report_recovery(0,(unsigned)value);
        oy_profile_heap((unsigned)value);
        if (value>=20) oy_api_finished=1;
    }
    if (event == 5) {
        struct Packet { const char *data; uint32_t size; };
        const struct Packet *packet=(const void *)(uintptr_t)value;
        oy_profile_append(packet->data,packet->size);
    }
    if (event == 7) __atomic_store_n(&oy_render_cycle_done,1,__ATOMIC_RELEASE);
    if (event == 8) oy_profile_try_flush(oy_api_finished && __atomic_load_n(&oy_profile_complete,__ATOMIC_ACQUIRE));
    if (event == 6) oy_context[1].transient=(unsigned)value;
}
static void oy_profile_start(void)
{
    if (sceKernelGetThreadId() == oy_api_thread) return;
    if (!oy_profile_active && !oy_profile_complete && __atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE))
    {
        oy_profile_active = 1;
        oy_profile_interval = oy_profile_now();
        oy_profile_owner = sceKernelGetThreadId();
    }
}
static void oy_profile_record(OyProfileCounter *c, uint64_t begin, uint64_t bytes, unsigned null_data)
{
    uint64_t elapsed = oy_profile_now() - begin;
    if (sceKernelGetThreadId()==oy_api_thread) {
        if (!oy_api_finished && __atomic_load_n(&oy_profile_requested,__ATOMIC_ACQUIRE)) {
            oy_api_gl_us+=elapsed; ++oy_api_gl_calls;
        }
        return;
    }
    if (!oy_profile_active) return;
    c->us += elapsed;
    if (elapsed > c->maximum) c->maximum = elapsed;
    c->bytes += bytes;
    c->null_data += null_data;
    ++c->calls;
}
void oy_profile_internal(unsigned kind, uint64_t elapsed)
{
    (void)kind;
    if (oy_profile_active) { oy_profile_finish_us += elapsed; ++oy_profile_finish_calls; }
}
void oy_profile_native_swap(uint64_t total, uint64_t queue, uint64_t cleanup)
{
    oy_profile_swap_us = total; oy_profile_queue_us = queue; oy_profile_cleanup_us = cleanup;
}
static uint64_t oy_profile_pixels(GLsizei width, GLsizei height, GLenum format, GLenum type)
{
    if (width < 0 || height < 0) return 0;
    unsigned components = format == GL_RGB ? 3 : format == GL_RGBA || format == GL_BGRA ? 4 :
        format == GL_LUMINANCE_ALPHA ? 2 : 1;
    unsigned bytes = type == GL_UNSIGNED_BYTE ? components : type == GL_FLOAT ? components * 4 : 2;
    return (uint64_t)width * height * bytes;
}
static void oy_profile_shader_source(GLuint id, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    uint32_t hash = 2166136261u; uint64_t bytes = 0;
    char version[96] = "implicit (inspect packaged shader profile)";
    for (GLsizei i = 0; i < count; ++i)
    {
        size_t size = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        bytes += size;
        for (size_t j = 0; j < size; ++j)
        {
            hash = (hash ^ (unsigned char)strings[i][j]) * 16777619u;
            if (j + 8 <= size && memcmp(strings[i] + j, "#version", 8) == 0)
            {
                size_t n = 0;
                while (j+n < size && strings[i][j+n] != '\n' && n < sizeof(version)-1)
                { version[n] = strings[i][j+n]; ++n; }
                version[n] = 0;
            }
        }
    }
    oy_profile_fprintf(stderr, "PROFILE SHADER_SOURCE id=%u render=%u bytes=%llu fnv=%08x version=%s\n",
        id, oy_profile_render_id, (unsigned long long)bytes, hash, version);
}

static void (*oy_raw_glUniform1fv)(GLint location, GLsizei count, const GLfloat *value);
static OyProfileCounter oy_stat_glUniform1fv;
static void oy_trace_glUniform1fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniform1fv(location, count, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniform1fv(location, count, value);
    oy_profile_record(&oy_stat_glUniform1fv, begin, 0, 0);
}

static void (*oy_raw_glUniform2fv)(GLint location, GLsizei count, const GLfloat *value);
static OyProfileCounter oy_stat_glUniform2fv;
static void oy_trace_glUniform2fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniform2fv(location, count, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniform2fv(location, count, value);
    oy_profile_record(&oy_stat_glUniform2fv, begin, 0, 0);
}

static void (*oy_raw_glUniform3fv)(GLint location, GLsizei count, const GLfloat *value);
static OyProfileCounter oy_stat_glUniform3fv;
static void oy_trace_glUniform3fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniform3fv(location, count, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniform3fv(location, count, value);
    oy_profile_record(&oy_stat_glUniform3fv, begin, 0, 0);
}

static void (*oy_raw_glUniform4fv)(GLint location, GLsizei count, const GLfloat *value);
static OyProfileCounter oy_stat_glUniform4fv;
static void oy_trace_glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniform4fv(location, count, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniform4fv(location, count, value);
    oy_profile_record(&oy_stat_glUniform4fv, begin, 0, 0);
}

static void (*oy_raw_glUniform1iv)(GLint location, GLsizei count, const GLint *value);
static OyProfileCounter oy_stat_glUniform1iv;
static void oy_trace_glUniform1iv(GLint location, GLsizei count, const GLint *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniform1iv(location, count, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniform1iv(location, count, value);
    oy_profile_record(&oy_stat_glUniform1iv, begin, 0, 0);
}

static void (*oy_raw_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
static OyProfileCounter oy_stat_glUniformMatrix4fv;
static void oy_trace_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniformMatrix4fv(location, count, transpose, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniformMatrix4fv(location, count, transpose, value);
    oy_profile_record(&oy_stat_glUniformMatrix4fv, begin, 0, 0);
}

static void (*oy_raw_glUniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
static OyProfileCounter oy_stat_glUniformMatrix3fv;
static void oy_trace_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUniformMatrix3fv(location, count, transpose, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUniformMatrix3fv(location, count, transpose, value);
    oy_profile_record(&oy_stat_glUniformMatrix3fv, begin, 0, 0);
}

static void (*oy_raw_glUseProgram)(GLuint program);
static OyProfileCounter oy_stat_glUseProgram;
static void oy_trace_glUseProgram(GLuint program)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glUseProgram(program); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glUseProgram(program);
    oy_profile_record(&oy_stat_glUseProgram, begin, 0, 0);
}

static void (*oy_raw_glActiveTexture)(GLenum texture);
static OyProfileCounter oy_stat_glActiveTexture;
static void oy_trace_glActiveTexture(GLenum texture)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glActiveTexture(texture); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glActiveTexture(texture);
    oy_profile_record(&oy_stat_glActiveTexture, begin, 0, 0);
}

static void (*oy_raw_glBindTexture)(GLenum target, GLuint texture);
static OyProfileCounter oy_stat_glBindTexture;
static void oy_trace_glBindTexture(GLenum target, GLuint texture)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glBindTexture(target, texture); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glBindTexture(target, texture);
    oy_profile_record(&oy_stat_glBindTexture, begin, 0, 0);
}

static void (*oy_raw_glTexParameteri)(GLenum target, GLenum pname, GLint value);
static OyProfileCounter oy_stat_glTexParameteri;
static void oy_trace_glTexParameteri(GLenum target, GLenum pname, GLint value)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glTexParameteri(target, pname, value); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glTexParameteri(target, pname, value);
    oy_profile_record(&oy_stat_glTexParameteri, begin, 0, 0);
}

static void (*oy_raw_glEnableVertexAttribArray)(GLuint index);
static OyProfileCounter oy_stat_glEnableVertexAttribArray;
static void oy_trace_glEnableVertexAttribArray(GLuint index)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glEnableVertexAttribArray(index); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glEnableVertexAttribArray(index);
    oy_profile_record(&oy_stat_glEnableVertexAttribArray, begin, 0, 0);
}

static void (*oy_raw_glDisableVertexAttribArray)(GLuint index);
static OyProfileCounter oy_stat_glDisableVertexAttribArray;
static void oy_trace_glDisableVertexAttribArray(GLuint index)
{
    if (!oy_profile_active && (oy_profile_complete
        || !__atomic_load_n(&oy_profile_requested, __ATOMIC_ACQUIRE)))
    { oy_raw_glDisableVertexAttribArray(index); return; }
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    oy_raw_glDisableVertexAttribArray(index);
    oy_profile_record(&oy_stat_glDisableVertexAttribArray, begin, 0, 0);
}

static void (*oy_raw_glBufferData)(GLenum target, GLsizei size, const GLvoid *data, GLenum usage);
static OyProfileCounter oy_stat_glBufferData;
static void oy_trace_glBufferData(GLenum target, GLsizei size, const GLvoid *data, GLenum usage)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glBufferData";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    oy_raw_glBufferData(target, size, data, usage);
    oy_profile_record(&oy_stat_glBufferData, begin, size > 0 ? (uint64_t)size : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
static OyProfileCounter oy_stat_glBufferSubData;
static void oy_trace_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glBufferSubData";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    oy_raw_glBufferSubData(target, offset, size, data);
    oy_profile_record(&oy_stat_glBufferSubData, begin, size > 0 ? (uint64_t)size : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static GLenum (*oy_raw_glCheckFramebufferStatus)(GLenum target);
static OyProfileCounter oy_stat_glCheckFramebufferStatus;
static GLenum oy_trace_glCheckFramebufferStatus(GLenum target)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCheckFramebufferStatus";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    uint64_t begin = oy_profile_now();
    GLenum result = oy_raw_glCheckFramebufferStatus(target);
    oy_profile_record(&oy_stat_glCheckFramebufferStatus, begin, 0, 0);
    if (result != GL_FRAMEBUFFER_COMPLETE) oy_profile_fprintf(stderr, "PROFILE FBO_CHECK status=%x render=%u (format probe may be optional)\n", result, oy_profile_render_id);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glCompileShader)(GLuint shader);
static OyProfileCounter oy_stat_glCompileShader;
static void oy_trace_glCompileShader(GLuint shader)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCompileShader";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glCompileShader(shader);
    oy_profile_record(&oy_stat_glCompileShader, begin, 0, 0);
    ++oy_profile_shader_compiles;
    GLint status = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    oy_profile_fprintf(stderr, "PROFILE COMPILE id=%u render=%u ms=%.3f status=%d postponed_binding=1\n", shader, oy_profile_render_id, (oy_profile_now()-begin)/1000.0, status);
    if (!status) { char info[1024] = {0}; glGetShaderInfoLog(shader, sizeof(info), NULL, info); oy_profile_fprintf(stderr, "PROFILE SHADER_INFO %s\n", info); }
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glCompressedTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
static OyProfileCounter oy_stat_glCompressedTexImage2D;
static void oy_trace_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCompressedTexImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    uint64_t begin = oy_profile_now();
    oy_raw_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    oy_profile_record(&oy_stat_glCompressedTexImage2D, begin, imageSize > 0 ? (uint64_t)imageSize : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glCompressedTextureImage2D)(GLuint texture, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
static OyProfileCounter oy_stat_glCompressedTextureImage2D;
static void oy_trace_glCompressedTextureImage2D(GLuint texture, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCompressedTextureImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glCompressedTextureImage2D(texture, level, internalFormat, width, height, border, imageSize, data);
    oy_profile_record(&oy_stat_glCompressedTextureImage2D, begin, imageSize > 0 ? (uint64_t)imageSize : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static GLuint (*oy_raw_glCreateProgram)(void);
static OyProfileCounter oy_stat_glCreateProgram;
static GLuint oy_trace_glCreateProgram(void)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCreateProgram";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLuint result = oy_raw_glCreateProgram();
    oy_profile_record(&oy_stat_glCreateProgram, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static GLuint (*oy_raw_glCreateShader)(GLenum shaderType);
static OyProfileCounter oy_stat_glCreateShader;
static GLuint oy_trace_glCreateShader(GLenum shaderType)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glCreateShader";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLuint result = oy_raw_glCreateShader(shaderType);
    oy_profile_record(&oy_stat_glCreateShader, begin, 0, 0);
    ++oy_profile_shader_creates;
    oy_profile_fprintf(stderr, "PROFILE SHADER_CREATE id=%u type=%x render=%u\n", result, shaderType, oy_profile_render_id);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
static OyProfileCounter oy_stat_glDrawArrays;
static void oy_trace_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glDrawArrays";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glDrawArrays(mode, first, count);
    oy_profile_record(&oy_stat_glDrawArrays, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
static OyProfileCounter oy_stat_glDrawElements;
static void oy_trace_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glDrawElements";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glDrawElements(mode, count, type, indices);
    oy_profile_record(&oy_stat_glDrawElements, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glFinish)(void);
static OyProfileCounter oy_stat_glFinish;
static void oy_trace_glFinish(void)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glFinish";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glFinish();
    oy_profile_record(&oy_stat_glFinish, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glFlush)(void);
static OyProfileCounter oy_stat_glFlush;
static void oy_trace_glFlush(void)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glFlush";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glFlush();
    oy_profile_record(&oy_stat_glFlush, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetActiveAttrib)(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
static OyProfileCounter oy_stat_glGetActiveAttrib;
static void oy_trace_glGetActiveAttrib(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetActiveAttrib";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetActiveAttrib(prog, index, bufSize, length, size, type, name);
    oy_profile_record(&oy_stat_glGetActiveAttrib, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetActiveUniform)(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
static OyProfileCounter oy_stat_glGetActiveUniform;
static void oy_trace_glGetActiveUniform(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetActiveUniform";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetActiveUniform(prog, index, bufSize, length, size, type, name);
    oy_profile_record(&oy_stat_glGetActiveUniform, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetAttachedShaders)(GLuint prog, GLsizei maxCount, GLsizei *count, GLuint *shads);
static OyProfileCounter oy_stat_glGetAttachedShaders;
static void oy_trace_glGetAttachedShaders(GLuint prog, GLsizei maxCount, GLsizei *count, GLuint *shads)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetAttachedShaders";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetAttachedShaders(prog, maxCount, count, shads);
    oy_profile_record(&oy_stat_glGetAttachedShaders, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static GLint (*oy_raw_glGetAttribLocation)(GLuint prog, const GLchar *name);
static OyProfileCounter oy_stat_glGetAttribLocation;
static GLint oy_trace_glGetAttribLocation(GLuint prog, const GLchar *name)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetAttribLocation";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLint result = oy_raw_glGetAttribLocation(prog, name);
    oy_profile_record(&oy_stat_glGetAttribLocation, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glGetBooleanv)(GLenum pname, GLboolean *params);
static OyProfileCounter oy_stat_glGetBooleanv;
static void oy_trace_glGetBooleanv(GLenum pname, GLboolean *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetBooleanv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetBooleanv(pname, params);
    oy_profile_record(&oy_stat_glGetBooleanv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetBufferParameteriv)(GLenum target, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetBufferParameteriv;
static void oy_trace_glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetBufferParameteriv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    oy_raw_glGetBufferParameteriv(target, pname, params);
    oy_profile_record(&oy_stat_glGetBufferParameteriv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetDoublev)(GLenum pname, GLdouble *data);
static OyProfileCounter oy_stat_glGetDoublev;
static void oy_trace_glGetDoublev(GLenum pname, GLdouble *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetDoublev";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetDoublev(pname, data);
    oy_profile_record(&oy_stat_glGetDoublev, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static GLenum (*oy_raw_glGetError)(void);
static OyProfileCounter oy_stat_glGetError;
static GLenum oy_trace_glGetError(void)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetError";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLenum result = oy_raw_glGetError();
    oy_profile_record(&oy_stat_glGetError, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glGetFloatv)(GLenum pname, GLfloat *data);
static OyProfileCounter oy_stat_glGetFloatv;
static void oy_trace_glGetFloatv(GLenum pname, GLfloat *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetFloatv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetFloatv(pname, data);
    oy_profile_record(&oy_stat_glGetFloatv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetFramebufferAttachmentParameteriv)(GLenum target, GLenum attachment, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetFramebufferAttachmentParameteriv;
static void oy_trace_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetFramebufferAttachmentParameteriv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
    oy_profile_record(&oy_stat_glGetFramebufferAttachmentParameteriv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetIntegerv)(GLenum pname, GLint *data);
static OyProfileCounter oy_stat_glGetIntegerv;
static void oy_trace_glGetIntegerv(GLenum pname, GLint *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetIntegerv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetIntegerv(pname, data);
    uint64_t elapsed = oy_profile_now() - begin;
    if (oy_profile_active && elapsed >= oy_profile_slowest_integer_us)
    { oy_profile_slowest_integer_us = elapsed; oy_profile_slowest_integer_query = pname; }
    oy_profile_record(&oy_stat_glGetIntegerv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetNamedBufferParameteriv)(GLuint buffer, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetNamedBufferParameteriv;
static void oy_trace_glGetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetNamedBufferParameteriv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetNamedBufferParameteriv(buffer, pname, params);
    oy_profile_record(&oy_stat_glGetNamedBufferParameteriv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetPointerv)(GLenum pname, void **params);
static OyProfileCounter oy_stat_glGetPointerv;
static void oy_trace_glGetPointerv(GLenum pname, void **params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetPointerv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetPointerv(pname, params);
    oy_profile_record(&oy_stat_glGetPointerv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetProgramBinary)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary);
static OyProfileCounter oy_stat_glGetProgramBinary;
static void oy_trace_glGetProgramBinary(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetProgramBinary";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetProgramBinary(program, bufSize, length, binaryFormat, binary);
    oy_profile_record(&oy_stat_glGetProgramBinary, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetProgramInfoLog)(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
static OyProfileCounter oy_stat_glGetProgramInfoLog;
static void oy_trace_glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetProgramInfoLog";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetProgramInfoLog(program, maxLength, length, infoLog);
    oy_profile_record(&oy_stat_glGetProgramInfoLog, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetProgramiv)(GLuint program, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetProgramiv;
static void oy_trace_glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetProgramiv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetProgramiv(program, pname, params);
    oy_profile_record(&oy_stat_glGetProgramiv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetQueryObjectiv)(GLuint id, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetQueryObjectiv;
static void oy_trace_glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetQueryObjectiv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetQueryObjectiv(id, pname, params);
    oy_profile_record(&oy_stat_glGetQueryObjectiv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetQueryObjectuiv)(GLuint id, GLenum pname, GLuint *params);
static OyProfileCounter oy_stat_glGetQueryObjectuiv;
static void oy_trace_glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetQueryObjectuiv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetQueryObjectuiv(id, pname, params);
    oy_profile_record(&oy_stat_glGetQueryObjectuiv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetShaderInfoLog)(GLuint handle, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
static OyProfileCounter oy_stat_glGetShaderInfoLog;
static void oy_trace_glGetShaderInfoLog(GLuint handle, GLsizei maxLength, GLsizei *length, GLchar *infoLog)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetShaderInfoLog";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetShaderInfoLog(handle, maxLength, length, infoLog);
    oy_profile_record(&oy_stat_glGetShaderInfoLog, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetShaderiv)(GLuint handle, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetShaderiv;
static void oy_trace_glGetShaderiv(GLuint handle, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetShaderiv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetShaderiv(handle, pname, params);
    oy_profile_record(&oy_stat_glGetShaderiv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetShaderSource)(GLuint handle, GLsizei bufSize, GLsizei *length, GLchar *source);
static OyProfileCounter oy_stat_glGetShaderSource;
static void oy_trace_glGetShaderSource(GLuint handle, GLsizei bufSize, GLsizei *length, GLchar *source)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetShaderSource";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetShaderSource(handle, bufSize, length, source);
    oy_profile_record(&oy_stat_glGetShaderSource, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static const GLubyte * (*oy_raw_glGetString)(GLenum name);
static OyProfileCounter oy_stat_glGetString;
static const GLubyte * oy_trace_glGetString(GLenum name)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetString";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    const GLubyte * result = oy_raw_glGetString(name);
    oy_profile_record(&oy_stat_glGetString, begin, 0, 0);
    if (name != GL_EXTENSIONS && result != NULL) oy_profile_fprintf(stderr, "PROFILE GL_STRING key=%x value=%s\n", name, result);
    oy_context[slot]=savedContext;
    return result;
}

static const GLubyte * (*oy_raw_glGetStringi)(GLenum name, GLuint index);
static OyProfileCounter oy_stat_glGetStringi;
static const GLubyte * oy_trace_glGetStringi(GLenum name, GLuint index)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetStringi";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    const GLubyte * result = oy_raw_glGetStringi(name, index);
    oy_profile_record(&oy_stat_glGetStringi, begin, 0, 0);
    if (name != GL_EXTENSIONS && result != NULL) oy_profile_fprintf(stderr, "PROFILE GL_STRING key=%x value=%s\n", name, result);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glGetTexEnviv)(GLenum target, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetTexEnviv;
static void oy_trace_glGetTexEnviv(GLenum target, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetTexEnviv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetTexEnviv(target, pname, params);
    oy_profile_record(&oy_stat_glGetTexEnviv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static GLuint (*oy_raw_glGetUniformBlockIndex)(GLuint prog, const GLchar *uniformBlockName);
static OyProfileCounter oy_stat_glGetUniformBlockIndex;
static GLuint oy_trace_glGetUniformBlockIndex(GLuint prog, const GLchar *uniformBlockName)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetUniformBlockIndex";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLuint result = oy_raw_glGetUniformBlockIndex(prog, uniformBlockName);
    oy_profile_record(&oy_stat_glGetUniformBlockIndex, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static GLint (*oy_raw_glGetUniformLocation)(GLuint prog, const GLchar *name);
static OyProfileCounter oy_stat_glGetUniformLocation;
static GLint oy_trace_glGetUniformLocation(GLuint prog, const GLchar *name)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetUniformLocation";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    GLint result = oy_raw_glGetUniformLocation(prog, name);
    oy_profile_record(&oy_stat_glGetUniformLocation, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glGetVertexAttribfv)(GLuint index, GLenum pname, GLfloat *params);
static OyProfileCounter oy_stat_glGetVertexAttribfv;
static void oy_trace_glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetVertexAttribfv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetVertexAttribfv(index, pname, params);
    oy_profile_record(&oy_stat_glGetVertexAttribfv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetVertexAttribiv)(GLuint index, GLenum pname, GLint *params);
static OyProfileCounter oy_stat_glGetVertexAttribiv;
static void oy_trace_glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetVertexAttribiv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetVertexAttribiv(index, pname, params);
    oy_profile_record(&oy_stat_glGetVertexAttribiv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGetVertexAttribPointerv)(GLuint index, GLenum pname, void **pointer);
static OyProfileCounter oy_stat_glGetVertexAttribPointerv;
static void oy_trace_glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGetVertexAttribPointerv";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGetVertexAttribPointerv(index, pname, pointer);
    oy_profile_record(&oy_stat_glGetVertexAttribPointerv, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glLinkProgram)(GLuint progr);
static OyProfileCounter oy_stat_glLinkProgram;
static void oy_trace_glLinkProgram(GLuint progr)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glLinkProgram";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glLinkProgram(progr);
    oy_profile_record(&oy_stat_glLinkProgram, begin, 0, 0);
    ++oy_profile_program_links;
    GLint status = 0; glGetProgramiv(progr, GL_LINK_STATUS, &status);
    oy_profile_fprintf(stderr, "PROFILE LINK id=%u render=%u ms=%.3f status=%d postponed_binding=1\n", progr, oy_profile_render_id, (oy_profile_now()-begin)/1000.0, status);
    if (!status) { char info[1024] = {0}; glGetProgramInfoLog(progr, sizeof(info), NULL, info); oy_profile_fprintf(stderr, "PROFILE SHADER_INFO %s\n", info); }
    oy_context[slot]=savedContext;
}

static void * (*oy_raw_glMapBuffer)(GLenum target, GLbitfield access);
static OyProfileCounter oy_stat_glMapBuffer;
static void * oy_trace_glMapBuffer(GLenum target, GLbitfield access)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glMapBuffer";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    void * result = oy_raw_glMapBuffer(target, access);
    oy_profile_record(&oy_stat_glMapBuffer, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void * (*oy_raw_glMapBufferRange)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
static OyProfileCounter oy_stat_glMapBufferRange;
static void * oy_trace_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glMapBufferRange";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    void * result = oy_raw_glMapBufferRange(target, offset, length, access);
    oy_profile_record(&oy_stat_glMapBufferRange, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void * (*oy_raw_glMapNamedBuffer)(GLuint buffer, GLenum access);
static OyProfileCounter oy_stat_glMapNamedBuffer;
static void * oy_trace_glMapNamedBuffer(GLuint buffer, GLenum access)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glMapNamedBuffer";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    void * result = oy_raw_glMapNamedBuffer(buffer, access);
    oy_profile_record(&oy_stat_glMapNamedBuffer, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void * (*oy_raw_glMapNamedBufferRange)(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
static OyProfileCounter oy_stat_glMapNamedBufferRange;
static void * oy_trace_glMapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glMapNamedBufferRange";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    void * result = oy_raw_glMapNamedBufferRange(buffer, offset, length, access);
    oy_profile_record(&oy_stat_glMapNamedBufferRange, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static void (*oy_raw_glNamedBufferData)(GLuint buffer, GLsizei size, const void *data, GLenum usage);
static OyProfileCounter oy_stat_glNamedBufferData;
static void oy_trace_glNamedBufferData(GLuint buffer, GLsizei size, const void *data, GLenum usage)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glNamedBufferData";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    oy_raw_glNamedBufferData(buffer, size, data, usage);
    oy_profile_record(&oy_stat_glNamedBufferData, begin, size > 0 ? (uint64_t)size : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glNamedBufferSubData)(GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data);
static OyProfileCounter oy_stat_glNamedBufferSubData;
static void oy_trace_glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glNamedBufferSubData";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    oy_raw_glNamedBufferSubData(buffer, offset, size, data);
    oy_profile_record(&oy_stat_glNamedBufferSubData, begin, size > 0 ? (uint64_t)size : 0, data == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glProgramBinary)(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length);
static OyProfileCounter oy_stat_glProgramBinary;
static void oy_trace_glProgramBinary(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glProgramBinary";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glProgramBinary(program, binaryFormat, binary, length);
    oy_profile_record(&oy_stat_glProgramBinary, begin, length > 0 ? (uint64_t)length : 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *data);
static OyProfileCounter oy_stat_glReadPixels;
static void oy_trace_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glReadPixels";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glReadPixels(x, y, width, height, format, type, data);
    oy_profile_record(&oy_stat_glReadPixels, begin, 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glShaderBinary)(GLsizei count, const GLuint *handles, GLenum binaryFormat, const void *binary, GLsizei length);
static OyProfileCounter oy_stat_glShaderBinary;
static void oy_trace_glShaderBinary(GLsizei count, const GLuint *handles, GLenum binaryFormat, const void *binary, GLsizei length)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glShaderBinary";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glShaderBinary(count, handles, binaryFormat, binary, length);
    oy_profile_record(&oy_stat_glShaderBinary, begin, length > 0 ? (uint64_t)length : 0, 0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glShaderSource)(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length);
static OyProfileCounter oy_stat_glShaderSource;
static void oy_trace_glShaderSource(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glShaderSource";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glShaderSource(handle, count, string, length);
    oy_profile_record(&oy_stat_glShaderSource, begin, 0, 0);
    oy_profile_shader_source(handle, count, string, length);
    oy_context[slot]=savedContext;
}

static void oy_profile_upload_bytes(unsigned object, GLint level, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *data)
{
    static unsigned checked;
    if (checked >= 128 || level != 0 || width != 256 || height != 256 || !data ||
        format != GL_RGBA || type != GL_UNSIGNED_BYTE) return;
    ++checked;
    const uint8_t *pixels=data;
    uint32_t hash=2166136261u;
    unsigned alpha_zero=0, cyan_zero=0;
    for (unsigned i=0;i<256u*256u*4u;++i) hash=(hash^pixels[i])*16777619u;
    for (unsigned i=0;i<256u*256u*4u;i+=4) {
        alpha_zero+=pixels[i+3]==0;
        cyan_zero+=pixels[i+3]==0 && pixels[i]<=8 && pixels[i+1]>=248 && pixels[i+2]>=248;
    }
    oy_profile_fprintf(stderr,"PROFILE UPLOAD_BYTES object=%u w=256 h=256 format=RGBA8 "
        "alpha_zero=%u zero_alpha_cyan=%u rgba_fnv=%08x\n",object,alpha_zero,cyan_zero,hash);
}
static void (*oy_raw_glTexImage2D)(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *data);
static OyProfileCounter oy_stat_glTexImage2D;
static void oy_trace_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *data)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glTexImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_profile_upload_bytes(oy_context[slot].object, level, width, height, format, type, data);
    uint64_t begin = oy_profile_now();
    oy_raw_glTexImage2D(target, level, internalFormat, width, height, border, format, type, data);
    oy_profile_record(&oy_stat_glTexImage2D, begin, oy_profile_pixels(width, height, format, type), data == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
static OyProfileCounter oy_stat_glTexSubImage2D;
static void oy_trace_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glTexSubImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    uint64_t begin = oy_profile_now();
    GLenum preceding = oy_profile_active ? oy_glGetError() : GL_NO_ERROR;
    oy_raw_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    if (oy_profile_active) oy_profile_texture_error(preceding,oy_glGetError(),format,type);
    oy_profile_record(&oy_stat_glTexSubImage2D, begin, oy_profile_pixels(width, height, format, type), pixels == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glTextureImage2D)(GLuint texture, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
static OyProfileCounter oy_stat_glTextureImage2D;
static void oy_trace_glTextureImage2D(GLuint texture, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glTextureImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_profile_upload_bytes(texture, level, width, height, format, type, pixels);
    uint64_t begin = oy_profile_now();
    oy_raw_glTextureImage2D(texture, level, internalformat, width, height, border, format, type, pixels);
    oy_profile_record(&oy_stat_glTextureImage2D, begin, oy_profile_pixels(width, height, format, type), pixels == NULL);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glTextureSubImage2D)(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
static OyProfileCounter oy_stat_glTextureSubImage2D;
static void oy_trace_glTextureSubImage2D(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glTextureSubImage2D";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glTextureSubImage2D(texture, level, xoffset, yoffset, width, height, format, type, pixels);
    oy_profile_record(&oy_stat_glTextureSubImage2D, begin, oy_profile_pixels(width, height, format, type), pixels == NULL);
    oy_context[slot]=savedContext;
}

static GLboolean (*oy_raw_glUnmapBuffer)(GLenum target);
static OyProfileCounter oy_stat_glUnmapBuffer;
static GLboolean oy_trace_glUnmapBuffer(GLenum target)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glUnmapBuffer";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    uint64_t begin = oy_profile_now();
    GLboolean result = oy_raw_glUnmapBuffer(target);
    oy_profile_record(&oy_stat_glUnmapBuffer, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}

static GLboolean (*oy_raw_glUnmapNamedBuffer)(GLuint buffer);
static OyProfileCounter oy_stat_glUnmapNamedBuffer;
static GLboolean oy_trace_glUnmapNamedBuffer(GLuint buffer)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glUnmapNamedBuffer";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    GLboolean result = oy_raw_glUnmapNamedBuffer(buffer);
    oy_profile_record(&oy_stat_glUnmapNamedBuffer, begin, 0, 0);
    oy_context[slot]=savedContext;
    return result;
}


static void (*oy_raw_glBindBuffer)(GLenum target, GLuint buffer);
static OyProfileCounter oy_stat_glBindBuffer;
static void oy_trace_glBindBuffer(GLenum target, GLuint buffer)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glBindBuffer";
    oy_context[slot].target=0; oy_context[slot].object=0;
    oy_context[slot].target=target;
    oy_context[slot].object=oy_bound_buffers[slot][target==GL_ELEMENT_ARRAY_BUFFER];
    oy_context[slot].object=buffer;
    uint64_t begin = oy_profile_now();
    oy_raw_glBindBuffer(target, buffer);
    if (target==GL_ARRAY_BUFFER || target==GL_ELEMENT_ARRAY_BUFFER)
        oy_bound_buffers[oy_profile_slot()][target==GL_ELEMENT_ARRAY_BUFFER]=buffer;
    oy_profile_record(&oy_stat_glBindBuffer,begin,0,0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGenBuffers)(GLsizei n, GLuint *buffers);
static OyProfileCounter oy_stat_glGenBuffers;
static void oy_trace_glGenBuffers(GLsizei n, GLuint *buffers)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGenBuffers";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGenBuffers(n, buffers);
    if (oy_profile_active && oy_profile_slot()) oy_generated_buffers+=n;
    oy_profile_record(&oy_stat_glGenBuffers,begin,0,0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glDeleteBuffers)(GLsizei n, const GLuint *gl_buffers);
static OyProfileCounter oy_stat_glDeleteBuffers;
static void oy_trace_glDeleteBuffers(GLsizei n, const GLuint *gl_buffers)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glDeleteBuffers";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glDeleteBuffers(n, gl_buffers);
    if (oy_profile_active && oy_profile_slot()) oy_deleted_buffers+=n;
    oy_profile_record(&oy_stat_glDeleteBuffers,begin,0,0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glGenTextures)(GLsizei n, GLuint *textures);
static OyProfileCounter oy_stat_glGenTextures;
static void oy_trace_glGenTextures(GLsizei n, GLuint *textures)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glGenTextures";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glGenTextures(n, textures);
    if (oy_profile_active && oy_profile_slot()) oy_generated_textures+=n;
    oy_profile_record(&oy_stat_glGenTextures,begin,0,0);
    oy_context[slot]=savedContext;
}

static void (*oy_raw_glDeleteTextures)(GLsizei n, const GLuint *textures);
static OyProfileCounter oy_stat_glDeleteTextures;
static void oy_trace_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    oy_profile_start();
    unsigned slot=oy_profile_slot();
    OyContext savedContext=oy_context[slot];
    oy_context[slot].operation="glDeleteTextures";
    oy_context[slot].target=0; oy_context[slot].object=0;
    uint64_t begin = oy_profile_now();
    oy_raw_glDeleteTextures(n, textures);
    if (oy_profile_active && oy_profile_slot()) oy_deleted_textures+=n;
    oy_profile_record(&oy_stat_glDeleteTextures,begin,0,0);
    oy_context[slot]=savedContext;
}
static void oy_profile_rearm_at_swap(void)
{
    if (!__atomic_load_n(&oy_capture_reset_requested,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&oy_capture_reset_ack,__ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&oy_profile_requested,0,__ATOMIC_RELEASE);
    oy_profile_active=0; oy_profile_complete=0; oy_profile_number=0;
    oy_profile_last_log_us=0; oy_profile_finish_us=0; oy_profile_finish_calls=0;
    oy_generated_buffers=oy_deleted_buffers=oy_generated_textures=oy_deleted_textures=0;
    oy_profile_slowest_integer_query=0; oy_profile_slowest_integer_us=0;
    memset(&oy_stat_glBufferData,0,sizeof(oy_stat_glBufferData));
    memset(&oy_stat_glBufferSubData,0,sizeof(oy_stat_glBufferSubData));
    memset(&oy_stat_glCheckFramebufferStatus,0,sizeof(oy_stat_glCheckFramebufferStatus));
    memset(&oy_stat_glCompileShader,0,sizeof(oy_stat_glCompileShader));
    memset(&oy_stat_glCompressedTexImage2D,0,sizeof(oy_stat_glCompressedTexImage2D));
    memset(&oy_stat_glCompressedTextureImage2D,0,sizeof(oy_stat_glCompressedTextureImage2D));
    memset(&oy_stat_glCreateProgram,0,sizeof(oy_stat_glCreateProgram));
    memset(&oy_stat_glCreateShader,0,sizeof(oy_stat_glCreateShader));
    memset(&oy_stat_glDrawArrays,0,sizeof(oy_stat_glDrawArrays));
    memset(&oy_stat_glDrawElements,0,sizeof(oy_stat_glDrawElements));
    memset(&oy_stat_glFinish,0,sizeof(oy_stat_glFinish));
    memset(&oy_stat_glFlush,0,sizeof(oy_stat_glFlush));
    memset(&oy_stat_glGetActiveAttrib,0,sizeof(oy_stat_glGetActiveAttrib));
    memset(&oy_stat_glGetActiveUniform,0,sizeof(oy_stat_glGetActiveUniform));
    memset(&oy_stat_glGetAttachedShaders,0,sizeof(oy_stat_glGetAttachedShaders));
    memset(&oy_stat_glGetAttribLocation,0,sizeof(oy_stat_glGetAttribLocation));
    memset(&oy_stat_glGetBooleanv,0,sizeof(oy_stat_glGetBooleanv));
    memset(&oy_stat_glGetBufferParameteriv,0,sizeof(oy_stat_glGetBufferParameteriv));
    memset(&oy_stat_glGetDoublev,0,sizeof(oy_stat_glGetDoublev));
    memset(&oy_stat_glGetError,0,sizeof(oy_stat_glGetError));
    memset(&oy_stat_glGetFloatv,0,sizeof(oy_stat_glGetFloatv));
    memset(&oy_stat_glGetFramebufferAttachmentParameteriv,0,sizeof(oy_stat_glGetFramebufferAttachmentParameteriv));
    memset(&oy_stat_glGetIntegerv,0,sizeof(oy_stat_glGetIntegerv));
    memset(&oy_stat_glGetNamedBufferParameteriv,0,sizeof(oy_stat_glGetNamedBufferParameteriv));
    memset(&oy_stat_glGetPointerv,0,sizeof(oy_stat_glGetPointerv));
    memset(&oy_stat_glGetProgramBinary,0,sizeof(oy_stat_glGetProgramBinary));
    memset(&oy_stat_glGetProgramInfoLog,0,sizeof(oy_stat_glGetProgramInfoLog));
    memset(&oy_stat_glGetProgramiv,0,sizeof(oy_stat_glGetProgramiv));
    memset(&oy_stat_glGetQueryObjectiv,0,sizeof(oy_stat_glGetQueryObjectiv));
    memset(&oy_stat_glGetQueryObjectuiv,0,sizeof(oy_stat_glGetQueryObjectuiv));
    memset(&oy_stat_glGetShaderInfoLog,0,sizeof(oy_stat_glGetShaderInfoLog));
    memset(&oy_stat_glGetShaderiv,0,sizeof(oy_stat_glGetShaderiv));
    memset(&oy_stat_glGetShaderSource,0,sizeof(oy_stat_glGetShaderSource));
    memset(&oy_stat_glGetString,0,sizeof(oy_stat_glGetString));
    memset(&oy_stat_glGetStringi,0,sizeof(oy_stat_glGetStringi));
    memset(&oy_stat_glGetTexEnviv,0,sizeof(oy_stat_glGetTexEnviv));
    memset(&oy_stat_glGetUniformBlockIndex,0,sizeof(oy_stat_glGetUniformBlockIndex));
    memset(&oy_stat_glGetUniformLocation,0,sizeof(oy_stat_glGetUniformLocation));
    memset(&oy_stat_glGetVertexAttribfv,0,sizeof(oy_stat_glGetVertexAttribfv));
    memset(&oy_stat_glGetVertexAttribiv,0,sizeof(oy_stat_glGetVertexAttribiv));
    memset(&oy_stat_glGetVertexAttribPointerv,0,sizeof(oy_stat_glGetVertexAttribPointerv));
    memset(&oy_stat_glLinkProgram,0,sizeof(oy_stat_glLinkProgram));
    memset(&oy_stat_glMapBuffer,0,sizeof(oy_stat_glMapBuffer));
    memset(&oy_stat_glMapBufferRange,0,sizeof(oy_stat_glMapBufferRange));
    memset(&oy_stat_glMapNamedBuffer,0,sizeof(oy_stat_glMapNamedBuffer));
    memset(&oy_stat_glMapNamedBufferRange,0,sizeof(oy_stat_glMapNamedBufferRange));
    memset(&oy_stat_glNamedBufferData,0,sizeof(oy_stat_glNamedBufferData));
    memset(&oy_stat_glNamedBufferSubData,0,sizeof(oy_stat_glNamedBufferSubData));
    memset(&oy_stat_glProgramBinary,0,sizeof(oy_stat_glProgramBinary));
    memset(&oy_stat_glReadPixels,0,sizeof(oy_stat_glReadPixels));
    memset(&oy_stat_glShaderBinary,0,sizeof(oy_stat_glShaderBinary));
    memset(&oy_stat_glShaderSource,0,sizeof(oy_stat_glShaderSource));
    memset(&oy_stat_glTexImage2D,0,sizeof(oy_stat_glTexImage2D));
    memset(&oy_stat_glTexSubImage2D,0,sizeof(oy_stat_glTexSubImage2D));
    memset(&oy_stat_glTextureImage2D,0,sizeof(oy_stat_glTextureImage2D));
    memset(&oy_stat_glTextureSubImage2D,0,sizeof(oy_stat_glTextureSubImage2D));
    memset(&oy_stat_glUnmapBuffer,0,sizeof(oy_stat_glUnmapBuffer));
    memset(&oy_stat_glUnmapNamedBuffer,0,sizeof(oy_stat_glUnmapNamedBuffer));
    memset(&oy_stat_glBindBuffer,0,sizeof(oy_stat_glBindBuffer));
    memset(&oy_stat_glGenBuffers,0,sizeof(oy_stat_glGenBuffers));
    memset(&oy_stat_glDeleteBuffers,0,sizeof(oy_stat_glDeleteBuffers));
    memset(&oy_stat_glGenTextures,0,sizeof(oy_stat_glGenTextures));
    memset(&oy_stat_glDeleteTextures,0,sizeof(oy_stat_glDeleteTextures));
    while (__atomic_exchange_n(&oy_capture_lock,1,__ATOMIC_ACQUIRE)) {}
    memset(&oy_stat_glUniform1fv, 0, sizeof(oy_stat_glUniform1fv));
    memset(&oy_stat_glUniform2fv, 0, sizeof(oy_stat_glUniform2fv));
    memset(&oy_stat_glUniform3fv, 0, sizeof(oy_stat_glUniform3fv));
    memset(&oy_stat_glUniform4fv, 0, sizeof(oy_stat_glUniform4fv));
    memset(&oy_stat_glUniform1iv, 0, sizeof(oy_stat_glUniform1iv));
    memset(&oy_stat_glUniformMatrix4fv, 0, sizeof(oy_stat_glUniformMatrix4fv));
    memset(&oy_stat_glUniformMatrix3fv, 0, sizeof(oy_stat_glUniformMatrix3fv));
    memset(&oy_stat_glUseProgram, 0, sizeof(oy_stat_glUseProgram));
    memset(&oy_stat_glActiveTexture, 0, sizeof(oy_stat_glActiveTexture));
    memset(&oy_stat_glBindTexture, 0, sizeof(oy_stat_glBindTexture));
    memset(&oy_stat_glTexParameteri, 0, sizeof(oy_stat_glTexParameteri));
    memset(&oy_stat_glEnableVertexAttribArray, 0, sizeof(oy_stat_glEnableVertexAttribArray));
    memset(&oy_stat_glDisableVertexAttribArray, 0, sizeof(oy_stat_glDisableVertexAttribArray));
    oy_capture_size=oy_capture_dropped=0; oy_capture_phase=2;
    oy_render_cycle_done=0;
    __atomic_store_n(&oy_capture_flushed,0,__ATOMIC_RELEASE);
    __atomic_store_n(&oy_capture_lock,0,__ATOMIC_RELEASE);
    __atomic_store_n(&oy_capture_reset_ack,1,__ATOMIC_RELEASE);
}
static EGLBoolean (*oy_raw_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean oy_trace_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    oy_profile_start();
    uint64_t begin = oy_profile_now();
    EGLBoolean result = oy_raw_eglSwapBuffers(display, surface);
    uint64_t finish = oy_profile_now();
    GLenum error = oy_glGetError(); // Actual GL thread, after its frame, never the API thread.
    if (oy_profile_active && error != GL_NO_ERROR) oy_profile_fprintf(stderr, "PROFILE GL_ERROR render=%u code=0x%x\n", oy_profile_render_id, error);
    oy_profile_memory_ring_sample();
    oy_profile_rearm_at_swap();
    if (!oy_profile_active) return result;
    ++oy_profile_number;
    static char text[16384];
    int size = snprintf(text, sizeof(text),
        "PROFILE GL n=%u warmup=%u render=%u thread=%d t_us=%llu interval_ms=%.3f swap_ms=%.3f "
        "native_swap_ms=%.3f queue_ms=%.3f cleanup_ms=%.3f internal_finish_calls=%u internal_finish_ms=%.3f "
        "shader_creates_total=%llu compiles_total=%llu links_total=%llu previous_log_ms=%.3f error=0x%x\n",
        oy_profile_number, 0u, oy_profile_render_id, oy_profile_owner,
        (unsigned long long)finish, (finish-oy_profile_interval)/1000.0, (finish-begin)/1000.0,
        oy_profile_swap_us/1000.0, oy_profile_queue_us/1000.0, oy_profile_cleanup_us/1000.0,
        oy_profile_finish_calls, oy_profile_finish_us/1000.0, (unsigned long long)oy_profile_shader_creates,
        (unsigned long long)oy_profile_shader_compiles, (unsigned long long)oy_profile_program_links,
        oy_profile_last_log_us/1000.0, error);
    if (oy_stat_glBufferData.calls) {
        OyProfileCounter *c = &oy_stat_glBufferData;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glBufferData count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glBufferData, 0, sizeof(oy_stat_glBufferData));
    if (oy_stat_glBufferSubData.calls) {
        OyProfileCounter *c = &oy_stat_glBufferSubData;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glBufferSubData count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glBufferSubData, 0, sizeof(oy_stat_glBufferSubData));
    if (oy_stat_glCheckFramebufferStatus.calls) {
        OyProfileCounter *c = &oy_stat_glCheckFramebufferStatus;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCheckFramebufferStatus count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCheckFramebufferStatus, 0, sizeof(oy_stat_glCheckFramebufferStatus));
    if (oy_stat_glCompileShader.calls) {
        OyProfileCounter *c = &oy_stat_glCompileShader;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCompileShader count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCompileShader, 0, sizeof(oy_stat_glCompileShader));
    if (oy_stat_glCompressedTexImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glCompressedTexImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCompressedTexImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCompressedTexImage2D, 0, sizeof(oy_stat_glCompressedTexImage2D));
    if (oy_stat_glCompressedTextureImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glCompressedTextureImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCompressedTextureImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCompressedTextureImage2D, 0, sizeof(oy_stat_glCompressedTextureImage2D));
    if (oy_stat_glCreateProgram.calls) {
        OyProfileCounter *c = &oy_stat_glCreateProgram;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCreateProgram count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCreateProgram, 0, sizeof(oy_stat_glCreateProgram));
    if (oy_stat_glCreateShader.calls) {
        OyProfileCounter *c = &oy_stat_glCreateShader;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glCreateShader count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glCreateShader, 0, sizeof(oy_stat_glCreateShader));
    if (oy_stat_glDrawArrays.calls) {
        OyProfileCounter *c = &oy_stat_glDrawArrays;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glDrawArrays count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glDrawArrays, 0, sizeof(oy_stat_glDrawArrays));
    if (oy_stat_glDrawElements.calls) {
        OyProfileCounter *c = &oy_stat_glDrawElements;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glDrawElements count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glDrawElements, 0, sizeof(oy_stat_glDrawElements));
    if (oy_stat_glFinish.calls) {
        OyProfileCounter *c = &oy_stat_glFinish;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glFinish count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glFinish, 0, sizeof(oy_stat_glFinish));
    if (oy_stat_glFlush.calls) {
        OyProfileCounter *c = &oy_stat_glFlush;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glFlush count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glFlush, 0, sizeof(oy_stat_glFlush));
    if (oy_stat_glGetActiveAttrib.calls) {
        OyProfileCounter *c = &oy_stat_glGetActiveAttrib;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetActiveAttrib count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetActiveAttrib, 0, sizeof(oy_stat_glGetActiveAttrib));
    if (oy_stat_glGetActiveUniform.calls) {
        OyProfileCounter *c = &oy_stat_glGetActiveUniform;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetActiveUniform count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetActiveUniform, 0, sizeof(oy_stat_glGetActiveUniform));
    if (oy_stat_glGetAttachedShaders.calls) {
        OyProfileCounter *c = &oy_stat_glGetAttachedShaders;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetAttachedShaders count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetAttachedShaders, 0, sizeof(oy_stat_glGetAttachedShaders));
    if (oy_stat_glGetAttribLocation.calls) {
        OyProfileCounter *c = &oy_stat_glGetAttribLocation;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetAttribLocation count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetAttribLocation, 0, sizeof(oy_stat_glGetAttribLocation));
    if (oy_stat_glGetBooleanv.calls) {
        OyProfileCounter *c = &oy_stat_glGetBooleanv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetBooleanv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetBooleanv, 0, sizeof(oy_stat_glGetBooleanv));
    if (oy_stat_glGetBufferParameteriv.calls) {
        OyProfileCounter *c = &oy_stat_glGetBufferParameteriv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetBufferParameteriv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetBufferParameteriv, 0, sizeof(oy_stat_glGetBufferParameteriv));
    if (oy_stat_glGetDoublev.calls) {
        OyProfileCounter *c = &oy_stat_glGetDoublev;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetDoublev count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetDoublev, 0, sizeof(oy_stat_glGetDoublev));
    if (oy_stat_glGetError.calls) {
        OyProfileCounter *c = &oy_stat_glGetError;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetError count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetError, 0, sizeof(oy_stat_glGetError));
    if (oy_stat_glGetFloatv.calls) {
        OyProfileCounter *c = &oy_stat_glGetFloatv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetFloatv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetFloatv, 0, sizeof(oy_stat_glGetFloatv));
    if (oy_stat_glGetFramebufferAttachmentParameteriv.calls) {
        OyProfileCounter *c = &oy_stat_glGetFramebufferAttachmentParameteriv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetFramebufferAttachmentParameteriv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetFramebufferAttachmentParameteriv, 0, sizeof(oy_stat_glGetFramebufferAttachmentParameteriv));
    if (oy_stat_glGetIntegerv.calls) {
        OyProfileCounter *c = &oy_stat_glGetIntegerv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetIntegerv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetIntegerv, 0, sizeof(oy_stat_glGetIntegerv));
    if (oy_stat_glGetNamedBufferParameteriv.calls) {
        OyProfileCounter *c = &oy_stat_glGetNamedBufferParameteriv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetNamedBufferParameteriv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetNamedBufferParameteriv, 0, sizeof(oy_stat_glGetNamedBufferParameteriv));
    if (oy_stat_glGetPointerv.calls) {
        OyProfileCounter *c = &oy_stat_glGetPointerv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetPointerv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetPointerv, 0, sizeof(oy_stat_glGetPointerv));
    if (oy_stat_glGetProgramBinary.calls) {
        OyProfileCounter *c = &oy_stat_glGetProgramBinary;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetProgramBinary count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetProgramBinary, 0, sizeof(oy_stat_glGetProgramBinary));
    if (oy_stat_glGetProgramInfoLog.calls) {
        OyProfileCounter *c = &oy_stat_glGetProgramInfoLog;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetProgramInfoLog count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetProgramInfoLog, 0, sizeof(oy_stat_glGetProgramInfoLog));
    if (oy_stat_glGetProgramiv.calls) {
        OyProfileCounter *c = &oy_stat_glGetProgramiv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetProgramiv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetProgramiv, 0, sizeof(oy_stat_glGetProgramiv));
    if (oy_stat_glGetQueryObjectiv.calls) {
        OyProfileCounter *c = &oy_stat_glGetQueryObjectiv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetQueryObjectiv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetQueryObjectiv, 0, sizeof(oy_stat_glGetQueryObjectiv));
    if (oy_stat_glGetQueryObjectuiv.calls) {
        OyProfileCounter *c = &oy_stat_glGetQueryObjectuiv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetQueryObjectuiv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetQueryObjectuiv, 0, sizeof(oy_stat_glGetQueryObjectuiv));
    if (oy_stat_glGetShaderInfoLog.calls) {
        OyProfileCounter *c = &oy_stat_glGetShaderInfoLog;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetShaderInfoLog count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetShaderInfoLog, 0, sizeof(oy_stat_glGetShaderInfoLog));
    if (oy_stat_glGetShaderiv.calls) {
        OyProfileCounter *c = &oy_stat_glGetShaderiv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetShaderiv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetShaderiv, 0, sizeof(oy_stat_glGetShaderiv));
    if (oy_stat_glGetShaderSource.calls) {
        OyProfileCounter *c = &oy_stat_glGetShaderSource;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetShaderSource count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetShaderSource, 0, sizeof(oy_stat_glGetShaderSource));
    if (oy_stat_glGetString.calls) {
        OyProfileCounter *c = &oy_stat_glGetString;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetString count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetString, 0, sizeof(oy_stat_glGetString));
    if (oy_stat_glGetStringi.calls) {
        OyProfileCounter *c = &oy_stat_glGetStringi;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetStringi count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetStringi, 0, sizeof(oy_stat_glGetStringi));
    if (oy_stat_glGetTexEnviv.calls) {
        OyProfileCounter *c = &oy_stat_glGetTexEnviv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetTexEnviv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetTexEnviv, 0, sizeof(oy_stat_glGetTexEnviv));
    if (oy_stat_glGetUniformBlockIndex.calls) {
        OyProfileCounter *c = &oy_stat_glGetUniformBlockIndex;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetUniformBlockIndex count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetUniformBlockIndex, 0, sizeof(oy_stat_glGetUniformBlockIndex));
    if (oy_stat_glGetUniformLocation.calls) {
        OyProfileCounter *c = &oy_stat_glGetUniformLocation;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetUniformLocation count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetUniformLocation, 0, sizeof(oy_stat_glGetUniformLocation));
    if (oy_stat_glGetVertexAttribfv.calls) {
        OyProfileCounter *c = &oy_stat_glGetVertexAttribfv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetVertexAttribfv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetVertexAttribfv, 0, sizeof(oy_stat_glGetVertexAttribfv));
    if (oy_stat_glGetVertexAttribiv.calls) {
        OyProfileCounter *c = &oy_stat_glGetVertexAttribiv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetVertexAttribiv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetVertexAttribiv, 0, sizeof(oy_stat_glGetVertexAttribiv));
    if (oy_stat_glGetVertexAttribPointerv.calls) {
        OyProfileCounter *c = &oy_stat_glGetVertexAttribPointerv;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGetVertexAttribPointerv count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glGetVertexAttribPointerv, 0, sizeof(oy_stat_glGetVertexAttribPointerv));
    if (oy_stat_glLinkProgram.calls) {
        OyProfileCounter *c = &oy_stat_glLinkProgram;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glLinkProgram count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glLinkProgram, 0, sizeof(oy_stat_glLinkProgram));
    if (oy_stat_glMapBuffer.calls) {
        OyProfileCounter *c = &oy_stat_glMapBuffer;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glMapBuffer count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glMapBuffer, 0, sizeof(oy_stat_glMapBuffer));
    if (oy_stat_glMapBufferRange.calls) {
        OyProfileCounter *c = &oy_stat_glMapBufferRange;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glMapBufferRange count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glMapBufferRange, 0, sizeof(oy_stat_glMapBufferRange));
    if (oy_stat_glMapNamedBuffer.calls) {
        OyProfileCounter *c = &oy_stat_glMapNamedBuffer;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glMapNamedBuffer count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glMapNamedBuffer, 0, sizeof(oy_stat_glMapNamedBuffer));
    if (oy_stat_glMapNamedBufferRange.calls) {
        OyProfileCounter *c = &oy_stat_glMapNamedBufferRange;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glMapNamedBufferRange count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glMapNamedBufferRange, 0, sizeof(oy_stat_glMapNamedBufferRange));
    if (oy_stat_glNamedBufferData.calls) {
        OyProfileCounter *c = &oy_stat_glNamedBufferData;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glNamedBufferData count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glNamedBufferData, 0, sizeof(oy_stat_glNamedBufferData));
    if (oy_stat_glNamedBufferSubData.calls) {
        OyProfileCounter *c = &oy_stat_glNamedBufferSubData;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glNamedBufferSubData count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glNamedBufferSubData, 0, sizeof(oy_stat_glNamedBufferSubData));
    if (oy_stat_glProgramBinary.calls) {
        OyProfileCounter *c = &oy_stat_glProgramBinary;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glProgramBinary count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glProgramBinary, 0, sizeof(oy_stat_glProgramBinary));
    if (oy_stat_glReadPixels.calls) {
        OyProfileCounter *c = &oy_stat_glReadPixels;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glReadPixels count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glReadPixels, 0, sizeof(oy_stat_glReadPixels));
    if (oy_stat_glShaderBinary.calls) {
        OyProfileCounter *c = &oy_stat_glShaderBinary;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glShaderBinary count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glShaderBinary, 0, sizeof(oy_stat_glShaderBinary));
    if (oy_stat_glShaderSource.calls) {
        OyProfileCounter *c = &oy_stat_glShaderSource;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glShaderSource count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glShaderSource, 0, sizeof(oy_stat_glShaderSource));
    if (oy_stat_glTexImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glTexImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glTexImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glTexImage2D, 0, sizeof(oy_stat_glTexImage2D));
    if (oy_stat_glTexSubImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glTexSubImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glTexSubImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glTexSubImage2D, 0, sizeof(oy_stat_glTexSubImage2D));
    if (oy_stat_glTextureImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glTextureImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glTextureImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glTextureImage2D, 0, sizeof(oy_stat_glTextureImage2D));
    if (oy_stat_glTextureSubImage2D.calls) {
        OyProfileCounter *c = &oy_stat_glTextureSubImage2D;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glTextureSubImage2D count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glTextureSubImage2D, 0, sizeof(oy_stat_glTextureSubImage2D));
    if (oy_stat_glUnmapBuffer.calls) {
        OyProfileCounter *c = &oy_stat_glUnmapBuffer;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glUnmapBuffer count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glUnmapBuffer, 0, sizeof(oy_stat_glUnmapBuffer));
    if (oy_stat_glUnmapNamedBuffer.calls) {
        OyProfileCounter *c = &oy_stat_glUnmapNamedBuffer;
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glUnmapNamedBuffer count=%u ms=%.3f max_ms=%.3f requested_bytes=%llu null_data=%u\n",
            oy_profile_number, c->calls, c->us/1000.0, c->maximum/1000.0, (unsigned long long)c->bytes, c->null_data);
    }
    memset(&oy_stat_glUnmapNamedBuffer, 0, sizeof(oy_stat_glUnmapNamedBuffer));

    if (oy_profile_slowest_integer_us)
    {
        size += snprintf(text+size, sizeof(text)-size,
            "PROFILE QUERY n=%u pname=0x%x max_ms=%.3f\n", oy_profile_number,
            oy_profile_slowest_integer_query, oy_profile_slowest_integer_us/1000.0);
    }
    oy_profile_slowest_integer_us = 0;
    if (oy_stat_glBindBuffer.calls) {
        OyProfileCounter *c=&oy_stat_glBindBuffer;
        size+=snprintf(text+size,sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glBindBuffer count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number,c->calls,c->us/1000.0,c->maximum/1000.0);
    }
    memset(&oy_stat_glBindBuffer,0,sizeof(oy_stat_glBindBuffer));
    if (oy_stat_glGenBuffers.calls) {
        OyProfileCounter *c=&oy_stat_glGenBuffers;
        size+=snprintf(text+size,sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGenBuffers count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number,c->calls,c->us/1000.0,c->maximum/1000.0);
    }
    memset(&oy_stat_glGenBuffers,0,sizeof(oy_stat_glGenBuffers));
    if (oy_stat_glDeleteBuffers.calls) {
        OyProfileCounter *c=&oy_stat_glDeleteBuffers;
        size+=snprintf(text+size,sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glDeleteBuffers count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number,c->calls,c->us/1000.0,c->maximum/1000.0);
    }
    memset(&oy_stat_glDeleteBuffers,0,sizeof(oy_stat_glDeleteBuffers));
    if (oy_stat_glGenTextures.calls) {
        OyProfileCounter *c=&oy_stat_glGenTextures;
        size+=snprintf(text+size,sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glGenTextures count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number,c->calls,c->us/1000.0,c->maximum/1000.0);
    }
    memset(&oy_stat_glGenTextures,0,sizeof(oy_stat_glGenTextures));
    if (oy_stat_glDeleteTextures.calls) {
        OyProfileCounter *c=&oy_stat_glDeleteTextures;
        size+=snprintf(text+size,sizeof(text)-size,
            "PROFILE GL_CALL n=%u name=glDeleteTextures count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number,c->calls,c->us/1000.0,c->maximum/1000.0);
    }
    memset(&oy_stat_glDeleteTextures,0,sizeof(oy_stat_glDeleteTextures));
    oy_profile_finish_calls = 0; oy_profile_finish_us = 0;
    oy_profile_append(text,size);
    oy_profile_fprintf(stderr, "PROFILE VERTEX_OFFSETS n=%u over_65535=%u max_bytes=%u safe_draw=1\n",
        oy_profile_number, oy_attrib_large_offsets, oy_attrib_max_offset);
    oy_attrib_large_offsets = oy_attrib_max_offset = 0;
    if (oy_stat_glUniform1fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniform1fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniform1fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniform1fv, 0, sizeof(oy_stat_glUniform1fv));
    if (oy_stat_glUniform2fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniform2fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniform2fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniform2fv, 0, sizeof(oy_stat_glUniform2fv));
    if (oy_stat_glUniform3fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniform3fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniform3fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniform3fv, 0, sizeof(oy_stat_glUniform3fv));
    if (oy_stat_glUniform4fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniform4fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniform4fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniform4fv, 0, sizeof(oy_stat_glUniform4fv));
    if (oy_stat_glUniform1iv.calls) {
        OyProfileCounter *c = &oy_stat_glUniform1iv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniform1iv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniform1iv, 0, sizeof(oy_stat_glUniform1iv));
    if (oy_stat_glUniformMatrix4fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniformMatrix4fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniformMatrix4fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniformMatrix4fv, 0, sizeof(oy_stat_glUniformMatrix4fv));
    if (oy_stat_glUniformMatrix3fv.calls) {
        OyProfileCounter *c = &oy_stat_glUniformMatrix3fv;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUniformMatrix3fv count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUniformMatrix3fv, 0, sizeof(oy_stat_glUniformMatrix3fv));
    if (oy_stat_glUseProgram.calls) {
        OyProfileCounter *c = &oy_stat_glUseProgram;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glUseProgram count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glUseProgram, 0, sizeof(oy_stat_glUseProgram));
    if (oy_stat_glActiveTexture.calls) {
        OyProfileCounter *c = &oy_stat_glActiveTexture;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glActiveTexture count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glActiveTexture, 0, sizeof(oy_stat_glActiveTexture));
    if (oy_stat_glBindTexture.calls) {
        OyProfileCounter *c = &oy_stat_glBindTexture;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glBindTexture count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glBindTexture, 0, sizeof(oy_stat_glBindTexture));
    if (oy_stat_glTexParameteri.calls) {
        OyProfileCounter *c = &oy_stat_glTexParameteri;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glTexParameteri count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glTexParameteri, 0, sizeof(oy_stat_glTexParameteri));
    if (oy_stat_glEnableVertexAttribArray.calls) {
        OyProfileCounter *c = &oy_stat_glEnableVertexAttribArray;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glEnableVertexAttribArray count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glEnableVertexAttribArray, 0, sizeof(oy_stat_glEnableVertexAttribArray));
    if (oy_stat_glDisableVertexAttribArray.calls) {
        OyProfileCounter *c = &oy_stat_glDisableVertexAttribArray;
        oy_profile_fprintf(stderr, "PROFILE GL_CALL n=%u name=glDisableVertexAttribArray count=%u ms=%.3f max_ms=%.3f requested_bytes=0 null_data=0\n",
            oy_profile_number, c->calls, c->us / 1000.0, c->maximum / 1000.0);
    }
    memset(&oy_stat_glDisableVertexAttribArray, 0, sizeof(oy_stat_glDisableVertexAttribArray));
    oy_profile_report_extra(oy_profile_number);
    oy_profile_last_log_us = oy_profile_now() - finish;
    oy_profile_interval = oy_profile_now();
    if (oy_profile_number >= 20) { oy_profile_active = 0; __atomic_store_n(&oy_profile_complete,1,__ATOMIC_RELEASE); }
    return result;
}

static void (*oy_raw_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void oy_trace_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer)
{
    uintptr_t offset = (uintptr_t)pointer;
    if (oy_profile_active)
    {
        oy_attrib_large_offsets += offset > UINT16_MAX;
        if (offset > oy_attrib_max_offset) oy_attrib_max_offset = offset;
    }
    oy_raw_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
void *oy_profile_wrap(const char *name, void *raw)
{
    if (raw == NULL) return NULL;
    if (strcmp(name, "glUniform1fv") == 0) { oy_raw_glUniform1fv = raw; return (void *)oy_trace_glUniform1fv; }
    if (strcmp(name, "glUniform2fv") == 0) { oy_raw_glUniform2fv = raw; return (void *)oy_trace_glUniform2fv; }
    if (strcmp(name, "glUniform3fv") == 0) { oy_raw_glUniform3fv = raw; return (void *)oy_trace_glUniform3fv; }
    if (strcmp(name, "glUniform4fv") == 0) { oy_raw_glUniform4fv = raw; return (void *)oy_trace_glUniform4fv; }
    if (strcmp(name, "glUniform1iv") == 0) { oy_raw_glUniform1iv = raw; return (void *)oy_trace_glUniform1iv; }
    if (strcmp(name, "glUniformMatrix4fv") == 0) { oy_raw_glUniformMatrix4fv = raw; return (void *)oy_trace_glUniformMatrix4fv; }
    if (strcmp(name, "glUniformMatrix3fv") == 0) { oy_raw_glUniformMatrix3fv = raw; return (void *)oy_trace_glUniformMatrix3fv; }
    if (strcmp(name, "glUseProgram") == 0) { oy_raw_glUseProgram = raw; return (void *)oy_trace_glUseProgram; }
    if (strcmp(name, "glActiveTexture") == 0) { oy_raw_glActiveTexture = raw; return (void *)oy_trace_glActiveTexture; }
    if (strcmp(name, "glBindTexture") == 0) { oy_raw_glBindTexture = raw; return (void *)oy_trace_glBindTexture; }
    if (strcmp(name, "glTexParameteri") == 0) { oy_raw_glTexParameteri = raw; return (void *)oy_trace_glTexParameteri; }
    if (strcmp(name, "glEnableVertexAttribArray") == 0) { oy_raw_glEnableVertexAttribArray = raw; return (void *)oy_trace_glEnableVertexAttribArray; }
    if (strcmp(name, "glDisableVertexAttribArray") == 0) { oy_raw_glDisableVertexAttribArray = raw; return (void *)oy_trace_glDisableVertexAttribArray; }
    if (strcmp(name, "glBindBuffer") == 0) { oy_raw_glBindBuffer=raw; return (void *)oy_trace_glBindBuffer; }
    if (strcmp(name, "glGenBuffers") == 0) { oy_raw_glGenBuffers=raw; return (void *)oy_trace_glGenBuffers; }
    if (strcmp(name, "glDeleteBuffers") == 0) { oy_raw_glDeleteBuffers=raw; return (void *)oy_trace_glDeleteBuffers; }
    if (strcmp(name, "glGenTextures") == 0) { oy_raw_glGenTextures=raw; return (void *)oy_trace_glGenTextures; }
    if (strcmp(name, "glDeleteTextures") == 0) { oy_raw_glDeleteTextures=raw; return (void *)oy_trace_glDeleteTextures; }
    if (strcmp(name, "eglSwapBuffers") == 0)
    { oy_raw_eglSwapBuffers = raw; return (void *)oy_trace_eglSwapBuffers; }
    if (strcmp(name, "glBufferData") == 0) { oy_raw_glBufferData = raw; return (void *)oy_trace_glBufferData; }
    if (strcmp(name, "glBufferSubData") == 0) { oy_raw_glBufferSubData = raw; return (void *)oy_trace_glBufferSubData; }
    if (strcmp(name, "glCheckFramebufferStatus") == 0) { oy_raw_glCheckFramebufferStatus = raw; return (void *)oy_trace_glCheckFramebufferStatus; }
    if (strcmp(name, "glCompileShader") == 0) { oy_raw_glCompileShader = raw; return (void *)oy_trace_glCompileShader; }
    if (strcmp(name, "glCompressedTexImage2D") == 0) { oy_raw_glCompressedTexImage2D = raw; return (void *)oy_trace_glCompressedTexImage2D; }
    if (strcmp(name, "glCompressedTextureImage2D") == 0) { oy_raw_glCompressedTextureImage2D = raw; return (void *)oy_trace_glCompressedTextureImage2D; }
    if (strcmp(name, "glCreateProgram") == 0) { oy_raw_glCreateProgram = raw; return (void *)oy_trace_glCreateProgram; }
    if (strcmp(name, "glCreateShader") == 0) { oy_raw_glCreateShader = raw; return (void *)oy_trace_glCreateShader; }
    if (strcmp(name, "glVertexAttribPointer") == 0) { oy_raw_glVertexAttribPointer = raw; return (void *)oy_trace_glVertexAttribPointer; }
    if (strcmp(name, "glDrawArrays") == 0) { oy_raw_glDrawArrays = raw; return (void *)oy_trace_glDrawArrays; }
    if (strcmp(name, "glDrawElements") == 0) { oy_raw_glDrawElements = raw; return (void *)oy_trace_glDrawElements; }
    if (strcmp(name, "glFinish") == 0) { oy_raw_glFinish = raw; return (void *)oy_trace_glFinish; }
    if (strcmp(name, "glFlush") == 0) { oy_raw_glFlush = raw; return (void *)oy_trace_glFlush; }
    if (strcmp(name, "glGetActiveAttrib") == 0) { oy_raw_glGetActiveAttrib = raw; return (void *)oy_trace_glGetActiveAttrib; }
    if (strcmp(name, "glGetActiveUniform") == 0) { oy_raw_glGetActiveUniform = raw; return (void *)oy_trace_glGetActiveUniform; }
    if (strcmp(name, "glGetAttachedShaders") == 0) { oy_raw_glGetAttachedShaders = raw; return (void *)oy_trace_glGetAttachedShaders; }
    if (strcmp(name, "glGetAttribLocation") == 0) { oy_raw_glGetAttribLocation = raw; return (void *)oy_trace_glGetAttribLocation; }
    if (strcmp(name, "glGetBooleanv") == 0) { oy_raw_glGetBooleanv = raw; return (void *)oy_trace_glGetBooleanv; }
    if (strcmp(name, "glGetBufferParameteriv") == 0) { oy_raw_glGetBufferParameteriv = raw; return (void *)oy_trace_glGetBufferParameteriv; }
    if (strcmp(name, "glGetDoublev") == 0) { oy_raw_glGetDoublev = raw; return (void *)oy_trace_glGetDoublev; }
    if (strcmp(name, "glGetError") == 0) { oy_raw_glGetError = raw; return (void *)oy_trace_glGetError; }
    if (strcmp(name, "glGetFloatv") == 0) { oy_raw_glGetFloatv = raw; return (void *)oy_trace_glGetFloatv; }
    if (strcmp(name, "glGetFramebufferAttachmentParameteriv") == 0) { oy_raw_glGetFramebufferAttachmentParameteriv = raw; return (void *)oy_trace_glGetFramebufferAttachmentParameteriv; }
    if (strcmp(name, "glGetIntegerv") == 0) { oy_raw_glGetIntegerv = raw; return (void *)oy_trace_glGetIntegerv; }
    if (strcmp(name, "glGetNamedBufferParameteriv") == 0) { oy_raw_glGetNamedBufferParameteriv = raw; return (void *)oy_trace_glGetNamedBufferParameteriv; }
    if (strcmp(name, "glGetPointerv") == 0) { oy_raw_glGetPointerv = raw; return (void *)oy_trace_glGetPointerv; }
    if (strcmp(name, "glGetProgramBinary") == 0) { oy_raw_glGetProgramBinary = raw; return (void *)oy_trace_glGetProgramBinary; }
    if (strcmp(name, "glGetProgramInfoLog") == 0) { oy_raw_glGetProgramInfoLog = raw; return (void *)oy_trace_glGetProgramInfoLog; }
    if (strcmp(name, "glGetProgramiv") == 0) { oy_raw_glGetProgramiv = raw; return (void *)oy_trace_glGetProgramiv; }
    if (strcmp(name, "glGetQueryObjectiv") == 0) { oy_raw_glGetQueryObjectiv = raw; return (void *)oy_trace_glGetQueryObjectiv; }
    if (strcmp(name, "glGetQueryObjectuiv") == 0) { oy_raw_glGetQueryObjectuiv = raw; return (void *)oy_trace_glGetQueryObjectuiv; }
    if (strcmp(name, "glGetShaderInfoLog") == 0) { oy_raw_glGetShaderInfoLog = raw; return (void *)oy_trace_glGetShaderInfoLog; }
    if (strcmp(name, "glGetShaderiv") == 0) { oy_raw_glGetShaderiv = raw; return (void *)oy_trace_glGetShaderiv; }
    if (strcmp(name, "glGetShaderSource") == 0) { oy_raw_glGetShaderSource = raw; return (void *)oy_trace_glGetShaderSource; }
    if (strcmp(name, "glGetString") == 0) { oy_raw_glGetString = raw; return (void *)oy_trace_glGetString; }
    if (strcmp(name, "glGetStringi") == 0) { oy_raw_glGetStringi = raw; return (void *)oy_trace_glGetStringi; }
    if (strcmp(name, "glGetTexEnviv") == 0) { oy_raw_glGetTexEnviv = raw; return (void *)oy_trace_glGetTexEnviv; }
    if (strcmp(name, "glGetUniformBlockIndex") == 0) { oy_raw_glGetUniformBlockIndex = raw; return (void *)oy_trace_glGetUniformBlockIndex; }
    if (strcmp(name, "glGetUniformLocation") == 0) { oy_raw_glGetUniformLocation = raw; return (void *)oy_trace_glGetUniformLocation; }
    if (strcmp(name, "glGetVertexAttribfv") == 0) { oy_raw_glGetVertexAttribfv = raw; return (void *)oy_trace_glGetVertexAttribfv; }
    if (strcmp(name, "glGetVertexAttribiv") == 0) { oy_raw_glGetVertexAttribiv = raw; return (void *)oy_trace_glGetVertexAttribiv; }
    if (strcmp(name, "glGetVertexAttribPointerv") == 0) { oy_raw_glGetVertexAttribPointerv = raw; return (void *)oy_trace_glGetVertexAttribPointerv; }
    if (strcmp(name, "glLinkProgram") == 0) { oy_raw_glLinkProgram = raw; return (void *)oy_trace_glLinkProgram; }
    if (strcmp(name, "glMapBuffer") == 0) { oy_raw_glMapBuffer = raw; return (void *)oy_trace_glMapBuffer; }
    if (strcmp(name, "glMapBufferRange") == 0) { oy_raw_glMapBufferRange = raw; return (void *)oy_trace_glMapBufferRange; }
    if (strcmp(name, "glMapNamedBuffer") == 0) { oy_raw_glMapNamedBuffer = raw; return (void *)oy_trace_glMapNamedBuffer; }
    if (strcmp(name, "glMapNamedBufferRange") == 0) { oy_raw_glMapNamedBufferRange = raw; return (void *)oy_trace_glMapNamedBufferRange; }
    if (strcmp(name, "glNamedBufferData") == 0) { oy_raw_glNamedBufferData = raw; return (void *)oy_trace_glNamedBufferData; }
    if (strcmp(name, "glNamedBufferSubData") == 0) { oy_raw_glNamedBufferSubData = raw; return (void *)oy_trace_glNamedBufferSubData; }
    if (strcmp(name, "glProgramBinary") == 0) { oy_raw_glProgramBinary = raw; return (void *)oy_trace_glProgramBinary; }
    if (strcmp(name, "glReadPixels") == 0) { oy_raw_glReadPixels = raw; return (void *)oy_trace_glReadPixels; }
    if (strcmp(name, "glShaderBinary") == 0) { oy_raw_glShaderBinary = raw; return (void *)oy_trace_glShaderBinary; }
    if (strcmp(name, "glShaderSource") == 0) { oy_raw_glShaderSource = raw; return (void *)oy_trace_glShaderSource; }
    if (strcmp(name, "glTexImage2D") == 0) { oy_raw_glTexImage2D = raw; return (void *)oy_trace_glTexImage2D; }
    if (strcmp(name, "glTexSubImage2D") == 0) { oy_raw_glTexSubImage2D = raw; return (void *)oy_trace_glTexSubImage2D; }
    if (strcmp(name, "glTextureImage2D") == 0) { oy_raw_glTextureImage2D = raw; return (void *)oy_trace_glTextureImage2D; }
    if (strcmp(name, "glTextureSubImage2D") == 0) { oy_raw_glTextureSubImage2D = raw; return (void *)oy_trace_glTextureSubImage2D; }
    if (strcmp(name, "glUnmapBuffer") == 0) { oy_raw_glUnmapBuffer = raw; return (void *)oy_trace_glUnmapBuffer; }
    if (strcmp(name, "glUnmapNamedBuffer") == 0) { oy_raw_glUnmapNamedBuffer = raw; return (void *)oy_trace_glUnmapNamedBuffer; }
    return raw;
}
