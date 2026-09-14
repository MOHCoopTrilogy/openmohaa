// HZM gl2 POST-FX PORT: bloom bright-pass. Two modes, selected by u_Color.z on the CPU.
//
// MODE 0 (u_Color.z == 0) - byte-for-byte the original bright pass (bug-1149/bug-1156/bug-1159):
// a byte-port of renderergl1 BRIGHT_FS, thresholding the CLAMPED [0,1] domain so r_ppBloomThreshold
// means on gl2 what it means on gl1. Kept identical for the A/B against mode 1.
//
// MODE 1 (u_Color.z >= 1) - EXPOSURE-AWARE glow (bug-1149 close-out). The threshold is applied in the
// DISPLAY domain for whichever tone curve is active this frame, so only things that are actually bright
// ON SCREEN (sky, fire, sunlit metal, muzzle flashes) glow - not every mid-tone. The kept colour is the
// ORIGINAL linear HDR value (clamped only at a firefly ceiling), written to a 16F half-res target, so
// auto-exposure - measured BEFORE this composite - cannot dim the glow away.
//   u_Color.z == 1  grade / ACES path: the grade clamps its input to [0,1] before ACES, so the display
//                   proxy is the clamped peak channel (same domain mode 0 and gl1 threshold in).
//   u_Color.z == 2  rend2 Hable + auto-exposure path: reproduce tonemap_fp up to the display value,
//                   using the same calcLevels texel, u_AutoExposureMinMax, u_ToneMinAvgMaxLinear and
//                   var_InvWhite the tone pass uses, so the numbers match exactly.
//
// u_Color = (threshold, knee, mode, maxBright). Threshold and knee are display-referred 0..1 in mode 1
// on BOTH tone paths, so the existing menu slider stays honest. maxBright caps single-pixel fireflies.
//
// The threshold arrives in u_Color.x: FBO_Blit hands its color argument over as UNIFORM_COLOR, resolved
// by NAME (u_Color) in tr_glsl.c uniformsInfo - any other name yields location -1 (bug-1148). u_InvTexRes
// is the source texel size (FBO_Blit sets it to 1/srcW,1/srcH when no srcTexScale is passed).
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C string
// literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;   // calcLevels 1x1 auto-exposure texel (mode 2 only)
uniform vec4      u_Color;       // (threshold, knee, mode, maxBright)
uniform vec2      u_InvTexRes;   // source texel size, for the 2x2 box tap in mode 1
uniform vec2      u_AutoExposureMinMax;
uniform vec3      u_ToneMinAvgMaxLinear;

varying vec2      var_TexCoords;
varying float     var_InvWhite;  // from tonemap_vp: 1.0 / Hable(white point)

float FilmicTonemap(float x)
{
	const float SS  = 0.22; // Shoulder Strength
	const float LS  = 0.30; // Linear Strength
	const float LA  = 0.10; // Linear Angle
	const float TS  = 0.20; // Toe Strength
	const float TAN = 0.01; // Toe Angle Numerator
	const float TAD = 0.30; // Toe Angle Denominator

	return ((x*(SS*x+LA*LS)+TS*TAN)/(x*(SS*x+LS)+TS*TAD)) - TAN/TAD;
}

void main()
{
	float mode = u_Color.z;

	if (mode < 0.5) {
		// MODE 0 - original clamped-LDR bright pass, unchanged.
		vec3  c = clamp(texture2D(u_TextureMap, var_TexCoords).rgb, 0.0, 1.0);
		float b = max(c.r, max(c.g, c.b));
		float f = max(b - u_Color.x, 0.0) / max(b, 1e-4);

		gl_FragColor = vec4(c * f, 1.0);
		return;
	}

	// MODE 1 - exposure-aware. 2x2 bilinear box on the (larger) source, so a bright sub-pixel
	// highlight cannot flicker as the half-res target samples it.
	vec2 o = 0.5 * u_InvTexRes;
	vec3 c = 0.25 * (
		texture2D(u_TextureMap, var_TexCoords + vec2( o.x,  o.y)).rgb +
		texture2D(u_TextureMap, var_TexCoords + vec2( o.x, -o.y)).rgb +
		texture2D(u_TextureMap, var_TexCoords + vec2(-o.x,  o.y)).rgb +
		texture2D(u_TextureMap, var_TexCoords + vec2(-o.x, -o.y)).rgb);

	float d;
	if (mode < 1.5) {
		// grade / ACES path: display proxy is the clamped peak channel.
		vec3 cc = clamp(c, 0.0, 1.0);
		d = max(cc.r, max(cc.g, cc.b));
	} else {
		// Hable path: reproduce tonemap_fp exactly, up to the display value.
		vec3 minAvgMax = texture2D(u_LevelsMap, var_TexCoords).rgb;
		vec3 logMinAvgMaxLum = clamp(minAvgMax * 20.0 - 10.0, -u_AutoExposureMinMax.y, -u_AutoExposureMinMax.x);
		float invAvgLum = u_ToneMinAvgMaxLinear.y * exp2(-logMinAvgMaxLum.y);
		vec3 e = max(vec3(0.0), c * invAvgLum - u_ToneMinAvgMaxLinear.xxx);
		float m = max(e.r, max(e.g, e.b));
		d = clamp(FilmicTonemap(m) * var_InvWhite, 0.0, 1.0);
	}

	float knee = max(u_Color.y, 1e-4);
	float f = smoothstep(u_Color.x - knee, u_Color.x + knee, d);

	// keep the original linear colour so the glow carries the scene hue; cap the firefly ceiling
	// only. The target is 16F, so values above 1.0 survive to the blur and additive composite.
	gl_FragColor = vec4(min(c, vec3(u_Color.w)) * f, 1.0);
}
