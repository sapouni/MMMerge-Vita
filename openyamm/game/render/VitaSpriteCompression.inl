// Run once on the sprite worker for cache misses, never on the game thread.
// Translucent images retain their original pixels; binary alpha is encoded exactly.
static void compressVitaSprite(VitaSpriteImage &image,
    const std::shared_ptr<Engine::BufferPool<uint8_t>> &pool)
{
    if (image.bc3) return;
    for (size_t i = 3; i < image.pixels.size(); i += 4)
        if (image.pixels[i] != 0 && image.pixels[i] != 255) return;
    static std::once_flag initialized;
    std::call_once(initialized, []
    {
        uint8_t source[64]{}, destination[16];
        stb_compress_dxt_block(destination, source, 1, STB_DXT_HIGHQUAL);
    });
    const size_t count = size_t(image.width) * image.height;
    VitaSpriteImage compressed(image.width, image.height,
        pool->take(size_t((image.width+3)/4) * ((image.height+3)/4) * 16));
    compressed.recycler = pool; compressed.bc3 = true;
    compressed.maskBits = pool->take((count+7)/8);
    std::fill(compressed.maskBits.begin(), compressed.maskBits.end(), 0);
    for (size_t i=0;i<count;++i)
        if (image.pixels[i*4+3]) compressed.maskBits[i/8] |= uint8_t(1u << (i%8));
    size_t offset = 0;
    for (int by=0;by<image.height;by+=4)
    {
        for (int bx=0;bx<image.width;bx+=4)
        {
            uint8_t block[64];
            for(int y=0;y<4;++y) for(int x=0;x<4;++x)
            {
                const size_t at=(size_t(std::min(by+y,image.height-1))*image.width+std::min(bx+x,image.width-1))*4;
                const unsigned to=(y*4+x)*4;
                block[to]=image.pixels[at+2];block[to+1]=image.pixels[at+1];
                block[to+2]=image.pixels[at];block[to+3]=image.pixels[at+3];
            }
            stb_compress_dxt_block(compressed.pixels.data()+offset,block,1,STB_DXT_HIGHQUAL);
            offset += 16;
        }
        // A newly encountered large sprite cannot monopolize the asset core.
        if ((by & 31) == 28) SDL_Delay(1);
    }
    pool->give(std::move(image.pixels));
    pool->give(std::move(image.maskBits));
    image = std::move(compressed);
}
