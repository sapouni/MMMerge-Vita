$input v_texcoord0
#include "common.sh"
SAMPLER2D(s_texColor, 0);
uniform vec4 u_vitaMenuTint;
void main()
{
    vec4 color = texture2D(s_texColor, v_texcoord0) * u_vitaMenuTint;
    if (color.a <= 0.1) { discard; }
    gl_FragColor = color;
}
