// HZM gl2 UNDERWATER VOLUME - v3 (2026-09-03). Replaces the v2 colour-filter pass.
//
// v2 (bug-2355) had no idea how far away anything was. Every term it applied was a function of
// the SCREEN position - a radial vignette stood in for depth - so the result was a filter laid
// over the picture rather than a volume of water in front of it. That is what reads as generic:
// a diver 3 metres away and the seabed 30 metres away got exactly the same treatment, and the
// eye is extremely good at noticing that nothing recedes.
//
// v3 reads the real per-pixel eye distance out of the scene depth buffer, exactly the way
// RB_GlobalFog / globalfog_fp.glsl already does (tr_postprocess.c RB_GlobalFog). The caller
// binds tr.renderDepthImage on TB_LEVELSMAP and hands over the two projection terms; the
// reconstruction below is the same one, and is exact for any zNear/zFar:
//
//   z_ndc = 2*zw - 1,   d = P14 / (P10 + z_ndc)     (both terms negative -> d > 0)
//
// FOUR THINGS CARRY THE LOOK, and every one of them is now distance-driven:
//
//  1. PER-CHANNEL ABSORPTION. Water does not tint - it EATS RED FIRST. Beer-Lambert per channel,
//     T = exp(-sigma * d), with sigma_red about 3x sigma_green and sigma_blue about 1.9x (silt
//     scatters and absorbs blue, which is why the Channel is green and not tropical blue). At
//     200 units red is already down to about 50% while green is at 80%; by 900 units red is
//     gone (4%) and green still reads at 37%. THAT gradient is the cue, and no amount of tint
//     strength can fake it, because a tint is the same everywhere.
//
//  2. SUSPENDED MATTER, twice. It modulates the DENSITY multiplicatively (two octaves of soft
//     value noise scale sigma by 0.78..1.22, so the water is thicker in some places than others
//     and the murk breathes instead of sitting flat), and it draws MOTES - two parallax layers
//     of soft round specks lit in silt colour, not white.
//     The v2 particulate is DELETED, not tuned. It was floor(uv * 360.0) - a nearest-cell hash,
//     i.e. a per-cell CONSTANT - run through smoothstep(0.9885, 1.0, n) (v2 lines 72-74). That
//     draws literal axis-aligned squares, achromatic +0.11 over whatever is behind them, which
//     over dark water is exactly the white squares the user kept reporting. It was also
//     stretched: 360 cells in both u and v on a 16:9 frame gives rectangles, not squares. Here
//     the noise is BILINEARLY INTERPOLATED (round blobs, no lattice) in aspect-corrected space.
//
//  3. DEPTH-SCALED SCATTER BLUR. Light that reaches you from far away has been scattered off
//     the silt on the way, so distance costs SHARPNESS as well as contrast. 12-tap golden-angle
//     spiral plus the centre tap, radius = r_ppUnderwaterBlur * murk, rotated by a per-pixel
//     hash so the ring dissolves into grain instead of banding. Near geometry keeps its edges;
//     the far field goes soft. Cost is stated in the C caller.
//
//  4. LIGHT SHAFTS. Slow vertical bands biased toward the top of the frame, scaled by murk, so
//     the light visibly comes from a surface above you.
//
// A BONUS THAT MATTERS ON m3l1a: a pixel that wrote no depth (zw at the far plane - the skybox)
// is treated as being at the far clamp, so it absorbs to solid silt. The seabed off Omaha stops
// drawing at y -2816 and the skybox wall behind it continues below the waterline, which is why
// the cinematic holds a hard yaw rail (bug-2320). Sky pixels now resolve to opaque water rather
// than clouds under the sea. Keep the rail - this softens the failure, it does not remove it.
//
// UNIFORMS - all of them already exist in uniformsInfo[] (tr_glsl.c:92/144/147/157/165). No new
// uniform slot, so nothing downstream of UNIFORM_ALPHAGENPARAMS shifts.
//   u_Color        (amount 0..1 eased by the cgame, time in seconds, r_ppUnderwaterAmt,
//                   AIR 0..1 - r_ppUnderwaterAir, 1.0 = full air = the look below unchanged)
//   u_ViewInfo     (projMat[10], projMat[14], depthValid, debugMode + heartPhase*0.499 in the
//                   fraction - integer part is the mode, fract() is the phase in cycles)
//
// [user 2026-09-06, bug-2507] AIR RAMP - the drowning beat. lack = 1 - u_Color.w drives, all
// scaled by a so they leave with the water: the edge darkening from 0.14 up to 0.75 and
// tightening toward the centre (a closing tunnel), a luminance mix up to 0.6 (the colour goes
// out of the world), the scatter blur radius x(1 + 1.5*lack), a mild darkening of the silt,
// and a heartbeat pulse of 8% on the vignette at bpm = 55 + 75*lack. The phase comes from the
// C caller, integrated there - see tr_postprocess.c - because sin(t*bpm) with a moving bpm
// and an absolute t jumps phase every frame.
//   u_FogDistance  (sigmaR, sigmaG, sigmaB, far clamp in units)
//   u_FogColorMask (siltColour.rgb, siltBoost)
//   u_HzmParams    (blur radius in texels, particle scale, shaft scale, ripple scale)
//   u_InvTexRes    (1/srcWidth, 1/srcHeight) - FBO_Blit sets this one itself
//
// u_ViewInfo.z MEANS SOMETHING DIFFERENT HERE THAN IN globalfog_fp.glsl. There it is skyEnable
// and is tested as (u_ViewInfo.z < 0.5) to EXEMPT the sky from fog (globalfog_fp.glsl:70). Here
// it is depthValid and is tested as (> 0.5) to ENABLE the depth path. The two shaders are
// separate GL programs with separate uniformBuffers (shaderProgram_t, tr_local.h:956-969), so
// there is no shared storage and no collision - but do not copy a guard between the two files
// without re-reading this note.
//
// SCREEN ORIENTATION: var_TexCoords.y is 1.0 at the TOP of the frame. Derived, not assumed -
// FBO_Blit is always called here with an explicit dstBox, which pairs quadVerts[0] (ortho y =
// dstBox[1] = 0, so NDC +1, the top) with srcTexCorners[1] = (srcBox[1] + srcBox[3]) / h = 1.0.
// raindrops_fp.glsl agrees: its trickle head runs hy from 1.0 down to 0.0 and lays its trail
// where uv.y > hy, described as above the head. (dropLayer in that same file has a comment
// claiming v increases upward - that comment is wrong; the trickle is the self-consistent one.)
//
// NOTE: no double quotes and no backslashes anywhere in this file, comments included - the
// stringify build step (code/tools/stringify.cpp) emits it as a C string literal with NO
// escaping at all, so either character breaks the build.

