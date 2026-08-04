// HZM gl2 NEW POST-FX (bug-1158): chromatic aberration. NOT a gl1 port - a new cinematic-look
// addition, not a parity fix. Offsets the red and blue channels radially outward from screen
// centre, so the effect concentrates toward the edges and leaves the centre (where the crosshair
// and the player's aim point sit) clean. Master switch r_ppChromaticAberration, strength
// r_ppChromaticAberrationAmount.
//
// u_Color.x = amount (UV offset scale at the screen edge)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (amount, unused, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	vec2  d      = var_TexCoords - vec2(0.5);
	float radius = length(d) * 1.4142136;             // 0 centre -> ~1 at the corners
	vec2  dir    = (radius > 0.0001) ? (d / length(d)) : vec2(0.0);
	vec2  off    = dir * radius * radius * u_Color.x;  // quadratic falloff - clean centre, edges pull harder

	float r = texture2D(u_TextureMap, var_TexCoords + off).r;
	float g = texture2D(u_TextureMap, var_TexCoords).g;
	float b = texture2D(u_TextureMap, var_TexCoords - off).b;

	gl_FragColor = vec4(r, g, b, 1.0);
}
