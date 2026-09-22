#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>
#include "so_util.h"
#include "vita-affinity.h"
#include "vita-thread-state.h"
#include <psp2/kernel/processmgr.h>
#include "bridge-house-metrics.c"
#include "bridge-stall.c"
static void oy_measure_gl_dispatch(void);

const so_default_dynlib *oy_get_symbols(size_t *pCount);
uintptr_t *oy_egl_state(void);

static GLenum bridgeGlError;
static EGLint bridgeEglError = EGL_SUCCESS;
static GLfloat blendColor[4];

// EGL 1.0 constants omitted from vitaGL's public subset header.
enum { AndroidEglDraw = 0x3059, AndroidEglRead = 0x305a, AndroidEglBadMatch = 0x3009 };

int oy_ANativeWindow_setBuffersGeometry(void *pWindow, int width, int height, int format)
{
    if (pWindow == NULL || (width != 0 && width != 960) || (height != 0 && height != 544) || format != 0)
    {
        fprintf(stderr, "Unsupported Vita window geometry: %dx%d format %d\n", width, height, format);
        return -22;
    }
    return 0;
}

EGLBoolean oy_eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context)
{
    EGLBoolean result = eglMakeCurrent(display, draw, read, context);
    if (result)
    {
        struct OyNativeThreadState *state = context != EGL_NO_CONTEXT ? oy_native_thread_state() : NULL;
        if (state && !state->renderPinned)
        {
            oy_pin_thread("render", SCE_KERNEL_CPU_MASK_USER_1);
            state->renderPinned = 1;
#if OY_PROFILE_ENABLED && !OY_PROFILE_AGGREGATE
            oy_measure_gl_dispatch();
#endif
        }
        uintptr_t *pState = oy_egl_state();
        pState[0] = (uintptr_t)draw;
        pState[1] = (uintptr_t)read;
        pState[2] = (uintptr_t)context;
    }
    return result;
}

EGLContext oy_eglGetCurrentContext(void)
{
    return (EGLContext)oy_egl_state()[2];
}

EGLSurface oy_eglGetCurrentSurface(EGLint selection)
{
    if (selection != AndroidEglDraw && selection != AndroidEglRead)
    {
        bridgeEglError = EGL_BAD_PARAMETER;
        return EGL_NO_SURFACE;
    }
    return (EGLSurface)oy_egl_state()[selection == AndroidEglDraw ? 0 : 1];
}

