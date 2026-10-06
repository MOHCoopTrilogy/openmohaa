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
// HZM gl2 stable sun shadows + B0 (plan P3, shadows vet section 7 D1-D3; tr_hzm_sunstable.c). u_HzmSunMaskOnly.x:
// 0 = today (every program starts at 0, and every draw uploads 0 while r_shadowStable resolves to legacy),
// 1 = B0 (the mask is V_entity only, V_world comes from W per pixel), 2 = B0 off the main view (no sun shadow),
// 3 = B0 where the mask does not describe this surface (not in the z-prepass): V_world only.
uniform sampler2DShadow u_HzmSunWorldMap;
uniform mat4      u_HzmSunWorldMvp;
uniform vec4      u_HzmSunMaskOnly;   // mode, normal offset (u), W bias (depth units, + = occluded), half a W texel (uv)
#if defined(MSAA_DEPTH_MINMAX)
uniform vec4      u_HzmShadowMatch;   // HZM gl2 MSAA P4c: on, depth-slope factor (0 = off, today)
#endif
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
#if defined(USE_LIGHTMAP) && defined(USE_MSAA_CENTROID)
centroid varying vec2 var_HzmLmCentroid;   // HZM gl2 MSAA P4b (lightall_vp)
#define HZM_LMCOORD var_HzmLmCentroid
#else
#define HZM_LMCOORD var_TexCoords.zw
#endif

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

// HZM coop [2026-10-05] SKY FOG MODEL (tr_shade.c RB_HZM_SkyFogUniforms, docs/proposals/volumetric_clouds_2026-09-26/v11.md).
// The sky is drawn at depth 1, so the depth fraction fogged it as if it hung at zFar - the farthest VISIBLE corner of
// the world, which shrinks in a street or a room: a thickly fogged map then showed a clear sky through every gap
// above the fog wall. A sky draw (u_HzmSkyTan.z 1: the box, its cloud stages, the sun, a portal-sky view) instead
// takes the path through a fog LAYER of height H toward its own elevation: the zenith is fogged as at H, the
// horizon fully - camera position no longer matters. Every other draw keeps its depth (the z 0 default).
uniform vec4      u_HzmSkyFog;
uniform vec4      u_HzmSkyVp;
uniform vec4      u_HzmSkyTan;

float HzmFogDist()
{
	if (u_HzmSkyTan.z > 0.5)
	{
		vec2 ndc = (gl_FragCoord.xy - u_HzmSkyVp.xy) * u_HzmSkyVp.zw * 2.0 - 1.0;
		vec3 ray = normalize(vec3(ndc.x * u_HzmSkyTan.x, ndc.y * u_HzmSkyTan.y, -1.0));
		return u_HzmSkyFog.w / max(dot(ray, u_HzmSkyFog.xyz), 0.0001);
	}
	float denom = u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0);
	return u_GlobalFogParams.y / min(denom, -1e-6);
}

vec3 ApplyGlobalFog(vec3 color)
{
	if (u_GlobalFogColor.a <= 0.0)
	{
		return color;
	}

	float dist  = HzmFogDist();
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

#if defined(MSAA_DEPTH_MINMAX)
	// HZM gl2 MSAA (plan P2b, MH-B1) two-surface fade: under MSAA the bound snapshot holds the edge pixel's nearest (r)
	// and farthest (g) depth. A particle behind the nearest surface is hidden on those samples by the multisample
	// depth test anyway, so it fades against the far one. g is 0 on a single-sample snapshot (the HOME bypass),
	// which keeps today's r exactly.
	vec2  zmm = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).rg;
	float zs  = (zmm.g > 0.0 && gl_FragCoord.z > zmm.r) ? zmm.g : zmm.r;
#else
	float zs = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).r;
#endif
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

// the top-down max-height map (world z of the highest shelter over xy): r sharp, g blurred (sigma 40 u, the
// reflection-ray taps only - a ray tap on the sharp map draws a ruled line where it crosses a wall's footprint)
float HzmOccHeight(vec2 xy)
{
	return texture2D(u_HzmOccMap, (xy - u_HzmWetOcc.xy) * u_HzmWetOcc.zw).r * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
}

