uniform sampler2D u_DiffuseMap;

#if defined(USE_LIGHTMAP)
uniform sampler2D u_LightMap;
#endif

#if defined(USE_NORMALMAP)
uniform sampler2D u_NormalMap;
#endif

#if defined(USE_DELUXEMAP)
uniform sampler2D u_DeluxeMap;
#endif

#if defined(USE_SPECULARMAP)
uniform sampler2D u_SpecularMap;
#endif

#if defined(USE_SHADOWMAP)
uniform sampler2D u_ShadowMap;
#endif

#if defined(USE_CUBEMAP)
uniform samplerCube u_CubeMap;
#endif

#if defined(USE_NORMALMAP) || defined(USE_DELUXEMAP) || defined(USE_SPECULARMAP) || defined(USE_CUBEMAP)
// y = deluxe, w = cube
uniform vec4      u_EnableTextures; 
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
uniform vec3  u_PrimaryLightColor;
uniform vec3  u_PrimaryLightAmbient;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
uniform vec4      u_NormalScale;
uniform vec4      u_SpecularScale;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
#if defined(USE_CUBEMAP)
uniform vec4      u_CubeMapInfo;
#endif
#endif

uniform int       u_AlphaTest;

varying vec4      var_TexCoords;

varying vec4      var_Color;
#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4      var_ColorAmbient;
#endif

#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4   var_Normal;
varying vec4   var_Tangent;
varying vec4   var_Bitangent;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4      var_LightDir;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
varying vec4      var_PrimaryLightDir;
#endif

// HZM gl2 FORWARD GLOBAL FOG (r_globalFogForward, bug-1306). MOHAA global farplane distance
// fog, mixed here in the surface shader instead of in a screen-space depth pass, which is the
// order gl1 uses: gl1 bakes fixed-function fog during rasterisation and only THEN runs its
// grade, so a fogged pixel reaches the screen as GRADE(mix(scene, fogColor)). gl2 tone stage
// IS that grade (glsl/tonemap_hzm_fp.glsl is a port of renderergl1 TONEMAP_FS), so mixing
// toward the RAW farplane colour here lands on the identical byte. Do not pre-invert the
// constant - an inverse grade is exposure-dependent and would make the horizon pump.
//
// Distance comes from gl_FragCoord.z, reconstructed with the same terms the screen pass uses:
//   z_ndc = 2*zw - 1,   d = P14 / (P10 + z_ndc)      both terms negative, so d is positive
// gl_FragCoord is a built-in, so this costs no varying, no vertex-shader change and no new
// permutation. Unlike the screen pass this reads the PRIMITIVE BEING RASTERISED, never the
// depth buffer, so a blendfunc surface that writes no depth (an explosion sprite, the glider
// windscreen) fogs by its own distance instead of being scored at the sky and erased/skipped.
//
// u_GlobalFogColor  = (fogTarget.rgb, fracScale)   fracScale 0 = fog off for this draw
// u_GlobalFogParams = (projMat[10], projMat[14], fogStart, 1/(fogEnd-fogStart))
// The colour is per-STAGE, classified from the stage blendFunc on the CPU
// (RB_SetGlobalFogUniforms): additive stages fog toward BLACK, modulate toward WHITE,
// everything else toward the fog colour - fogging an additive stage toward grey would
// BRIGHTEN it with distance. Alpha is deliberately untouched (GL spec: fog does not modify
// alpha), so coverage, silhouettes and the alpha test are identical fogged or not.
// HZM coop [2026-09-27] realistic lightning: outdoor surface light (see the use below main)
uniform vec4      u_HzmLtWorld;
uniform vec4      u_GlobalFogColor;
uniform vec4      u_GlobalFogParams;

// HZM coop [2026-09-27] realistic lightning: fog IN-SCATTER toward the strike (tr_hzm_lightning.c). The lit cloud
// region lights the haze between it and the eye, so fogged distance in the strike's direction brightens in a 35-degree
// lobe - never a flat band. u_HzmLtFog = (strike direction in EYE space, energy); w 0 = inert (the default: every
// uniform starts at 0), so with lightning off the fog colour below is exactly u_GlobalFogColor.rgb.
uniform vec4      u_HzmLtFog;
uniform vec4      u_HzmLtView;
uniform vec4      u_HzmLtProj;

vec3 HzmLtFogColor()
{
	vec3 fogCol = u_GlobalFogColor.rgb;

	if (u_HzmLtFog.w > 0.0)
	{
		vec2  ndc = (gl_FragCoord.xy - u_HzmLtView.xy) * u_HzmLtView.zw * 2.0 - 1.0;
		vec3  ray = normalize(vec3(ndc.x * u_HzmLtProj.x, ndc.y * u_HzmLtProj.y, -1.0));
		float ang = acos(clamp(dot(ray, u_HzmLtFog.xyz), -1.0, 1.0));
		float lob = exp(-0.5 * ang * ang / (0.611 * 0.611));

		fogCol += vec3(0.86, 0.90, 1.0) * (u_HzmLtFog.w * (lob + 0.10));
	}

	return fogCol;
}

