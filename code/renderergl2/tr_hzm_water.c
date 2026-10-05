/*
===========================================================================
HZM coop - WATER PASS for allowlisted world water (r_hzmWater). docs/proposals/water_wetness_2026-09-27 (plan.md W3).

WHAT. One extra draw over a real water surface, after its retail stages and before the fog pass: a Fresnel sky
reflection with two world-space ripple layers, a soft sun/moon glint, and no sky where the reflected ray runs into a
roof, a hull or a quay wall. glsl/hzmwater_fp.glsl is the look; the offline twin that fixed every constant is tools/ww_look.py
hzm_water (lookdev/RUBRIC.md: no added sparkle, seams <= 1.07, far field stable, motion checked).

WHERE (the vet's rules, all enforced here or in the shader):
  * ONLY the shaders in hzm_waterwet.h HZM_WaterAllowIndex, tagged at BSP load (R_HZMWetTagShader). No Omaha shader
    is on that list, and the pass ALSO refuses the five Omaha BSPs by name (tr.hzmOmahaWorld).
  * only the world entity's main view: no bmodel, no portal / mirror / sky portal, no shadow or cube view, no depth
    prepass, no 2D.
  * only horizontal faces (the BSP face normal's z >= 0.7) seen from above (the shader discards the rest).

HOW IT DRAWS (the RB_FogPass recipe, tr_shade.c): the same tess, the same deform (ComputeDeformValues -> the
USE_DEFORM_VERTEXES variant), LEQUAL, no depth write. Output is premultiplied (ONE, ONE_MINUS_SRC_ALPHA) and fogged
by the pass itself with generic_fp's global-fog fraction, so over the already-fogged retail colour the result equals
fogging the combined colour once.

OFF. r_hzmWater is flags 0, "-1" = HZM_WATER_AUTO (1, ON, since v1.10.3). With the master off (and after its 1.5 s fade) the pass
returns before touching any GL state: the frame is byte-identical to a build without it. A flip eases over 1.5 s.

BUILT ONCE PER MAP LOAD (R_HZMWaterBuild, from R_HZMWetBuild), only on a non-Omaha map that has allowlisted water:
  * a 256^2 tileable ripple normal map, RGBA8, uncompressed, mipmapped - the Toksvig term reads the shortening of the
    mip-averaged normal, so it must never be block-compressed or renormalised (tools/ww_look.py make_ripple_normal:
    a filtered-noise spectrum, k^-2.2 * exp(-(k / 0.18 N)^2), no wavelengths above half the tile, normals from
    wrapped central differences, slope scaled so the 99th percentile is 0.55).
  * per allowlisted shader NAME, the 90th percentile of its own lightmap's luminance over its surfaces (read back
    from the lightmap texture the shader samples, so overbright, r_mergeLightmaps and HDR lightmaps are all already
    applied): the glint is gated relative to that, so no glint in the water's own baked shadow (vet #13).
  * the occlusion is tr_hzm_wet.c's rain-occlusion map itself (the sigma-48 soft copy is gone, waterfix 2026-10-05).
===========================================================================
*/

#include "tr_local.h"
#include "tr_dsa.h"
#include "../renderercommon/hzm_waterwet.h"
#include "../renderercommon/hzm_storm.h" // HZM coop [2026-09-28] storm darkness
void R_HZM_StormWetUniforms( shaderProgram_t *sp );   // tr_hzm_wet.c

#define HZM_RIPPLE_SIZE     256         // power of two: the FFT below
#define HZM_RIPPLE_SLOPE    0.55f
#define HZM_WATER_TIMEWRAP  1000.0      // every scroll speed x 1000 s is a whole number of tiles: the wrap is seamless

static cvar_t *r_hzmWater;

