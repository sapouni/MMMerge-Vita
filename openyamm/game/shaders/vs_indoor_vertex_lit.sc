$input a_position, a_texcoord0, a_texcoord1, a_texcoord2, a_texcoord3, a_color0
$output v_texcoord0, v_worldPosition, v_texcoord1, v_screenspace, v_flowInfo, v_color0

#include "common.sh"

uniform vec4 u_indoorLightPositions[12];
uniform vec4 u_indoorLightColors[12];
uniform vec4 u_indoorLightParams;

vec3 getIndoorLighting(vec3 worldPosition, vec3 vertexLighting)
{
    vec3 lighting = u_indoorLightParams.yzw + vertexLighting * 2.0;

    for (int i = 0; i < 12; ++i)
    {
        if (float(i) >= u_indoorLightParams.x)
        {
            break;
        }

        vec3 toLight = u_indoorLightPositions[i].xyz - worldPosition;
        float distanceSquared = dot(toLight, toLight);
#if OPENYAMM_VITA_SHADER
        // The matching Vita renderer prepares this once per light, not per fragment.
        float inverseRadiusSquared = u_indoorLightPositions[i].w;
#else
        float radius = max(u_indoorLightPositions[i].w, 1.0);
        float inverseRadiusSquared = 1.0 / (radius * radius);
#endif
        float attenuation = 1.0 - clamp(distanceSquared * inverseRadiusSquared, 0.0, 1.0);
        attenuation *= attenuation;
        lighting += u_indoorLightColors[i].rgb * (u_indoorLightColors[i].w * attenuation);
    }

    return clamp(lighting, vec3(0.0, 0.0, 0.0), vec3(2.0, 2.0, 2.0));
}

void main()
{
    vec4 worldPosition = vec4(a_position, 1.0);
    gl_Position = mul(u_modelViewProj, worldPosition);
    v_texcoord0 = a_texcoord0;
    v_worldPosition = worldPosition.xyz;
    v_texcoord1 = a_texcoord1;
    v_screenspace = a_texcoord2;
    v_flowInfo = a_texcoord3;
    v_color0 = vec4(getIndoorLighting(worldPosition.xyz, a_color0.rgb), a_color0.a);
}