vec3 ApplyGlobalFog(vec3 color)
{
	if (u_GlobalFogColor.a <= 0.0)
	{
		return color;
	}

	float denom = u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0);
	float dist  = u_GlobalFogParams.y / min(denom, -1e-6);
	float frac  = clamp((dist - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0);

	frac = clamp(frac * u_GlobalFogColor.a, 0.0, 1.0);

	// bug-1299 CLAMP, carried into the forward mix. gl1 blends into a fixed-point backbuffer
	// so its operand is inherently at most 1.0; here the operand sits in the float HDR FBO
	// scaled by tr.overbrightMult, and mixing unclamped SKIPS fog on every bright surface -
	// mix(2.0, 0.6, 0.5) = 1.3, still white. That is the distant-trees-render-white defect,
	// relocated. STEP, not ramp: any fogged fragment (frac above 0) gets the clamped operand,
	// which is byte-exact against gl1 at every fraction; an unfogged fragment (frac == 0)
	// keeps full HDR so near-field highlights and bloom are untouched. A frac-proportional
	// ramp was measured +8..+13/255 too bright at frac 0.125-0.2 and rejected.
	vec3 operand = (frac > 0.0) ? clamp(color, 0.0, 1.0) : color;

	return mix(operand, HzmLtFogColor(), frac);
}

// HZM gl2 soft particles (r_softParticles). u_ScreenDepthMap is the scene-depth snapshot bound on
// TB_SCREENDEPTH (TMU 7); u_InvTexRes is 1/scene-FBO size so uv = gl_FragCoord.xy * u_InvTexRes.
// u_SoftParticle = (1/fadeDistance, projMat[10], projMat[14], mode). mode 0 leaves this inert, so
// with r_softParticles off the fragment output is byte-identical. Body kept identical to the copy
// in lightall_fp.glsl (these two fragment shaders are stringified independently - no includes).
uniform sampler2D u_ScreenDepthMap;
uniform vec2      u_InvTexRes;
uniform vec4      u_SoftParticle;

void ApplySoftParticle(inout vec3 rgb, inout float a)
{
	if (u_SoftParticle.w < 0.5)
	{
		return;
	}

	float zs = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).r;
	float ds = u_SoftParticle.z / min(u_SoftParticle.y + (2.0 * zs - 1.0), -1e-6);
	float df = u_SoftParticle.z / min(u_SoftParticle.y + (2.0 * gl_FragCoord.z - 1.0), -1e-6);
	float k  = clamp((ds - df) * u_SoftParticle.x, 0.0, 1.0);

	if (u_SoftParticle.w > 3.5)
	{
		rgb = vec3(k);                // debug: the fade factor as greyscale
		a   = 1.0;
	}
	else if (u_SoftParticle.w > 2.5)
	{
		rgb = mix(vec3(1.0), rgb, k); // mode 3: modulate, fade toward white
	}
	else if (u_SoftParticle.w > 1.5)
	{
		rgb *= k;                     // mode 2: additive, fade toward black
	}
	else
	{
		a *= k;                       // mode 1: alpha blend
	}
}

#define EPSILON 0.00000001

#if defined(USE_PARALLAXMAP)
float SampleDepth(sampler2D normalMap, vec2 t)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - texture2D(normalMap, t).r;
  #else
	return 1.0 - texture2D(normalMap, t).a;
  #endif
}

float RayIntersectDisplaceMap(vec2 dp, vec2 ds, sampler2D normalMap)
{
	const int linearSearchSteps = 16;
	const int binarySearchSteps = 6;

	// current size of search window
	float size = 1.0 / float(linearSearchSteps);

	// adjust position if offset above surface
	dp -= ds * r_parallaxMapOffset;

	// current depth position
	float depth = 0.0;

	// best match found (starts with last position 1.0)
	float bestDepth = 1.0;

	// texture depth at best depth
	float texDepth = 0.0;

	float prevT = SampleDepth(normalMap, dp);
	float prevTexDepth = prevT;

	// search front to back for first point inside object
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		depth += size;
		
		float t = SampleDepth(normalMap, dp + ds * depth);
		
		if(bestDepth > 0.996)		// if no depth found yet
			if(depth >= t)
			{
				bestDepth = depth;	// store best depth
				texDepth = t;
				prevTexDepth = prevT;
			}
		prevT = t;
	}

	depth = bestDepth;

#if !defined (USE_RELIEFMAP)
	float div = 1.0 / (1.0 + (prevTexDepth - texDepth) * float(linearSearchSteps));
	bestDepth -= (depth - size - prevTexDepth) * div;
