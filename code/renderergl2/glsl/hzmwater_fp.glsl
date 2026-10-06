// HZM water pass (r_hzmWater, tr_hzm_water.c; docs/proposals/water_wetness_2026-09-27 plan W3). The offline twin that
// fixed every constant is tools/ww_look.py hzm_water (lookdev/RUBRIC.md: no added sparkle, seams <= 1.07, far field
// stable). Premultiplied output for GL_ONE, GL_ONE_MINUS_SRC_ALPHA over the retail stages, fogged by the pass itself.
uniform vec3      u_ViewOrigin;
uniform vec4      u_HzmWater;       // master fade, time mod 3600, lightmap reference (0 = no lightmap gate), glint on
uniform vec4      u_HzmWetOcc;      // occlusion map world->uv (x0, y0, 1/width, 1/height)
uniform vec4      u_HzmWetOccZ;     // its zmin, zrange
uniform vec4      u_HzmWetSkyZ;     // sky zenith rgb
uniform vec4      u_HzmWetSkyH;     // sky horizon rgb
uniform vec4      u_HzmWetSun;      // toward-sun xyz, sun present
uniform vec4      u_HzmWetSunCol;   // sun rgb
uniform vec4      u_GlobalFogColor;
uniform vec4      u_GlobalFogParams;
uniform vec4      u_HzmSkyFog;      // [2026-10-05] sky fog model: w = fog-layer height H (0 = off), see generic_fp HzmFogDist
uniform sampler2D u_HzmOccMap;      // the rain-occlusion map (8 u top-down max height: r sharp, g blurred; tr_hzm_wet.c)
uniform sampler2D u_HzmRippleMap;   // 256^2 tileable ripple normals, RGBA8 UNCOMPRESSED with mips (Toksvig reads |n|)
uniform sampler2D u_LightMap;       // the water shader's own lightmap, when it has one

varying vec3      var_Position;
varying vec2      var_LightTex;
varying float     var_NormalZ;