float HzmOccBlur(vec2 xy)
{
	return texture2D(u_HzmOccMap, (xy - u_HzmWetOcc.xy) * u_HzmWetOcc.zw).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
}

// [waterfix 2026-10-05] debug view (r_hzmWetDebug 1 -> u_HzmWet.w 2): red = sheltered, green = exposed with the sky
// reflection visible, blue = puddle
float hzmDbgE = 0.0;
float hzmDbgSky = 0.0;
float hzmDbgPm = 0.0;

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

	// one world-space noise fetch (tr_hzm_wet.c R_HZMWetBuildNoise): r = puddle zones (170-680 u), g = puddle detail
	// (73-146 u), ba = a smooth jitter vector (51-128 u). World space, so nothing crawls when the camera moves.
	vec4  nz  = texture2D(u_HzmWetNoise, P.xy / 2048.0, -1.0);   // bias -1: the detail bands keep their shape far off
	vec2  jit = nz.ba * 2.0 - 1.0;

	// [waterfix 2026-10-05] exposure, 5 taps. The occlusion map is an 8 u max-height raster: one sharp tap drew a
	// dead-straight dry strip along every wall base, curb and eave, so the wet sheen stopped in a hard line that
	// traced each object's outline. Floors now average five taps on a 15-41 u ring whose rotation and radius follow
	// the world-space jitter (an irregular rain shadow feathered over ~60 u), half-way to their max (a strip that is
	// not under anything stays wet). Walls march the rain ray slanted 20 deg out of the wall, 5 taps, the minimum.
	// Slopes mix the two tap sets.
	float upf = smoothstep(0.3, 0.9, n.z);
	vec2  hn  = normalize(n.xy + vec2(0.00001, 0.0));
	float ang = 3.0 * jit.x;
	vec2  cs  = vec2(cos(ang), sin(ang)) * (28.0 * (1.0 + 0.45 * jit.y));
	vec2  cs2 = vec2(0.309 * cs.x - 0.951 * cs.y, 0.951 * cs.x + 0.309 * cs.y);     // +72 deg
	vec2  cs3 = vec2(-0.809 * cs.x - 0.588 * cs.y, 0.588 * cs.x - 0.809 * cs.y);    // +144 deg
	vec2  pf  = P.xy + n.xy * 4.0;
	float sf  = mix(24.0, 32.0, upf);
	float zf0 = P.z + 4.0;
	float e0  = smoothstep(-sf, 2.0, mix(P.z + 72.0,  zf0, upf) - HzmOccHeight(mix(P.xy + hn * 26.0, pf + cs, upf)) + 2.0);
	float e1  = smoothstep(-sf, 2.0, mix(P.z + 122.0, zf0, upf) - HzmOccHeight(mix(P.xy + hn * 44.0, pf + cs2, upf)) + 2.0);
	float e2  = smoothstep(-sf, 2.0, mix(P.z + 172.0, zf0, upf) - HzmOccHeight(mix(P.xy + hn * 62.0, pf + cs3, upf)) + 2.0);
	float e3  = smoothstep(-sf, 2.0, mix(P.z + 222.0, zf0, upf) - HzmOccHeight(mix(P.xy + hn * 80.0, pf - cs3 * 0.6, upf)) + 2.0);
	float e4  = smoothstep(-sf, 2.0, mix(P.z + 260.0, zf0, upf) - HzmOccHeight(mix(P.xy + hn * 94.0, pf - cs2 * 0.6, upf)) + 2.0);
	float eMx = max(max(e0, e1), max(max(e2, e3), e4));
	float eMn = min(min(e0, e1), min(min(e2, e3), e4));
	float e   = mix(eMn, 0.5 * ((e0 + e1 + e2 + e3 + e4) * 0.2 + eMx), upf);
	float wf  = u_HzmWet.x * e * (0.55 + 0.45 * upf);   // walls take less rain

	// puddles: flat, exposed, allowed. [waterfix] zones broken up by the detail field (smaller, scattered puddles,
	// not 3-12 m sheets), a wider rim (at least one pixel, no crawling) and a soaked, darker halo around each one
	float pz  = mix(nz.r, nz.g, 0.4);
	float lvl = u_HzmWet.y * e;
	float thr = u_HzmWetOccZ.z * lvl;
	float edg = 0.04 + 0.5 * fp / 64.0;
	float flt = smoothstep(0.96, 0.99, n.z) * u_HzmWetMat.z * step(0.01, lvl);
	float pm  = (1.0 - smoothstep(thr - edg, thr + edg, pz)) * flt;
	float hal = (1.0 - smoothstep(thr, thr + 0.12, pz)) * flt * (1.0 - pm);

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
	r   = r - 0.14 * cav * wf - 0.08 * hal;
	dk *= (1.0 - 0.10 * cav * wf) * (1.0 - 0.08 * hal);
	r   = mix(r, 0.03, pm);
	// [waterfix] a wet surface is darker AND more saturated (the film removes the diffuse surface scatter)
	albedo = max(mix(vec3(al), albedo, 1.0 + 0.30 * max(wf, pm)), vec3(0.0)) * dk;

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

	// Fresnel-Schlick-roughness: a rough film can never reach mirror reflectance at grazing (the anti-plastic term).
	// [waterfix] times Smith-Schlick view masking, which a rough film loses at grazing: the bright sky line that ran
	// along every face seen edge-on (a sloped rock top, a step) was this term without it. A puddle (r 0.03) keeps it.
	float ndv = max(dot(Nr, E), 0.001);
	float kv  = 0.5 * r * r;
	F  = 0.02 + (max(1.0 - r, 0.02) - 0.02) * pow(1.0 - ndv, 5.0);
	F *= ndv / (ndv * (1.0 - kv) + kv);
	F *= e * max(wf, pm) * (1.0 - u_HzmWetMat.w);

	// [waterfix] the reflected ray, marched twice through the BLURRED height map (36 u and 150 u, jittered in world
	// space): where it runs into a wall, a bank, a crate or a roof the film reflects that scenery instead of the bright
	// sky - a dim stand-in (0.35 x the horizon, then the same light cap), so the reflection never just stops. Heights
	// are taken RELATIVE to the blurred height under the pixel itself, so the blur's own lift beside a tall wall does
	// not block a ray that points away from it.
	vec3  Rg  = reflect(-E, n);
	float t1  = 36.0 * (1.0 + 0.35 * jit.x);
	float t2  = 150.0 * (1.0 + 0.35 * jit.y);
	float hb0 = max(HzmOccBlur(P.xy), P.z);
	float o1  = smoothstep(4.0, 30.0, HzmOccBlur(P.xy + Rg.xy * t1) - hb0 - Rg.z * t1);
	float o2  = smoothstep(8.0, 70.0, HzmOccBlur(P.xy + Rg.xy * t2) - hb0 - Rg.z * t2);
	float skv = (1.0 - o1) * (1.0 - o2);

	// the sky it reflects, fogged exactly as gl2 fogs the visible sky at zFar (r_globalFogSky)
	vec3 R = reflect(-E, Nr);
	R.z = max(R.z, 0.03);
	R = normalize(R);
	env = mix(u_HzmWetSkyH.rgb, u_HzmWetSkyZ.rgb, smoothstep(-0.3 * r, 0.6 + 0.3 * r, R.z));
	float haze = 0.0;
	if (u_GlobalFogColor.a > 0.0)
	{
		// [2026-10-05] the sky fog model (u_HzmSkyFog.w = H > 0): the reflected sky is fogged as the visible sky now is,
		// by the reflected ray's elevation - no longer at zFar (tr_shade.c RB_HZM_SkyFogUniforms)
		float zf = (u_HzmSkyFog.w > 0.0) ? u_HzmSkyFog.w / max(R.z, 0.03)
		                                 : u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
		haze = clamp((zf - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a;
	}
	env = mix(env, u_GlobalFogColor.rgb, haze);
	env = mix(env * 0.5, env, skv);
	// [waterfix] never brighter than the light that reaches this spot allows: an overcast or fogged sky (often near
	// white) and a night sky no longer paint white sheets over dim ground. The baked light is the measure.
	float le  = dot(env, vec3(0.299, 0.587, 0.114));
	float cap = 0.04 + 0.45 * dot(lm, vec3(0.299, 0.587, 0.114));
	env *= 0.8 * min(1.0, cap / max(le, 0.001));

	hzmDbgE = e;
	hzmDbgSky = skv;
	hzmDbgPm = pm;

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
	// [waterfix] Smith-Schlick masking for view AND light: the old 1 / (4 n.v) grew without bound on faces seen edge-on
	// and drew the bright line along the m4l2 rock slab; this form is bounded by 1 / (4 k)
	float kg = 0.5 * a;
	glint = a / (3.14159265 * d2 * d2) * fh * nl * (nl / (nl * (1.0 - kg) + kg)) / (4.0 * (ndv * (1.0 - kg) + kg));
	glint *= e * sv * max(wf, pm) * (1.0 - 0.8 * haze) * u_HzmWetSun.w * (1.0 - u_HzmWetMat.w);
	// [waterfix] a smaller, dimmer hot spot: the cap (1.5 -> 0.8) shrinks the part of the lobe that clips to white
	glint = min(glint * (0.4 + 0.6 * skv), 0.8);
}
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
// HZM ground variety (r_groundVariety, tr_hzm_groundvar.c; docs/proposals/ground_variety_2026-09-29). Every name here
// starts HzmGv / hzmGv (bug-3252: two features declaring the same local name is a fatal redefinition).
// u_HzmGroundVar  = (mode, macro strength, 1 / hex cell in world units, blend sharpness)
//   mode 0 = OFF: every draw that is not a tagged world ground stage uploads ALL ZERO and HzmGvDiffuse / HzmGvTap take
//   the original texture2D path, so the output is unchanged byte for byte. 1 = macro variation, 2 = macro + hex tiling.
// u_HzmGroundVar2 = (luminance weighting, steep-face gate lo, gate hi, debug: 1 class tint, 2 hex weights)
// Both branches on mode are UNIFORM (one value per draw), so the implicit-derivative texture2D calls stay legal; the
// hex fetches use explicit gradients of the UNSHIFTED coordinate, so a lattice edge never changes the mip level.
uniform vec4 u_HzmGroundVar;
uniform vec4 u_HzmGroundVar2;

vec3  hzmGvW  = vec3(1.0, 0.0, 0.0);   // this pixel's hex weights (sum 1)
vec2  hzmGvO1 = vec2(0.0);             // the three lattice vertices' texture offsets
vec2  hzmGvO2 = vec2(0.0);
vec2  hzmGvO3 = vec2(0.0);
vec2  hzmGvDx = vec2(0.0);             // gradients of the unshifted texture coordinate
vec2  hzmGvDy = vec2(0.0);
float hzmGvG  = 0.0;                   // steep-face gate: 0 on walls, 1 on ground. Written ONLY by HzmGvDiffuse, which
                                       // main() calls before any HzmGvTap (normal, specular) - keep that order

#if __VERSION__ >= 130
// hash without sine (Hoskins): stable on every GPU for the small lattice coordinates used here
float HzmGvHash1(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return fract((p3.x + p3.y) * p3.z);
}

vec2 HzmGvHash2(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xx + p3.yz) * p3.zy);
}

