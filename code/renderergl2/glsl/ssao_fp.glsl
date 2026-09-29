uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_ViewInfo; // zfar / znear, zfar, 1/width, 1/height

// HZM gl2 (bug-1177): gl1-parity SSAO tunables, so the shipped coop_postfx.urc AO sliders drive gl2
// too - radius and bias were hardcoded constants here and intensity did not exist at all.
// Carried in the general-purpose spare vec4 (UNIFORM_HZMPARAMS / u_HzmParams) that already exists
// from the post-FX port, so no new uniform had to be plumbed through tr_glsl.c / tr_local.h.
// The defaults below reproduce the ORIGINAL hardcoded behaviour byte-for-byte (see the *64.0 and
// *2.0 scalings), so a default install looks exactly as it did before this change.
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C string
// literal without escaping, so a quote even inside a comment breaks the build.
uniform vec4   u_HzmParams; // (radius, intensity, bias, unused)

// HZM gl2 [2026-09-27] r_hzmFogAO (bug-3084; docs/proposals/fog_lod_pop_2026-09-27 plan piece A). The AO buffer is
// multiplied onto the frame AFTER the forward global fog, so a surface at 100% fog kept its occlusion and that
// darkening vanished in one frame at the far-plane cull. These are the SAME two uniforms the forward fog uses
// (RB_SetGlobalFogUniforms): u_GlobalFogParams = (projMat10, projMat14, fog start, 1 / (end - start)) and
// u_GlobalFogColor.a = r_globalFogScale. u_GlobalFogColor.a == 0 (r_hzmFogAO 0, no fog, screen-space fog path)
// skips the block, so the output is unchanged. The fade is computed here, before the depth-aware blur.
uniform vec4   u_GlobalFogColor;
uniform vec4   u_GlobalFogParams;

varying vec2   var_ScreenTex;

#if 0
vec2 poissonDisc[9] = vec2[9](
vec2(-0.7055767, 0.196515),    vec2(0.3524343, -0.7791386),
vec2(0.2391056, 0.9189604),    vec2(-0.07580382, -0.09224417),
vec2(0.5784913, -0.002528916), vec2(0.192888, 0.4064181),
vec2(-0.6335801, -0.5247476),  vec2(-0.5579782, 0.7491854),
vec2(0.7320465, 0.6317794)
);
#endif

#define NUM_SAMPLES 3

// Input: It uses texture coords as the random number seed.
// Output: Random number: [0,1), that is between 0.0 and 0.999999... inclusive.
// Author: Michael Pohoreski
// Copyright: Copyleft 2012 :-)
// Source: http://stackoverflow.com/questions/5149544/can-i-generate-a-random-number-inside-a-pixel-shader

float random( const vec2 p )
{
  // We need irrationals for pseudo randomness.
  // Most (all?) known transcendental numbers will (generally) work.
  const vec2 r = vec2(
    23.1406926327792690,  // e^pi (Gelfond's constant)
     2.6651441426902251); // 2^sqrt(2) (Gelfond-Schneider constant)
  //return fract( cos( mod( 123456789., 1e-7 + 256. * dot(p,r) ) ) );
  return mod( 123456789., 1e-7 + 256. * dot(p,r) );  
}

mat2 randomRotation( const vec2 p )
{
	float r = random(p);
	float sinr = sin(r);
	float cosr = cos(r);
	return mat2(cosr, sinr, -sinr, cosr);
}

float getLinearDepth(sampler2D depthMap, const vec2 tex, const float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

float ambientOcclusion(sampler2D depthMap, const vec2 tex, const float zFarDivZNear, const float zFar, const vec2 scale)
{
	vec2 poissonDisc[9];

	poissonDisc[0] = vec2(-0.7055767, 0.196515);
	poissonDisc[1] = vec2(0.3524343, -0.7791386);
	poissonDisc[2] = vec2(0.2391056, 0.9189604);
	poissonDisc[3] = vec2(-0.07580382, -0.09224417);
	poissonDisc[4] = vec2(0.5784913, -0.002528916);
	poissonDisc[5] = vec2(0.192888, 0.4064181);
	poissonDisc[6] = vec2(-0.6335801, -0.5247476);
	poissonDisc[7] = vec2(-0.5579782, 0.7491854);
	poissonDisc[8] = vec2(0.7320465, 0.6317794);

	float result = 0.0;

	float sampleZ = getLinearDepth(depthMap, tex, zFarDivZNear);
	float scaleZ = zFarDivZNear * sampleZ;

	vec2 slope = vec2(dFdx(sampleZ), dFdy(sampleZ)) / vec2(dFdx(tex.x), dFdy(tex.y));

	if (length(slope) * zFar > 5000.0)
		return 1.0;

	// radius: default 16 * 64.0 = 1024.0, identical to the old hardcoded value. Menu 2..48 -> 128..3072.
	vec2 offsetScale = vec2(scale * (u_HzmParams.x * 64.0) / scaleZ);

	mat2 rmat = randomRotation(tex);

	float invZFar = 1.0 / zFar;
	float zLimit = 20.0 * invZFar;
	for (int i = 0; i < NUM_SAMPLES; i++)
	{
		vec2 offset = rmat * poissonDisc[i] * offsetScale;
		float sampleDiff = getLinearDepth(depthMap, tex + offset, zFarDivZNear) - sampleZ;

		bool s1 = abs(sampleDiff) > zLimit;
		// bias: default 0.5 * 2.0 = 1.0, so this reduces to the old (sampleDiff + invZFar). Menu 0.1..4 -> 0.2..8.
		bool s2 = sampleDiff + (u_HzmParams.z * 2.0) * invZFar > dot(slope, offset);
		result += float(s1 || s2);
	}

	result *= 1.0 / float(NUM_SAMPLES);

	return result;
}

void main()
{
	float result = ambientOcclusion(u_ScreenDepthMap, var_ScreenTex, u_ViewInfo.x, u_ViewInfo.y, u_ViewInfo.wz);

	// intensity: applied HERE, not inside ambientOcclusion(), because that function early-outs with
	// 1.0 on steep slopes and that path must stay fully lit (1.0 maps to 1.0 through this too).
	// Same algebraic form as gl1's grade. Default 1.0 = identity; 0 = no AO; 3 = 3x darkening.
	result = clamp(1.0 - (1.0 - result) * u_HzmParams.y, 0.0, 1.0);

	// r_hzmFogAO: fade the occlusion out exactly as the forward fog faded the surface in (generic_fp ApplyGlobalFog):
	// same window depth, same projection terms, same linear ramp, same scale. Fully fogged = no occlusion.
	if (u_GlobalFogColor.a > 0.0)
	{
		float fogZw    = texture2D(u_ScreenDepthMap, var_ScreenTex).r;
		float fogDenom = u_GlobalFogParams.x + (2.0 * fogZw - 1.0);
		float fogDist  = u_GlobalFogParams.y / min(fogDenom, -1e-6);
		float fogFrac  = clamp((fogDist - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0);
		fogFrac = clamp(fogFrac * u_GlobalFogColor.a, 0.0, 1.0);
		result = mix(result, 1.0, fogFrac);
	}

	gl_FragColor = vec4(vec3(result), 1.0);
}
