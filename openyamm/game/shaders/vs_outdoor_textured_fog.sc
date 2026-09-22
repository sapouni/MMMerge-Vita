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
}