// smooth (quintic) value noise, 0..1
float HzmGvNoise(vec2 x)
{
	vec2 i = floor(x);
	vec2 f = fract(x);
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	return mix(mix(HzmGvHash1(i), HzmGvHash1(i + vec2(1.0, 0.0)), u.x),
	           mix(HzmGvHash1(i + vec2(0.0, 1.0)), HzmGvHash1(i + vec2(1.0, 1.0)), u.x), u.y);
}

// the MACRO field: brightness from two octaves (~1400 u and ~450 u) plus a very small warm/cool drift (~2300 u).
// Mean 1.0; strength 0.10 is about +-8 % at the extremes. Never a hue change: the art keeps its own colour.
vec3 HzmGvMacro(vec2 p)
{
	float n = HzmGvNoise(p * (1.0 / 1400.0)) * 0.65 + HzmGvNoise(p * (1.0 / 450.0) + vec2(17.0, 5.0)) * 0.35;
	float t = HzmGvNoise(p * (1.0 / 2300.0) + vec2(41.0, 23.0)) - 0.5;
	return vec3(1.0) + u_HzmGroundVar.y * ((n - 0.5) * 2.0 * vec3(1.0) + t * vec3(0.30, 0.0, -0.30));
}

// Mikkelsen hex-tiling triangle lattice (JCGT 2022), in WORLD units: q = world xy / cell
void HzmGvLattice(vec2 q)
{
	q *= 3.4641016;   // 2 sqrt 3: hex cells of about one cell unit across
	vec2  sk   = vec2(q.x - 0.57735027 * q.y, 1.15470054 * q.y);
	vec2  base = floor(sk);
	vec3  t    = vec3(fract(sk), 0.0);
	t.z = 1.0 - t.x - t.y;
	float s  = step(0.0, -t.z);
	float s2 = 2.0 * s - 1.0;
	vec3  w  = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	hzmGvO1 = HzmGvHash2(base + vec2(s, s));
	hzmGvO2 = HzmGvHash2(base + vec2(s, 1.0 - s));
	hzmGvO3 = HzmGvHash2(base + vec2(1.0 - s, s));
	w = pow(max(w, vec3(0.0)), vec3(u_HzmGroundVar.w));
	hzmGvW = w / max(w.x + w.y + w.z, 1e-6);
}