void main()
{
	vec3  P    = var_Position;
	vec3  V    = u_ViewOrigin - P;
	float dist = max(length(V), 0.001);
	vec3  E    = V / dist;

	// two world-space ripple layers (continuous across faces: no seams by construction), faded out with range
	float fade = 1.0 - smoothstep(350.0, 1300.0, dist);
	float amp1 = 0.55 * fade;
	float amp2 = amp1 * 0.7;
	float t    = u_HzmWater.y;
	vec3  s1   = texture2D(u_HzmRippleMap, P.xy / 384.0 + vec2(0.012, 0.006) * t).xyz * 2.0 - vec3(1.0);
	vec3  s2   = texture2D(u_HzmRippleMap, (P.xy + vec2(37.0, -91.0)) / 144.0 + vec2(-0.018, 0.011) * t).xyz * 2.0 - vec3(1.0);
	float lmg  = 1.0;
	float lmb  = 1.0;
	if (u_HzmWater.z > 0.0)
	{
		// no glint in the water's own baked shadow; relative to this shader's brightest water on the map
		float lum = dot(texture2D(u_LightMap, var_LightTex).rgb, vec3(0.299, 0.587, 0.114));
		lmg = smoothstep(0.45 * u_HzmWater.z, 0.85 * u_HzmWater.z, lum);
		lmb = clamp(lum / u_HzmWater.z, 0.0, 1.0);     // [waterfix] how lit this water is: dims the blocked-ray stand-in
	}

	// [waterfix 2026-10-05] where the reflected sky is blocked. This used a Gaussian-blurred (sigma 48 u) copy of the
	// height map tested straight up: blurring HEIGHTS raised every bank and quay wall over the water beside it, so the
	// reflection died along canals and near banks and only open water kept it, and the fade traced each wall's outline.
	// Now the reflected ray itself is marched through the map's BLURRED channel (g, sigma 40 u: on the sharp raster
	// each tap drew a ruled line parallel to every wall it crossed) at 40, 140 and 380 u, wide soft compares: under a roof
	// or a bridge, toward a hull or the far quay wall the water shows its own colour (what it would mirror there is
	// scenery, not sky); open water beside a bank keeps its sky. The ray takes a quarter of the ripple tilt, so the edge
	// of a mirrored wall is gently broken up the way a real one is, never a ruled line. Fetched before the discard.
	vec3  Rg  = reflect(-E, normalize(vec3((s1.xy * amp1 + s2.xy * amp2) * 0.25, 1.0)));
	float sk  = 1.0;
	if (u_HzmWetOccZ.y > 0.0)
	{
		vec2  uv = u_HzmWetOcc.zw;
		// relative to the blurred height over the water point itself: a bank's blur lift beside the water does not
		// block a ray that runs away from the bank
		float h0 = max(texture2D(u_HzmOccMap, (P.xy - u_HzmWetOcc.xy) * uv).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x, P.z);
		float ha = texture2D(u_HzmOccMap, (P.xy + Rg.xy * 40.0 - u_HzmWetOcc.xy) * uv).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
		float hb = texture2D(u_HzmOccMap, (P.xy + Rg.xy * 140.0 - u_HzmWetOcc.xy) * uv).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
		float hc = texture2D(u_HzmOccMap, (P.xy + Rg.xy * 380.0 - u_HzmWetOcc.xy) * uv).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
		sk = (1.0 - 0.8 * smoothstep(8.0, 60.0, ha - h0 - Rg.z * 40.0))
		   * (1.0 - 0.8 * smoothstep(12.0, 100.0, hb - h0 - Rg.z * 140.0))
		   * (1.0 - 0.8 * smoothstep(20.0, 160.0, hc - h0 - Rg.z * 380.0));
	}
	float e   = sk;

	// horizontal, from above only: sloped or vertical 'water' faces and cull-none undersides get nothing
	if (var_NormalZ < 0.7 || u_ViewOrigin.z < P.z)
	{
		discard;
	}

	// Toksvig: each layer's mip-averaged normal is shorter where it is rough; that is slope variance
	float l1  = clamp(length(s1), 0.2, 1.0);
	float l2  = clamp(length(s2), 0.2, 1.0);
	float var = amp1 * amp1 * (1.0 - l1) / l1 + amp2 * amp2 * (1.0 - l2) / l2;
	vec3  N   = normalize(vec3(s1.xy * amp1 + s2.xy * amp2, 1.0));

	// Schlick, water F0 0.02, capped. [waterfix] no longer x e: where the ray is blocked the water still reflects -
	// the scenery's dim stand-in below - instead of nothing
	float ndv = max(dot(N, E), 0.001);
	float F   = min(0.02 + 0.98 * pow(1.0 - ndv, 5.0), 0.60);

	// the sky it reflects, fogged exactly as gl2 fogs the visible sky at zFar (r_globalFogSky)
	vec3 R = reflect(-E, N);
	R.z = max(R.z, 0.03);
	R = normalize(R);
	vec3  env  = mix(u_HzmWetSkyH.rgb, u_HzmWetSkyZ.rgb, smoothstep(0.0, 0.6, R.z));
	float haze = 0.0;
	float f    = 0.0;
	if (u_GlobalFogColor.a > 0.0)
	{
		// [2026-10-05] the sky fog model (u_HzmSkyFog.w = H > 0): the reflected sky is fogged as the visible sky now is,
		// by the reflected ray's elevation - no longer at zFar (tr_shade.c RB_HZM_SkyFogUniforms)
		float zf = (u_HzmSkyFog.w > 0.0) ? u_HzmSkyFog.w / max(R.z, 0.03)
		                                 : u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
		haze = clamp(clamp((zf - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
		// generic_fp ApplyGlobalFog's fraction, term for term
		float dp = u_GlobalFogParams.y / min(u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0), -0.000001);
		f = clamp(clamp((dp - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
	}
	env = mix(env, u_GlobalFogColor.rgb, haze);
	env = mix(env * (0.55 * lmb), env, e);     // [waterfix] blocked ray: a dim stand-in for the mirrored bank / wall / hull

	// sun / moon glint: roughness floor 0.26 widened by Toksvig variance, range and haze (a fogged sky has no crisp
	// disc to reflect) - a narrower lobe is single-pixel sparkle in motion
	vec3  Ls = u_HzmWetSun.xyz;
	vec3  Hs = normalize(Ls + E);
	float a  = 0.0676 + min(2.0 * var, 0.18) + (1.0 - fade) * 0.03 + 0.30 * haze;
	float nh = max(dot(N, Hs), 0.0);
	float nl = max(dot(N, Ls), 0.0);
	float d2 = nh * nh * (a - 1.0) + 1.0;
	float g  = a / (3.14159265 * d2 * d2) * F * nl / (4.0 * ndv + 0.001) * 0.6;
	g *= e * lmg * (1.0 - 0.4 * haze) * u_HzmWetSun.w * u_HzmWater.w;     // e again: the twin's F already carries it
	g  = min(g, 1.5);

	// premultiplied and self-fogged: over an already-fogged base, rgb = add * (1 - f) + fogColor * f * F, alpha = F
	vec3 add = env * F + u_HzmWetSunCol.rgb * (g * 0.6);
	F   *= u_HzmWater.x;
	add *= u_HzmWater.x;
	gl_FragColor = vec4(add * (1.0 - f) + u_GlobalFogColor.rgb * (F * f), F);
}
