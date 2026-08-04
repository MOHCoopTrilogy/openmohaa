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

	gl_FragColor = vec4(c, 1.0);
}
