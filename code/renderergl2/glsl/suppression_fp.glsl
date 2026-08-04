// HZM gl2 POST-FX PORT (bug-1151): byte-port of renderergl1's SUPPRESSION_FS
// (tr_postprocess_gl1.c). When rounds crack past you or you take fire, the world desaturates and a
// dark TUNNEL vignette with a peripheral blur closes in - the flinch / keep-your-head-down
// instinct. Driven by r_ppSuppression and the cgame-published r_ppSuppress (a decaying 0..1 that
// the cgame spikes on near-miss bullet zings and on taking damage), scaled by r_ppSuppressAmount.
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (suppress 0..1, unused, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	vec2  d   = var_TexCoords - vec2(0.5);
	float s   = clamp(u_Color.x, 0.0, 1.0);
	vec3  c   = texture2D(u_TextureMap, var_TexCoords).rgb;

	// [user 2026-08-03] bug-1313 - TUNNEL VISION. The user asked for a more aggressive effect;
	// the old curve was linear in s, so a scripted 0.55 floor read as a mild grey wash. Two
	// changes, both weighted to the TOP of the range so ordinary near-miss suppression (s ~ 0.2
	// to 0.4, tuned previously) barely moves while a set-piece genuinely closes the tunnel:
	//   - the vignette radius TIGHTENS with s (2.4 -> up to 4.2), so the clear centre shrinks
	//     instead of the edges merely darkening. That shrinking aperture is what reads as tunnel
	//     vision rather than as a vignette filter.
	//   - the darkening and desaturation take s squared alongside s, so the curve is gentle
	//     early and steep late.
	float sHi = s * s;
	float vig = clamp(dot(d, d) * (2.4 + 1.8 * s), 0.0, 1.0);

	// peripheral blur that grows with how pinned down you are -> tunnel-vision smear at the edges
	float blurAmt = clamp(s * (0.35 + vig * 1.4) + sHi * 0.25, 0.0, 0.95);

	if (blurAmt > 0.01) {
		vec2 o = vec2(0.0035 + 0.0075 * s);
		vec3 b = texture2D(u_TextureMap, var_TexCoords + vec2( o.x,  o.y)).rgb
		       + texture2D(u_TextureMap, var_TexCoords + vec2(-o.x,  o.y)).rgb
		       + texture2D(u_TextureMap, var_TexCoords + vec2( o.x, -o.y)).rgb
		       + texture2D(u_TextureMap, var_TexCoords + vec2(-o.x, -o.y)).rgb;
		c = mix(c, b * 0.25, blurAmt);
	}

	float l = dot(c, vec3(0.299, 0.587, 0.114));
	c = mix(c, vec3(l), clamp(s * 0.80 + sHi * 0.15, 0.0, 0.95));   // desaturate, steeper at the top
	c *= (1.0 - vig * (s * 0.72 + sHi * 0.20));                     // close a darker tunnel as suppression rises

	gl_FragColor = vec4(c, 1.0);
}
