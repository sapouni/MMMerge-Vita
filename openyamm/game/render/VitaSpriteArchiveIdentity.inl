// Content metadata, not timestamps/installation paths. No sprite decode/source hashing.
// ZIP CRC+size/name identify intended archive entries; prepared payloads retain SHA checksums.
static bool spriteArchiveIdentity(const std::string &path, std::string &revision)
{
    const int fd=::open(path.c_str(),O_RDONLY,0); if(fd<0) return false;
    struct Close { int fd; ~Close(){::close(fd);} } close{fd};
    const int64_t size=::lseek(fd,0,SEEK_END); if(size<22) return false;
    const auto readAll=[&](void *out,size_t count) {
        auto *p=static_cast<uint8_t *>(out);
        while(count) { auto got=::read(fd,p,count); if(got<0 && errno==EINTR) continue;
            if(got<=0) return false; p+=got;count-=size_t(got); } return true;
    };
    const auto u16=[](const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8);};
    const auto u32=[&](const uint8_t *p){return u16(p)|(u16(p+2)<<16);};
    std::array<uint8_t,65557> tail{};
    size_t n=size_t(std::min<int64_t>(size,tail.size()));
    if(::lseek(fd,size-n,SEEK_SET)<0 || !readAll(tail.data(),n)) return false;
    size_t e=n;
    for(size_t i=n-22;;--i) {
        if(u32(tail.data()+i)==0x06054b50 && i+22+u16(tail.data()+i+20)==n) { e=i;break; }
        if(!i) break;
    }
    if(e==n) return false;
    const auto *end=tail.data()+e;
    const uint32_t count=u16(end+10),bytes=u32(end+12),offset=u32(end+16);
    if(u16(end+4)||u16(end+6)||u16(end+8)!=count||count==65535
        ||uint64_t(offset)+bytes>uint64_t(size)-n+e||bytes>16*1024*1024) return false;
    std::vector<uint8_t> directory(bytes);
    if(::lseek(fd,offset,SEEK_SET)<0 || !readAll(directory.data(),bytes)) return false;
    std::string identity;
    size_t at=0;
    for(uint32_t i=0;i<count;++i) {
        if(at+46>directory.size()||u32(directory.data()+at)!=0x02014b50) return false;
        const auto *entry=directory.data()+at;
        const size_t length=u16(entry+28),extra=u16(entry+30),comment=u16(entry+32);
        if(at+46+length+extra+comment>directory.size()) return false;
        std::string name(reinterpret_cast<const char *>(entry+46),length);
        for(char &c:name) { if(c>='A' && c<='Z') c+=('a'-'A'); if(c=='\\') c='/'; }
        const bool sprite=name.starts_with("sprites/")||name.find("/sprites/")!=std::string::npos;
        const bool palette=name.ends_with(".act");
        if(sprite||palette) {
            // Include relevant entry ordering and mount order for overlay semantics.
            identity.append(reinterpret_cast<const char *>(entry+28),2);
            identity+=name;
            identity.append(reinterpret_cast<const char *>(entry+16),4); // CRC
            identity.append(reinterpret_cast<const char *>(entry+24),4); // uncompressed size
        }
        at+=46+length+extra+comment;
    }
    if(at!=directory.size()) return false;
    if(!identity.empty()) {
        const auto digest=hashBytes(reinterpret_cast<const uint8_t *>(identity.data()),identity.size());
        revision+='|'; revision.append(reinterpret_cast<const char *>(digest.data()),digest.size());
    }
    return true;
}
