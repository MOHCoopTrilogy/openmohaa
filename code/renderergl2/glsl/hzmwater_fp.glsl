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
// water presets (hzm_waterwet.h HZM_WPRESET_*, tr_hzm_water.c). u_HzmCold0.x = 0 keeps the plain reflection layer.
uniform vec4      u_HzmCold0;       // mode, cover, swell gain, chop gain
uniform vec4      u_HzmCold1;       // deep rgb, floe amount
uniform vec4      u_HzmCold2;       // shallow rgb, planar mix
uniform vec4      u_HzmCold3;       // quay map world->uv (x0, y0, 1/w, 1/h)
uniform vec4      u_HzmCold4;       // quay map max distance, drift
uniform vec4      u_HzmCold5;       // whitecap amount, shoreline alpha ramp (min, max, axis 0/1 S/2 T)
uniform vec4      u_HzmCold6;       // swell yaw (rad), shore foam amount, flow 0/1
uniform vec4      u_HzmCold7;       // flow direction xy
// contact foam (scene-depth snapshot): view basis, projection terms, foam size; u_HzmCont2.w = 0 switches it off
uniform vec4      u_HzmCont0;       // forward xyz, tan half fov x
uniform vec4      u_HzmCont1;       // right xyz, tan half fov y
uniform vec4      u_HzmCont2;       // up xyz, on
uniform vec4      u_HzmCont3;       // proj[10], proj[14], 1/viewport w, 1/viewport h
uniform vec4      u_HzmCont4;       // focal px, foam width (world units)
uniform sampler2D u_HzmDepthMap;
uniform sampler2D u_HzmQuayMap;     // R: distance to the nearest quay wall / max distance
uniform sampler2D u_HzmFloeMap;     // 256^2 tileable fbm, one field per channel, mean 0.5 sd 0.11
uniform sampler2D u_HzmIceMap;      // ice colour tile
uniform sampler2D u_HzmPlanarMap;   // planar reflection of the scene (white = none)

