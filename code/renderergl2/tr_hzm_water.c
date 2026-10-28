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
qboolean RB_HZM_SceneDepthSnapshot( void );           // tr_backend.c
void RB_HZM_BindSceneDepth( void );

#define HZM_V4(v, a, b, c, d) ( (v)[0] = (a), (v)[1] = (b), (v)[2] = (c), (v)[3] = (d) )
#define HZM_RIPPLE_SIZE     256         // power of two: the FFT below
#define HZM_RIPPLE_SLOPE    0.55f
#define HZM_WATER_TIMEWRAP  1000.0      // every scroll speed x 1000 s is a whole number of tiles: the wrap is seamless

static cvar_t *r_hzmWater;
static cvar_t *r_hzmWaterDbg;
static cvar_t *r_hzmWaterPlanar;     // 1 = render the planar reflection of COLD preset water; 0 (default, until proven) = sky probe only
static cvar_t *r_hzmWaterPlanarDbg;  // debug bits: 1 no oblique clip, 4 isMirror off, 8 no axis reflection, 16 render without VPF_NOVIEWMODEL
qboolean hzmWaterSnapValid;          // the scene-depth snapshot of THIS view was taken at its first preset water batch (reset in RB_DrawSurfs)
int hzmPlanarNoOblique;              // read by R_SetupProjection (tr_main.c)
static cvar_t *r_hzmWaterBody;       // tuning: multiplies the preset body colours (default 2.0)
static cvar_t *r_hzmWaterIce;        // tuning: ICE preset lightmap gain multiplier (default 0.7)
static cvar_t *r_hzmWaterFloe;       // tuning: multiplies the floe amount (default 1.0)
static cvar_t *r_hzmWaterContact;    // contact foam / darkening around geometry that meets the water (needs the scene-depth snapshot); default 1
static cvar_t *r_hzmWaterPreset;     // 1 (default) = the map's water preset, 0 = the plain reflection layer (A/B captures)

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
WATER PRESETS: the load-time pieces (hzm_waterwet.h HZM_WPRESET_*). Built only when this map has preset water.
  * hzmFloeImage: 256^2 tileable fbm noise, one independent field per channel, standardised to mean 0.5 sd 0.11 (the
    prototype's thresholds - tools/water_look.py - are written against that distribution).
  * hzmIceImage: textures/hzm/ice_pack (ambientCG Ice001, CC0: R,G normal xy, B roughness, A colour luminance) when the pak carries it, else a generated flat tile.
  * hzmQuayImage: R8, distance to the nearest QUAY WALL (near-vertical drawn faces that reach the waterline), 16 u cells.
    A roofed pen is a roof over every cell, so the rain-occlusion height map cannot say where the walls are.
================
*/
static unsigned int HZM_Hash2( int x, int y, unsigned int seed )
{
	unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u + seed * 2246822519u;
	h = ( h ^ ( h >> 13 ) ) * 1274126177u;
	return h ^ ( h >> 16 );
}

// periodic value noise: lattice of `period` cells over the tile, smoothstep interpolation
static float HZM_PerNoise( float u, float v, int period, unsigned int seed )
{
	float	fx = u * period, fy = v * period, tx, ty;
	int		x0 = (int)floorf( fx ), y0 = (int)floorf( fy );
	float	a, b, c, d;

	tx = fx - x0;
	ty = fy - y0;
	tx = tx * tx * ( 3.0f - 2.0f * tx );
	ty = ty * ty * ( 3.0f - 2.0f * ty );
	a = ( HZM_Hash2( ( x0 % period + period ) % period, ( y0 % period + period ) % period, seed ) & 0xffff ) / 65535.0f;
	b = ( HZM_Hash2( ( ( x0 + 1 ) % period + period ) % period, ( y0 % period + period ) % period, seed ) & 0xffff ) / 65535.0f;
	c = ( HZM_Hash2( ( x0 % period + period ) % period, ( ( y0 + 1 ) % period + period ) % period, seed ) & 0xffff ) / 65535.0f;
	d = ( HZM_Hash2( ( ( x0 + 1 ) % period + period ) % period, ( ( y0 + 1 ) % period + period ) % period, seed ) & 0xffff ) / 65535.0f;
	return ( a * ( 1.0f - tx ) + b * tx ) * ( 1.0f - ty ) + ( c * ( 1.0f - tx ) + d * tx ) * ty;
}

static void R_HZMWaterBuildFloeNoise( void )
{
	enum { N = 256 };
	float	*f;
	byte	*pic;
	int		ch, x, y, o;

	f = ri.Malloc( N * N * 3 * sizeof( float ) );
	pic = ri.Malloc( N * N * 4 );
	for ( ch = 0; ch < 3; ch++ ) {
		double sum = 0.0, sum2 = 0.0, mean, sd;
		for ( y = 0; y < N; y++ ) {
			for ( x = 0; x < N; x++ ) {
				float	u = ( x + 0.5f ) / N, v = ( y + 0.5f ) / N, a = 1.0f, t = 0.0f, val = 0.0f;
				int		per = 4;
				for ( o = 0; o < 5; o++, per *= 2, a *= 0.55f ) {
					val += a * HZM_PerNoise( u, v, per, 17u + ch * 31u + o );
					t += a;
				}
				val /= t;
				f[( y * N + x ) * 3 + ch] = val;
				sum += val;
				sum2 += (double)val * val;
			}
		}
		mean = sum / ( N * N );
		sd = sqrt( MAX( sum2 / ( N * N ) - mean * mean, 1e-12 ) );
		for ( y = 0; y < N; y++ ) {
			for ( x = 0; x < N; x++ ) {
				float z = (float)( ( f[( y * N + x ) * 3 + ch] - mean ) / sd * 0.11 + 0.5 );
				pic[( y * N + x ) * 4 + ch] = (byte)( MAX( 0.0f, MIN( 1.0f, z ) ) * 255.0f + 0.5f );
			}
		}
	}
	for ( y = 0; y < N * N; y++ ) {
		pic[y * 4 + 3] = 255;
	}
	ri.Free( f );
	tr.hzmFloeImage = R_CreateImage( "*hzmFloeNoise", pic, N, N, IMGTYPE_COLORALPHA,
	                                 IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	ri.Free( pic );
}

static void R_HZMWaterBuildIce( void )
{
	enum { N = 64 };
	byte	pic[N * N * 4];
	int		i;

	tr.hzmIceImage = R_FindImageFile( "textures/hzm/ice_pack", IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE );
	if ( tr.hzmIceImage && tr.hzmIceImage != tr.defaultImage ) {
		return;
	}
	// no pak art: a flat mid-bright tile; the shader maps its luminance to ice colours, so this reads as plain plates
	for ( i = 0; i < N * N; i++ ) {
		pic[i * 4 + 0] = 128; pic[i * 4 + 1] = 128; pic[i * 4 + 2] = 128; pic[i * 4 + 3] = 150;     // pack: normal xy, roughness, luminance
	}
	tr.hzmIceImage = R_CreateImage( "*hzmIceFlat", pic, N, N, IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
}

static void HZM_QuayMark( byte *grid, int gw, int gh, float x0, float y0, float cell, const float *p )
{
	int ix = (int)( ( p[0] - x0 ) / cell ), iy = (int)( ( p[1] - y0 ) / cell );
	if ( ix >= 0 && ix < gw && iy >= 0 && iy < gh ) {
		grid[iy * gw + ix] = 1;
	}
}

static void R_HZMWaterBuildQuay( void )
{
	enum { MAXDIM = 1024 };
	float		lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f }, zsum = 0.0f, zw, cell = 16.0f, maxDist = 512.0f;
	int			zn = 0, i, t, k, gw, gh, x, y;
	byte		*grid, *img;
	float		*dist;

	// the waterline and the extent of the preset water
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		msurface_t *surf = &tr.world->surfaces[i];
		srfBspSurface_t *srf;
		if ( !surf->shader || !surf->shader->hzmWaterPreset || !surf->data ) {
			continue;
		}
		if ( *surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES ) {
			continue;
		}
		srf = (srfBspSurface_t *)surf->data;
		for ( k = 0; k < srf->numVerts; k++ ) {
			const float *p = srf->verts[k].xyz;
			lo[0] = MIN( lo[0], p[0] ); lo[1] = MIN( lo[1], p[1] );
			hi[0] = MAX( hi[0], p[0] ); hi[1] = MAX( hi[1], p[1] );
			zsum += p[2];
			zn++;
		}
	}
	if ( !zn ) {
		return;
	}
	zw = zsum / zn;
	while ( ( hi[0] - lo[0] ) / cell > MAXDIM || ( hi[1] - lo[1] ) / cell > MAXDIM ) {
		cell *= 2.0f;
	}
	gw = (int)( ( hi[0] - lo[0] ) / cell ) + 1;
	gh = (int)( ( hi[1] - lo[1] ) / cell ) + 1;
	grid = ri.Malloc( gw * gh );
	Com_Memset( grid, 0, gw * gh );

	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		msurface_t *surf = &tr.world->surfaces[i];
		srfBspSurface_t *srf;
		if ( !surf->shader || surf->shader->hzmWater || surf->shader->isSky || !surf->data ) {
			continue;
		}
		if ( *surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES ) {
			continue;
		}
		srf = (srfBspSurface_t *)surf->data;
		for ( t = 0; t + 2 < srf->numIndexes; t += 3 ) {
			const float *a, *b, *c;
			vec3_t	e1, e2, n;
			float	zmin, zmax, len, maxEdge;
			int		ns, u, v;

			if ( srf->indexes[t] >= (glIndex_t)srf->numVerts || srf->indexes[t + 1] >= (glIndex_t)srf->numVerts
			     || srf->indexes[t + 2] >= (glIndex_t)srf->numVerts ) {
				continue;
			}
			a = srf->verts[srf->indexes[t]].xyz;
			b = srf->verts[srf->indexes[t + 1]].xyz;
			c = srf->verts[srf->indexes[t + 2]].xyz;
			zmin = MIN( a[2], MIN( b[2], c[2] ) );
			zmax = MAX( a[2], MAX( b[2], c[2] ) );
			if ( zmin >= zw + 160.0f || zmax <= zw - 40.0f ) {
				continue;
			}
			VectorSubtract( b, a, e1 );
			VectorSubtract( c, a, e2 );
			CrossProduct( e1, e2, n );
			len = VectorLength( n );
			if ( len < 1e-3f || fabsf( n[2] ) / len >= 0.5f ) {
				continue;       // not a wall
			}
			maxEdge = MAX( VectorLength( e1 ), VectorLength( e2 ) );
			ns = MIN( 64, MAX( 1, (int)ceilf( maxEdge / ( cell * 0.5f ) ) ) );
			for ( u = 0; u <= ns; u++ ) {
				for ( v = 0; v <= ns - u; v++ ) {
					float p[3], fu = (float)u / ns, fv = (float)v / ns;
					p[0] = a[0] + e1[0] * fu + e2[0] * fv;
					p[1] = a[1] + e1[1] * fu + e2[1] * fv;
					p[2] = a[2] + e1[2] * fu + e2[2] * fv;
					if ( p[2] > zw - 40.0f && p[2] < zw + 160.0f ) {
						HZM_QuayMark( grid, gw, gh, lo[0], lo[1], cell, p );
					}
				}
			}
		}
	}

	// 3-4 chamfer distance transform (two passes), in cells
	dist = ri.Malloc( gw * gh * sizeof( float ) );
	for ( i = 0; i < gw * gh; i++ ) {
		dist[i] = grid[i] ? 0.0f : 1e6f;
	}
	for ( y = 0; y < gh; y++ ) {
		for ( x = 0; x < gw; x++ ) {
			float d = dist[y * gw + x];
			if ( x > 0 )               d = MIN( d, dist[y * gw + x - 1] + 1.0f );
			if ( y > 0 )               d = MIN( d, dist[( y - 1 ) * gw + x] + 1.0f );
			if ( x > 0 && y > 0 )      d = MIN( d, dist[( y - 1 ) * gw + x - 1] + 1.4142f );
			if ( x < gw - 1 && y > 0 ) d = MIN( d, dist[( y - 1 ) * gw + x + 1] + 1.4142f );
			dist[y * gw + x] = d;
		}
	}
	for ( y = gh - 1; y >= 0; y-- ) {
		for ( x = gw - 1; x >= 0; x-- ) {
			float d = dist[y * gw + x];
			if ( x < gw - 1 )                d = MIN( d, dist[y * gw + x + 1] + 1.0f );
			if ( y < gh - 1 )                d = MIN( d, dist[( y + 1 ) * gw + x] + 1.0f );
			if ( x < gw - 1 && y < gh - 1 )  d = MIN( d, dist[( y + 1 ) * gw + x + 1] + 1.4142f );
			if ( x > 0 && y < gh - 1 )       d = MIN( d, dist[( y + 1 ) * gw + x - 1] + 1.4142f );
			dist[y * gw + x] = d;
		}
	}
	img = ri.Malloc( gw * gh * 4 );
	for ( i = 0; i < gw * gh; i++ ) {
		float d = MIN( dist[i] * cell, maxDist ) / maxDist;
		img[i * 4 + 0] = (byte)( d * 255.0f + 0.5f );
		img[i * 4 + 1] = img[i * 4 + 2] = 0;
		img[i * 4 + 3] = 255;
	}
	// the image is gw x gh; GL needs nothing special (non power of two is fine on gl2); linear filter, clamp
	tr.hzmQuayImage = R_CreateImage( "*hzmQuay", img, gw, gh, IMGTYPE_COLORALPHA,
	                                 IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
	tr.hzmQuayXform[0] = lo[0];
	tr.hzmQuayXform[1] = lo[1];
	tr.hzmQuayXform[2] = 1.0f / ( gw * cell );
	tr.hzmQuayXform[3] = 1.0f / ( gh * cell );
	tr.hzmQuayMax = maxDist;
	ri.Free( img );
	ri.Free( dist );
	ri.Free( grid );
	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMWATER quay map %dx%d cell %.0f waterline %.1f\n", gw, gh, cell, zw );
}

FBO_t	*FBO_Create( const char *name, int width, int height );
void	FBO_CreateBuffer( FBO_t *fbo, int format, int index, int multisample );
void	FBO_AttachImage( FBO_t *fbo, image_t *image, GLenum attachment, GLuint cubemapside );
qboolean R_CheckFBO( const FBO_t *fbo );

// the planar reflection target: half the scene size, the scene's colour format, own depth renderbuffer
static void R_HZMWaterBuildPlanar( void )
{
	float	zsum = 0.0f;
	int		zn = 0, i, k, w, h, fmt;

	tr.hzmColdWorld = qfalse;
	tr.hzmPlanarLive = qfalse;
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		msurface_t *surf = &tr.world->surfaces[i];
		srfBspSurface_t *srf;
		if ( !surf->shader || surf->shader->hzmWaterPreset != HZM_WPRESET_COLD || !surf->data ) {
			continue;
		}
		if ( *surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES ) {
			continue;
		}
		srf = (srfBspSurface_t *)surf->data;
		for ( k = 0; k < srf->numVerts; k++ ) {
			zsum += srf->verts[k].xyz[2];
			zn++;
		}
	}
	if ( !zn ) {
		return;
	}
	tr.hzmColdWorld = qtrue;
	tr.hzmColdZ = zsum / zn;
	if ( !glRefConfig.framebufferObject || !tr.renderImage ) {
		return;
	}
	w = MAX( 256, tr.renderImage->width / 2 );
	h = MAX( 144, tr.renderImage->height / 2 );
	fmt = ( r_hdr->integer && glRefConfig.textureFloat ) ? GL_RGBA16F_ARB : GL_RGBA8;
	if ( tr.hzmPlanarFbo && tr.hzmPlanarImage ) {
		return;       // created on an earlier map of this renderer instance: reused (FBOs live until vid_restart)
	}
	tr.hzmPlanarImage = R_CreateImage( "*hzmPlanar", NULL, w, h, IMGTYPE_COLORALPHA,
	                                   IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, fmt );
	tr.hzmPlanarFbo = FBO_Create( "_hzmPlanar", w, h );
	FBO_AttachImage( tr.hzmPlanarFbo, tr.hzmPlanarImage, GL_COLOR_ATTACHMENT0, 0 );
	FBO_CreateBuffer( tr.hzmPlanarFbo, GL_DEPTH_COMPONENT24, 0, 0 );
	if ( !R_CheckFBO( tr.hzmPlanarFbo ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: hzm water planar FBO incomplete; sky reflection only\n" );
		tr.hzmPlanarFbo = NULL;
		tr.hzmPlanarImage = NULL;
	}
	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMWATER planar %dx%d waterline %.1f fbo %d\n", w, h, tr.hzmColdZ, tr.hzmPlanarFbo ? 1 : 0 );
}

// RIVER preset: the principal axis (PCA of the vertex xy) of the surfaces that carry it - the way the water runs
static void R_HZMWaterBuildFlow( void )
{
	double	n = 0, mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0;
	int		i, k;

	tr.hzmFlowDir[0] = 1.0f;
	tr.hzmFlowDir[1] = 0.0f;
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		msurface_t *surf = &tr.world->surfaces[i];
		srfBspSurface_t *srf;
		if ( !surf->shader || surf->shader->hzmWaterPreset != HZM_WPRESET_RIVER || !surf->data ) {
			continue;
		}
		if ( *surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES ) {
			continue;
		}
		srf = (srfBspSurface_t *)surf->data;
		for ( k = 0; k < srf->numVerts; k++ ) {
			double x = srf->verts[k].xyz[0], y = srf->verts[k].xyz[1];
			n += 1; mx += x; my += y; sxx += x * x; syy += y * y; sxy += x * y;
		}
	}
	if ( n > 3 ) {
		double vx, vy, cxy, th;
		mx /= n; my /= n;
		vx = sxx / n - mx * mx; vy = syy / n - my * my; cxy = sxy / n - mx * my;
		th = 0.5 * atan2( 2.0 * cxy, vx - vy );
		tr.hzmFlowDir[0] = (float)cos( th );
		tr.hzmFlowDir[1] = (float)sin( th );
	}
}

