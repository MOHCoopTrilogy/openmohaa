// HZM coop [bug-3462] MODERN SCOPE VIEW - the optics of a WWII rifle scope, applied to the zoomed scene.
//
// cgame (cg_hzmscope.c) owns the geometry and publishes it every frame in r_ppScopeState; RB_HZMScope
// (tr_hzm_scope.c) feeds it here. The reticle is NOT drawn here: cgame draws it as a 2D quad on top, on both
// renderers, so it stays crisp and identical on gl1.
//   lens     - a SOFT circular aperture (no hard black frame); its radius is in units of screen height
//   pupil    - the exit pupil: a second, softer circle offset by where the eye sits behind the eyepiece. Where the
//              two do not overlap you see the scope shadow - the crescent that swings in as you sway or move
//   inside   - a touch of radial chromatic fringe and barrel toward the rim, a faint glass tint, a rim vignette
//   outside  - the eyepiece housing: black; optionally a faint blurred, desaturated share of the frame (r_ppScopeOuter < 1)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outer darkness 0..1)
uniform vec2      u_InvTexRes;  // 1 / scene size in texels (FBO_Blit, no texture scale)

varying vec2      var_TexCoords;

void main()
{
	float amt    = clamp(u_Color.x, 0.0, 1.0);
	float aspect = u_InvTexRes.y / u_InvTexRes.x;            // width / height
	vec2  d      = var_TexCoords - vec2(0.5);
	vec2  p      = d * vec2(aspect, -1.0);                     // screen-height units, +y down
	float R      = max(u_Color.y, 0.05);
	vec2  q      = p / R;                                      // lens radii
	float r      = length(q);

	float lens   = 1.0 - smoothstep(0.955, 1.0, r);
	float pr     = length(q - u_Color.zw) / max(u_HzmParams.x, 0.5);
	float pupil  = 1.0 - smoothstep(0.86, 1.0, pr);
	float vis    = lens * pupil;

	vec3 scene = texture2D(u_TextureMap, var_TexCoords).rgb;

	// ---- inside the lens
	float r2   = min(r * r, 1.0);
	float bar  = 1.0 - 0.015 * r2;                             // slight barrel toward the rim
	float ca   = u_HzmParams.z * 0.004 * r2;                   // radial fringe: red out, blue in
	vec3  inside;
	inside.r = texture2D(u_TextureMap, vec2(0.5) + d * (bar + ca)).r;
	inside.g = texture2D(u_TextureMap, vec2(0.5) + d * bar).g;
	inside.b = texture2D(u_TextureMap, vec2(0.5) + d * (bar - ca)).b;
	float w  = u_HzmParams.y;
	inside  *= vec3(1.0 + 0.25 * w, 1.0 + 0.05 * w, 1.0 - w);   // uncoated glass: a faint warm or cool cast
	inside  *= 1.0 - 0.18 * pow(smoothstep(0.55, 1.0, r), 1.5); // tube vignette toward the rim

	// ---- outside: the housing. Flat black by default (r_ppScopeOuter 1). Below 1 it keeps that share of a wide blur
	// of the frame, desaturated - at 0.97 this read as a grey smudge on bright maps, so it is opt-in now, and the
	// 17 taps are only paid for when it is asked for.
	float dark    = clamp(u_HzmParams.w, 0.0, 1.0);
	vec3  outside = vec3(0.0);
	if (dark < 0.999)
	{
		// two rings of 8 taps (1.2% and 2.6% of screen height) - one ring left visible ghost copies of bright edges
		vec2  o1  = vec2(0.012 / aspect, 0.012);
		vec2  o2  = vec2(0.026 / aspect, 0.026);
		vec3  b   = scene * 0.12;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 1.0,  0.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-1.0,  0.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.0,  1.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.0, -1.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.7,  0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-0.7,  0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.7, -0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-0.7, -0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.92,  0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.92,  0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.92, -0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.92, -0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.38,  0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.38,  0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.38, -0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.38, -0.92)).rgb * 0.055;
		float l   = dot(b, vec3(0.299, 0.587, 0.114));
		b         = mix(b, vec3(l), 0.55);
		outside = b * (1.0 - dark) * (1.0 - 0.7 * smoothstep(1.0, 2.0, r));
	}

	vec3 col = mix(outside, inside, vis);
	gl_FragColor = vec4(mix(scene, col, amt), 1.0);
}
