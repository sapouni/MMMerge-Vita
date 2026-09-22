#pragma once
#include "game/render/VitaPerformanceConfig.h"
#include <bgfx/bgfx.h>
#include "engine/VitaAdaptiveResolution.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace OpenYAMM::Game::VitaWorldResolution
{
#if defined(OPENYAMM_VITA_LOADER)
inline bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
inline bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
inline bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
inline bool attempted = false;
inline void resetViews()
{
    const auto &settings = vitaPerformanceSettings();
    Engine::VitaFrameLimit::fps = settings.frameLimit;
    Engine::VitaFrameLimit::gameplay = false;
    auto &enabled = Engine::VitaAdaptiveResolution::enabled;
    enabled = settings.adaptiveWorldResolution && !settings.worldResolution;
    Engine::VitaAdaptiveResolution::gameplayThisFrame = false;
    Engine::VitaAdaptiveResolution::lowThisFrame = false;
    Engine::VitaAdaptiveResolution::mode = enabled ? 2 : 0;
    if (!settings.worldResolution && !enabled) { return; }
    bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
    bgfx::setViewFrameBuffer(1, BGFX_INVALID_HANDLE);
    const bgfx::ViewId order[] = {0, 1, 4, 2, 3};
    bgfx::setViewOrder(0, 5, order);
}
inline bgfx::ShaderHandle shader(const char *name)
{
    const std::filesystem::path root = OPENYAMM_BGFX_SHADER_DIR;
    const auto path = (root.is_absolute() ? root : std::filesystem::path(SDL_GetBasePath()) / root)
        / "essl" / (std::string(name) + ".bin");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0) { return BGFX_INVALID_HANDLE; }
    std::vector<char> bytes(size_t(file.tellg())); file.seekg(0); file.read(bytes.data(), bytes.size());
    if (!file) { return BGFX_INVALID_HANDLE; }
    const bgfx::Memory *memory = bgfx::copy(bytes.data(), uint32_t(bytes.size()));
    return memory ? bgfx::createShader(memory) : bgfx::ShaderHandle{bgfx::kInvalidHandle};
}
inline void shutdown()
{
    if (bgfx::isValid(target)) bgfx::destroy(target);
    if (bgfx::isValid(program)) bgfx::destroy(program);
    if (bgfx::isValid(sampler)) bgfx::destroy(sampler);
    target = BGFX_INVALID_HANDLE; program = BGFX_INVALID_HANDLE; sampler = BGFX_INVALID_HANDLE;
    attempted = false;
    Engine::VitaAdaptiveResolution::controller = {};
}
inline void composite(int width, int height, bool adaptiveGameplay = true)
{
    using namespace Engine::VitaAdaptiveResolution;
    Engine::VitaFrameLimit::gameplay = adaptiveGameplay;
    const bool fixedLow = vitaPerformanceSettings().worldResolution != 0;
    gameplayThisFrame = enabled && adaptiveGameplay;
    if ((!fixedLow && !enabled) || width <= 0 || height <= 0) { return; }
    if (!attempted)
    {
        attempted = true;
        bgfx::ShaderHandle vs = BGFX_INVALID_HANDLE, fs = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle textures[] = {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
        try {
        vs = shader("vs_vita_world_resolve"); fs = shader("fs_vita_world_resolve");
        if (bgfx::isValid(vs) && bgfx::isValid(fs)) program = bgfx::createProgram(vs, fs, false);
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        vs = BGFX_INVALID_HANDLE; fs = BGFX_INVALID_HANDLE;
        sampler = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
        textures[0] = bgfx::createTexture2D(720, 408, false, 1, bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        textures[1] = bgfx::createTexture2D(720, 408, false, 1, bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
        if (bgfx::isValid(textures[0]) && bgfx::isValid(textures[1]))
            target = bgfx::createFrameBuffer(2, textures, true);
        if (bgfx::isValid(target)) textures[0] = textures[1] = BGFX_INVALID_HANDLE;
        } catch (const std::bad_alloc &) {
            if (bgfx::isValid(vs)) bgfx::destroy(vs);
            if (bgfx::isValid(fs)) bgfx::destroy(fs);
            shutdown(); attempted = true; // Keep the native path; do not retry on every frame.
        }
        for (auto texture : textures) if (bgfx::isValid(texture)) bgfx::destroy(texture);
        std::fprintf(stderr, "VITA WORLD_RESOLUTION 720x408 HUD=native initialized=%u\n",
            unsigned(bgfx::isValid(target) && bgfx::isValid(program)));
        if (!bgfx::isValid(target) || !bgfx::isValid(program) || !bgfx::isValid(sampler))
        {
            shutdown(); attempted = true; // Release partial resources; native fallback without retries.
            return;
        }
    }
    if (!bgfx::isValid(target) || !bgfx::isValid(program) || !bgfx::isValid(sampler)) { return; }
    // Keep one target allocated for the session; resolution transitions do not
    // create/destroy textures or touch the card. Menus and HUD remain native.
    const bool useLow = fixedLow || (enabled && adaptiveGameplay && controller.low);
    if (!useLow) return;
    struct Vertex { float x,y,z,u,v; };
    static const bgfx::VertexLayout layout = [] { bgfx::VertexLayout value;
        value.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0,2,bgfx::AttribType::Float).end(); return value; }();
    if (bgfx::getAvailTransientVertexBuffer(6, layout) < 6) { return; }
    bgfx::TransientVertexBuffer buffer; bgfx::allocTransientVertexBuffer(&buffer,6,layout);
    if (!buffer.data) return;
    lowThisFrame = true;
    mode = fixedLow ? 1 : 3;
    const float bottom = bgfx::getCaps()->originBottomLeft ? 0.0f : 1.0f, top = 1.0f-bottom;
    const Vertex vertices[] = {{-1,-1,0,0,bottom},{1,-1,0,1,bottom},{1,1,0,1,top},
        {-1,-1,0,0,bottom},{1,1,0,1,top},{-1,1,0,0,top}};
    std::memcpy(buffer.data, vertices, sizeof(vertices));
    for (bgfx::ViewId view : {0,1}) {
        bgfx::setViewFrameBuffer(view,target); bgfx::setViewRect(view,0,0,720,408);
    }
    bgfx::setViewFrameBuffer(4,BGFX_INVALID_HANDLE);
    bgfx::setViewRect(4,0,0,uint16_t(width),uint16_t(height));
    bgfx::setViewClear(4,BGFX_CLEAR_NONE);
    bgfx::setVertexBuffer(0,&buffer);
    bgfx::setTexture(0,sampler,bgfx::getTexture(target));
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(4,program);
}
#else
inline void resetViews() {}
inline void composite(int,int,bool = true) {}
inline void shutdown() {}
#endif
}