/*
================
the ripple normal map
================
*/
// in-place iterative radix-2 complex FFT (inverse sign), n a power of two
static void HZM_FFT1( float *re, float *im, int n, int stride )
{
	int		i, j, k, len;

	for ( i = 1, j = 0; i < n; i++ ) {
		int bit = n >> 1;
		for ( ; j & bit; bit >>= 1 ) {
			j ^= bit;
		}
		j ^= bit;
		if ( i < j ) {
			float t;
			t = re[i * stride]; re[i * stride] = re[j * stride]; re[j * stride] = t;
			t = im[i * stride]; im[i * stride] = im[j * stride]; im[j * stride] = t;
		}
	}
	for ( len = 2; len <= n; len <<= 1 ) {
		double	ang = 2.0 * M_PI / len;
		float	wr = (float)cos( ang ), wi = (float)sin( ang );
		for ( i = 0; i < n; i += len ) {
			float cr = 1.0f, ci = 0.0f;
			for ( k = 0; k < len / 2; k++ ) {
				int		a = ( i + k ) * stride, b = ( i + k + len / 2 ) * stride;
				float	xr = re[b] * cr - im[b] * ci;
				float	xi = re[b] * ci + im[b] * cr;
				float	nr;
				re[b] = re[a] - xr; im[b] = im[a] - xi;
				re[a] += xr;        im[a] += xi;
				nr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = nr;
			}
		}
	}
}

static void R_HZMWaterBuildRipple( void )
{
	enum { N = HZM_RIPPLE_SIZE, NBINS = 4096 };
	float			*re, *im, *gx, *gy;
	float			mean = 0.0f, var = 0.0f, sd, hlo = 1e9f, hhi = -1e9f, gmax = 0.0f, g99, s;
	unsigned int	seed = 927u, *hist;
	byte			*pic;
	int				x, y, i, acc;

	re = ri.Malloc( N * N * sizeof( float ) );
	im = ri.Malloc( N * N * sizeof( float ) );
	for ( y = 0; y < N; y++ ) {
		for ( x = 0; x < N; x++ ) {
			// signed integer frequencies (numpy fftfreq * N): periodic over the tile by construction
			float	kx = (float)( x < N / 2 ? x : x - N ), ky = (float)( y < N / 2 ? y : y - N );
			float	k = sqrtf( kx * kx + ky * ky ), amp, ph;

			seed = seed * 1664525u + 1013904223u;
			ph = ( ( seed >> 8 ) & 0xffff ) / 65536.0f * (float)( 2.0 * M_PI );
			amp = ( k < 2.0f ) ? 0.0f : powf( k, -2.2f ) * expf( -( k / ( N * 0.18f ) ) * ( k / ( N * 0.18f ) ) );
			re[y * N + x] = amp * cosf( ph );
			im[y * N + x] = amp * sinf( ph );
		}
	}
	for ( y = 0; y < N; y++ ) {
		HZM_FFT1( re + y * N, im + y * N, N, 1 );
	}
	for ( x = 0; x < N; x++ ) {
		HZM_FFT1( re + x, im + x, N, N );
	}
	ri.Free( im );

	// height = the real part, standardised
	for ( i = 0; i < N * N; i++ ) {
		mean += re[i];
	}
	mean /= N * N;
	for ( i = 0; i < N * N; i++ ) {
		var += ( re[i] - mean ) * ( re[i] - mean );
	}
	sd = sqrtf( var / ( N * N ) ) + 1e-12f;
	for ( i = 0; i < N * N; i++ ) {
		re[i] = ( re[i] - mean ) / sd;
		hlo = MIN( hlo, re[i] );
		hhi = MAX( hhi, re[i] );
	}

	// wrapped central differences, then the slope scale from the 99th percentile of |grad|
	gx = ri.Malloc( N * N * sizeof( float ) );
	gy = ri.Malloc( N * N * sizeof( float ) );
	for ( y = 0; y < N; y++ ) {
		for ( x = 0; x < N; x++ ) {
			float g;
			gx[y * N + x] = ( re[y * N + ( ( x + 1 ) & ( N - 1 ) )] - re[y * N + ( ( x - 1 ) & ( N - 1 ) )] ) * 0.5f;
			gy[y * N + x] = ( re[( ( y + 1 ) & ( N - 1 ) ) * N + x] - re[( ( y - 1 ) & ( N - 1 ) ) * N + x] ) * 0.5f;
			g = sqrtf( gx[y * N + x] * gx[y * N + x] + gy[y * N + x] * gy[y * N + x] );
			gmax = MAX( gmax, g );
		}
	}
	hist = ri.Malloc( NBINS * sizeof( unsigned int ) );
	Com_Memset( hist, 0, NBINS * sizeof( unsigned int ) );
	for ( i = 0; i < N * N; i++ ) {
		float g = sqrtf( gx[i] * gx[i] + gy[i] * gy[i] );
		hist[MIN( NBINS - 1, (int)( g / MAX( gmax, 1e-9f ) * NBINS ) )]++;
	}
	for ( i = 0, acc = 0; i < NBINS; i++ ) {
		acc += hist[i];
		if ( acc >= (int)( 0.99f * N * N ) ) {
			break;
		}
	}
	ri.Free( hist );
	g99 = ( i + 0.5f ) / NBINS * gmax;
	s = HZM_RIPPLE_SLOPE / ( g99 + 1e-9f );

	pic = ri.Malloc( N * N * 4 );
	for ( i = 0; i < N * N; i++ ) {
		vec3_t n;
		VectorSet( n, -gx[i] * s, -gy[i] * s, 1.0f );
		VectorNormalize( n );
		pic[i * 4 + 0] = (byte)( ( n[0] * 0.5f + 0.5f ) * 255.0f + 0.5f );
		pic[i * 4 + 1] = (byte)( ( n[1] * 0.5f + 0.5f ) * 255.0f + 0.5f );
		pic[i * 4 + 2] = (byte)( ( n[2] * 0.5f + 0.5f ) * 255.0f + 0.5f );
		pic[i * 4 + 3] = (byte)( ( re[i] - hlo ) / MAX( hhi - hlo, 1e-6f ) * 255.0f + 0.5f );
	}
	ri.Free( gx );
	ri.Free( gy );
	ri.Free( re );

	// COLORALPHA, not NORMAL: the mips must stay plain averages (a renormalised mip would erase the Toksvig signal)
	tr.hzmRippleImage = R_CreateImage( "*hzmRipple", pic, N, N, IMGTYPE_COLORALPHA,
	                                   IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	ri.Free( pic );
}

/*
================
the per-shader lightmap reference
================
*/
// the lightmap texture a shader's stages sample, or NULL (generic: bundle[0].isLightmap; lightall: bundle[TB_LIGHTMAP])
static image_t *HZM_WaterLightmapImage( const shader_t *sh )
{
	int s, b;

	for ( s = 0; s < MAX_SHADER_STAGES && sh->stages[s] && sh->stages[s]->active; s++ ) {
		for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
			const textureBundle_t *bundle = &sh->stages[s]->bundle[b];
			if ( bundle->isLightmap && bundle->image[0] && bundle->image[0] != tr.whiteImage ) {
				return bundle->image[0];
			}
		}
	}
	return NULL;
}

