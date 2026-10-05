#if defined(USE_SHADOW_STABLE)
// HZM gl2 STABLE SUN SHADOWS (plan P3 + shadows plan B0, tr_hzm_sunstable.c). The mask carries only V_entity: three
// camera-centred RADIAL cascades of entity casters (B0: the world casts only through W, which lightall samples per
// pixel). Radial select with a blend band, a fixed Poisson kernel per cascade (no screen noise, so nothing crawls),
// hardware 2x2 PCF (the depth images are LINEAR while stable), a light-axis constant bias. 1.0 beyond cascade 2.
// u_HzmShadowKernel.w 5 = r_shadowDebug 5: V_world instead (W, image 4), for the freeze captures (vet R8).
// NOTE keep this file free of double quotes - stringify wraps each line in a C string.
uniform sampler2D u_ScreenDepthMap;
uniform sampler2DShadow u_ShadowMap;
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
uniform mat4   u_ShadowMvp;
uniform mat4   u_ShadowMvp2;
uniform mat4   u_ShadowMvp3;
uniform mat4   u_ShadowMvp4;
uniform vec3   u_ViewOrigin;
uniform vec4   u_ViewInfo; // zfar / znear, zfar
uniform vec4   u_HzmShadowSplits; // R0 R1 R2 band
uniform vec4   u_HzmShadowKernel; // PCF radius (uv) c0 c1 c2, debug mode
uniform vec4   u_HzmShadowBias;   // depth bias c0 c1 c2, W bias

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

#define DEPTH_MAX_ERROR 0.000000059604644775390625

float HzmTap(sampler2DShadow m, vec3 c)
{
	return vec4(shadow2D(m, c)).r;
}

float HzmPcf(sampler2DShadow m, vec3 s, float r)
{
	float v = HzmTap(m, vec3(s.xy + vec2(-0.7071, -0.7071) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2( 0.7071, -0.7071) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2(-0.7071,  0.7071) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2( 0.7071,  0.7071) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2( 0.0,  -0.35) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2( 0.35,  0.0) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2( 0.0,   0.35) * r, s.z));
	v += HzmTap(m, vec3(s.xy + vec2(-0.35,  0.0) * r, s.z));
	return v * 0.125;
}

// cascade coordinates in [0,1]; w < 0 = the point is outside the cascade box
vec4 HzmCascadePos(mat4 mvp, vec4 p, float bias)
{
	vec4 s = mvp * p;
	s.xyz = s.xyz / s.w;
	float inside = (all(lessThan(abs(s.xyz), vec3(1.0)))) ? 1.0 : -1.0;
	return vec4(s.xyz * 0.5 + vec3(0.5) - vec3(0.0, 0.0, bias), inside);
}

float getLinearDepth(sampler2D depthMap, vec2 tex, float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r - DEPTH_MAX_ERROR;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

void main()
{
	float result = 1.0;
	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);

	if (depth < 0.999)
	{
		vec4 P = vec4(u_ViewOrigin + var_ViewDir * (depth - 0.5 / u_ViewInfo.x), 1.0);

		if (u_HzmShadowKernel.w > 4.5)
		{
			vec4 w = HzmCascadePos(u_ShadowMvp4, P, u_HzmShadowBias.w);
			result = (w.w > 0.0) ? HzmPcf(u_ShadowMap4, w.xyz, 0.0) : 1.0;
		}
		else
		{
			float d = length(P.xyz - u_ViewOrigin);
			float band = u_HzmShadowSplits.w;
			vec4 c0 = HzmCascadePos(u_ShadowMvp,  P, u_HzmShadowBias.x);
			vec4 c1 = HzmCascadePos(u_ShadowMvp2, P, u_HzmShadowBias.y);
			vec4 c2 = HzmCascadePos(u_ShadowMvp3, P, u_HzmShadowBias.z);
			float v2 = (c2.w > 0.0 && d < u_HzmShadowSplits.z) ? HzmPcf(u_ShadowMap3, c2.xyz, u_HzmShadowKernel.z) : 1.0;
			// the far edge of cascade 2 fades out to 1.0 over the band
			v2 = mix(v2, 1.0, smoothstep(u_HzmShadowSplits.z * (1.0 - band), u_HzmShadowSplits.z, d));
			float v1 = v2;
			if (c1.w > 0.0 && d < u_HzmShadowSplits.y)
			{
				v1 = HzmPcf(u_ShadowMap2, c1.xyz, u_HzmShadowKernel.y);
				v1 = mix(v1, v2, smoothstep(u_HzmShadowSplits.y * (1.0 - band), u_HzmShadowSplits.y, d));
			}
			result = v1;
			if (c0.w > 0.0 && d < u_HzmShadowSplits.x)
			{
				float v0 = HzmPcf(u_ShadowMap, c0.xyz, u_HzmShadowKernel.x);
				result = mix(v0, v1, smoothstep(u_HzmShadowSplits.x * (1.0 - band), u_HzmShadowSplits.x, d));
			}
		}
	}

	gl_FragColor = vec4(vec3(result), 1.0);
}
#else
uniform sampler2D u_ScreenDepthMap;

