// HZM gl2 POST-FX PORT (bug-1151): byte-port of renderergl1's LOWHEALTH_FS
// (tr_postprocess_gl1.c). Colour drains and a luminance-PROPORTIONAL red wash closes in as the
// player bleeds out, heavier at the edges. Driven by r_ppLowHealth plus the cgame-published
// r_ppHealthFrac; the CPU side computes the ramp and heartbeat exactly as gl1 does and hands the
// finished intensity over as u_Color.x.
//
// The red is luminance-proportional on purpose: a fixed red looked vivid in dark halls and washed
// out in bright rooms, which read as the tint changing as you moved around the world.
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (hurt 0..1, unused, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	float hurt = clamp(u_Color.x, 0.0, 1.0);
	vec3  c = texture2D(u_TextureMap, var_TexCoords).rgb;
	float l = dot(c, vec3(0.299, 0.587, 0.114));

	c = mix(c, vec3(l), hurt * 0.5);                 // drain colour as you weaken

	vec3  red = vec3(l) * vec3(1.0, 0.30, 0.26);
	vec2  d   = var_TexCoords - vec2(0.5);
	float vig = clamp(dot(d, d) * 2.2, 0.0, 1.0);    // 0 centre -> 1 at the edges
	float amt = hurt * (0.30 + 0.50 * vig);

	c = mix(c, red, amt);
	c *= (1.0 - vig * hurt * 0.22);                  // gentle edge darken

	gl_FragColor = vec4(c, 1.0);
}
