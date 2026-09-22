#pragma once
#include "engine/VitaStall.h"
#include <SDL3/SDL_timer.h>
namespace OpenYAMM::Game::VitaPickMetrics
{
inline thread_local uint64_t startNs=0, publishedNs=0;
inline thread_local uint32_t totals[24]={};
inline bool enabled() { return Engine::VitaStall::progress != nullptr; }
inline uint64_t start() { return enabled() ? SDL_GetTicksNS() : 0; }
inline void count(unsigned field) { if(enabled()) ++totals[field]; }
inline void finish(unsigned field, uint64_t start)
{
    if(start) totals[field] += uint32_t((SDL_GetTicksNS()-start)/1000);
}
inline void frame(uint64_t now)
{
    if(!enabled()) return;
    if(!startNs || now<startNs) { startNs=now; publishedNs=0; for(auto &v:totals) v=0; }
    ++totals[1];
    if(now-publishedNs<1000000000) return;
    publishedNs=now; totals[0]=uint32_t((now-startNs)/1000000);
    auto *p=Engine::VitaStall::progress;
    __atomic_fetch_add(&p->pickSequence,1,__ATOMIC_ACQ_REL);
    for(unsigned i=0;i<24;++i) __atomic_store_n(&p->pickTotals[i],totals[i],__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->pickSequence,1,__ATOMIC_RELEASE);
}
class StationaryFrame
{
    bool active=false;
    unsigned base=0, ms=0;
    uint32_t before[5]={};
public:
    template<class Request> StationaryFrame(uint64_t now,const Request &request,bool visible,bool pressed)
    {
        if(!enabled()) return;
        static thread_local Request previous{};
        static thread_local uint64_t previousNs=0;
        static thread_local bool previousVisible=false;
        active=previousNs && now>previousNs && now-previousNs<1000000000
            && !pressed && visible==previousVisible && request.hasRay==previous.hasRay
            && request.screenX==previous.screenX && request.screenY==previous.screenY
            && request.viewWidth==previous.viewWidth && request.viewHeight==previous.viewHeight
            && request.viewMatrix==previous.viewMatrix && request.projectionMatrix==previous.projectionMatrix;
        ms=active ? uint32_t((now-previousNs)/1000000) : 0;
        base=visible?17:10;
        previous=request;previousNs=now;previousVisible=visible;
        const unsigned fields[]={2,7,3,8,4};
        for(unsigned i=0;i<5;++i) before[i]=totals[fields[i]];
    }
    ~StationaryFrame()
    {
        if(!active) return;
        totals[base]+=ms; ++totals[base+1];
        const unsigned fields[]={2,7,3,8,4};
        for(unsigned i=0;i<5;++i) totals[base+2+i]+=totals[fields[i]]-before[i];
    }
};
}