uniform sampler2DShadow u_ShadowMap;
#if defined(USE_SHADOW_CASCADE)
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
#endif

uniform mat4      u_ShadowMvp;
#if defined(USE_SHADOW_CASCADE)
uniform mat4      u_ShadowMvp2;
uniform mat4      u_ShadowMvp3;
uniform mat4      u_ShadowMvp4;
#endif

uniform vec3   u_ViewOrigin;
uniform vec4   u_ViewInfo; // zfar / znear, zfar

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

// depth is GL_DEPTH_COMPONENT24
// so the maximum error is 1.0 / 2^24
#define DEPTH_MAX_ERROR 0.000000059604644775390625

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

float PCF(const sampler2DShadow shadowmap, const vec2 st, const float dist)
{
	float mult;
	float scale = 2.0 / r_shadowMapSize;

#if 0
	// from http://http.developer.nvidia.com/GPUGems/gpugems_ch11.html
	vec2 offset = vec2(greaterThan(fract(var_DepthTex.xy * r_FBufScale * 0.5), vec2(0.25)));
	offset.y += offset.x;
	if (offset.y > 1.1) offset.y = 0.0;
	
	mult = shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5, -1.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5, -1.5)) * scale, dist));
	 
	mult *= 0.25;
#endif

#if defined(USE_SHADOW_FILTER)
	float r = random(var_DepthTex.xy);
	float sinr = sin(r) * scale;
	float cosr = cos(r) * scale;
	mat2 rmat = mat2(cosr, sinr, -sinr, cosr);

	mult =  shadow2D(shadowmap, vec3(st + rmat * vec2(-0.7055767, 0.196515), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.3524343, -0.7791386), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.2391056, 0.9189604), dist));
  #if defined(USE_SHADOW_FILTER2)
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.07580382, -0.09224417), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.5784913, -0.002528916), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.192888, 0.4064181), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.6335801, -0.5247476), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.5579782, 0.7491854), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.7320465, 0.6317794), dist));

	mult *= 0.11111;
  #else
    mult *= 0.33333;
  #endif
#else
	mult = shadow2D(shadowmap, vec3(st, dist));
#endif

	return mult;
}

float getLinearDepth(sampler2D depthMap, vec2 tex, float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r - DEPTH_MAX_ERROR;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

void main()
{
	float result;

	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);
	// [user 2026-09-21] REVERTED to stock 0.5. The 3.0 receiver bias pushed the reconstructed receiver so
	// far along the view ray that the sampled shadow depth became strongly camera-relative: shadows SLID
	// with the camera and detached from rubble. Screen-space receiver-plane bias is the wrong lever for
	// shadow-bleed-through-rubble (that is a resolution/self-shadow issue); leave at 0.5.
	// NOTE: keep this comment quote-free - stringify.cpp wraps each glsl LINE in a C string literal, so an
	// embedded double quote breaks the generated shadowmask_fp.c (build error C2146, seen 2026-09-21).
	vec4 biasPos = vec4(u_ViewOrigin + var_ViewDir * (depth - 0.5 / u_ViewInfo.x), 1.0);

	vec4 shadowpos = u_ShadowMvp * biasPos;

	if ( depth >= 0.999 )
	{
		result = 1.0;
	}
	else
#if defined(USE_SHADOW_CASCADE)
	if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
#endif
	{
		shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
		result = PCF(u_ShadowMap, shadowpos.xy, shadowpos.z);
	}
#if defined(USE_SHADOW_CASCADE)
	else
	{
		shadowpos = u_ShadowMvp2 * biasPos;

		if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
		{
			shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
			result = PCF(u_ShadowMap2, shadowpos.xy, shadowpos.z);
		}
		else
		{
			shadowpos = u_ShadowMvp3 * biasPos;

			if (all(lessThan(abs(shadowpos.xyz), vec3(abs(shadowpos.w)))))
			{
				shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
				result = PCF(u_ShadowMap3, shadowpos.xy, shadowpos.z);
			}
			else
			{
				shadowpos = u_ShadowMvp4 * biasPos;
				shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
				result = PCF(u_ShadowMap4, shadowpos.xy, shadowpos.z);
			}
		}
	}
#endif

	gl_FragColor = vec4(vec3(result), 1.0);
}
#endif