#else
	// recurse around first point (depth) for closest match
	for(int i = 0; i < binarySearchSteps; ++i)
	{
		size *= 0.5;

		float t = SampleDepth(normalMap, dp + ds * depth);
		
		if(depth >= t)
		{
			bestDepth = depth;
			depth -= 2.0 * size;
		}

		depth += size;
	}
#endif

	return bestDepth - r_parallaxMapOffset;
}

float LightRay(vec2 dp, vec2 ds, sampler2D normalMap)
{
	const int linearSearchSteps = 16;

	// current size of search window
	float size = 1.0 / float(linearSearchSteps);

	// current height from initial texel depth
	float height = 0.0;

	float startDepth = SampleDepth(normalMap, dp);

	// find a collision or escape
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		height += size;

		if (startDepth < height)
			return 1.0;
		
		float t = SampleDepth(normalMap, dp + ds * height);

		if (startDepth > t + height)
			return 0.0;
	}

	return 1.0;
}
#endif

vec3 CalcDiffuse(vec3 diffuseAlbedo, float NH, float EH, float roughness)
{
#if defined(USE_BURLEY)
	// modified from https://disney-animation.s3.amazonaws.com/library/s2012_pbs_disney_brdf_notes_v2.pdf
	float fd90 = -0.5 + EH * EH * roughness;
	float burley = 1.0 + fd90 * 0.04 / NH;
	burley *= burley;
	return diffuseAlbedo * burley;
#else
	return diffuseAlbedo;
#endif
}

vec3 EnvironmentBRDF(float roughness, float NE, vec3 specular)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float v = 1.0 - max(roughness, NE);
	v *= v * v;
	return vec3(v) + specular;
}

vec3 CalcSpecular(vec3 specular, float NH, float EH, float roughness)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float rr = roughness*roughness;
	float rrrr = rr*rr;
	float d = (NH * NH) * (rrrr - 1.0) + 1.0;
	float v = (EH * EH) * (roughness + 0.5) + EPSILON;
	return specular * (rrrr / (4.0 * d * d * v));
}


float CalcLightAttenuation(float point, float normDist)
{
	// zero light at 1.0, approximating q3 style
	// also don't attenuate directional light
	float attenuation = (0.5 * normDist - 1.5) * point + 1.0;

	// clamp attenuation
	#if defined(NO_LIGHT_CLAMP)
	attenuation = max(attenuation, 0.0);
	#else
	attenuation = clamp(attenuation, 0.0, 1.0);
	#endif

	return attenuation;
}

#if defined(USE_LIGHT_VECTOR) && !defined(USE_FAST_LIGHT)
// HZM gl2 [2026-09-26] Phase S1 SPOT CONE (renderergl2/tr_hzm_spot.c). u_HzmLightSpot = (cone axis * k, cosOuter),
// k = 1 / (cosInner - cosOuter); cosOuter + 4 = the r_hzmSpotDebug 2 magenta tint. ALL ZERO = an omni light, which is
// what every other draw of this program carries (GPU uniforms start at 0): the cone is then exactly 1.0 and the tint
// exactly vec3(1.0). The same two helpers are in lightall_vp, lightall_fp and dlight_fp - each file is stringified on
// its own (no includes), and none of them may contain a double quote.
uniform vec4      u_HzmLightSpot;