static void R_HZMWaterBuildPresets( void )
{
	tr.hzmQuayImage = NULL;
	tr.hzmFloeImage = NULL;
	tr.hzmIceImage = NULL;
	tr.hzmColdWorld = qfalse;
	tr.hzmPlanarLive = qfalse;
	if ( !tr.hzmWaterPresetWorld ) {
		return;
	}
	R_HZMWaterBuildFlow();
	R_HZMWaterBuildFloeNoise();
	R_HZMWaterBuildIce();
	R_HZMWaterBuildPlanar();      // finds the COLD waterline (tr.hzmColdWorld) and makes the planar target
	if ( tr.hzmColdWorld ) {
		R_HZMWaterBuildQuay();    // the quay-distance map is only for the floes of the COLD preset
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
	r_hzmWaterDbg = ri.Cvar_Get( "r_hzmWaterDbg", "0", 0 );
	r_hzmWaterPreset = ri.Cvar_Get( "r_hzmWaterPreset", "1", 0 );
	r_hzmWaterPlanar = ri.Cvar_Get( "r_hzmWaterPlanar", "0", 0 );     // OFF until the planar view is proven (user 2026-10-25)
	r_hzmWaterBody = ri.Cvar_Get( "r_hzmWaterBody", "2.0", 0 );
	r_hzmWaterFloe = ri.Cvar_Get( "r_hzmWaterFloe", "1.0", 0 );
	r_hzmWaterContact = ri.Cvar_Get( "r_hzmWaterContact", "1", 0 );
	r_hzmWaterIce = ri.Cvar_Get( "r_hzmWaterIce", "0.7", 0 );
	r_hzmWaterPlanarDbg = ri.Cvar_Get( "r_hzmWaterPlanarDbg", "0", 0 );

	tr.hzmWaterReady = qfalse;
	tr.hzmRippleImage = NULL;
	if ( ( tr.hzmOmahaWorld && !tr.hzmWaterPresetWorld ) || !tr.hzmWaterWorld ) {     // Omaha: water presets only (user 2026-10-25)
		return;
	}
	R_HZMWaterBuildRipple();
	R_HZMWaterBuildLightmapRefs();
	R_HZMWaterBuildPresets();
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
	target = ( master == 1 && tr.hzmWaterReady && ( !tr.hzmOmahaWorld || tr.hzmWaterPresetWorld ) ) ? 1.0f : 0.0f;
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
the Omaha sea is drawn by brush-model entities (script_object ocean sheets at origin 0 0 0, LANE-A in water_omaha_2026-09-05): the
pass must not skip them, but only when the entity transform is the identity (the vertex shader reads model-space positions as
world positions) and the shader carries a preset
*/
static qboolean HZM_WaterEntityAtOrigin( void )
{
	int i, j;

	if ( !tess.shader || !tess.shader->hzmWaterPreset ) {
		return qfalse;
	}
	for ( i = 0; i < 3; i++ ) {
		if ( fabsf( backEnd.or.origin[i] ) > 0.5f ) {
			return qfalse;
		}
		for ( j = 0; j < 3; j++ ) {
			if ( fabsf( backEnd.or.axis[i][j] - ( i == j ? 1.0f : 0.0f ) ) > 1e-3f ) {
				return qfalse;
			}
		}
	}
	return qtrue;
}


/*
================
R_HZMWaterLazyTag - shaders that never come from a BSP surface (a script_model's skin, e.g. coop_sea_deep) are tagged
at first draw: the same allowlist and preset rule R_HZMWetTagShader / R_HZMWetBuild apply to world surfaces
================
*/
void R_HZMWaterLazyTag( shader_t *sh )
{
	int idx;

	sh->hzmWaterLazy = 1;
	if ( !tr.world ) {
		return;
	}
	idx = HZM_WaterAllowIndex( sh->name );
	if ( idx >= 0 ) {
		sh->hzmWater = idx + 1;
		sh->hzmWaterPreset = HZM_WaterPresetFor( sh->name, tr.world->baseName );
		ri.Printf( PRINT_ALL, "^~^~^ HZMWATER lazy-tag %s preset %d\n", sh->name, sh->hzmWaterPreset );
	}
}

/*
================
RB_HZMWaterDbg - r_hzmWaterDbg 2: every distinct water-ish shader (and the entity drawing it) once per ~2 s, with the pass decision
================
*/
void RB_HZMWaterDbg( const char *why )
{
	static struct { char key[160]; int t; } seen[48];
	static int next;
	char key[160];
	int i, now = ri.Milliseconds();
	const char *n = tess.shader ? tess.shader->name : "?";

	if ( !r_hzmWaterDbg || !( r_hzmWaterDbg->integer & 2 ) ) {
		return;
	}
	if ( !( tess.shader->hzmWater || strstr( n, "sea" ) || strstr( n, "water" ) || strstr( n, "surf" ) || strstr( n, "wave" ) || strstr( n, "foam" )
	        || strstr( n, "wake" ) || strstr( n, "shore" ) || strstr( n, "ocean" ) || strstr( n, "splash" ) || strstr( n, "runup" ) || strstr( n, "blood" ) ) ) {
		return;
	}
	Com_sprintf( key, sizeof( key ), "%s ent=%d tag=%d pre=%d %s", n, (int)( backEnd.currentEntity == &tr.worldEntity ? -1 : backEnd.currentEntity - backEnd.refdef.entities ),
	             tess.shader->hzmWater, tess.shader->hzmWaterPreset, why );
	for ( i = 0; i < 48; i++ ) {
		if ( !strcmp( seen[i].key, key ) ) {
			if ( now - seen[i].t < 2000 ) {
				return;
			}
			break;
		}
	}
	if ( i == 48 ) {
		i = next++ % 48;
		Q_strncpyz( seen[i].key, key, sizeof( seen[i].key ) );
	}
	seen[i].t = now;
	ri.Printf( PRINT_ALL, "^~^~^ HZMWDBG %s org=%.0f,%.0f,%.0f n=%d\n", key, backEnd.or.origin[0], backEnd.or.origin[1], backEnd.or.origin[2], tess.numIndexes );
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
	qboolean		contactOn;

	if ( r_hzmWaterDbg && ( r_hzmWaterDbg->integer & 4 ) ) {
		return;     // instant retail bypass for before/after captures (no 1.5 s fade)
	}
	fade = HZM_WaterFade();
	if ( fade > 0.0f && backEnd.currentEntity != &tr.worldEntity && !HZM_WaterEntityAtOrigin() ) {
		RB_HZMWaterDbg( "entity-transform(now handled by u_ModelMatrix)" );
	}
	if ( fade <= 0.0f
	     || ( backEnd.currentEntity != &tr.worldEntity && !tess.shader->hzmWaterPreset )
	     || backEnd.depthFill
	     || backEnd.projection2D
	     || ( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) )
	     || backEnd.viewParms.isPortal
	     || backEnd.viewParms.isPortalSky
	     || ( backEnd.viewParms.flags & VPF_HZMPLANAR )
	     || ( tr.hzmOmahaWorld && !tess.shader->hzmWaterPreset )     // Omaha: only the preset water, never the plain layer
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
	// contact foam needs the scene-depth snapshot RB_HZMWaterDepthSnapshot took at the first preset water batch of this view
	contactOn = ( tess.shader->hzmWaterPreset && tess.shader->hzmWaterPreset != HZM_WPRESET_ICE && r_hzmWaterContact && r_hzmWaterContact->integer
	              && tr.hdrDepthImage && hzmWaterSnapValid ) ? qtrue : qfalse;
	GLSL_BindProgram( sp );

	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix );   // moving / rotated wave sheets: world position in the vp
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

	// water presets (hzm_waterwet.h): the body / swell / chop / floe parameters of this shader's preset; mode 0 = legacy
	{
		const hzmWaterPreset_t	*pr = &hzmWaterPresets[tess.shader->hzmWaterPreset > 0 && tess.shader->hzmWaterPreset < HZM_WPRESET_COUNT
		                                                 ? tess.shader->hzmWaterPreset : 0];
		vec4_t					c0, c1, c2, c4;
		qboolean				on = ( pr->cover > 0.0f && tr.hzmFloeImage && tr.hzmIceImage && r_hzmWaterPreset && r_hzmWaterPreset->integer ) ? qtrue : qfalse;

		HZM_V4( c0, on ? (float)tess.shader->hzmWaterPreset : 0.0f, pr->cover, pr->swell, pr->chop );
		float					bg = ( tess.shader->hzmWaterPreset != HZM_WPRESET_ICE && r_hzmWaterBody ) ? r_hzmWaterBody->value : 1.0f;
		float					fg = r_hzmWaterFloe ? r_hzmWaterFloe->value : 1.0f;
		HZM_V4( c1, pr->deep[0] * bg, pr->deep[1] * bg, pr->deep[2] * bg, pr->floe * fg * ( tr.hzmQuayImage ? 1.0f : 0.0f ) );
		HZM_V4( c2, pr->shallow[0] * bg, pr->shallow[1] * bg, pr->shallow[2] * bg, ( on && tr.hzmPlanarLive && tr.hzmPlanarImage ) ? pr->planar : 0.0f );
		HZM_V4( c4, tr.hzmQuayMax, pr->drift, pr->lmgain * ( r_hzmWaterIce ? r_hzmWaterIce->value : 1.0f ), 0.0f );
		if ( tess.shader->hzmWaterPreset == HZM_WPRESET_COLD ) {
			c4[2] = 1.0f / MAX( 1, backEnd.viewParms.viewportWidth );     // COLD: 1 / main viewport, for the planar lookup
			c4[3] = 1.0f / MAX( 1, backEnd.viewParms.viewportHeight );
		}
		GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD0, c0 );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD1, c1 );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD2, c2 );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD3, tr.hzmQuayXform );
		GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD4, c4 );
		{
			vec4_t c5;
			float rmin, rmax;
			int raxis = HZM_WaterRampFor( tess.shader->name, &rmin, &rmax );
			HZM_V4( c5, pr->whitecap, rmin, rmax, (float)raxis );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD5, c5 );
			{
				vec4_t c6, c7;
				HZM_V4( c6, pr->yaw, pr->shore, pr->flow, pr->contact );
				HZM_V4( c7, tr.hzmFlowDir[0], tr.hzmFlowDir[1], 0.0f, 0.0f );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD6, c6 );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCOLD7, c7 );
			}
			{
				const float	*pm = backEnd.viewParms.projectionMatrix;
				const vec3_t *ax = (const vec3_t *)backEnd.viewParms.or.axis;
				vec4_t		k0, k1, k2, k3, k4;
				float		vw = (float)MAX( 1, backEnd.viewParms.viewportWidth ), vh = (float)MAX( 1, backEnd.viewParms.viewportHeight );
				HZM_V4( k0, ax[0][0], ax[0][1], ax[0][2], 1.0f / pm[0] );
				HZM_V4( k1, ax[1][0] * -1.0f, ax[1][1] * -1.0f, ax[1][2] * -1.0f, 1.0f / pm[5] );     // axis[1] is LEFT in this engine
				HZM_V4( k2, ax[2][0], ax[2][1], ax[2][2], contactOn ? 1.0f : 0.0f );
				HZM_V4( k3, pm[10], pm[14], 1.0f / vw, 1.0f / vh );
				HZM_V4( k4, 0.5f * vh * pm[5], 34.0f, 0.0f, 0.0f );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCONT0, k0 );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCONT1, k1 );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCONT2, k2 );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCONT3, k3 );
				GLSL_SetUniformVec4( sp, UNIFORM_HZMCONT4, k4 );
				if ( contactOn ) {
					RB_HZM_BindSceneDepth();
				}
			}
		}
		GL_BindToTMU( tr.hzmQuayImage ? tr.hzmQuayImage : tr.whiteImage, TB_HZMQUAY );
		GL_BindToTMU( tr.hzmFloeImage ? tr.hzmFloeImage : tr.whiteImage, TB_HZMFLOE );
		GL_BindToTMU( tr.hzmIceImage ? tr.hzmIceImage : tr.whiteImage, TB_HZMICE );
		GL_BindToTMU( ( tr.hzmPlanarLive && tr.hzmPlanarImage ) ? tr.hzmPlanarImage : tr.whiteImage, TB_HZMPLANAR );
	}

	GL_BindToTMU( tr.hzmRainOccImage ? tr.hzmRainOccImage : tr.whiteImage, TB_HZMRAINOCC );
	GL_BindToTMU( tr.hzmRippleImage, TB_HZMWETNOISE );
	GL_BindToTMU( lm ? lm : tr.whiteImage, TB_LIGHTMAP );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	R_DrawElements( tess.numIndexes, tess.firstIndex );
}