uniform sampler2D u_TextureMap;    // scene colour - FBO_Blit binds this on TB_COLORMAP
uniform sampler2D u_LevelsMap;     // scene depth  - the caller binds it on TB_LEVELSMAP

uniform vec4      u_Color;
uniform vec4      u_ViewInfo;
uniform vec4      u_FogDistance;
uniform vec4      u_FogColorMask;
uniform vec4      u_HzmParams;
uniform vec2      u_InvTexRes;

varying vec2      var_TexCoords;

// sin-free hash. The old h21 (fract(sin(dot(p, k)) * 43758.5453)) banks on sin() having enough
// precision to be chaotic, which is driver-dependent and on some mediump paths collapses into
// visible stripes.
float uwHash(vec2 p)
{
	vec3 q = fract(vec3(p.x, p.y, p.x) * vec3(0.1031, 0.1030, 0.0973));
	q += dot(q, vec3(q.y, q.z, q.x) + 33.33);
	return fract((q.x + q.y) * q.z);
}

// bilinear value noise. THE point of it: the result varies smoothly WITHIN a cell, so a high
// smoothstep of it is a round soft blob. A per-cell constant, which is what v2 used, can only
// ever produce a square.
float uwNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);

	f = f * f * (3.0 - 2.0 * f);

	float a0 = uwHash(i);
	float b0 = uwHash(i + vec2(1.0, 0.0));
	float c0 = uwHash(i + vec2(0.0, 1.0));
	float d0 = uwHash(i + vec2(1.0, 1.0));

	return mix(mix(a0, b0, f.x), mix(c0, d0, f.x), f.y);
}

