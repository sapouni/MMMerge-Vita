#pragma once
#include <array>
#include <cstddef>
#include <vector>
#include <type_traits>
#include <utility>
namespace OpenYAMM::Engine
{
// Small POD event lists avoid allocating for the usual zero/one event. Overflow
// retains every event; this is not a cap on audio/effects or gameplay behavior.
template<class T, size_t N> class InlineEventList
{
    static_assert(std::is_trivially_copyable_v<T> && N>0);
    std::array<T,N> local{};
    std::vector<T> overflow;
    size_t used=0;
public:
    InlineEventList() = default;
    InlineEventList(const InlineEventList&) = default;
    InlineEventList& operator=(const InlineEventList&) = default;
    InlineEventList(InlineEventList&& rhs) noexcept
        : local(rhs.local), overflow(std::move(rhs.overflow)), used(std::exchange(rhs.used,0)) {}
    InlineEventList& operator=(InlineEventList&& rhs) noexcept
    { if(this!=&rhs){local=rhs.local;overflow=std::move(rhs.overflow);used=std::exchange(rhs.used,0);}return *this; }
    void push_back(const T& item)
    {
        if (used < N) local[used++] = item;
        else { if(used==N) overflow.assign(local.begin(),local.end()); overflow.push_back(item); ++used; }
    }
    size_t size() const {return used;}
    bool empty() const {return !used;}
    void clear() {overflow.clear();used=0;}
    T* begin(){return used>N?overflow.data():local.data();}
    const T* begin() const{return used>N?overflow.data():local.data();}
    T* end(){return begin()+used;}
    const T* end()const{return begin()+used;}
    T& front(){return *begin();}
    const T& front()const{return *begin();}
    T& operator[](size_t i){return begin()[i];}
    const T& operator[](size_t i)const{return begin()[i];}
};
}
