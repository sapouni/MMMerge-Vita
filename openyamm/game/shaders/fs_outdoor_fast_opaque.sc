$input v_color0, v_texcoord0, v_depth, v_worldPosition, v_texcoord1, v_flowInfo
#include "common.sh"
SAMPLER2D(s_texColor,0);
uniform vec4 u_fogColor;
uniform vec4 u_fogDensities;
void main() {
    vec3 color=mix(texture2D(s_texColor,v_texcoord0).rgb,u_fogColor.rgb,u_fogDensities.z)*v_color0.rgb;
    gl_FragColor=mix(vec4(color,1.0),vec4(u_fogColor.rgb,v_color0.a),v_depth);
}
