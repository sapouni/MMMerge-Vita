#pragma once
#include "engine/BufferPool.h"
#include "engine/ImageAssetLoader.h"
#include <memory>

namespace OpenYAMM::Game
{
// One owner per mounted world. Shutdown joins before the filesystem is changed.
struct VitaSpriteImage : Engine::ImagePixelsBgra
{
    std::shared_ptr<Engine::BufferPool<uint8_t>> recycler;
    ~VitaSpriteImage() { if(recycler){recycler->give(std::move(pixels));recycler->give(std::move(maskBits));} }
    VitaSpriteImage(VitaSpriteImage&&) = default;
    VitaSpriteImage& operator=(VitaSpriteImage&&) = default;
    int sourceWidth = width, sourceHeight = height;
    bool bc3 = false;
    std::vector<uint8_t> maskBits;
    VitaSpriteImage(int w, int h, std::vector<uint8_t> data)
        : Engine::ImagePixelsBgra{w, h, std::move(data)} {}
    VitaSpriteImage(Engine::ImagePixelsBgra &&image) : Engine::ImagePixelsBgra(std::move(image)) {}
};
class VitaSpriteStream
{
public:
    VitaSpriteStream(const Engine::AssetFileSystem &files, std::string world);
    ~VitaSpriteStream();
    void touch(uint32_t ticket, uint64_t frame);
    const VitaSpriteImage *request(uint32_t &ticket, const std::string &name,
        int16_t palette, uint64_t frame);
    void consumed(uint32_t ticket);
    bool failed(uint32_t ticket);
    uint32_t readyTicket();
    void deferReady(uint32_t ticket);
    void report(uint64_t frame);
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