static void R_HZMWaterBuildLightmapRefs( void )
{
	enum { BINS = 256, MAXREAD = 8 };
	static const float bary[7][3] = {
		{ 0.3333f, 0.3333f, 0.3334f }, { 0.6667f, 0.1667f, 0.1666f }, { 0.1667f, 0.6667f, 0.1666f },
		{ 0.1667f, 0.1667f, 0.6666f }, { 0.5f, 0.5f, 0.0f }, { 0.0f, 0.5f, 0.5f }, { 0.5f, 0.0f, 0.5f },
	};
	unsigned int	*hist;
	float			ref[HZM_WATER_NUM_ALLOW];
	image_t			*readImg[MAXREAD];
	byte			*readPix[MAXREAD];
	int				numRead = 0, i, k, t, slot;

	hist = ri.Malloc( HZM_WATER_NUM_ALLOW * BINS * sizeof( unsigned int ) );
	Com_Memset( hist, 0, HZM_WATER_NUM_ALLOW * BINS * sizeof( unsigned int ) );

	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		msurface_t			*surf = &tr.world->surfaces[i];
		srfBspSurface_t		*srf;
		image_t				*lm;
		byte				*pix = NULL;
		int					w, h;

		if ( !surf->shader || !surf->shader->hzmWater || !surf->data ) {
			continue;
		}
		if ( *surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES ) {
			continue;
		}
		lm = HZM_WaterLightmapImage( surf->shader );
		if ( !lm || !lm->texnum || !qglGetTexImage ) {
			continue;
		}
		w = lm->uploadWidth;
		h = lm->uploadHeight;
		if ( w < 1 || h < 1 || w > glConfig.maxTextureSize || h > glConfig.maxTextureSize ) {
			continue;
		}
		for ( k = 0; k < numRead; k++ ) {
			if ( readImg[k] == lm ) {
				pix = readPix[k];
				break;
			}
		}
		if ( !pix ) {
			if ( numRead == MAXREAD ) {
				continue;     // more lightmap pages under water than any stock map has: those surfaces keep no gate
			}
			pix = ri.Malloc( w * h * 4 );
			// the tr_gore.c R_GoreReadTexImage pattern: raw bind on unit 0, read, then re-sync the DSA bind cache
			qglActiveTexture( GL_TEXTURE0 );
			qglBindTexture( GL_TEXTURE_2D, lm->texnum );
			qglGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix );
			GL_BindNullTextures();
			readImg[numRead] = lm;
			readPix[numRead] = pix;
			numRead++;
		}

		slot = surf->shader->hzmWater - 1;
		srf = (srfBspSurface_t *)surf->data;
		for ( t = 0; t + 2 < srf->numIndexes; t += 3 ) {
			const srfVert_t *a = &srf->verts[srf->indexes[t]];
			const srfVert_t *b = &srf->verts[srf->indexes[t + 1]];
			const srfVert_t *c = &srf->verts[srf->indexes[t + 2]];
			if ( srf->indexes[t] >= (glIndex_t)srf->numVerts || srf->indexes[t + 1] >= (glIndex_t)srf->numVerts
			     || srf->indexes[t + 2] >= (glIndex_t)srf->numVerts ) {
				continue;
			}
			for ( k = 0; k < 7; k++ ) {
				float	u = bary[k][0] * a->lightmap[0] + bary[k][1] * b->lightmap[0] + bary[k][2] * c->lightmap[0];
				float	v = bary[k][0] * a->lightmap[1] + bary[k][1] * b->lightmap[1] + bary[k][2] * c->lightmap[1];
				int		px = MAX( 0, MIN( w - 1, (int)floor( u * w ) ) );
				int		py = MAX( 0, MIN( h - 1, (int)floor( v * h ) ) );
				const byte *p = &pix[( py * w + px ) * 4];
				float	lum = ( 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] ) / 255.0f;
				hist[slot * BINS + MIN( BINS - 1, (int)( lum * BINS ) )]++;
			}
		}
	}
	for ( k = 0; k < numRead; k++ ) {
		ri.Free( readPix[k] );
	}

	// p90 per shader name; below 0.02 there is no light variation worth gating (the twin's rule)
	for ( slot = 0; slot < HZM_WATER_NUM_ALLOW; slot++ ) {
		unsigned int total = 0, acc = 0;
		ref[slot] = 0.0f;
		for ( k = 0; k < BINS; k++ ) {
			total += hist[slot * BINS + k];
		}
		if ( !total ) {
			continue;
		}
		for ( k = 0; k < BINS; k++ ) {
			acc += hist[slot * BINS + k];
			if ( acc >= (unsigned int)( 0.9f * total ) ) {
				break;
			}
		}
		ref[slot] = ( k + 0.5f ) / BINS;
		if ( ref[slot] < 0.02f ) {
			ref[slot] = 0.0f;
		}
	}
	ri.Free( hist );

	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		shader_t *sh = tr.world->surfaces[i].shader;
		if ( sh && sh->hzmWater ) {
			sh->hzmWaterLmRef = HZM_WaterLightmapImage( sh ) ? ref[sh->hzmWater - 1] : 0.0f;
		}
	}
}