// one hex-blended fetch with the current weights; a weight that cannot show (under 0.1 percent) skips its fetch
vec4 HzmGvHex(sampler2D s, vec2 uv)
{
	vec4 c = vec4(0.0);
	if (hzmGvW.x > 0.001) c += hzmGvW.x * textureGrad(s, uv + hzmGvO1, hzmGvDx, hzmGvDy);
	if (hzmGvW.y > 0.001) c += hzmGvW.y * textureGrad(s, uv + hzmGvO2, hzmGvDx, hzmGvDy);
	if (hzmGvW.z > 0.001) c += hzmGvW.z * textureGrad(s, uv + hzmGvO3, hzmGvDx, hzmGvDy);
	return c;
}

// the diffuse fetch. p = world position, n = geometric normal
vec4 HzmGvDiffuse(vec2 uv, vec3 p, vec3 n)
{
	if (u_HzmGroundVar.x < 0.5)
		return texture2D(u_DiffuseMap, uv);           // OFF: the original fetch

	hzmGvDx = dFdx(uv);
	hzmGvDy = dFdy(uv);
	hzmGvG  = smoothstep(u_HzmGroundVar2.y, u_HzmGroundVar2.z, normalize(n).z);
	vec4 c;
	if (u_HzmGroundVar.x > 1.5 && hzmGvG <= 0.0)
	{
		// a wall of a hex-tagged shader: the plain texture, through an explicit-gradient fetch (the implicit one is not
		// legal in this per-pixel branch). Same texels and mip; not guaranteed bit-identical to texture2D on every driver
		c = textureGrad(u_DiffuseMap, uv, hzmGvDx, hzmGvDy);
	}
	else if (u_HzmGroundVar.x > 1.5)
	{
		HzmGvLattice(p.xy * u_HzmGroundVar.z);
		// luminance weighting (Mikkelsen): the brighter texel wins a blend zone, so the zones follow the art's own
		// light/dark structure; weights under 0.1 percent are dropped (and their fetch skipped), then renormalised
		vec3 L = vec3(0.0);
		vec4 c1 = vec4(0.0);
		vec4 c2 = vec4(0.0);
		vec4 c3 = vec4(0.0);
		if (hzmGvW.x > 0.001) { c1 = textureGrad(u_DiffuseMap, uv + hzmGvO1, hzmGvDx, hzmGvDy); L.x = dot(c1.rgb, vec3(0.299, 0.587, 0.114)); }
		if (hzmGvW.y > 0.001) { c2 = textureGrad(u_DiffuseMap, uv + hzmGvO2, hzmGvDx, hzmGvDy); L.y = dot(c2.rgb, vec3(0.299, 0.587, 0.114)); }
		if (hzmGvW.z > 0.001) { c3 = textureGrad(u_DiffuseMap, uv + hzmGvO3, hzmGvDx, hzmGvDy); L.z = dot(c3.rgb, vec3(0.299, 0.587, 0.114)); }
		vec3 w0 = hzmGvW * step(vec3(0.001), hzmGvW);
		w0 /= max(w0.x + w0.y + w0.z, 1e-6);
		vec3 w  = w0 * mix(vec3(1.0), L, u_HzmGroundVar2.x);
		float ws = w.x + w.y + w.z;
		hzmGvW = (ws > 0.001) ? w / ws : w0;   // black texels under full luminance weighting: plain barycentric
		c = hzmGvW.x * c1 + hzmGvW.y * c2 + hzmGvW.z * c3;
		if (hzmGvG < 1.0)
		{
			vec4 c0 = textureGrad(u_DiffuseMap, uv, hzmGvDx, hzmGvDy);
			c = mix(c0, c, hzmGvG);
		}
	}
	else
	{
		c = texture2D(u_DiffuseMap, uv);
	}
	c.rgb *= mix(vec3(1.0), HzmGvMacro(p.xy), hzmGvG);

	if (u_HzmGroundVar2.w > 0.5)
	{
		if (u_HzmGroundVar2.w < 1.5)      // debug 1: class tint (green = hex, blue = macro only), faded by the gate
			c.rgb = mix(c.rgb, (u_HzmGroundVar.x > 1.5) ? vec3(0.2, 0.9, 0.2) : vec3(0.2, 0.4, 1.0), 0.45 * hzmGvG);
		else if (u_HzmGroundVar.x > 1.5)  // debug 2: the three hex weights as rgb
			c.rgb = hzmGvW;
	}
	return c;
}

