#pragma once
#include <cmath>
namespace OpenYAMM::Game
{
// Conservative sphere test in camera coordinates; side-plane normals are not unit length.
inline bool vitaSphereVisible(float x, float y, float z, float radius, float aspect, float farClip)
{
    constexpr float tanHalfFovY = 0.57735026919f; // Existing 60 degree vertical projection.
    const float tanHalfFovX = tanHalfFovY * aspect;
    return z + radius >= 4.0f && z - radius <= farClip
        && std::fabs(x) <= z * tanHalfFovX + radius * std::sqrt(1.0f + tanHalfFovX * tanHalfFovX)
        && std::fabs(y) <= z * tanHalfFovY + radius * std::sqrt(1.0f + tanHalfFovY * tanHalfFovY);
}
}