/*
================
R_HZMWaterBuild - from R_HZMWetBuild (tr_hzm_wet.c), after the occlusion maps; never on an Omaha BSP.
================
*/
void R_HZMWaterBuild( void )
{
	r_hzmWater = ri.Cvar_Get( "r_hzmWater", "-1", 0 );

	tr.hzmWaterReady = qfalse;
	tr.hzmRippleImage = NULL;
	if ( tr.hzmOmahaWorld || !tr.hzmWaterWorld ) {
		return;
	}
	R_HZMWaterBuildRipple();
	R_HZMWaterBuildLightmapRefs();
	tr.hzmWaterReady = ( tr.hzmRippleImage && tr.hzmWaterShader[0].program ) ? qtrue : qfalse;
	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMWATER %d surfaces, ready %d\n", tr.hzmWaterWorld, tr.hzmWaterReady );
}

/*
================
the live master fade (the r_hzmWet one, its own clock)
================
*/
static float HZM_WaterFade( void )
{
	static float	s_fade, s_last;
	float			t = backEnd.refdef.floatTime, target, dt;
	int				master;

	if ( !r_hzmWater ) {
		return 0.0f;
	}
	master = r_hzmWater->integer < 0 ? HZM_WATER_AUTO : r_hzmWater->integer;
	target = ( master == 1 && tr.hzmWaterReady && !tr.hzmOmahaWorld ) ? 1.0f : 0.0f;
	if ( t != s_last ) {
		dt = t - s_last;
		s_last = t;
		if ( dt < 0.0f || dt > 0.25f ) {
			dt = ( dt < 0.0f ) ? 1.5f : 0.25f;     // a map change resets the clock: settle at once
		}
		if ( s_fade < target ) {
			s_fade = MIN( target, s_fade + dt / 1.5f );
		} else if ( s_fade > target ) {
			s_fade = MAX( target, s_fade - dt / 1.5f );
		}
	}
	return tr.hzmWaterReady ? s_fade : 0.0f;
}

