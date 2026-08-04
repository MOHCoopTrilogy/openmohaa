// HZM gl2 NEW POST-FX (bug-1158): film grain. NOT a gl1 port - a new cinematic-look addition.
// Animated per-pixel luminance noise, applied multiplicatively so it reads as sensor/film grain
// rather than a static dither pattern (a new random field every frame, driven by time). Master
// switch r_ppFilmGrain, strength r_ppFilmGrainAmount.
//
// u_Color.x = amount (0..1), u_Color.y = time (seconds, reseeds the noise every frame)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (amount, time, unused, unused)

varying vec2      var_TexCoords;

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main()
{
	vec3  c     = texture2D(u_TextureMap, var_TexCoords).rgb;
	float noise = h21(var_TexCoords * 4096.0 + vec2(u_Color.y * 97.0, u_Color.y * 57.0)) - 0.5;

	c += noise * u_Color.x * 0.12;

	gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
