// HZM gl2 re-port: MOHAA GLOBAL farplane distance fog as a screen-space depth pass.
// gl2's GLSL pipeline only fogs Q3 fog-BRUSH volumes; MOHAA's global atmospheric distance
// fog (viewParms.farplane_distance/bias/color) was never applied. This mirrors gl1's
// fixed-function GL_FOG_LINEAR (START = farplane_bias, END = farplane_distance,
// COLOR = farplane_color * tr.identityLight) - renderergl1/tr_backend.c RB_SetupFog.
//
// v2 (fog-parity pass): the eye distance is reconstructed straight from the PROJECTION
// MATRIX terms that were actually used to rasterise this view, instead of from r_znear /
// viewParms.zFar. Those can diverge from the real projection (portal oblique matrices,
// the ADS weapon projection, a sub-view clobbering tr.viewParms, an r_znear change made
// after the matrix was built) and ANY divergence skews the whole fog curve, which reads
// on screen as the fog is much thinner than gl1.
//
//   z_ndc = 2*zw - 1,  d = P14 / (P10 + z_ndc)      (both terms negative -> d > 0)
//   d(zw=0) = zNear and d(zw=1) = zFar exactly, for any zNear/zFar.

uniform sampler2D u_TextureMap;    // scene colour  (TB_COLORMAP)
uniform sampler2D u_LevelsMap;     // scene depth   (TB_LEVELSMAP)

uniform vec4      u_ViewInfo;      // (projMat[10], projMat[14], skyEnable, debugMode)
uniform vec4      u_FogColorMask;  // (fogColor.rgb, unused)
uniform vec4      u_FogDistance;   // (fogStart, fogEnd, 1/(fogEnd-fogStart), fogScale)
uniform vec4      u_FogDepth;      // (1/projMat[0], 1/projMat[5], radialEnable, unused)

varying vec2      var_TexCoords;

void main()
{
	vec4  scene = texture2D(u_TextureMap, var_TexCoords);

	float zw    = texture2D(u_LevelsMap, var_TexCoords).r;

	// exact eye-space (planar) distance from the real projection matrix. clamp the
	// denominator away from zero so a depth of exactly 1.0 cannot produce inf/NaN.
	float denom = u_ViewInfo.x + (2.0 * zw - 1.0);
	float dist  = u_ViewInfo.y / min(denom, -1e-6);

	// gl1's fixed-function fog coordinate is the planar eye Z. r_globalFogRadial 1
	// switches to the true radial eye distance (what the GL spec nominally asks for)
	// so the two can be A/B'd.
	if (u_FogDepth.z > 0.0)
	{
		vec2 ndc = var_TexCoords * 2.0 - 1.0;
		dist *= length(vec3(ndc.x * u_FogDepth.x, ndc.y * u_FogDepth.y, 1.0));
	}

	// gl1: f = (END - c) / (END - START); the fogged fraction is 1 - f
	float frac = clamp((dist - u_FogDistance.x) * u_FogDistance.z, 0.0, 1.0) * u_FogDistance.w;

	// r_globalFogSky 0 -> leave sky pixels (depth at the far plane) untouched, which is
	// what gl1 does for any sky shader carrying the nofog keyword (tr_shader.c clears
	// GLS_FOG_ENABLED on those stages).
	//
	// [user 2026-08-02] bug-1296 - EPSILON TIGHTENED 1e-5 -> 1e-7. This test is meant to mean
	// 'nothing wrote depth at this pixel', but WINDOW DEPTH IS NONLINEAR: 1-zw ~= zNear*(1/d - 1/zFar),
	// and with r_znear 4 an epsilon of 1e-5 exempts everything beyond 98.0% of zFar at zFar=8000,
	// 95.2% at 20000 and 88.9% at 50000 - i.e. a slab up to ~5500 units DEEP of ordinary
	// depth-writing geometry. Distant treelines and aircraft fell inside it and were skipped by the
	// fog entirely, rendering at full brightness against a fogged scene: the user's 'trees and the
	// plane look kinda highlighted in white' on e2l1. (It also explains why r_globalFogSky 1 buried
	// them instead - same slab, fogged at zFar rather than at their true distance.)
	// 1e-7 shrinks the slab to 1.6 / 10 / 62 units at those same far planes - below one pixel of
	// depth precision - while still catching true no-geometry pixels, because qglClearDepth is
	// exactly 1.0 (tr_init.c:1195) and the skybox writes no depth at all (tr_sky.c:878 depthRange
	// 1,1 + :885 GL_State(0)). Note 0.9999999 is also the smallest float32 step below 1.0, so this
	// is effectively 'exactly at the far plane' and cannot be tightened further in a 32-bit float.
	// NOT a fix for genuinely non-depth-writing surfaces (blendfunc without depthWrite, e.g. a
	// propeller disc) - those leave depth at 1.0 and no depth-based pass can reach them. That is a
	// separate, larger problem; see docs/OPEN.md.
	if (u_ViewInfo.z < 0.5 && zw >= 0.9999999)
	{
		frac = 0.0;
	}

	frac = clamp(frac, 0.0, 1.0);

	if (u_ViewInfo.w > 1.5)
	{
		// r_globalFogDebug 3 - reconstructed distance, one full ramp per 4096 units
		float d = fract(dist * (1.0 / 4096.0));
		gl_FragColor = vec4(d, d, d, 1.0);
	}
	else if (u_ViewInfo.w > 0.5)
	{
		// r_globalFogDebug 2 - fog fraction as greyscale
		gl_FragColor = vec4(frac, frac, frac, 1.0);
	}
	else
	{
		// [user 2026-08-03] bug-1299 - CLAMP THE SCENE TO gl1's INPUT DOMAIN BEFORE MIXING.
		// gl1 blends fog into a fixed-point RGBA8 backbuffer, so its input is inherently <= 1.0.
		// gl2's source is a FLOAT HDR FBO (GL_RGBA16F_ARB, tr_fbo.c:269 / tr_image.c:3513) and with
		// overbright in play (tr.overbrightMult, tr_image.c:3666) a lit surface sits near 2.0. Mixing
		// unclamped means fog is effectively SKIPPED on every bright pixel:
		//     gl1  mix(1.0, 0.6, 0.5) = 0.80   -> fogs correctly
		//     gl2  mix(2.0, 0.6, 0.5) = 1.30   -> still above 1.0, displays WHITE
		// That is the reported 'distant trees / flak guns / objects render white', and why it was
		// immune to every fog, DoF, bloom, exposure, tonemap, anisotropy and MSAA cvar - it is not a
		// setting, it is the mix operand. The identical defect was already found and fixed one stage
		// later in tonemap_hzm_fp.glsl (its comment measures e2l2 near 84.7 vs gl1 57.4); the fog
		// pass was missed. Same remedy, same justification.
		gl_FragColor = vec4(mix(clamp(scene.rgb, 0.0, 1.0), u_FogColorMask.rgb, frac), scene.a);
	}
}