// every other map sampled at the diffuse coordinate (normal, specular): the SAME offsets and weights
vec4 HzmGvTap(sampler2D s, vec2 uv)
{
	if (u_HzmGroundVar.x < 1.5)
		return texture2D(s, uv);                      // OFF or macro only: the original fetch
	if (hzmGvG <= 0.0)
		return textureGrad(s, uv, hzmGvDx, hzmGvDy);
	vec4 h = HzmGvHex(s, uv);
	if (hzmGvG < 1.0)
		h = mix(textureGrad(s, uv, hzmGvDx, hzmGvDy), h, hzmGvG);
	return h;
}
#else
// GLSL 1.20 has no textureGrad: the feature compiles out and every fetch is the original one
vec4 HzmGvDiffuse(vec2 uv, vec3 p, vec3 n) { return texture2D(u_DiffuseMap, uv); }
vec4 HzmGvTap(sampler2D s, vec2 uv) { return texture2D(s, uv); }
#endif
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
	vec4 lightmapColor = texture2D(u_LightMap, HZM_LMCOORD);
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

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec4 diffuse = HzmGvDiffuse(texCoords, u_ViewOrigin - viewDir, surfNormal);   // HZM ground variety (plain fetch when off)
#else
	vec4 diffuse = texture2D(u_DiffuseMap, texCoords);
