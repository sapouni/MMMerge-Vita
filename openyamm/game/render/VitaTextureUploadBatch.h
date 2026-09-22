#include "engine/VitaLoadProfile.h"
#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>

namespace OpenYAMM::Game
{
// Only active during synchronous level initialization, never during a draw pass.
// bgfx retains upload memory across frame boundaries. Keep pending uploads small.
#if defined(OPENYAMM_VITA_LOADER)
inline uint32_t vitaUploadBatchDepth = 0;
inline uint64_t vitaPendingUploadBytes = 0;
#endif

inline void flushVitaTextureUploads()
{
#if defined(OPENYAMM_VITA_LOADER)
    if (vitaUploadBatchDepth != 0)
    {
        OY_LOAD(Engine::VitaLoad::Scope loadDrain(Engine::VitaLoad::TextureDrain));
        bgfx::frame();
        bgfx::frame();
        vitaPendingUploadBytes = 0;
    }
#endif
}

inline void accountVitaTextureUpload(uint32_t bytes)
{
#if defined(OPENYAMM_VITA_LOADER)
    if (vitaUploadBatchDepth != 0)
    {
        vitaPendingUploadBytes += bytes;
        if (vitaPendingUploadBytes >= 2 * 1024 * 1024)
        {
            flushVitaTextureUploads();
        }
    }
#endif
}

class VitaTextureUploadBatch
{
public:
    VitaTextureUploadBatch()
    {
#if defined(OPENYAMM_VITA_LOADER)
        if (vitaUploadBatchDepth++ == 0)
        {
            vitaPendingUploadBytes = 0;
        }
#endif
    }
    ~VitaTextureUploadBatch()
    {
#if defined(OPENYAMM_VITA_LOADER)
        --vitaUploadBatchDepth;
#endif
    }
    VitaTextureUploadBatch(const VitaTextureUploadBatch &) = delete;
    VitaTextureUploadBatch &operator=(const VitaTextureUploadBatch &) = delete;
};
}