EGLBoolean oy_eglReleaseThread(void)
{
    return oy_eglMakeCurrent(eglGetDisplay(NULL), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

EGLSurface oy_eglCreatePbufferSurface(EGLDisplay display, EGLConfig config, const EGLint *pAttributes)
{
    (void)display;
    (void)config;
    (void)pAttributes;
    fprintf(stderr, "Offscreen EGL pbuffer requested; this Vita port supports its display surface only.\n");
    bridgeEglError = AndroidEglBadMatch;
    return EGL_NO_SURFACE;
}

EGLint oy_eglGetError(void)
{
    if (bridgeEglError != EGL_SUCCESS)
    {
        EGLint result = bridgeEglError;
        bridgeEglError = EGL_SUCCESS;
        return result;
    }
    return eglGetError();
}

GLenum oy_glGetError(void)
{
    if (bridgeGlError != GL_NO_ERROR)
    {
        GLenum result = bridgeGlError;
        bridgeGlError = GL_NO_ERROR;
        return result;
    }
    return glGetError();
}

void oy_glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    GLfloat values[4] = {red, green, blue, alpha};
    for (unsigned int index = 0; index < 4; ++index)
    {
        blendColor[index] = values[index] < 0 ? 0 : values[index] > 1 ? 1 : values[index];
    }
}

void oy_glGetFloatv(GLenum name, GLfloat *pValues)
{
    if (name == 0x8005) // GL_BLEND_COLOR
    {
        memcpy(pValues, blendColor, sizeof(blendColor));
        return;
    }
    glGetFloatv(name, pValues);
}

void oy_glBlendFuncSeparate(GLenum sourceRgb, GLenum destinationRgb, GLenum sourceAlpha, GLenum destinationAlpha)
{
    const GLenum factors[] = {sourceRgb, destinationRgb, sourceAlpha, destinationAlpha};
    for (unsigned int index = 0; index < 4; ++index)
    {
        if (factors[index] >= 0x8001 && factors[index] <= 0x8004)
        {
            fprintf(stderr, "Constant-color blending is not implemented by the Vita graphics backend.\n");
            bridgeGlError = GL_INVALID_OPERATION;
            return;
        }
    }
    glBlendFuncSeparate(sourceRgb, destinationRgb, sourceAlpha, destinationAlpha);
}

void oy_glTexParameterfv(GLenum target, GLenum name, const GLfloat *pValues)
{
    // Every GLES 2 texture parameter accepted here is scalar.
    if (name == GL_TEXTURE_MIN_FILTER || name == GL_TEXTURE_MAG_FILTER ||
        name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T || name == GL_TEXTURE_MAX_ANISOTROPY_EXT)
    {
        glTexParameterf(target, name, pValues[0]);
        return;
    }
    bridgeGlError = GL_INVALID_ENUM;
}

void oy_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
    GLsizei height, GLenum format, GLsizei size, const void *pData)
{
    (void)target; (void)level; (void)x; (void)y; (void)width; (void)height;
    (void)format; (void)size; (void)pData;
    fprintf(stderr, "Compressed texture subregion update is unsupported by this Vita backend.\n");
    bridgeGlError = GL_INVALID_OPERATION;
}

#if OY_PROFILE_ENABLED && OY_PROFILE_AGGREGATE
#include "bridge-profile-aggregate.c"
#elif OY_PROFILE_ENABLED
#include "bridge-profile.c"
#else
#define oy_profile_wrap(name, raw) (raw)
#endif

