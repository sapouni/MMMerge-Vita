// Included next to the existing PCX header/RLE parser. No full BGRA image allocation.
template<class Sink>
bool decodeMenuPcxRows(const std::vector<uint8_t> &bytes, int &width, int &height, Sink sink)
{
    if (bytes.size() < sizeof(PcxHeader)) return false;
    PcxHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    width = int(header.xMax) - int(header.xMin) + 1;
    height = int(header.yMax) - int(header.yMin) + 1;
    if (header.manufacturer != 10 || header.encoding != 1 || header.bitsPerPixel != 8
        || (header.colorPlanes != 1 && header.colorPlanes != 3)
        || width <= 0 || height <= 0 || width > 4096 || height > 4096
        || header.bytesPerLine < width) return false;
    const uint8_t *palette = nullptr;
    if (header.colorPlanes == 1)
    {
        if (bytes.size() < sizeof(PcxHeader) + 769 || bytes[bytes.size()-769] != 12) return false;
        palette = bytes.data() + bytes.size() - 768;
    }
    const size_t rowBytes = size_t(width) * 4;
    const unsigned rowsPerChunk = unsigned(65536 / rowBytes);
    std::vector<uint8_t> scanline(size_t(header.bytesPerLine) * header.colorPlanes);
    std::vector<uint8_t> chunk(rowBytes * rowsPerChunk);
    size_t offset = sizeof(PcxHeader);
    for (int first = 0; first < height; first += int(rowsPerChunk))
    {
        const unsigned rows = std::min(rowsPerChunk, unsigned(height-first));
        for (unsigned y=0; y<rows; ++y)
        {
            if (!decodePcxRleScanline(bytes,offset,scanline)) return false;
            if (palette && offset > bytes.size()-769) return false;
            for (int x=0; x<width; ++x)
            {
                const uint8_t red = palette ? palette[scanline[x]*3] : scanline[x];
                const uint8_t green = palette ? palette[scanline[x]*3+1] : scanline[header.bytesPerLine+x];
                const uint8_t blue = palette ? palette[scanline[x]*3+2] : scanline[header.bytesPerLine*2+x];
                const bool key = (red>=248 && green<=8 && blue>=248)
                    || (red>=248 && green>=48 && green<=64 && blue>=248)
                    || (red<=8 && green>=248 && blue>=248) || (red<=8 && green<=8 && blue>=248);
                uint8_t *p=chunk.data()+size_t(y)*rowBytes+size_t(x)*4;
                p[0]=blue; p[1]=green; p[2]=red; p[3]=key ? 0 : 255;
            }
        }
        if (!sink(unsigned(first),rows,chunk.data(),unsigned(rowBytes*rows))) return false;
    }
    return true;
}

#if defined(OPENYAMM_VITA_LOADER)
bgfx::TextureHandle uploadLargeMenuPcx(const std::vector<uint8_t> &bytes, int &width, int &height)
{
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    try
    {
        const bool complete = decodeMenuPcxRows(bytes,width,height,
            [&](unsigned first,unsigned rows,const uint8_t *pixels,unsigned count)
            {
                if (!bgfx::isValid(texture))
                    texture=createEmptyBgraTexture2D(uint16_t(width),uint16_t(height),TextureFilterProfile::Ui,
                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
                if (!bgfx::isValid(texture)) return false;
                const bgfx::Memory *memory=copyBgraTextureUploadMemory(pixels,count);
                if (!memory) return false;
                bgfx::updateTexture2D(texture,0,0,0,uint16_t(first),uint16_t(width),uint16_t(rows),memory);
                return true;
            });
        if (complete) return texture;
    }
    catch (const std::bad_alloc &) {} // Keep an incomplete atlas out of the texture cache.
    if (bgfx::isValid(texture)) bgfx::destroy(texture);
    return BGFX_INVALID_HANDLE;
}
#endif
