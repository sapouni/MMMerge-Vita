$input a_position, a_texcoord0, a_texcoord1, a_texcoord3
$output v_color0, v_texcoord0, v_depth, v_worldPosition, v_texcoord1, v_flowInfo

#include "common.sh"

#if OPENYAMM_VITA_SHADER
uniform vec4 u_fxLightPositions[8];
uniform vec4 u_fxLightColors[8];
uniform vec4 u_fxLightParams;
uniform vec4 u_cameraPosition;
uniform vec4 u_secretPulseParams;

vec3 getFxLighting(vec3 worldPosition)
{
    vec3 lighting = vec3(u_fxLightParams.y, u_fxLightParams.y, u_fxLightParams.y);

    for (int i = 0; i < 8; ++i)
    {
        if (float(i) >= u_fxLightParams.x)
        {
            continue;
        }

        vec3 toLight = u_fxLightPositions[i].xyz - worldPosition;
        float radius = max(u_fxLightPositions[i].w, 1.0);
        float distanceSquared = dot(toLight, toLight);
        float inverseRadiusSquared = 1.0 / (radius * radius);
        float attenuation = 1.0 - clamp(distanceSquared * inverseRadiusSquared, 0.0, 1.0);
        attenuation *= attenuation;
        lighting += u_fxLightColors[i].rgb * (u_fxLightColors[i].w * attenuation * u_fxLightParams.z);
    }

    return clamp(lighting, vec3(0.0, 0.0, 0.0), vec3(2.0, 2.0, 2.0));
}
#endif

uniform vec4 u_fogColor;
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

float getFogAlpha(float dist)
{
    if (u_fogDensities.w > 0.5)
    {
        return 1.0;
    }

    return 1.0 - safeSmoothstep(u_fogDistances.y, u_fogDistances.z, dist);
}


void main()
{
    vec4 worldPosition = vec4(a_position, 1.0);
    vec4 viewPosition = mul(u_modelView, worldPosition);
    gl_Position = mul(u_modelViewProj, worldPosition);
    v_texcoord0 = a_texcoord0;
    v_depth = abs(viewPosition.z);
    v_worldPosition = worldPosition.xyz;
    v_texcoord1 = a_texcoord1;
    v_flowInfo = a_texcoord3;
#if OPENYAMM_VITA_SHADER
    // Interpolate lighting and distance across triangles instead of recomputing per pixel.
    v_color0 = vec4(getFxLighting(worldPosition.xyz), 0.5 + 0.5 * sin(u_secretPulseParams.y * 4.0));
    v_depth = length(worldPosition.xyz - u_cameraPosition.xyz);
#else
    v_color0 = vec4(1.0, 1.0, 1.0, 1.0);
#endif
    if (a_texcoord1.x >= -0.5) v_texcoord0 += a_texcoord3.xy * u_secretPulseParams.y;
    bool classicSecret = a_texcoord1.x > 0.5 && a_texcoord1.x < 1.5;
    bool authored = a_texcoord1.x >= 1.5;
    if ((classicSecret && u_secretPulseParams.x > 0.5)
        || (authored && u_secretPulseParams.z >= a_texcoord1.x - 2.0))
        v_color0.rgb *= vec3(1.0, v_color0.a, v_color0.a);
    v_color0.a = getFogAlpha(v_depth);
    v_depth = getFogRatio(v_depth);
}