float HzmSpotCone(vec3 toSurf)
{
	float k = length(u_HzmLightSpot.xyz);
	if (k <= 0.0)
		return 1.0;
	float cosOuter = u_HzmLightSpot.w;
	if (cosOuter > 2.0)
		cosOuter -= 4.0;
	float t = clamp((dot(toSurf, u_HzmLightSpot.xyz) / k - cosOuter) * k, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

vec3 HzmSpotTint()
{
	return (u_HzmLightSpot.w > 2.0) ? vec3(1.0, 0.15, 1.0) : vec3(1.0);
}
#endif

#if defined(USE_BOX_CUBEMAP_PARALLAX)
vec4 hitCube(vec3 ray, vec3 pos, vec3 invSize, float lod, samplerCube tex)
{
	// find any hits on cubemap faces facing the camera
	vec3 scale = (sign(ray) - pos) / ray;

	// find the nearest hit
	float minScale = min(min(scale.x, scale.y), scale.z);

	// if the nearest hit is behind the camera, ignore
	// should not be necessary as long as pos is inside the cube
	//if (minScale < 0.0)
		//return vec4(0.0);

	// calculate the hit position, that's our texture coordinates
	vec3 tc = pos + ray * minScale;

	// if the texture coordinates are outside the cube, ignore
	// necessary since we're not fading out outside the cube
	if (any(greaterThan(abs(tc), vec3(1.00001))))
		return vec4(0.0);

	// fade out when approaching the cubemap edges
	//vec3 fade3 = abs(pos);
	//float fade = max(max(fade3.x, fade3.y), fade3.z);
	//fade = clamp(1.0 - fade, 0.0, 1.0);
			
	//return vec4(textureCubeLod(tex, tc, lod).rgb * fade, fade);
	return vec4(textureCubeLod(tex, tc, lod).rgb, 1.0);
}
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
// HZM rain wetness (r_hzmWet, tr_hzm_wet.c; docs/proposals/water_wetness_2026-09-27 plan W4). The offline twin that
// fixed every constant below is tools/ww_look.py hzm_wet. u_HzmWet.w == 0 (every draw that is not the first opaque
// stage of a tagged world surface while it rains, and every draw with r_hzmWet off) skips the whole block, so the
// output is unchanged byte for byte. The block is a UNIFORM branch: the implicit-LOD fetches inside it are legal.
uniform vec3      u_ViewOrigin;
uniform vec4      u_HzmWet;       // film, puddle, time, on
uniform vec4      u_HzmWetMat;    // darken, wet roughness, puddles allowed, sealed
uniform vec4      u_HzmWetOcc;    // occlusion map world->uv (x0, y0, 1/width, 1/height)
uniform vec4      u_HzmWetOccZ;   // zmin, zrange, puddle threshold, lightmap sheen
uniform vec4      u_HzmWetSkyZ;   // sky zenith rgb
uniform vec4      u_HzmWetSkyH;   // sky horizon rgb
uniform vec4      u_HzmWetSun;    // toward-sun xyz, sun present
uniform vec4      u_HzmWetSunCol; // sun rgb
uniform sampler2D u_HzmOccMap;
uniform sampler2D u_HzmWetNoise;

// sky exposure 0..1 at world point p: the top-down max-height map, soft compare
float HzmOccExposure(vec3 p, float soft)
{
	float h = texture2D(u_HzmOccMap, (p.xy - u_HzmWetOcc.xy) * u_HzmWetOcc.zw).r * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
	return smoothstep(-soft, 2.0, p.z - h + 2.0);
}

// procedural rain rings: 20 u cells, one drop per cell per ~0.9 s; returns the xy tilt
vec2 HzmRainRings(vec2 p, float t)
{
	vec2 acc = vec2(0.0);
	vec2 c0 = floor(p / 20.0);
	for (int oy = -1; oy <= 1; oy++)
	{
		for (int ox = -1; ox <= 1; ox++)
		{
			vec2  hc  = c0 + vec2(float(ox), float(oy));
			float r1  = fract(sin(dot(hc, vec2(127.1, 311.7))) * 43758.5453);
			float r2  = fract(sin(dot(hc, vec2(269.5, 183.3))) * 43758.5453);
			vec2  dc  = (hc + vec2(0.25) + 0.5 * vec2(r1, r2)) * 20.0;
			float fr  = fract(t / 0.9 + r1 * 7.0);
			vec2  d   = p - dc;
			float dd  = length(d) + 0.001;
			float x   = dd - fr * 11.0;
			float rng = sin(x * 1.6) * exp(-(x * x) / 6.0) * (1.0 - fr) * (1.0 - fr);
			acc += rng * d / dd;
		}
	}
	return acc;
}

// the wet term: darkens albedo, flattens N, and returns the film reflectance F, film roughness r, sun glint and the
// reflected (fogged) sky for the composite at the end of main()
void HzmWet(vec3 viewDir, vec3 E, vec3 n, vec3 lm, inout vec3 albedo, inout vec3 N,
            out float F, out float r, out float glint, out vec3 env)
{
	vec3  P    = u_ViewOrigin - viewDir;
	float dist = length(viewDir);
	float fp   = max(length(dFdx(P.xy)), length(dFdy(P.xy)));   // pixel footprint, world units

	// exposure: floors test straight up; walls march a rain ray slanted 20 deg out of the wall (4 taps), so an eave
	// only shelters the strip beneath it
	float upf = smoothstep(0.3, 0.9, n.z);
	float ef  = HzmOccExposure(vec3(P.xy + n.xy * 4.0, P.z + 4.0), 32.0);
	vec2  hn  = normalize(n.xy + vec2(0.00001, 0.0));
	float ew  = min(min(HzmOccExposure(vec3(P.xy + hn * 26.0, P.z + 72.2), 24.0),
	                    HzmOccExposure(vec3(P.xy + hn * 44.0, P.z + 122.2), 24.0)),
	                min(HzmOccExposure(vec3(P.xy + hn * 62.0, P.z + 172.2), 24.0),
	                    HzmOccExposure(vec3(P.xy + hn * 80.0, P.z + 222.2), 24.0)));
	float e   = mix(ew, ef, upf);
	float wf  = u_HzmWet.x * e * (0.55 + 0.45 * upf);   // walls take less rain

	// puddles: flat, exposed, allowed; the mask edge is at least one pixel wide (no crawling rims)
	float nse = texture2D(u_HzmWetNoise, P.xy / 2048.0).r;
	float lvl = u_HzmWet.y * e;
	float thr = u_HzmWetOccZ.z * lvl;
	float edg = 0.03 + 0.5 * fp / 64.0;
	float pm  = (1.0 - smoothstep(thr - edg, thr + edg, nse)) * smoothstep(0.96, 0.99, n.z) * u_HzmWetMat.z * step(0.01, lvl);

	// [in-engine tune 2026-09-28, docs/proposals/water_wetness_2026-09-27/ingame/INGAME.md] NIGHT: under a dark sky
	// a puddle reflects almost nothing, so its extra darkening and flattening read as black, detail-less patches (m4l2
	// full puddles measured P1 0.70, P2 0.58). The sky the surface reflects - the horizon, fogged as the visible sky -
	// decides it: at night a puddle darkens no more than the film around it and keeps half its relief, and the film
	// darkens 10 % less. m4l2's fogged night sky is 0.20, the day maps 0.33 and up: day is unchanged.
	float hzF = 0.0;
	if (u_GlobalFogColor.a > 0.0)
	{
		float zf0 = u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
		hzF = clamp((zf0 - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a;
	}
	float night = 1.0 - smoothstep(0.22, 0.32, dot(mix(u_HzmWetSkyH.rgb, u_GlobalFogColor.rgb, hzF), vec3(0.299, 0.587, 0.114)));

	// darken + roughness by class; cavities (darker texels) hold water first
	float dk  = 1.0 + (u_HzmWetMat.x - 1.0) * wf * (1.0 - 0.1 * night);
	dk = mix(dk, mix(u_HzmWetMat.x * 0.72, dk, night), pm);
	r  = 1.0 + (u_HzmWetMat.y - 1.0) * wf;
	float al  = dot(albedo, vec3(0.299, 0.587, 0.114));
	float cav = 1.0 - smoothstep(0.12, 0.55, al);
	r   = r - 0.14 * cav * wf;
	dk *= 1.0 - 0.10 * cav * wf;
	r   = mix(r, 0.03, pm);
	albedo *= dk;

	// water fills the relief; a puddle is flat
	N = normalize(mix(N, n, clamp(0.5 * wf + pm * (1.0 - 0.5 * night), 0.0, 1.0)));

	// reflection normal: the surface's own, plus rain rings in puddles, faded out before they go sub-pixel
	vec3 Nr = n;
	if (pm > 0.01)
	{
		float fd = (1.0 - smoothstep(350.0, 900.0, dist)) * (1.0 - smoothstep(1.17, 2.73, fp));
		vec2  rg = HzmRainRings(P.xy, u_HzmWet.z) * (0.22 * fd * pm * u_HzmWet.x);
		Nr = normalize(n + vec3(rg, 0.0));
	}

	// Fresnel-Schlick-roughness: a rough film can never reach mirror reflectance at grazing (the anti-plastic term)
	float ndv = max(dot(Nr, E), 0.001);
	F  = 0.02 + (max(1.0 - r, 0.02) - 0.02) * pow(1.0 - ndv, 5.0);
	F *= e * max(wf, pm) * (1.0 - u_HzmWetMat.w);

	// the sky it reflects, fogged exactly as gl2 fogs the visible sky at zFar (r_globalFogSky)
	vec3 R = reflect(-E, Nr);
	R.z = max(R.z, 0.03);
	R = normalize(R);
	env = mix(u_HzmWetSkyH.rgb, u_HzmWetSkyZ.rgb, smoothstep(-0.3 * r, 0.6 + 0.3 * r, R.z));
	float haze = 0.0;
	if (u_GlobalFogColor.a > 0.0)
	{
		float zf = u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
		haze = clamp((zf - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a;
	}
	env = mix(env, u_GlobalFogColor.rgb, haze);

	// sun / moon glint: roughness floor 0.26 (a narrower lobe sparkles in motion), gated by exposure and by the baked
	// light (no glint in lightmap shadow), widened away by haze, never on sealed surfaces
	vec3  Ls = u_HzmWetSun.xyz;
	vec3  Hs = normalize(Ls + E);
	float a  = max(r * r, 0.068);
	float nh = max(dot(Nr, Hs), 0.0);
	float nl = max(dot(Nr, Ls), 0.0);
	float d2 = nh * nh * (a - 1.0) + 1.0;
	float fh = 0.02 + 0.98 * pow(1.0 - max(dot(Hs, E), 0.0), 5.0);
	float sv = smoothstep(0.35, 0.9, dot(lm, vec3(0.299, 0.587, 0.114)));
	glint = a / (3.14159265 * d2 * d2) * fh * nl / (4.0 * ndv + 0.001);
	glint *= e * sv * max(wf, pm) * (1.0 - 0.8 * haze) * u_HzmWetSun.w * (1.0 - u_HzmWetMat.w);
	glint = min(glint, 1.5);
}
#endif

void main()
{
	vec3 viewDir, lightColor, ambientColor, reflectance;
	vec3 L, N, E, H;
	float NL, NH, NE, EH, attenuation;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
	mat3 tangentToWorld = mat3(var_Tangent.xyz, var_Bitangent.xyz, surfNormal);
	viewDir = vec3(var_Normal.w, var_Tangent.w, var_Bitangent.w);
	E = normalize(viewDir);
#endif

	lightColor = var_Color.rgb;

#if defined(USE_LIGHTMAP)
	vec4 lightmapColor = texture2D(u_LightMap, var_TexCoords.zw);
  #if defined(RGBM_LIGHTMAP)
	lightmapColor.rgb *= lightmapColor.a;
  #endif
  #if defined(USE_PBR) && !defined(USE_FAST_LIGHT)
	lightmapColor.rgb *= lightmapColor.rgb;
  #endif
	lightColor *= lightmapColor.rgb;
#endif

	vec2 texCoords = var_TexCoords.xy;

#if defined(USE_PARALLAXMAP)
	// HZM [user 2026-08-28] DISTANCE FALLOFF. Stock rend2 raymarches at full strength to the horizon,
	// and because the 16 taps land on different texels for neighbouring pixels, distant ground turns to
	// static. u_NormalScale.z is the range at which the effect reaches zero; <= 1.0 means 'no fade',
	// which is what every stage the HZM material hook does not touch still carries.
	{
		float pxFade = 1.0;

		if (u_NormalScale.z > 1.0)
		{
			// hold full strength over the near half, then ease out - a linear ramp from the eye is
			// visible as a gradient on a flat floor, which is worse than the artefact it fixes
			float d = length(viewDir);
			pxFade = clamp(1.0 - (d - u_NormalScale.z * 0.5) / (u_NormalScale.z * 0.5), 0.0, 1.0);
			pxFade *= pxFade;   // squared, so it is already small well before it is gone
		}

		if (pxFade > 0.004)
		{
			vec3 offsetDir = E * tangentToWorld;

			offsetDir.xy *= -u_NormalScale.a * pxFade / offsetDir.z;

			texCoords += offsetDir.xy * RayIntersectDisplaceMap(texCoords, offsetDir.xy, u_NormalMap);
		}
	}
#endif

	vec4 diffuse = texture2D(u_DiffuseMap, texCoords);
	vec3 hzmLtAlbedo = diffuse.rgb;   // HZM coop [2026-09-27] lightning: the surface colour the flash lights
	float hzmLtVis = 0.0;             // HZM coop [2026-09-27] lightning: open to the sky (the sun mask), 0 without one
	
	float alpha = diffuse.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	L = var_LightDir.xyz;
  #if defined(USE_DELUXEMAP)
	L += (texture2D(u_DeluxeMap, var_TexCoords.zw).xyz - vec3(0.5)) * u_EnableTextures.y;
  #endif
	float sqrLightDist = dot(L, L);
	L /= sqrt(sqrLightDist);

  #if defined(USE_LIGHT_VECTOR)
	attenuation  = CalcLightAttenuation(float(var_LightDir.w > 0.0), var_LightDir.w / sqrLightDist);
	// HZM gl2 [2026-09-26] Phase S1: the spot cone per pixel (L is the unit vector to the light here)
	attenuation *= HzmSpotCone(-L);
	lightColor  *= HzmSpotTint();
  #else
	attenuation  = 1.0;
  #endif

  #if defined(USE_NORMALMAP)
    #if defined(SWIZZLE_NORMALMAP)
	N.xy = texture2D(u_NormalMap, texCoords).ag - vec2(0.5);
    #else
	N.xy = texture2D(u_NormalMap, texCoords).rg - vec2(0.5);
    #endif
	N.xy *= u_NormalScale.xy;
	N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
	N = tangentToWorld * N;
  #else
	N = surfNormal;
  #endif

	N = normalize(N);

	// HZM rain wetness (see HzmWet above): darkens the albedo and flattens N BEFORE the lighting below
	vec3  hzmLm  = lightColor;
	float hzmF   = 0.0;
	float hzmR   = 1.0;
	float hzmG   = 0.0;
	vec3  hzmEnv = vec3(0.0);
	if (u_HzmWet.w > 0.0)
	{
		vec3 hzmAlb = diffuse.rgb;
		HzmWet(viewDir, E, surfNormal, hzmLm, hzmAlb, N, hzmF, hzmR, hzmG, hzmEnv);
		diffuse.rgb = hzmAlb;
	}

  #if defined(USE_SHADOWMAP) 
	vec2 shadowTex = gl_FragCoord.xy * r_FBufScale;
	float shadowValue = texture2D(u_ShadowMap, shadowTex).r;
	// HZM [bug-3171] the sun mask means 'open to the sky' only on a surface that FACES the sun. Facing away, it reads
	// the wall's own sun-side face through the shadow bias (a thin wall's interior face reads lit), so the flash lit
	// interior walls whose rays to the sun AND to the strike enter the wall itself. Gated on the geometric normal,
	// before the sun N.L below (the flash comes from the strike, not the sun).
	hzmLtVis = shadowValue * smoothstep(0.02, 0.20, dot(normalize(surfNormal), normalize(var_PrimaryLightDir.xyz)));

	// surfaces not facing the light are always shadowed
	shadowValue *= clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0);

    #if defined(SHADOWMAP_MODULATE)
	// HZM coop [2026-09-28] storm darkness: .b / .r is the SUN'S VISIBILITY (1 = the map's sun; tr_scene.c lowers .b
	// under a storm deck). Only the sun's share fades: a shadowed texel keeps its ambient share, nothing brightens.
	float hzmSunVis = (u_PrimaryLightAmbient.r > 0.0) ? clamp(u_PrimaryLightAmbient.b / u_PrimaryLightAmbient.r, 0.0, 1.0) : 1.0;
	lightColor *= shadowValue * (1.0 - u_PrimaryLightAmbient.r) * hzmSunVis + u_PrimaryLightAmbient.r;
    #endif
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	// HZM [2026-09-26] DECLARED here: the parallax distance fade above keeps its own offsetDir in a nested block, so
	// this path (and the primary-light copy below, only ever active with this one) used an undeclared name - a
	// fatal compile with r_parallaxMapping + r_parallaxMapShadows. Found by docs/tools/glsl_variant_lint.py.
	vec3 offsetDir = L * tangentToWorld;
	offsetDir.xy *= u_NormalScale.a / offsetDir.z;
	lightColor *= LightRay(texCoords, offsetDir.xy, u_NormalMap);
  #endif


  #if !defined(USE_LIGHT_VECTOR)
	ambientColor = lightColor;
	float surfNL = clamp(dot(surfNormal, L), 0.0, 1.0);

	// reserve 25% ambient to avoid black areas on normalmaps
	lightColor *= 0.75;

	// Scale the incoming light to compensate for the baked-in light angle
	// attenuation.
	lightColor /= max(surfNL, 0.25);

	// Recover any unused light as ambient, in case attenuation is over 4x or
	// light is below the surface
	ambientColor = max(ambientColor - lightColor * surfNL, vec3(0.0));
  #else
	ambientColor = var_ColorAmbient.rgb;
  #endif

	NL = clamp(dot(N, L), 0.0, 1.0);
	NE = clamp(dot(N, E), 0.0, 1.0);
	H = normalize(L + E);
	EH = clamp(dot(E, H), 0.0, 1.0);
	NH = clamp(dot(N, H), 0.0, 1.0);

  #if defined(USE_SPECULARMAP)
	vec4 specular = texture2D(u_SpecularMap, texCoords);
  #else
	vec4 specular = vec4(1.0);
  #endif
	specular *= u_SpecularScale;

  #if defined(USE_PBR)
	diffuse.rgb *= diffuse.rgb;
  #endif

  #if defined(USE_PBR)
	// diffuse rgb is base color
	// specular red is gloss
	// specular green is metallicness
	float gloss = specular.r;
	float metal = specular.g;
	specular.rgb = metal * diffuse.rgb + vec3(0.04 - 0.04 * metal);
	diffuse.rgb *= 1.0 - metal;
  #else
	// diffuse rgb is diffuse
	// specular rgb is specular reflectance at normal incidence
	// specular alpha is gloss
	float gloss = specular.a;

	// adjust diffuse by specular reflectance, to maintain energy conservation
	diffuse.rgb *= vec3(1.0) - specular.rgb;
  #endif

  #if defined(GLOSS_IS_GLOSS)
	float roughness = exp2(-3.0 * gloss);
  #elif defined(GLOSS_IS_SMOOTHNESS)
	float roughness = 1.0 - gloss;
  #elif defined(GLOSS_IS_ROUGHNESS)
	float roughness = gloss;
  #elif defined(GLOSS_IS_SHININESS)
	float roughness = pow(2.0 / (8190.0 * gloss + 2.0), 0.25);
  #endif

	reflectance  = CalcDiffuse(diffuse.rgb, NH, EH, roughness);

  #if defined(r_deluxeSpecular)
    #if defined(USE_LIGHT_VECTOR)
	reflectance += CalcSpecular(specular.rgb, NH, EH, roughness) * r_deluxeSpecular;
    #else
	reflectance += CalcSpecular(specular.rgb, NH, EH, pow(roughness, r_deluxeSpecular));
    #endif
  #endif

	gl_FragColor.rgb  = lightColor   * reflectance * (attenuation * NL);
	gl_FragColor.rgb += ambientColor * diffuse.rgb;

  #if defined(USE_CUBEMAP)
	reflectance = EnvironmentBRDF(roughness, NE, specular.rgb);

	vec3 R = reflect(E, N);

	// parallax corrected cubemap (cheaper trick)
	// from http://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
	vec3 parallax = u_CubeMapInfo.xyz + u_CubeMapInfo.w * viewDir;

  #if defined(USE_BOX_CUBEMAP_PARALLAX)
	vec3 cubeLightColor = hitCube(R * u_CubeMapInfo.w, parallax, u_CubeMapInfo.www, ROUGHNESS_MIPS * roughness, u_CubeMap).rgb * u_EnableTextures.w;
  #else
	vec3 cubeLightColor = textureCubeLod(u_CubeMap, R + parallax, ROUGHNESS_MIPS * roughness).rgb * u_EnableTextures.w;
  #endif

	// normalize cubemap based on last roughness mip (~diffuse)
	// multiplying cubemap values by lighting below depends on either this or the cubemap being normalized at generation
	//vec3 cubeLightDiffuse = max(textureCubeLod(u_CubeMap, N, ROUGHNESS_MIPS).rgb, 0.5 / 255.0);
	//cubeLightColor /= dot(cubeLightDiffuse, vec3(0.2125, 0.7154, 0.0721));

    #if defined(USE_PBR)
	cubeLightColor *= cubeLightColor;
    #endif

	// multiply cubemap values by lighting
	// not technically correct, but helps make reflections look less unnatural
	//cubeLightColor *= lightColor * (attenuation * NL) + ambientColor;

	gl_FragColor.rgb += cubeLightColor * reflectance;
  #endif

  #if defined(USE_PRIMARY_LIGHT) || defined(SHADOWMAP_MODULATE)
	vec3 L2, H2;
	float NL2, EH2, NH2;

	L2 = var_PrimaryLightDir.xyz;

	// enable when point lights are supported as primary lights
	//sqrLightDist = dot(L2, L2);
	//L2 /= sqrt(sqrLightDist);

	NL2 = clamp(dot(N, L2), 0.0, 1.0);
	H2 = normalize(L2 + E);
	EH2 = clamp(dot(E, H2), 0.0, 1.0);
	NH2 = clamp(dot(N, H2), 0.0, 1.0);

	reflectance  = CalcSpecular(specular.rgb, NH2, EH2, roughness);

	// bit of a hack, with modulated shadowmaps, ignore diffuse
    #if !defined(SHADOWMAP_MODULATE)
	reflectance += CalcDiffuse(diffuse.rgb, NH2, EH2, roughness);
    #endif

	lightColor = u_PrimaryLightColor;

    #if defined(USE_SHADOWMAP)
	lightColor *= shadowValue;
    #endif

	// enable when point lights are supported as primary lights
	//lightColor *= CalcLightAttenuation(float(u_PrimaryLightDir.w > 0.0), u_PrimaryLightDir.w / sqrLightDist);

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	offsetDir = L2 * tangentToWorld;
	offsetDir.xy *= u_NormalScale.a / offsetDir.z;
	lightColor *= LightRay(texCoords, offsetDir.xy, u_NormalMap);
  #endif

	gl_FragColor.rgb += lightColor * reflectance * NL2;
  #endif

  #if defined(USE_PBR)
	gl_FragColor.rgb = sqrt(gl_FragColor.rgb);
  #endif

	// HZM rain wetness composite: the film reflects the fogged sky, the baked light around it (plan D6 sheen) and the
	// sun; all of it before the forward global fog below, like every other term
	if (u_HzmWet.w > 0.0)
	{
		gl_FragColor.rgb = gl_FragColor.rgb * (1.0 - hzmF) + hzmEnv * hzmF
		                 + hzmLm * (hzmF * u_HzmWetOccZ.w * (1.0 - hzmR)) + u_HzmWetSunCol.rgb * (hzmG * 0.5);
	}
  // HZM coop [2026-09-27] realistic lightning: the outdoor surface light. u_HzmLtWorld = (strike direction, world
  // space; energy). hzmLtVis is the sun shadow mask (1 where the sun reaches = open to the sky, 0 under a roof), so a
  // room stays dark except where daylight comes in. w 0 = inert (the default), so with lightning off nothing changes.
  #if defined(USE_SHADOWMAP)
	if (u_HzmLtWorld.w > 0.0)
	{
		gl_FragColor.rgb += hzmLtAlbedo * vec3(0.86, 0.90, 1.0) * (u_HzmLtWorld.w * hzmLtVis * (0.25 + 0.75 * clamp(dot(N, u_HzmLtWorld.xyz), 0.0, 1.0)));
	}
  #endif

#else

	gl_FragColor.rgb = diffuse.rgb * lightColor;

#endif

	ApplySoftParticle(gl_FragColor.rgb, alpha);

	gl_FragColor.rgb = ApplyGlobalFog(gl_FragColor.rgb);

	gl_FragColor.a = alpha;
}
