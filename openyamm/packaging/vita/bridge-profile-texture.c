typedef struct OyTextureSample {
    unsigned id,w,h,mips,calls,syncs,avoided_bytes,errors;
    uint64_t sync_us;
} OyTextureSample;
static OyTextureSample oy_texture_samples[64];
static unsigned oy_texture_sample_count, oy_texture_overflow, oy_texture_error_details;
static struct { unsigned id,w,h,mips; int level,x,y,width,height; } oy_texture_current;
void oy_profile_texture(unsigned phase,unsigned id,unsigned w,unsigned h,unsigned mips,
    int level,int x,int y,int width,int height,unsigned long long elapsed,unsigned bytes)
{
    if (!oy_profile_active || !oy_profile_slot()) return;
    oy_context[1].object=id;
    oy_texture_current.id=id; oy_texture_current.w=w; oy_texture_current.h=h; oy_texture_current.mips=mips;
    oy_texture_current.level=level; oy_texture_current.x=x; oy_texture_current.y=y;
    oy_texture_current.width=width; oy_texture_current.height=height;
    unsigned i;
    for(i=0;i<oy_texture_sample_count;++i) if(oy_texture_samples[i].id==id) break;
    if(i==oy_texture_sample_count) {
        if(i==64) { ++oy_texture_overflow; return; }
        ++oy_texture_sample_count;
        oy_texture_samples[i]=(OyTextureSample){.id=id,.w=w,.h=h,.mips=mips};
    }
    OyTextureSample *v=&oy_texture_samples[i];
    if(!phase) ++v->calls;
    else { ++v->syncs; v->sync_us+=elapsed; v->avoided_bytes+=bytes; }
}
static void oy_profile_texture_error(GLenum before,GLenum after,GLenum format,GLenum type)
{
    if(!oy_profile_active) return;
    if(before || after) {
        for(unsigned i=0;i<oy_texture_sample_count;++i)
            if(oy_texture_samples[i].id==oy_texture_current.id) oy_texture_samples[i].errors+=after!=0;
        if(oy_texture_error_details++<64)
            oy_profile_fprintf(stderr,
                "PROFILE TEX_ERROR n=%u object=%u base_w=%u base_h=%u mips=%u level=%d x=%d y=%d w=%d h=%d "
                "upload_format=0x%x upload_type=0x%x preceding=0x%x result=0x%x\n",
                oy_profile_number+1,oy_texture_current.id,oy_texture_current.w,oy_texture_current.h,
                oy_texture_current.mips,oy_texture_current.level,oy_texture_current.x,oy_texture_current.y,
                oy_texture_current.width,oy_texture_current.height,format,type,before,after);
    }
}
static void oy_profile_report_textures(unsigned n)
{
    for(unsigned i=0;i<oy_texture_sample_count;++i) {
        OyTextureSample *v=&oy_texture_samples[i];
        oy_profile_fprintf(stderr,
            "PROFILE TEXTURE n=%u object=%u w=%u h=%u mips=%u subimages=%u syncs=%u sync_ms=%.3f "
            "avoided_copy_bytes=%u errors=%u\n",n,v->id,v->w,v->h,v->mips,v->calls,v->syncs,
            v->sync_us/1000.0,v->avoided_bytes,v->errors);
    }
    oy_profile_fprintf(stderr,"PROFILE TEXTURE_COVERAGE n=%u overflow=%u error_details=%u\n",
        n,oy_texture_overflow,oy_texture_error_details);
    oy_texture_sample_count=oy_texture_overflow=oy_texture_error_details=0;
}