#endif
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
#if defined(MSAA_DEPTH_MINMAX)
	else if (u_AlphaTest >= 4)
	{
		// HZM gl2 MSAA P4a (MH-B2): alpha-to-coverage. The coverage ramp is a pixel wide in alpha (fwidth), capped
		// so a nomip cutout cannot smear; 4 centres it on the old 0.5 threshold, 5 (light / unknown matte) keeps
		// it on the opaque side so no texel more contaminated than today's test ever shows
		float hzmA2c = (alpha - 0.5) / max(min(fwidth(alpha), 0.5), 0.0001);
		if (u_AlphaTest == 4)
			hzmA2c += 0.5;
		hzmA2c = clamp(hzmA2c, 0.0, 1.0);
		if (hzmA2c <= 0.0)
			discard;
		alpha = hzmA2c;
	}
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	L = var_LightDir.xyz;
  #if defined(USE_DELUXEMAP)
	L += (texture2D(u_DeluxeMap, HZM_LMCOORD).xyz - vec3(0.5)) * u_EnableTextures.y;
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
	N.xy = HzmGvTap(u_NormalMap, texCoords).ag - vec2(0.5);   // HZM ground variety: same hex offsets/weights
    #else
	N.xy = HzmGvTap(u_NormalMap, texCoords).rg - vec2(0.5);   // HZM ground variety: same hex offsets/weights
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
#if defined(MSAA_DEPTH_MINMAX)
	// HZM gl2 MSAA P4c (ME-F4): the mask holds each pixel's NEAREST sample. A fragment behind that depth by more than
	// its own slope belongs to the farther surface of an edge pixel: take the 4-neighbour mask texel whose depth is
	// closest to its own. Searched only then, so interior pixels (and foliage) cost one extra fetch.
	if (u_HzmShadowMatch.x > 0.5)
	{
		vec2  hzmT   = gl_FragCoord.xy * u_InvTexRes;
		float hzmFz  = gl_FragCoord.z;
		float hzmEps = u_HzmShadowMatch.y * fwidth(hzmFz) + 0.0000001;
		float hzmZ0  = texture2D(u_ScreenDepthMap, hzmT).r;
		if (hzmFz > hzmZ0 + hzmEps)
		{
			float hzmBest = hzmFz - hzmZ0;
			vec2  hzmPick = vec2(0.0);
			float hzmZn   = texture2D(u_ScreenDepthMap, hzmT + vec2(u_InvTexRes.x, 0.0)).r;
			if (abs(hzmFz - hzmZn) < hzmBest) { hzmBest = abs(hzmFz - hzmZn); hzmPick = vec2( 1.0,  0.0); }
			hzmZn = texture2D(u_ScreenDepthMap, hzmT - vec2(u_InvTexRes.x, 0.0)).r;
			if (abs(hzmFz - hzmZn) < hzmBest) { hzmBest = abs(hzmFz - hzmZn); hzmPick = vec2(-1.0,  0.0); }
			hzmZn = texture2D(u_ScreenDepthMap, hzmT + vec2(0.0, u_InvTexRes.y)).r;
			if (abs(hzmFz - hzmZn) < hzmBest) { hzmBest = abs(hzmFz - hzmZn); hzmPick = vec2( 0.0,  1.0); }
			hzmZn = texture2D(u_ScreenDepthMap, hzmT - vec2(0.0, u_InvTexRes.y)).r;
			if (abs(hzmFz - hzmZn) < hzmBest) { hzmBest = abs(hzmFz - hzmZn); hzmPick = vec2( 0.0, -1.0); }
			shadowValue = texture2D(u_ShadowMap, (gl_FragCoord.xy + hzmPick) * r_FBufScale).r;
		}
	}
