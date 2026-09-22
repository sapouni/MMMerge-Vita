$input v_texcoord0, v_color0, v_depth
#include "common.sh"
SAMPLER2D(s_texColor, 0);
uniform vec4 u_billboardAmbient;
uniform vec4 u_fogColor;
uniform vec4 u_fogDensities;
void main()
{
    if (v_texcoord0.x < 0.0 || v_texcoord0.x > 1.0 || v_texcoord0.y < 0.0 || v_texcoord0.y > 1.0) { discard; }
    vec4 color = texture2D(s_texColor, v_texcoord0);
    float alpha = color.a * v_color0.a;
    if (alpha <= 0.001) { discard; }
    vec3 lit = color.rgb * (u_billboardAmbient.rgb + v_color0.rgb);
    lit = mix(lit, u_fogColor.rgb, u_fogDensities.z);
    gl_FragColor = vec4(mix(lit, u_fogColor.rgb, v_depth), alpha);
}
