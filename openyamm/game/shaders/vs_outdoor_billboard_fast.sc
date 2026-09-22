$input a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0, v_depth
#include "common.sh"
uniform vec4 u_fogDensities;
uniform vec4 u_fogDistances;
float safeSmoothstep(float edge0, float edge1, float value)
{
    if (edge0 == edge1)
    {
        return 0.0;
    }

    float t = clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float getFogRatio(float dist)
{
    if (u_fogDensities.w > 0.5)
    {
        return clamp((dist - u_fogDistances.x) / max(u_fogDistances.y - u_fogDistances.x, 1.0), 0.0, 1.0);
    }

    return
        u_fogDensities.x
        + (u_fogDensities.y - u_fogDensities.x) * safeSmoothstep(u_fogDistances.x, u_fogDistances.y, dist)
        + (1.0 - u_fogDensities.y) * safeSmoothstep(u_fogDistances.y, u_fogDistances.z, dist);
}


void main()
{
    vec4 position = vec4(a_position, 1.0);
    gl_Position = mul(u_modelViewProj, position);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
    // Experimental: interpolate fog calculated at the quad vertices.
    v_depth = getFogRatio(abs(mul(u_modelView, position).z));
}