void main()
{
	float amt = clamp(u_Color.x, 0.0, 1.0);
	float t   = u_Color.y;
	float k   = max(u_Color.z, 0.0);
	float a   = clamp(amt * k, 0.0, 1.0);

	// [bug-2507] lack of air, 0 at full air. lackA is the version every visual term uses, so
	// surfacing (a -> 0) takes the drowning look out with the water.
	float lack  = clamp(1.0 - u_Color.w, 0.0, 1.0);
	float lackA = lack * a;
	float hphase = fract(u_ViewInfo.w) * 2.004;   // 0.499 packing -> cycles 0..1

	// aspect-corrected space, so every noise cell is SQUARE on screen and the motes are round.
	// u_InvTexRes is (1/w, 1/h), so its ratio is the aspect (tr_fbo.c:601-602, srcTexScale NULL).
	float aspect = u_InvTexRes.y / max(u_InvTexRes.x, 0.0000001);
	vec2  pm     = vec2(var_TexCoords.x * aspect, var_TexCoords.y);

	// ---- 1. refraction. Two octaves at different rates: one sine reads as a wobbling screen.
	float rip = u_HzmParams.w * a;
	vec2  uv  = var_TexCoords;

	uv.x += (sin(var_TexCoords.y * 24.0 + t * 1.30) * 0.0065
	       + sin(var_TexCoords.y *  7.3 - t * 0.70) * 0.0040) * rip;
	uv.y += (sin(var_TexCoords.x * 18.0 + t * 1.10) * 0.0065
	       + sin(var_TexCoords.x *  5.1 + t * 0.90) * 0.0040) * rip;

	// ---- 2. eye distance from the depth buffer.
	// tr.renderDepthImage is IMGFLAG_CLAMPTOEDGE (tr_image.c:3578), so the refracted lookup
	// cannot wrap round to the opposite edge when uv leaves 0..1 at the frame border.
	float farDist = max(u_FogDistance.w, 1.0);
	float sky     = 0.0;
	float zw      = 1.0;
	float dist;

	if (u_ViewInfo.z > 0.5)
	{
		// sample depth at the REFRACTED coordinate so it belongs to the colour we fetch below
		zw = texture2D(u_LevelsMap, uv).r;

		float denom = u_ViewInfo.x + (2.0 * zw - 1.0);

		dist = u_ViewInfo.y / min(denom, -0.000001);

		// nothing wrote depth here (skybox, or a blendfunc surface with no depthWrite). 0.9999999
		// is the smallest float32 step below 1.0, so this is exactly at the far plane - the same
		// epsilon globalfog_fp.glsl settled on in bug-1296 after 1e-5 was found to exempt a slab
		// thousands of units deep. Push it to the far clamp: under water, sky must never be seen.
		if (zw >= 0.9999999)
		{
			dist = farDist;
			sky  = 1.0;
		}

		dist = clamp(dist, 0.0, farDist);
	}
	else
	{
		// no usable projection latch this frame - fall back to a flat mid-distance so the pass
		// degrades to something close to the v2 look instead of going black or fully opaque.
		// 0.12 of the clamp is T_g = 0.49, i.e. about half murk - deliberately not 0.35, which
		// would be T_g = 0.12 and leave the frame nearly opaque on a fallback nobody asked for.
		// The C caller announces this case as depth=0 in its per-submersion marker.
		dist = farDist * 0.12;
	}

	// ---- 3. suspended density. This is the MULTIPLICATIVE particulate: it scales the extinction
	// itself, so the water is genuinely thicker in some places, rather than having specks pasted
	// on top of a uniform wash.
	float dens = uwNoise(pm * 3.1 + vec2( t * 0.013, -t * 0.021)) * 0.55
	           + uwNoise(pm * 7.9 + vec2(-t * 0.019, -t * 0.034)) * 0.45;
	float densMul = 0.78 + 0.44 * dens;

	// ---- 4. per-channel transmittance. mix toward 1.0 by the eased amount so surfacing fades
	// the whole volume out cleanly instead of snapping.
	vec3  T    = mix(vec3(1.0), exp(-u_FogDistance.xyz * (dist * densMul)), a);
	float murk = clamp(1.0 - T.g, 0.0, 1.0);

	// ---- 5. scatter blur, radius scaled by how much water is in the way.
	// The branch is on a UNIFORM (r_ppUnderwaterBlur), so it is coherent across the whole draw -
	// setting that cvar to 0 really does buy back the 12 taps.
	vec3 c;

	if (u_HzmParams.x > 0.0)
	{
		// [bug-2507] the scatter radius climbs with lack of air: x1 at full air, x2.5 at none
		vec2  r   = (u_HzmParams.x * murk * (1.0 + 1.5 * lackA)) * u_InvTexRes;
		float ang = uwHash(gl_FragCoord.xy) * 6.2831853;
		float cs  = cos(ang);
		float sn  = sin(ang);
		mat2  rot = mat2(cs, sn, -sn, cs);
		int   i;

		// weights sum to exactly 1.0 : 0.22 + 12 * 0.065
		c = texture2D(u_TextureMap, uv).rgb * 0.22;

		for (i = 0; i < 12; i++)
		{
			float fi = float(i);
			float ph = fi * 2.39996323;             // golden angle, radians
			float rr = sqrt((fi + 0.5) / 12.0);     // even coverage of the disc, not a ring
			vec2  o  = vec2(cos(ph), sin(ph)) * rr;

			// rotate the UNIT offset, then scale by the texel size. Scaling first and rotating
			// after would shear the kernel on a non-square pixel.
			c += texture2D(u_TextureMap, uv + (rot * o) * r).rgb * 0.065;
		}
	}
	else
	{
		c = texture2D(u_TextureMap, uv).rgb;
	}

	// ---- 6. absorb the scene and inscatter the silt, per channel.
	//
	// CLAMP FIRST. This pass runs after the tone stage so the image is normally display-referred,
	// but with r_hdr on and tonemapping forced off the source is a float FBO that can sit near
	// 2.0 - and mix(2.0, silt, 0.5) is still above 1.0, i.e. the water would visibly SKIP every
	// bright pixel. That is bug-1299, found in globalfog_fp.glsl and in tonemap_hzm_fp.glsl
	// before it. Same operand, same remedy.
	// [bug-2507] mild silt darkening with lack of air - the water itself goes darker, 25% at none
	vec3 silt = u_FogColorMask.rgb * u_FogColorMask.w * (1.0 - 0.25 * lackA);

	c = clamp(c, 0.0, 1.0);
	c = c * T + silt * (1.0 - T);

	// ---- 7. motes. Two parallax layers: near ones larger, faster, with more drift; far ones
	// small and slow. Both drift UP past a sinking camera - subtracting time from the lookup
	// offset moves the pattern toward larger v, which is up (see the orientation note above).
	if (u_HzmParams.y > 0.0)
	{
		vec2 p1 = pm *  44.0 + vec2( 0.030 * sin(t * 0.21), -t * 0.055);
		vec2 p2 = pm * 116.0 + vec2(-0.020 * sin(t * 0.17), -t * 0.024);

		// the second noise breaks the lattice of the first, so the blobs are irregular
		float n1 = uwNoise(p1) * (0.55 + 0.45 * uwNoise(p1 * 1.73 + 11.3));
		float n2 = uwNoise(p2);

		// HIGH thresholds on purpose. The noise mean sits near 0.39 for n1 and 0.5 for n2, so
		// these keep the motes SPARSE - a few percent of the frame. Drop them and it stops
		// reading as suspended matter and starts reading as video static.
		float m1 = smoothstep(0.78, 0.94, n1);
		float m2 = smoothstep(0.90, 0.995, n2);

		float motes = m1 * 0.70 + m2 * 0.35;

		// lit in silt colour lifted toward white - a pale green-grey, never a white speck
		vec3 mcol = mix(u_FogColorMask.rgb * 3.2, vec3(1.0), 0.45);

		// backscatter is what you see against the deep field, not across a face two feet away
		c += motes * mcol * u_HzmParams.y * a * (0.08 + 0.42 * murk);
	}

	// ---- 8. light shafts from the surface above
	if (u_HzmParams.z > 0.0)
	{
		float band = uwNoise(vec2(pm.x * 4.6 - t * 0.030, pm.y * 1.15 + t * 0.008));
		float top  = smoothstep(0.05, 0.90, var_TexCoords.y);

		band = smoothstep(0.52, 0.94, band);

		// brighter where the frame is pure water with nothing behind it (looking UP through the
		// one-sided surface at what used to be sky). Without this, replacing sky with silt above
		// also removes the only cue that the light is coming from up there.
		c += band * top * murk * a * u_HzmParams.z * (1.0 + 1.5 * sky) * vec3(0.055, 0.075, 0.070);
	}

	// ---- 9. a small edge darkening. Deliberately small: the murk cue is DISTANCE now, and a
	// heavy vignette is the single clearest tell that a water effect is a screen filter.
	// [bug-2507] ... unless the air is going. Then it IS a tunnel, and that is the point: the
	// weight climbs 0.14 -> 0.75, the radius tightens (1.6 -> 3.2 on r^2, so the dark reaches
	// the centre third), and it throbs at the heart rate - 1 + 0.08*lack*sin(2*pi*phase).
	// A luminance mix up to 0.6 goes first, so the colour leaves before the light does.
	vec2  d     = var_TexCoords - vec2(0.5);
	float vig   = clamp(dot(d, d) * (1.6 + 1.6 * lackA), 0.0, 1.0);
	float lum   = dot(c, vec3(0.299, 0.587, 0.114));
	float pulse = 1.0 + 0.08 * lackA * sin(hphase * 6.2831853);
	float vigW  = mix(0.14, 0.75, lackA) * pulse;

	c = mix(c, vec3(lum), 0.6 * lackA);
	c *= (1.0 - a * (0.06 + vigW * vig));

	// ---- r_ppUnderwaterDebug. 1 = absorbed fraction per channel (red should dominate and grow
	// with distance), 2 = murk as greyscale, 3 = reconstructed distance, one ramp per 1024 units,
	// 4 = the RAW window depth the sampler returned, remapped so the useful range is visible.
	//
	// 4 IS THE SAMPLER TEST, and that is the reason it exists. If u_LevelsMap is left pointing at
	// texture unit 0 (the scene COLOUR - what happens if tr_glsl.c or the caller ever loses its
	// GLSL_SetUniformInt(UNIFORM_LEVELSMAP, TB_LEVELSMAP) line) then mode 4 shows a recognisable
	// picture of the scene. Correctly bound it shows a smooth depth field with no scene albedo in
	// it at all - dark near, white far, nothing that looks like a texture. Mode 3 proves the
	// RECONSTRUCTION is real: it must move when you move.
	if (u_ViewInfo.w > 3.5)
	{
		c = vec3(clamp((zw - 0.9) * 10.0, 0.0, 1.0));
	}
	else if (u_ViewInfo.w > 2.5)
	{
		c = vec3(fract(dist * (1.0 / 1024.0)));
	}
	else if (u_ViewInfo.w > 1.5)
	{
		c = vec3(murk);
	}
	else if (u_ViewInfo.w > 0.5)
	{
		c = vec3(1.0) - T;
	}

	gl_FragColor = vec4(c, 1.0);
}