/*
================
RB_HZMWaterPass - RB_StageIteratorGeneric, after the retail stages and dynamic lights, before the fog pass, for a
tess whose shader is tagged hzmWater. deformGen/deformParams are that iterator's own ComputeDeformValues().
================
*/
void RB_HZMWaterPass( int deformGen, const vec5_t deformParams )
{
	shaderProgram_t	*sp;
	vec4_t			water, occz;
	float			fade;
	image_t			*lm = NULL;

	fade = HZM_WaterFade();
	if ( fade <= 0.0f
	     || backEnd.currentEntity != &tr.worldEntity
	     || backEnd.depthFill
	     || backEnd.projection2D
	     || ( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) )
	     || backEnd.viewParms.isPortal
	     || backEnd.viewParms.isPortalSky
	     || ( tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo )
	     || glState.vertexAnimation
	     || glState.boneAnimation
	     || r_lightmap->integer
	     || !tess.numIndexes ) {
		return;
	}

	sp = &tr.hzmWaterShader[deformGen != DGEN_NONE ? 1 : 0];
	if ( !sp->program ) {
		return;
	}
	GLSL_BindProgram( sp );

	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
	if ( deformGen != DGEN_NONE ) {
		GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
		GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );
	}
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );

	if ( tess.shader->hzmWaterLmRef > 0.0f ) {
		lm = HZM_WaterLightmapImage( tess.shader );
	}
	water[0] = fade;
	water[1] = (float)fmod( backEnd.refdef.floatTime, HZM_WATER_TIMEWRAP );
	water[2] = lm ? tess.shader->hzmWaterLmRef : 0.0f;
	water[3] = 1.0f;
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWATER, water );

	// [waterfix 2026-10-05] the SHARP rain-occlusion map; the shader marches the reflected ray through it (the old
	// Gaussian-blurred height copy raised every bank over the water beside it and killed the reflection along canals)
	if ( tr.hzmRainOccImage ) {
		occz[0] = tr.hzmRainOccZ[0];
		occz[1] = tr.hzmRainOccZ[1];
		GLSL_SetUniformVec4( sp, UNIFORM_HZMWETOCC, tr.hzmRainOccXform );
	} else {
		static const vec4_t open = { 0.0f, 0.0f, 0.0f, 0.0f };
		occz[0] = -1e6f;     // nothing on this map shelters anything: zrange 0 makes the shader skip the march
		occz[1] = 0.0f;
		GLSL_SetUniformVec4( sp, UNIFORM_HZMWETOCC, open );
	}
	occz[2] = occz[3] = 0.0f;
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWETOCCZ, occz );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSKYZ, tr.hzmSkyZenith );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSKYH, tr.hzmSkyHorizon );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSUN, tr.hzmWetSun );
	// HZM coop [2026-09-28] storm darkness (vet F14): the water, like the wet film, loses the sun glint under a storm
	// deck and reflects the darker sky
	R_HZM_StormWetUniforms( sp );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSUNCOL, tr.hzmWetSunCol );

	// the fog colour a SRC_ALPHA/ONE_MINUS_SRC_ALPHA draw would get; the shader applies it premultiplied itself
	RB_SetGlobalFogUniforms( sp, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA, qfalse );

	GL_BindToTMU( tr.hzmRainOccImage ? tr.hzmRainOccImage : tr.whiteImage, TB_HZMRAINOCC );
	GL_BindToTMU( tr.hzmRippleImage, TB_HZMWETNOISE );
	GL_BindToTMU( lm ? lm : tr.whiteImage, TB_LIGHTMAP );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	R_DrawElements( tess.numIndexes, tess.firstIndex );
}