varying vec3      var_Position;
varying vec2      var_LightTex;
varying vec2      var_St;
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


	// ---------------------------------------------------------------------------------------------------------------
	// ICE PRESET (frozenriver). Cracked thin ice: clear parts are dark cold blue, bubbles and cracks scatter white; the
	// BSP lightmap still lights it; fresnel sky reflection + normal-mapped glint; frost patches and a drift band along
	// the snow banks are cut out of the layer (alpha 0), so the retail snow texture shows through there. Two samples of
	// the same packed tile at 640 u and 1090 u (rotated) kill the repetition; the pack is textures/hzm/ice_pack
	// (R,G normal xy, B roughness, A colour luminance).
	// ---------------------------------------------------------------------------------------------------------------
	if (u_HzmCold0.x > 1.5 && u_HzmCold0.x < 2.5)
	{
		vec2  q   = P.xy;
		vec2  q2  = vec2(q.x * 0.8196 - q.y * 0.5729, q.x * 0.5729 + q.y * 0.8196) / 1090.0 + vec2(0.37, 0.11);
		vec4  pk1 = texture2D(u_HzmIceMap, q / 640.0);
		vec4  pk2 = texture2D(u_HzmIceMap, q2);
		float mac = texture2D(u_HzmFloeMap, q / 900.0).r;
		float w2  = clamp(0.5 + (mac - 0.5) * 3.0, 0.1, 0.9);
		float dd  = clamp(1.0 - dist / 3500.0, 0.15, 1.0);

		// frost: noise patches + a band where the BLURRED height map says a bank is close (open-sky maps only)
		float fn = texture2D(u_HzmFloeMap, q / 380.0 + vec2(0.31, 0.17)).g;
		float fm = clamp((fn - 0.60) * 5.0, 0.0, 1.0) * 0.6;
		if (u_HzmWetOccZ.y > 0.0)
		{
			float hb = texture2D(u_HzmOccMap, (P.xy - u_HzmWetOcc.xy) * u_HzmWetOcc.zw).g * u_HzmWetOccZ.y + u_HzmWetOccZ.x;
			fm = max(fm, smoothstep(8.0, 70.0, hb - P.z));
		}
		if (fm >= 0.995)
		{
			discard;
		}

		vec2  nm   = mix(pk1.xy, pk2.xy, w2) * 2.0 - vec2(1.0);
		float rgh  = clamp(mix(pk1.z, pk2.z, w2) * 0.55 + 0.10, 0.05, 1.0);
		float lum  = mix(pk1.w, pk2.w, w2);
		float kk   = pow(clamp((lum - 0.18) / 0.55, 0.0, 1.0), 1.1);
		vec3  alb  = mix(u_HzmCold1.rgb, u_HzmCold2.rgb, kk);
		vec3  Ni   = normalize(vec3(nm * 1.8 * dd, 1.0));
		float ndvI = max(dot(Ni, E), 0.02);
		float Fi   = (0.04 + 0.96 * pow(1.0 - ndvI, 5.0)) * (1.0 - fm);

		vec3 Ri = reflect(-E, Ni);
		Ri.z = max(Ri.z, 0.03);
		Ri = normalize(Ri);
		vec3  envi  = mix(u_HzmWetSkyH.rgb, u_HzmWetSkyZ.rgb, smoothstep(0.0, 0.6, Ri.z));
		float hazeI = 0.0;
		float fI    = 0.0;
		if (u_GlobalFogColor.a > 0.0)
		{
			float zfi = (u_HzmSkyFog.w > 0.0) ? u_HzmSkyFog.w / max(Ri.z, 0.03)
			                                  : u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
			hazeI = clamp(clamp((zfi - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
			float dpi = u_GlobalFogParams.y / min(u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0), -0.000001);
			fI = clamp(clamp((dpi - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
		}
		envi = mix(envi, u_GlobalFogColor.rgb, hazeI);

		vec3  lmc  = (u_HzmWater.z > 0.0) ? texture2D(u_LightMap, var_LightTex).rgb * u_HzmCold4.z : vec3(0.5);
		vec3  diff = alb * lmc;

		vec3  Lsi  = u_HzmWetSun.xyz;
		vec3  Hsi  = normalize(Lsi + E);
		float ai   = max(0.0676, rgh * rgh * rgh * rgh) + (1.0 - dd) * 0.03 + 0.30 * hazeI;
		float nhi  = max(dot(Ni, Hsi), 0.0);
		float nli  = max(dot(Ni, Lsi), 0.0);
		float d2i  = nhi * nhi * (ai - 1.0) + 1.0;
		float gi   = ai / (3.14159265 * d2i * d2i) * Fi * nli / (4.0 * ndvI + 0.001) * 0.6;
		gi *= lmg * (1.0 - 0.4 * hazeI) * u_HzmWetSun.w * u_HzmWater.w;
		gi  = min(gi, 1.5);

		vec3  icol = diff * (1.0 - Fi) + envi * Fi + u_HzmWetSunCol.rgb * gi * 0.6;
		icol = mix(icol, u_GlobalFogColor.rgb, fI);
		float aI = (1.0 - fm) * u_HzmWater.x;
		gl_FragColor = vec4(icol * aI, aI);
		return;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// WATER PRESET (COLD HARBOUR). The GLSL twin of docs/proposals/kickoff_2026-10-18/ice_1025/tools/water_look.py:
	// body colour instead of the retail milk, three world-space swells + ripple-map chop, fresnel sky reflection with
	// the blocked-ray stand-in, the legacy anti-sparkle glint, and ice plates thickening toward the quay walls.
	// Premultiplied like the legacy path: rgb = colour * a, a = cover * master fade.
	// ---------------------------------------------------------------------------------------------------------------
	if (u_HzmCold0.x > 0.5 && (u_HzmCold0.x < 1.5 || u_HzmCold0.x > 2.5))     // every water preset; 2 = ICE has its own block above
	{
		const float TAU = 6.28318531;
		const float TW  = TAU / 1000.0;      // every phase speed below is a whole number of cycles per 1000 s: seamless wrap
		// five crossing wave trains: directions spread +-60 deg around the main one, wavelengths 230..900 u that are not
		// multiples of each other, amplitude modulated by a slow noise so no train runs unbroken to the horizon (that read as
		// radial light rays toward the camera). Every phase speed is a whole number of cycles per 1000 s.
		float cyw = cos(u_HzmCold6.x);
		float syw = sin(u_HzmCold6.x);
		vec2  d1 = vec2(0.93 * cyw - 0.37 * syw, 0.93 * syw + 0.37 * cyw);
		vec2  d2 = vec2(0.70 * cyw + 0.71 * syw, 0.70 * syw - 0.71 * cyw);
		vec2  d3 = vec2(0.75 * cyw - 0.66 * syw, 0.75 * syw + 0.66 * cyw);
		vec2  d4 = vec2(0.30 * cyw + 0.95 * syw, 0.30 * syw - 0.95 * cyw);
		vec2  d5 = vec2(0.43 * cyw - 0.90 * syw, 0.43 * syw + 0.90 * cyw);
		float am = 0.55 + 0.9 * texture2D(u_HzmFloeMap, P.xy / 3100.0 + vec2(0.44, 0.09)).r;      // 0.65 .. 1.15 around the mean
		float c1s = cos(TAU / 900.0 * dot(d1, P.xy) + 43.0 * TW * t);
		float c2s = cos(TAU / 610.0 * dot(d2, P.xy) + 67.0 * TW * t + 1.3);
		float c3s = cos(TAU / 430.0 * dot(d3, P.xy) + 95.0 * TW * t + 2.1);
		float c4s = cos(TAU / 300.0 * dot(d4, P.xy) + 131.0 * TW * t + 0.7);
		float c5s = cos(TAU / 230.0 * dot(d5, P.xy) + 173.0 * TW * t + 4.0);
		vec2  sw = d1 * (0.027 * c1s) + d2 * (0.019 * c2s * am) + d3 * (0.013 * c3s) + d4 * (0.009 * c4s * am) + d5 * (0.006 * c5s);
		sw *= -u_HzmCold0.z;
		float crest = 0.5 + 0.5 * (0.40 * c1s + 0.28 * c2s * am + 0.17 * c3s + 0.10 * c4s + 0.05 * c5s);
		float calm  = smoothstep(0.45, 0.62, 0.5 + 0.28 * sin(P.x / 430.0 + 1.3) * sin(P.y / 390.0) + 0.22 * sin((P.x + P.y) / 610.0 + 0.4));
		float fadeC = clamp(1.0 - dist / 2600.0, 0.12, 1.0);
		// chop: the two legacy ripple layers + a third, larger and rotated one (no visible tile on calm water) and a
		// large-scale amplitude variation; rivers stretch the ripples along the water's own axis and drift them downstream
		vec2  chopv = s1.xy * 0.55 + s2.xy * 0.40;
		vec2  s3v   = texture2D(u_HzmRippleMap, vec2(P.x * 0.8660 - P.y * 0.5, P.x * 0.5 + P.y * 0.8660) / 1150.0 + vec2(0.031, -0.017) * t).xy * 2.0 - vec2(1.0);
		chopv += s3v * 0.45;
		float macro = texture2D(u_HzmFloeMap, P.xy / 2300.0 + vec2(0.13, 0.71)).r;
		float mm    = clamp(1.0 + (macro - 0.5) * 4.0, 0.5, 1.5);
		if (u_HzmCold6.z > 0.5)
		{
			vec2 fd  = u_HzmCold7.xy;
			vec2 fpp = vec2(-fd.y, fd.x);
			vec2 f1  = texture2D(u_HzmRippleMap, vec2(dot(P.xy, fd) / 1300.0 - 0.009 * t, dot(P.xy, fpp) / 240.0)).xy * 2.0 - vec2(1.0);
			vec2 f2  = texture2D(u_HzmRippleMap, vec2(dot(P.xy, fd) / 520.0 - 0.023 * t, dot(P.xy, fpp) / 130.0)).xy * 2.0 - vec2(1.0);
			chopv = f1 * 0.6 + f2 * 0.4;
		}
		vec2  ch    = chopv * 0.10 * u_HzmCold0.w * fadeC * mm * (1.0 - ((u_HzmCold0.x < 1.5) ? 0.6 : 0.0) * calm);     // oily slicks only in the cold harbour
		vec3  Nn    = normalize(vec3(sw + ch, 1.0));
		float ndvC  = max(dot(Nn, E), 0.02);
		float Fc    = min(0.02 + 0.98 * pow(1.0 - ndvC, 5.0), 0.80);

		vec3 body = mix(u_HzmCold1.rgb, u_HzmCold2.rgb, pow(ndvC, 2.2)) + u_HzmWetSkyZ.rgb * 0.025;
		body *= 0.55 + 0.45 * lmb;           // the baked light: lamp pools lift it, baked shadow keeps it dark
		body *= mix(1.0, 0.62 + 0.76 * crest, clamp(u_HzmCold0.z, 0.0, 1.0));     // crests a little lighter than the troughs: the swell reads as rolling
		body *= 0.92 + 0.16 * (macro - 0.5) * 4.0 * 0.5 + 0.0;

		vec3 Rc = reflect(-E, Nn);
		Rc.z = max(Rc.z, 0.03);
		Rc = normalize(Rc);
		vec3  envc  = mix(u_HzmWetSkyH.rgb, u_HzmWetSkyZ.rgb, smoothstep(0.0, 0.6, Rc.z));
		float hazeC = 0.0;
		float fC    = 0.0;
		if (u_GlobalFogColor.a > 0.0)
		{
			float zfc = (u_HzmSkyFog.w > 0.0) ? u_HzmSkyFog.w / max(Rc.z, 0.03)
			                                  : u_GlobalFogParams.y / min(u_GlobalFogParams.x + 1.0, -0.000001);
			hazeC = clamp(clamp((zfc - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
			float dpc = u_GlobalFogParams.y / min(u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0), -0.000001);
			fC = clamp(clamp((dpc - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0) * u_GlobalFogColor.a, 0.0, 1.0);
		}
		envc = mix(envc, u_GlobalFogColor.rgb, hazeC);
		vec3 reflc = mix(envc * (0.14 + 0.41 * lmb), envc, (u_HzmCold0.x < 1.5) ? e : 1.0);     // the blocked-ray stand-in is for roofed pens only; open water mirrors the sky     // blocked ray: a dim stand-in for the pen, never black
		if (u_HzmCold2.w > 0.0)
		{
			// planar reflection: the scene from the mirrored camera, same screen position as this fragment
			float pk = clamp(1.0 - dist / 3000.0, 0.2, 1.0);
			vec2  puv = gl_FragCoord.xy * u_HzmCold4.zw + (sw + ch) * vec2(0.20, 0.45) * pk;
			reflc = mix(reflc, texture2D(u_HzmPlanarMap, clamp(puv, vec2(0.002), vec2(0.998))).rgb, u_HzmCold2.w);
		}

		// glint: the legacy lobe (roughness floor 0.26 + Toksvig + range + haze): a narrower one is sparkle in motion
		float l1c  = clamp(length(s1), 0.2, 1.0);
		float l2c  = clamp(length(s2), 0.2, 1.0);
		float varc = 0.01 * ((1.0 - l1c) / l1c + (1.0 - l2c) / l2c);
		vec3  Lsc  = u_HzmWetSun.xyz;
		vec3  Hsc  = normalize(Lsc + E);
		float ac   = 0.0676 + min(2.0 * varc, 0.18) + (1.0 - fadeC) * 0.03 + 0.30 * hazeC;
		float nhc  = max(dot(Nn, Hsc), 0.0);
		float nlc  = max(dot(Nn, Lsc), 0.0);
		float d2c  = nhc * nhc * (ac - 1.0) + 1.0;
		float gc   = ac / (3.14159265 * d2c * d2c) * Fc * nlc / (4.0 * ndvC + 0.001) * 0.6;
		gc *= e * lmg * (1.0 - 0.4 * hazeC) * u_HzmWetSun.w * u_HzmWater.w;
		gc  = min(gc, 1.5);

		// ice crust / slush / floes: plates thicken toward the quay walls
		float iceA   = 0.0;
		vec3  iceCol = vec3(0.0);
		if (u_HzmCold1.w > 0.0)
		{
			vec2  quv  = (P.xy - u_HzmCold3.xy) * u_HzmCold3.zw;
			float bd   = texture2D(u_HzmQuayMap, quv).r * u_HzmCold4.x;
			float edge = pow(clamp(1.0 - bd / max(u_HzmCold4.y, 1.0), 0.0, 1.0), 1.2);
			if (edge > 0.02)
			{
				vec2  fp = P.xy + 18.0 * vec2(sin(33.0 * TW * t + P.y / 260.0), cos(27.0 * TW * t + P.x / 240.0));
				// two layers at non-commensurate scales, one rotated 30 deg, so the 256 px noise tile never shows as a grid;
				// the slush uses a third lookup at its own scale and rotation
				vec2  fr = vec2(fp.x * 0.8660 - fp.y * 0.5, fp.x * 0.5 + fp.y * 0.8660);
				vec3  nzA = texture2D(u_HzmFloeMap, fp / 700.0).rgb;
				vec3  nzB = texture2D(u_HzmFloeMap, fr / 1130.0 + vec2(0.37, 0.11)).rgb;
				vec3  nz  = clamp((nzA + nzB - vec3(1.0)) * 0.7071 + vec3(0.5), 0.0, 1.0);
				float nzS = clamp((texture2D(u_HzmFloeMap, fr / 290.0 + vec2(0.61, 0.23)).g + texture2D(u_HzmFloeMap, fp / 470.0).b - 1.0) * 0.7071 + 0.5, 0.0, 1.0);
				float cr = clamp((nz.r - (0.58 - 0.36 * edge)) * 4.5, 0.0, 1.0);
				cr = max(cr, clamp(edge - 0.85, 0.0, 1.0) * 4.0);
				float slush = clamp((nzS - 0.46) * 4.0, 0.0, 1.0) * edge * 0.95;
				iceA = clamp(cr + slush, 0.0, 1.0) * u_HzmCold1.w;
				vec4  ic = texture2D(u_HzmIceMap, fp / 330.0);
				float kk = clamp((ic.a - 0.15) / 0.5, 0.0, 1.0);
				iceCol = mix(vec3(0.07, 0.14, 0.20), vec3(0.78, 0.88, 0.96), kk) * (0.45 + 0.55 * min(lmb * 1.3, 1.4));
				float rim = step(0.02, iceA) * (1.0 - step(0.85, iceA));
				iceCol *= 1.0 - 0.45 * rim;
				iceCol = iceCol * 0.55 + envc * 0.04;
			}
		}

		vec3  wcol = body * (1.0 - Fc) + reflc * Fc;
		// whitecaps: foam on the steep crests of the swell, broken up by noise; more with a bigger swell, fading with range
		float wcap = 0.0;
		if (u_HzmCold5.x > 0.0)
		{
			// crest lines: foam only on the swell crests, in streaks along the wave front (noise stretched along it)
			vec2  dpw = vec2(-d1.y, d1.x);
			float nW  = texture2D(u_HzmFloeMap, vec2(dot(P.xy, dpw) / 4200.0, dot(P.xy, d1) / 150.0) + vec2(0.19, 0.43)).b;
			float nH  = texture2D(u_HzmFloeMap, vec2(dot(P.xy, dpw) / 900.0, dot(P.xy, d1) / 70.0) + vec2(0.71, 0.29)).g;     // fine breakup: thin broken lines, not blobs
			wcap = 0.8 * smoothstep(0.86, 0.99, crest + (nW - 0.5) * 1.3) * smoothstep(0.42, 0.62, nH) * u_HzmCold5.x * clamp(u_HzmCold0.z / 1.8, 0.2, 1.0) * fadeC;
		}
		if (u_HzmCold6.y > 0.0 && u_HzmCold5.w > 0.5)
		{
			// surf lines parallel to the shore: where the shoreline sheet fades toward the sand, bands of foam run along it
			float rcoord = (u_HzmCold5.w < 1.5) ? var_St.x : var_St.y;
			float rampv  = clamp((u_HzmCold5.z - u_HzmCold5.y) * rcoord + u_HzmCold5.y, 0.0, 1.0);
			float band   = smoothstep(0.97, 0.62, rampv) * smoothstep(0.02, 0.28, rampv);
			float nS     = texture2D(u_HzmFloeMap, P.xy / 640.0 + vec2(0.52, 0.07)).r;
			float nS2    = texture2D(u_HzmFloeMap, P.xy / 210.0 + vec2(0.33, 0.58)).g;
			float nL     = texture2D(u_HzmFloeMap, P.xy / 1900.0 + vec2(0.81, 0.27)).b;
			// bands of uneven spacing; every band has its own thickness and a slow fade-out / re-form of its own
			float ph     = rcoord * (70.0 + 130.0 * (nL - 0.5) + 55.0) + (nS - 0.5) * 22.0 - 95.0 * TW * t;
			float bid    = floor((ph + 3.14159265) / TAU);
			float h1     = fract(sin(bid * 12.9898) * 43758.5453);
			float h2     = fract(sin(bid * 78.233 + 1.7) * 24634.6345);
			float life   = smoothstep(0.15, 0.60, 0.5 + 0.5 * sin(TW * (30.0 + floor(h2 * 40.0)) * t + bid * 2.3));
			float lines  = 0.5 + 0.5 * cos(ph);
			float fthr   = 0.50 + 0.40 * h1;
			wcap = max(wcap, band * smoothstep(fthr, fthr + 0.16, lines) * life * smoothstep(0.38, 0.62, nS2 + (nS - 0.5)) * u_HzmCold6.y * clamp(1.0 - dist / 7000.0, 0.0, 1.0));
		}
		if (u_HzmCont2.w > 0.5)
		{
			// contact foam: where something solid that stands near the waterline is next to this water pixel on the screen
			// (hedgehogs, posts, hulls, quay walls), foam gathers around it. Eight taps on a ring whose pixel radius is the foam
			// width in world units; a tap only counts when the occluder it finds is within 80 u of the waterline.
			vec2  suv = gl_FragCoord.xy * u_HzmCont3.zw;
			float myD = dot(P - u_ViewOrigin, u_HzmCont0.xyz);
			float rpx = clamp(u_HzmCont4.y * u_HzmCont4.x / max(myD, 1.0), 2.0, 48.0);
			float acc = 0.0;
			for (int k = 0; k < 8; k++)
			{
				float ang = 0.7853982 * float(k) + 0.3;
				vec2  nuv = suv + vec2(cos(ang), sin(ang)) * rpx * u_HzmCont3.zw;
				float zn  = texture2D(u_HzmDepthMap, nuv).r;
				float dn  = u_HzmCont3.y / (u_HzmCont3.x + (2.0 * zn - 1.0));
				if (zn < 0.9999 && dn < myD - 5.0)
				{
					vec2 ndc = nuv * 2.0 - vec2(1.0);
					vec3 Wp  = u_ViewOrigin + (u_HzmCont0.xyz + u_HzmCont1.xyz * ndc.x * u_HzmCont0.w + u_HzmCont2.xyz * ndc.y * u_HzmCont1.w) * dn;
					acc += step(abs(Wp.z - P.z), 80.0);
				}
			}
			float nC = texture2D(u_HzmFloeMap, P.xy / 150.0 + vec2(0.27, 0.64)).g;
			float cf = smoothstep(0.12, 0.62, acc / 8.0) * smoothstep(0.25, 0.60, nC + 0.35) * ((u_HzmCold0.x < 1.5) ? 0.55 : 0.85);
			wcap = max(wcap, cf * clamp(1.0 - dist / 5000.0, 0.0, 1.0));
		}
		wcol = mix(wcol, vec3(0.74, 0.80, 0.82) * (0.40 + 0.60 * min(lmb * 1.3, 1.2)), wcap * 0.9);
		vec3  col  = mix(wcol + u_HzmWetSunCol.rgb * gc * 0.6 * (1.0 - wcap), iceCol, iceA);
		col = mix(col, u_GlobalFogColor.rgb, fC);
		float ramp = 1.0;
		if (u_HzmCold5.w > 0.5)
		{
			float rc = (u_HzmCold5.w < 1.5) ? var_St.x : var_St.y;
			ramp = clamp((u_HzmCold5.z - u_HzmCold5.y) * rc + u_HzmCold5.y, 0.0, 1.0);
		}
		float ac2 = u_HzmCold0.y * u_HzmWater.x * ramp;
		gl_FragColor = vec4(col * ac2, ac2);
		return;
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
