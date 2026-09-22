$input v_texcoord0
#include "common.sh"
SAMPLER2D(s_texColor, 0);
uniform vec4 u_hudTint;
uniform vec4 u_hudTintMode;
void main()
{
    vec4 color = texture2D(s_texColor, v_texcoord0);
    color.rgb = u_hudTintMode.x < 1.5 ? u_hudTint.rgb : color.rgb * u_hudTint.rgb;
    color.a *= u_hudTint.a;
    if (color.a <= 0.1) { discard; }
    gl_FragColor = color;
}