// Optional whole-frame render resolution. Input/UI coordinates remain 960x544;
// vitaGL submits the smaller display surface and Vita display scaling presents it.
// Render-to-texture targets keep their exact pixel dimensions. Normal mode returns
// the original entry points and pays no per-call scaling wrapper overhead.
int oy_render_width = 960, oy_render_height = 544;
static GLuint oy_draw_framebuffer;
static GLint oy_logical_viewport[4] = {0, 0, 960, 544};
static GLint oy_logical_scissor[4] = {0, 0, 960, 544};
static void oy_apply_rect(const GLint *r, int scissor)
{
    GLint x = r[0], y = r[1]; GLsizei w = r[2], h = r[3];
    if (!oy_draw_framebuffer)
    {
        const GLint endX = (GLint)(((int64_t)x + w) * oy_render_width / 960);
        const GLint endY = (GLint)(((int64_t)y + h) * oy_render_height / 544);
        x = (GLint)((int64_t)x * oy_render_width / 960);
        y = (GLint)((int64_t)y * oy_render_height / 544);
        w = endX - x; h = endY - y;
    }
    if (scissor) glScissor(x, y, w, h); else glViewport(x, y, w, h);
}
static void oy_scaled_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    oy_logical_viewport[0]=x; oy_logical_viewport[1]=y; oy_logical_viewport[2]=w; oy_logical_viewport[3]=h;
    oy_apply_rect(oy_logical_viewport, 0);
}
static void oy_scaled_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    oy_logical_scissor[0]=x; oy_logical_scissor[1]=y; oy_logical_scissor[2]=w; oy_logical_scissor[3]=h;
    oy_apply_rect(oy_logical_scissor, 1);
}
static void oy_scaled_bind_framebuffer(GLenum target, GLuint framebuffer)
{
    glBindFramebuffer(target, framebuffer);
    if (target == GL_FRAMEBUFFER || target == 0x8CA9 /* GL_DRAW_FRAMEBUFFER */)
    {
        oy_draw_framebuffer = framebuffer;
        oy_apply_rect(oy_logical_viewport, 0); oy_apply_rect(oy_logical_scissor, 1);
    }
}
static void oy_scaled_get_integer(GLenum name, GLint *value)
{
    if (name == GL_VIEWPORT) memcpy(value, oy_logical_viewport, sizeof(oy_logical_viewport));
    else if (name == GL_SCISSOR_BOX) memcpy(value, oy_logical_scissor, sizeof(oy_logical_scissor));
    else glGetIntegerv(name, value);
}
static EGLBoolean oy_scaled_query_surface(EGLDisplay d, EGLSurface s, EGLint attribute, EGLint *value)
{
    const EGLBoolean result = eglQuerySurface(d, s, attribute, value);
    if (result && value)
    {
        if (attribute == EGL_WIDTH) *value = 960;
        if (attribute == EGL_HEIGHT) *value = 544;
    }
    return result;
}
void (*oy_eglGetProcAddress(const char *pName))(void)
{
    if (oy_render_width != 960)
    {
        if (!strcmp(pName, "glViewport")) return (void (*)(void))oy_scaled_viewport;
        if (!strcmp(pName, "glScissor")) return (void (*)(void))oy_scaled_scissor;
        if (!strcmp(pName, "glBindFramebuffer")) return (void (*)(void))oy_scaled_bind_framebuffer;
        if (!strcmp(pName, "glGetIntegerv")) return (void (*)(void))oy_scaled_get_integer;
        if (!strcmp(pName, "eglQuerySurface")) return (void (*)(void))oy_scaled_query_surface;
    }
    if (strcmp(pName, "glTexSubImage2D") == 0) { return (void (*)(void))oy_house_glTexSubImage2D; }
    size_t count;
    const so_default_dynlib *pSymbols = oy_get_symbols(&count);
    for (size_t index = 0; index < count; ++index)
    {
        if (strcmp(pName, pSymbols[index].symbol) == 0)
        {
            return (void (*)(void))oy_profile_wrap(pName, (void *)pSymbols[index].func);
        }
    }
    void *pFunction = vglGetProcAddress(pName);
#if OY_PROFILE_ENABLED
    if (pFunction == NULL) { fprintf(stderr, "PROFILE OPTIONAL_PROC missing=%s\n", pName); }
#endif
    return (void (*)(void))oy_profile_wrap(pName, pFunction);
}

// A one-off state-call dispatch test, outside any captured frame. No upload, draw or GPU wait.
// This bounds dispatch overhead for this call; it does not benchmark all loader shims or rendering.
static void oy_measure_gl_dispatch(void)
{
    static int measured;
    if (measured) return;
    measured = 1;
    typedef void (*ActiveTextureCall)(GLenum);
    ActiveTextureCall volatile throughLoader = (ActiveTextureCall)oy_eglGetProcAddress("glActiveTexture");
    if (!throughLoader) return;
    GLint savedTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedTexture);
    for (unsigned repeat = 0; repeat < 5; ++repeat)
    {
        uint64_t directUs = 0, loaderUs = 0;
        for (unsigned order = 0; order < 2; ++order)
        {
            const int viaLoader = (order + repeat) & 1;
            const uint64_t begin = sceKernelGetProcessTimeWide();
            if (viaLoader)
                for (unsigned i = 0; i < 10000; ++i) throughLoader(GL_TEXTURE0 + (i & 1));
            else
                for (unsigned i = 0; i < 10000; ++i) glActiveTexture(GL_TEXTURE0 + (i & 1));
            const uint64_t elapsed = sceKernelGetProcessTimeWide() - begin;
            if (viaLoader) loaderUs = elapsed; else directUs = elapsed;
        }
        glActiveTexture((GLenum)savedTexture);
        fprintf(stderr, "VITA GL_DISPATCH repeat=%u calls=10000 direct_us=%llu resolved_loader_us=%llu "
            "capture_active=0 state_restored=1\n", repeat,
            (unsigned long long)directUs, (unsigned long long)loaderUs);
    }
}