#endif
	// HZM [bug-3171] the sun mask means 'open to the sky' only on a surface that FACES the sun. Facing away, it reads
	// the wall's own sun-side face through the shadow bias (a thin wall's interior face reads lit), so the flash lit
	// interior walls whose rays to the sun AND to the strike enter the wall itself. Gated on the geometric normal,
	// before the sun N.L below (the flash comes from the strike, not the sun).
	hzmLtVis = shadowValue * smoothstep(0.02, 0.20, dot(normalize(surfNormal), normalize(var_PrimaryLightDir.xyz)));
	float hzmSunVis;          // HZM gl2 P3: the sun visibility the primary-light specular below uses

	if (u_HzmSunMaskOnly.x > 1.5 && u_HzmSunMaskOnly.x < 2.5)
	{
		// B0, not the main view (portal sky, mirror, cube bake - vet L4): no sun shadow at all
		shadowValue = 1.0;
		hzmLtVis = smoothstep(0.02, 0.20, dot(normalize(surfNormal), normalize(var_PrimaryLightDir.xyz)));   // bug-3171 gate
		hzmSunVis = 1.0;
	}
	else if (u_HzmSunMaskOnly.x > 0.5)
	{
		// B0 (D1): V_world from W at this pixel, looked up off the FACE normal (derivative normal: it agrees with W's
		// winding, L2) by ~1 W texel (L1, S2), with an occluded-leaning bias; 4 bilinear PCF taps (kernel 3)
		vec3  hzmL  = normalize(var_PrimaryLightDir.xyz);
		vec3  hzmNf = normalize(cross(dFdx(viewDir), dFdy(viewDir)));
		if (dot(hzmNf, surfNormal) < 0.0)
			hzmNf = -hzmNf;
		vec4  hzmW  = u_HzmSunWorldMvp * vec4(u_ViewOrigin - viewDir + hzmNf * u_HzmSunMaskOnly.y, 1.0);
		vec3  hzmWs = hzmW.xyz * (0.5 / hzmW.w) + vec3(0.5);
		float hzmZ  = hzmWs.z + u_HzmSunMaskOnly.z;
		float hzmO  = u_HzmSunMaskOnly.w;
		float hzmVw = 0.25 * (vec4(shadow2D(u_HzmSunWorldMap, vec3(hzmWs.xy + vec2(-hzmO, -hzmO), hzmZ))).r
		                    + vec4(shadow2D(u_HzmSunWorldMap, vec3(hzmWs.xy + vec2( hzmO, -hzmO), hzmZ))).r
		                    + vec4(shadow2D(u_HzmSunWorldMap, vec3(hzmWs.xy + vec2(-hzmO,  hzmO), hzmZ))).r
		                    + vec4(shadow2D(u_HzmSunWorldMap, vec3(hzmWs.xy + vec2( hzmO,  hzmO), hzmZ))).r);
		if (any(lessThan(hzmWs.xy, vec2(0.0))) || any(greaterThan(hzmWs.xy, vec2(1.0))))
			hzmVw = 1.0;
		float hzmVe = (u_HzmSunMaskOnly.x > 2.5) ? 1.0 : shadowValue;
		hzmLtVis = hzmVw * hzmVe * smoothstep(0.02, 0.20, dot(hzmNf, hzmL));   // bug-3171 gate, on the face normal
    #if defined(USE_LIGHT_VECTOR)
		// D3: in shade (Vw 0) the grid alone, as gl1; in sun, today's Ve * N.L
		shadowValue = 1.0 - hzmVw * (1.0 - hzmVe * clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0));
		hzmSunVis = hzmVw * hzmVe;
    #else
		// world stages (vet R1): the lightmap already carries N.L and the world's own shadow. Entity shadows only
		// where W says the sun reaches, and only on faces that face it
		float hzmF = smoothstep(0.0, 0.1, dot(hzmNf, hzmL));
		shadowValue = mix(1.0, 1.0 - hzmVw * (1.0 - hzmVe), hzmF);
		hzmSunVis = hzmF * hzmVw * hzmVe;
    #endif
	}
	else
	{
	// surfaces not facing the light are always shadowed
	shadowValue *= clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0);
	hzmSunVis = shadowValue;
	}

    #if defined(SHADOWMAP_MODULATE)
	// HZM coop [2026-09-28] storm darkness: .b / .r is the SUN'S VISIBILITY (1 = the map's sun; tr_scene.c lowers .b
	// under a storm deck). Only the sun's share fades: a shadowed texel keeps its ambient share, nothing brightens.
	// (gfx tree: named hzmStormVis here - the gfx P3 B0 block above already declares hzmSunVis, the specular's visibility)
	float hzmStormVis = (u_PrimaryLightAmbient.r > 0.0) ? clamp(u_PrimaryLightAmbient.b / u_PrimaryLightAmbient.r, 0.0, 1.0) : 1.0;
	lightColor *= shadowValue * (1.0 - u_PrimaryLightAmbient.r) * hzmStormVis + u_PrimaryLightAmbient.r;
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
	vec4 specular = HzmGvTap(u_SpecularMap, texCoords);        // HZM ground variety: same hex offsets/weights
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
	lightColor *= hzmSunVis;   // HZM gl2 P3: == shadowValue unless B0 is on (vet R4: no glints in baked shadow)
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
		                 + hzmLm * (hzmF * u_HzmWetOccZ.w * (1.0 - hzmR)) + u_HzmWetSunCol.rgb * (hzmG * 0.35);
		if (u_HzmWet.w > 1.5)
		{
			float hzmDl = 0.25 + 0.75 * dot(gl_FragColor.rgb, vec3(0.299, 0.587, 0.114));
			gl_FragColor.rgb = vec3(1.0 - hzmDbgE, hzmDbgE * hzmDbgSky, hzmDbgPm) * hzmDl;
		}
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
