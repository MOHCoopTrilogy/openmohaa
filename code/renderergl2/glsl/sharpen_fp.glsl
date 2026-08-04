// HZM gl2 POST-FX PORT (bug-1150): byte-port of renderergl1's SHARPEN_FS
// (tr_postprocess_gl1.c), driven by the same r_ppSharpen / r_ppSharpenAmount levers. Runs after
// FXAA, exactly where gl1 runs it.
//
// Contrast-adaptive sharpening (CAS-style): the per-pixel weight scales with local contrast
// headroom, so flat areas and already-hard edges stay untouched - no haloes, unlike a plain
// unsharp mask. u_Color.x = amount, 0..1.
//
// gl1's u_rcp (one texel) arrives as UNIFORM_INVTEXRES; gl1's u_amount arrives as u_Color.x -
// the name u_Color is load-bearing, see bug-1148.
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C
// string literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform vec2      u_InvTexRes;   // = gl1's u_rcp
uniform vec4      u_Color;       // (amount, unused, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	vec3 e = texture2D(u_TextureMap, var_TexCoords).rgb;
	vec3 b = texture2D(u_TextureMap, var_TexCoords + vec2(0.0, -u_InvTexRes.y)).rgb;
	vec3 h = texture2D(u_TextureMap, var_TexCoords + vec2(0.0,  u_InvTexRes.y)).rgb;
	vec3 d = texture2D(u_TextureMap, var_TexCoords + vec2(-u_InvTexRes.x, 0.0)).rgb;
	vec3 f = texture2D(u_TextureMap, var_TexCoords + vec2( u_InvTexRes.x, 0.0)).rgb;

	vec3 mn = min(min(min(d, e), min(f, b)), h);
	vec3 mx = max(max(max(d, e), max(f, b)), h);

	vec3 amp = clamp(min(mn, vec3(2.0) - mx) / (mx + vec3(1e-5)), 0.0, 1.0);
	amp = sqrt(amp);

	float peak = -1.0 / mix(8.0, 5.0, clamp(u_Color.x, 0.0, 1.0));
	vec3  w    = amp * peak;
	vec3  sharp = ((b + d + f + h) * w + e) / (4.0 * w + vec3(1.0));

	gl_FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