/*
================
R_HZM_RenderWaterPlanar - from RE_RenderScene, BEFORE the main R_RenderView, for the world view only. Renders the scene
from the camera mirrored through the COLD waterline into tr.hzmPlanarFbo (oblique clip at the waterline through the
portalPlane path of R_SetupProjection, the water-pass shaders not drawn: R_AddDrawSurf), so the water pass can show the
pen, the quay lamps and the opening in the water. Only when the preset is on, the camera is above the water and the
water pass itself is faded in. mainParms are the main view's fully built parms.
================
*/
void R_HZM_RenderWaterPlanar( viewParms_t *mainParms, const refdef_t *fd )
{
	viewParms_t	pp;
	float		zw;
	int			i, dbg, ds0;

	tr.hzmPlanarLive = qfalse;
	if ( !tr.hzmColdWorld || !tr.hzmPlanarFbo || !tr.hzmPlanarImage || tr.hzmOmahaWorld
	     || !r_hzmWaterPlanar || !r_hzmWaterPlanar->integer || !r_hzmWaterPreset || !r_hzmWaterPreset->integer ) {
		return;
	}
	if ( ( fd->rdflags & RDF_NOWORLDMODEL ) || mainParms->isPortal || mainParms->isPortalSky || mainParms->targetFbo
	     || ( mainParms->flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) || HZM_WaterFade() <= 0.0f ) {
		return;
	}
	zw = tr.hzmColdZ;
	if ( fd->vieworg[2] < zw + 8.0f ) {
		return;
	}
	dbg = r_hzmWaterPlanarDbg ? r_hzmWaterPlanarDbg->integer : 0;
	// in-engine finding (t5, 2026-10-25): mirrored camera + oblique near-plane clip renders black (the Lengyel clip matrix
	// drops everything for this plane); without it the reflection is correct (the staged boat is mirrored). So the oblique
	// clip is OPT-IN (bit 1); the water-pass shaders are still skipped in the view (R_AddDrawSurf).
	hzmPlanarNoOblique = ( dbg & 1 ) ? 0 : 1;
	pp = *mainParms;
	pp.viewportX = 0;
	pp.viewportY = 0;
	pp.viewportWidth = tr.hzmPlanarFbo->width;
	pp.viewportHeight = tr.hzmPlanarFbo->height;
	pp.isPortal = qfalse;
	pp.isMirror = ( dbg & 4 ) ? qfalse : qtrue;
	pp.flags = VPF_NOVIEWMODEL | VPF_NOCUBEMAPS | VPF_HZMPLANAR;     // no sun shadow mask: the reflection is a cheap view
	VectorCopy( fd->vieworg, pp.pvsOrigin );                          // visibility from where the player really is
	VectorCopy( fd->vieworg, pp.or.origin );
	pp.or.origin[2] = 2.0f * zw - fd->vieworg[2];
	for ( i = 0; i < 3; i++ ) {
		VectorCopy( fd->viewaxis[i], pp.or.axis[i] );
		if ( !( dbg & 8 ) ) {
			pp.or.axis[i][2] = -pp.or.axis[i][2];
		}
	}
	Com_Memset( &pp.portalPlane, 0, sizeof( pp.portalPlane ) );
	// Q3 portal-plane convention (R_MirrorViewBySurface): the normal points toward the mirrored camera and what lies on
	// that side is clipped. The mirrored camera is BELOW the waterline, so the normal points down and everything below
	// the water is cut; what is above it (the reflected world) is kept.
	pp.portalPlane.normal[2] = -1.0f;
	pp.portalPlane.dist = -zw;
	pp.targetFbo = tr.hzmPlanarFbo;
	pp.targetFboLayer = 0;
	pp.targetFboCubemapIndex = 0;
	ds0 = tr.refdef.numDrawSurfs;
	R_RenderView( &pp );
	tr.hzmPlanarLive = qtrue;
	{
		static int s_dbgN;
		if ( s_dbgN < 6 && ( dbg || s_dbgN < 2 ) ) {
			s_dbgN++;
			ri.Printf( PRINT_ALL, "^~^~^ HZMPLANAR dbg %d drawsurfs +%d origin %.0f %.0f %.0f zFar %.0f proj %.3f %.3f %.3f %.3f\n", dbg,
			           tr.refdef.numDrawSurfs - ds0, pp.or.origin[0], pp.or.origin[1], pp.or.origin[2], tr.viewParms.zFar,
			           tr.viewParms.projectionMatrix[0], tr.viewParms.projectionMatrix[5], tr.viewParms.projectionMatrix[10], tr.viewParms.projectionMatrix[14] );
		}
	}
}


/*
================
RB_HZMWaterDepthSnapshot - called from RB_RenderDrawSurfList between batches, at the first preset water surface of a view
================
*/
void RB_HZMWaterDepthSnapshot( void )
{
	mat4_t	projection;

	if ( !r_hzmWaterContact || !r_hzmWaterContact->integer || !tr.hdrDepthFbo || !tr.hdrDepthImage || backEnd.projection2D
	     || backEnd.viewParms.targetFbo || ( backEnd.viewParms.flags & ( VPF_DEPTHSHADOW | VPF_SHADOWMAP | VPF_PSHADOW ) )
	     || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		return;
	}
	Mat4Copy( glState.projection, projection );
	hzmWaterSnapValid = RB_HZM_SceneDepthSnapshot();
	GL_SetProjectionMatrix( projection );
}
