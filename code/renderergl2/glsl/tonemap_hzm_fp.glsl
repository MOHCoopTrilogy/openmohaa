// HZM gl2 PARITY GRADE (2026-07-28): a byte-for-byte port of gl1's TONEMAP_FS
// (renderergl1/tr_postprocess_gl1.c) so gl2 reproduces the look the OG trilogy has on gl1.
//
// Discovery that motivated this (workflow wf_053a935d-2dd): gl1 is NOT untonemapped. It runs
// this exact grade inline every frame (RB_PostFxApply, renderergl1/tr_backend.c:1302) with the
// shipped values r_ppExposure 0.889971 / r_ppContrast 0.951289 / r_ppSaturation 1.031519.
// gl2 instead ran rend2's Hable filmic, and that curve difference alone accounts for gl2 being
// ~15-20% darker than gl1 (predicted within 1.5 luma on 3 of 4 measured maps, no fitting).
//
// Order matters: exposure -> ACES (Narkowicz) -> contrast about mid-grey -> saturation -> tint,
// identical to gl1. Selected by r_tonemapMode 1; r_tonemapMode 0 keeps rend2's Hable.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;     // (exposure, contrast, saturation, temp tint)
// NOTE (bug-1148): this MUST be named u_Color. FBO_Blit hands the grade over as UNIFORM_COLOR,
// and GLSL_InitUniforms resolves that by the NAME in tr_glsl.c uniformsInfo, which is u_Color.
// It was declared u_Grade, a name no uniform table entry matches, so the location came back -1,
// every set was dropped and the shader ran with an all-zero grade.

varying vec2      var_TexCoords;
// HZM coop [2026-09-28] storm darkness: x = 1 - the display-referred gain (0 = none; FBO_Blit pushes a zero vec4 unless
// tr_postprocess.c RB_ToneMap set it). A gain after the curve keeps the ACES toe, which an exposure cut crushes.
uniform vec4      u_HzmParams;

// HZM gl2 volumetric clouds (tr_hzm_clouds.c RB_HZM_CloudsToneDither; plan 2.4 step 4): an ANIMATED cloud-masked
// triangular dither right before the RGBA8 write. u_VclDither.x = amplitude / 255 (0 = off: this pass is then
// byte-identical), y = the golden-ratio frame offset, z = 1 - the sky's fog fraction. The mask is the cloud presence
// from the tent target x sky (depth at the far plane) - vc_post.py chain(mask=presence*sky*(1-f), dframe=frame).
uniform sampler2D u_VclCloud;
uniform sampler2D u_ScreenDepthMap;
uniform vec4      u_VclDither;
uniform vec4      u_VclVp;
uniform vec4      u_VclMisc;
uniform vec4      u_VclView;

float VclIgn(vec2 p)
{
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

vec3 VclDither(vec3 c)
{
	if (u_VclDither.x <= 0.0)
	{
		return c;
	}
	vec2 uv = (gl_FragCoord.xy - u_VclVp.xy) * u_VclVp.zw;
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
	{
		return c;
	}
	if (texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_VclView.zw).r < 1.0)
	{
		return c;
	}
	vec2 tp = clamp(uv * u_VclMisc.zw, vec2(0.5), u_VclMisc.zw - vec2(0.5));
	vec4 cl = texture2D(u_VclCloud, tp * u_VclView.xy);
	float pres = clamp(max(1.0 - cl.a, max(cl.r, max(cl.g, cl.b)) * 4.0) * 8.0, 0.0, 1.0) * u_VclDither.z;
	vec2 p = floor(gl_FragCoord.xy) + vec2(u_VclDither.y);
	float n = VclIgn(p) + VclIgn(p + vec2(17.0, 31.0)) - 1.0;
	return c + vec3(n * u_VclDither.x * pres);
}

void main()
{
	// gl1 grades the CLAMPED 8-bit backbuffer; gl2's source is a float HDR FBO whose values
	// exceed 1.0, which pushes the ACES shoulder and lifts near/bright regions (measured:
	// e2l2 near 84.7 vs gl1 57.4). Clamp to gl1's input domain first.
	vec3 c = clamp(texture2D(u_TextureMap, var_TexCoords).rgb, 0.0, 1.0) * u_Color.x;

	// ACES filmic approximation (Narkowicz) - same constants as gl1
	c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);

	// contrast about mid-grey
	c = clamp((c - 0.5) * u_Color.y + 0.5, 0.0, 1.0);

	// saturation about luma
	float l = dot(c, vec3(0.299, 0.587, 0.114));
	c = clamp(mix(vec3(l), c, u_Color.z), 0.0, 1.0);

	// white-balance tint: warm boosts red / cools blue
	c.r = clamp(c.r * (1.0 + u_Color.w), 0.0, 1.0);
	c.b = clamp(c.b * (1.0 - u_Color.w), 0.0, 1.0);

	c *= 1.0 - u_HzmParams.x;   // HZM coop [2026-09-28] storm darkness
	gl_FragColor = vec4(VclDither(c), 1.0);
}
