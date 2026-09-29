/*
===========================================================================
HZM coop - RAIN WETNESS for world surfaces (r_hzmWet). docs/proposals/water_wetness_2026-09-27 (plan.md W2/W4).

WHAT. While the dynamic weather rains, outdoor BSP world surfaces and terrain turn darker and glossier, flat porous
ground grows puddles with rain rings, and walls under eaves stay dry. The look is lightall_fp.glsl's HzmWet block;
the offline twin that fixed every constant is tools/ww_look.py hzm_wet (lookdev/RUBRIC.md: no added sparkle, no
seams, no flicker, <= 0.17 LSB/frame when rain starts or stops).

INPUTS.
  r_hzmWet        flags 0, "-1" = HZM_WET_AUTO (1, ON, since v1.10.3). The master.
  r_hzmWetNow     flags 0, 0..1, the film level; published every frame by cgame (cg_view.c CG_HZMWorldWetness)
  r_hzmPuddleNow  flags 0, 0..1, the puddle level; same publisher. cgame eases both (hzm_waterwet.h).
  Both are 0 on the five Omaha BSPs (cgame) and the renderer ALSO refuses them there (tr.hzmOmahaWorld).

BUILT ONCE PER MAP LOAD (R_HZMWetBuild, from RE_LoadWorldMap while the BSP file is still in memory), on every
non-Omaha map whatever the switch says, so turning the option on mid-map works at once - never a lazy mid-game build
(that would be a hitch), never a menu row that silently does nothing (vet.md B5):
  * the RAIN-OCCLUSION map: top-down max height of every drawn world-brush face and terrain patch that can shelter
    (not sky, not nodraw, not textures/common, not water, not 'trans', not alpha-tested - rain passes fences and
    foliage; |n.z| >= 0.1 - walls roof nothing; no brush models - doors and movers move), 8 u cells (grown only to
    stay <= 2048^2), no dilation, GL_R16 normalised to the occluders' z range, bilinear. tools/ww_inventory.py
    occlusion_grid is the same raster.
  * the SKY colours the wet film reflects: zenith = the 1x1 mip of the sky box's up face, horizon = the mean of the
    four side faces' 1x1 mips. The shader then fogs them exactly as gl2 fogs the visible sky (r_globalFogSky), so on
    the 25 fogged maps the reflection is the fog colour, as the player's own sky is.
  * a 256^2 tileable band-limited puddle mask (integer-frequency cosines, so periodic by construction) and the mask
    value at its 25% quantile (the puddle coverage at level 1).
  * for the WATER pass (tr_hzm_water.c), only on a map with allowlisted water: the soft occlusion copy, then
    R_HZMWaterBuild. R_HZMWetTagShader also tags those shaders (shader->hzmWater).

APPLIED ONLY WHERE IT IS SAFE (RB_HZMWetUniforms, vet.md B2): the main lit pass of the WORLD entity, the FIRST stage
of the shader, opaque (no blend bits, sort <= SS_OPAQUE), not water/slime, a lit lightall permutation, a surface the
BSP tagged (R_HZMWetTagShader) whose material class is not 'never' (carpet, paper, foliage, snow). Everything else -
dynamic-light passes (RB_HZMWetOff), depth prepass, shadow / portal / cube views, entities, characters - gets zero.
With the master off every draw uploads zero and the lightall branch is never taken: byte-identical output.

MATERIAL CLASS from the BSP shader lump's surface-type bits (qcommon/surfaceflags.h), OR-ed per shader because gl2's
infoParms[] never parses them into shader->surfaceFlags (vet.md B3):
  never  (carpet paper foliage snow)            untouched
  sealed (metal glass grill)                    darken 0.94, rough 0.50, no sheen and no glint (thin rails crawled)
  wood                                          0.72 / 0.42
  porous (rock dirt sand gravel mud grass puddle) 0.60 / 0.60, puddles
  generic (unflagged plaster, brick, concrete)  0.70 / 0.44, puddles
===========================================================================
*/

#include "tr_local.h"
#include "tr_dsa.h"
#include "../renderercommon/hzm_waterwet.h"

// tr_image.c; not in tr_local.h (only tr_image.c called it before). Storage-only when pic is NULL.
image_t *R_CreateImage2( const char *name, byte *pic, int width, int height, GLenum picFormat, int numMips, imgType_t type,
                         imgFlags_t flags, int internalFormat );

#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif

#define HZM_WET_TAGGED     0x00000001   // the BSP drew this shader on a world surface (never a real type bit)
#define HZM_WET_TYPEMASK   ( SURF_PAPER | SURF_WOOD | SURF_METAL | SURF_ROCK | SURF_DIRT | SURF_GRILL | SURF_GRASS \
                           | SURF_MUD | SURF_PUDDLE | SURF_GLASS | SURF_GRAVEL | SURF_SAND | SURF_FOLIAGE | SURF_SNOW \
                           | SURF_CARPET )
#define HZM_OCC_CELL       8.0f
#define HZM_OCC_MAXTEXELS  ( 2048.0f * 2048.0f )
#define HZM_NOISE_SIZE     256
#define HZM_PUDDLE_COVER   0.25f

static cvar_t *r_hzmWet;
static cvar_t *r_hzmWetNow;
static cvar_t *r_hzmPuddleNow;

// per-class material: darken at full wetness, wet roughness, puddles allowed, sealed (no sheen/glint)
static const float s_hzmClassMat[4][4] = {
	{ 0.70f, 0.44f, 1.0f, 0.0f },   // 0 generic
	{ 0.60f, 0.60f, 1.0f, 0.0f },   // 1 porous
	{ 0.72f, 0.42f, 0.0f, 0.0f },   // 2 wood
	{ 0.94f, 0.50f, 0.0f, 1.0f },   // 3 sealed
};

static void R_HZMWetCvars( void )
{
	// registered here, once, by the only module that reads them (a second registration OR-merges flags)
	r_hzmWet       = ri.Cvar_Get( "r_hzmWet",       "-1", 0 );
	r_hzmWetNow    = ri.Cvar_Get( "r_hzmWetNow",    "0",  0 );
	r_hzmPuddleNow = ri.Cvar_Get( "r_hzmPuddleNow", "0",  0 );
}

static int HZM_WetClass( int bits )
{
	if ( !( bits & HZM_WET_TAGGED ) ) {
		return -1;
	}
	if ( bits & ( SURF_CARPET | SURF_PAPER | SURF_FOLIAGE | SURF_SNOW ) ) {
		return -1;
	}
	if ( bits & ( SURF_METAL | SURF_GLASS | SURF_GRILL ) ) {
		return 3;
	}
	if ( bits & SURF_WOOD ) {
		return 2;
	}
	if ( bits & ( SURF_ROCK | SURF_DIRT | SURF_SAND | SURF_GRAVEL | SURF_MUD | SURF_GRASS | SURF_PUDDLE ) ) {
		return 1;
	}
	return 0;
}

/*
================
R_HZMWetTagShader - from ShaderForShaderNum (tr_bsp.c), for every BSP world surface and terrain patch.
================
*/
void R_HZMWetTagShader( shader_t *sh, int surfaceFlags )
{
	if ( !sh || sh == tr.defaultShader ) {
		return;
	}
	sh->hzmWetBits |= HZM_WET_TAGGED | ( surfaceFlags & HZM_WET_TYPEMASK );
	// the water pass (tr_hzm_water.c): allowlist slot + 1, by exact shader name only
	sh->hzmWater = HZM_WaterAllowIndex( sh->name ) + 1;
}

/*
================
top-down max-height raster (tools/ww_inventory.py raster_max, C twin)
================
*/
typedef struct {
	float	*H;
	int		nx, ny;
	float	x0, y0, cell;
} hzmOccGrid_t;

static void HZM_OccTri( hzmOccGrid_t *g, const float *a, const float *b, const float *c )
{
	float	e1[3], e2[3], n[3], len, den;
	float	mnx, mxx, mny, mxy;
	int		i, j, i0, i1, j0, j1;

	VectorSubtract( b, a, e1 );
	VectorSubtract( c, a, e2 );
	CrossProduct( e1, e2, n );
	len = VectorLength( n );
	if ( len < 1e-6f || fabs( n[2] ) < 0.1f * len ) {
		return;     // degenerate, or a wall: walls roof nothing
	}
	den = ( b[1] - c[1] ) * ( a[0] - c[0] ) + ( c[0] - b[0] ) * ( a[1] - c[1] );
	if ( fabs( den ) < 1e-6f ) {
		return;
	}
	mnx = MIN( a[0], MIN( b[0], c[0] ) ); mxx = MAX( a[0], MAX( b[0], c[0] ) );
	mny = MIN( a[1], MIN( b[1], c[1] ) ); mxy = MAX( a[1], MAX( b[1], c[1] ) );
	i0 = MAX( (int)floor( ( mnx - g->x0 ) / g->cell ), 0 ); i1 = MIN( (int)floor( ( mxx - g->x0 ) / g->cell ), g->nx - 1 );
	j0 = MAX( (int)floor( ( mny - g->y0 ) / g->cell ), 0 ); j1 = MIN( (int)floor( ( mxy - g->y0 ) / g->cell ), g->ny - 1 );
	for ( j = j0; j <= j1; j++ ) {
		float py = g->y0 + ( j + 0.5f ) * g->cell;
		for ( i = i0; i <= i1; i++ ) {
			float px = g->x0 + ( i + 0.5f ) * g->cell;
			float w0 = ( ( b[1] - c[1] ) * ( px - c[0] ) + ( c[0] - b[0] ) * ( py - c[1] ) ) / den;
			float w1 = ( ( c[1] - a[1] ) * ( px - c[0] ) + ( a[0] - c[0] ) * ( py - c[1] ) ) / den;
			float w2 = 1.0f - w0 - w1;
			float z;
			if ( w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f ) {
				continue;
			}
			z = w0 * a[2] + w1 * b[2] + w2 * c[2];
			if ( z > g->H[j * g->nx + i] ) {
				g->H[j * g->nx + i] = z;
			}
		}
	}
}

// the occluder test for a BSP surface (see the file comment); i is the surface index = tr.world->surfaces[i]
static qboolean HZM_OccSurfaceShelters( const dsurface_t *ds, int i, const dshader_t *dshaders, int numShaders )
{
	int					sn = LittleLong( ds->shaderNum );
	const dshader_t		*dsh;
	const shader_t		*sh;

	if ( sn < 0 || sn >= numShaders ) {
		return qfalse;
	}
	dsh = &dshaders[sn];
	if ( LittleLong( dsh->surfaceFlags ) & ( SURF_SKY | SURF_NODRAW ) ) {
		return qfalse;
	}
	if ( LittleLong( dsh->contentFlags ) & ( CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_TRANSLUCENT ) ) {
		return qfalse;
	}
	if ( !Q_stricmpn( dsh->shader, "textures/common/", 16 ) ) {
		return qfalse;
	}
	sh = ( i >= 0 && i < tr.world->numsurfaces ) ? tr.world->surfaces[i].shader : NULL;
	if ( !sh || sh->defaultShader || sh->hzmWater || !sh->stages[0] || ( sh->stages[0]->stateBits & GLS_ATEST_BITS ) ) {
		return qfalse;
	}
	return qtrue;
}

// iterate the sheltering triangles of the world model + terrain; pass 0 = bounds, pass 1 = raster
static void HZM_OccWalk( dheader_t *header, hzmOccGrid_t *g, vec3_t mins, vec3_t maxs )
{
	lump_t			*lsh = Q_GetLumpByVersion( header, LUMP_SHADERS );
	lump_t			*lsu = Q_GetLumpByVersion( header, LUMP_SURFACES );
	lump_t			*ldv = Q_GetLumpByVersion( header, LUMP_DRAWVERTS );
	lump_t			*lix = Q_GetLumpByVersion( header, LUMP_DRAWINDEXES );
	lump_t			*lmo = Q_GetLumpByVersion( header, LUMP_MODELS );
	byte			*base = (byte *)header;
	const dshader_t	*dshaders = (const dshader_t *)( base + lsh->fileofs );
	const dsurface_t *dsurf = (const dsurface_t *)( base + lsu->fileofs );
	const drawVert_t *dv = (const drawVert_t *)( base + ldv->fileofs );
	const int		*ix = (const int *)( base + lix->fileofs );
	int				numShaders = lsh->filelen / sizeof( dshader_t );
	int				numSurf = lsu->filelen / sizeof( dsurface_t );
	int				numVerts = ldv->filelen / sizeof( drawVert_t );
	int				numIdx = lix->filelen / sizeof( int );
	int				first = 0, count = numSurf, s, k, i, j;

	if ( lmo->filelen >= (int)sizeof( dmodel_t ) ) {
		const dmodel_t *m0 = (const dmodel_t *)( base + lmo->fileofs );
		first = LittleLong( m0->firstSurface );
		count = LittleLong( m0->numSurfaces );
	}

	for ( s = first; s < first + count && s < numSurf; s++ ) {
		const dsurface_t *ds = &dsurf[s];
		int type = LittleLong( ds->surfaceType );
		int fv = LittleLong( ds->firstVert ), nv = LittleLong( ds->numVerts );
		float p[3][3];

		if ( fv < 0 || fv + nv > numVerts || !HZM_OccSurfaceShelters( ds, s, dshaders, numShaders ) ) {
			continue;
		}
		if ( type == MST_PLANAR || type == MST_TRIANGLE_SOUP ) {
			int fi = LittleLong( ds->firstIndex ), ni = LittleLong( ds->numIndexes );
			if ( fi < 0 || fi + ni > numIdx ) {
				continue;
			}
			for ( k = 0; k + 2 < ni; k += 3 ) {
				for ( i = 0; i < 3; i++ ) {
					int v = fv + LittleLong( ix[fi + k + i] );
					if ( v < 0 || v >= numVerts ) {
						break;
					}
					for ( j = 0; j < 3; j++ ) {
						p[i][j] = LittleFloat( dv[v].xyz[j] );
					}
				}
				if ( i < 3 ) {
					continue;
				}
				if ( g ) {
					HZM_OccTri( g, p[0], p[1], p[2] );
				} else {
					AddPointToBounds( p[0], mins, maxs ); AddPointToBounds( p[1], mins, maxs ); AddPointToBounds( p[2], mins, maxs );
				}
			}
		} else if ( type == MST_PATCH ) {
			// the control grid as quads - coarse, but MOHAA patches that roof anything are near-planar
			int pw = LittleLong( ds->patchWidth ), ph = LittleLong( ds->patchHeight ), x, y;
			if ( pw < 2 || ph < 2 || pw * ph > nv ) {
				continue;
			}
			for ( y = 0; y + 1 < ph; y++ ) {
				for ( x = 0; x + 1 < pw; x++ ) {
					const float *a = dv[fv + y * pw + x].xyz, *b = dv[fv + y * pw + x + 1].xyz;
					const float *c = dv[fv + ( y + 1 ) * pw + x].xyz, *d = dv[fv + ( y + 1 ) * pw + x + 1].xyz;
					if ( g ) {
						HZM_OccTri( g, a, c, b );
						HZM_OccTri( g, b, c, d );
					} else {
						AddPointToBounds( a, mins, maxs ); AddPointToBounds( d, mins, maxs );
						AddPointToBounds( b, mins, maxs ); AddPointToBounds( c, mins, maxs );
					}
				}
			}
		}
	}

	// terrain: 9x9 heights per 512 u patch, z = z0 + 2 * height (tr_terrain.c R_PreTessellateTerrain)
	for ( s = 0; s < tr.world->numTerraPatches; s++ ) {
		const cTerraPatchUnpacked_t *tp = &tr.world->terraPatches[s];
		for ( j = 0; j < 8; j++ ) {
			for ( i = 0; i < 8; i++ ) {
				float q[4][3];
				int c;
				for ( c = 0; c < 4; c++ ) {
					int ii = i + ( c & 1 ), jj = j + ( c >> 1 );
					q[c][0] = tp->x0 + 64.0f * ii;
					q[c][1] = tp->y0 + 64.0f * jj;
					q[c][2] = tp->z0 + 2.0f * tp->heightmap[jj * 9 + ii];
				}
				if ( g ) {
					HZM_OccTri( g, q[0], q[1], q[3] );
					HZM_OccTri( g, q[0], q[3], q[2] );
				} else {
					for ( c = 0; c < 4; c++ ) {
						AddPointToBounds( q[c], mins, maxs );
					}
				}
			}
		}
	}
}

/*
================
the water pass's SOFT copy (tools/ww_look.py build_occlusion occ_soft): empty cells floored at the lowest real height
- 512 u, then a Gaussian of sigma 48 u, so a ship's flared hull or a quay lip over the water fades the reflection over
~100 u instead of drawing a line, while the U-boat pens stay fully roofed. Built on a 4x coarser grid (a 4x4 mean,
then the Gaussian in coarse cells) with the same origin as the fine map.
================
*/
static void R_HZMWaterBuildSoftOcclusion( const hzmOccGrid_t *g )
{
	enum { F = 4 };
	float			lo = 1e9f, hi = -1e9f, floorZ, cell = g->cell * F, sigma = 48.0f / ( g->cell * F ), wsum = 0.0f, zr;
	float			*S, *T, w[32];
	unsigned short	*pix;
	int				sx = ( ( g->nx + F - 1 ) / F + 1 ) & ~1, sy = ( g->ny + F - 1 ) / F;     // sx even: 4-byte rows
	int				i, j, x, y, r, n = sx * sy;
	image_t			*img;

	for ( i = 0; i < g->nx * g->ny; i++ ) {
		if ( g->H[i] > -1e8f ) {
			lo = MIN( lo, g->H[i] );
			hi = MAX( hi, g->H[i] );
		}
	}
	if ( lo > hi ) {
		return;
	}
	floorZ = lo - 512.0f;

	// 4x4 mean (cells past the fine grid's edge read as floor)
	S = ri.Malloc( n * sizeof( float ) );
	T = ri.Malloc( n * sizeof( float ) );
	for ( y = 0; y < sy; y++ ) {
		for ( x = 0; x < sx; x++ ) {
			float acc = 0.0f;
			for ( j = 0; j < F; j++ ) {
				for ( i = 0; i < F; i++ ) {
					int		fx = x * F + i, fy = y * F + j;
					float	h = ( fx < g->nx && fy < g->ny ) ? g->H[fy * g->nx + fx] : floorZ;
					acc += ( h > -1e8f ) ? h : floorZ;
				}
			}
			S[y * sx + x] = acc / ( F * F );
		}
	}

	// separable Gaussian, clamp-to-edge
	r = MIN( 15, (int)ceil( 3.0f * sigma ) );
	for ( i = -r; i <= r; i++ ) {
		w[i + r] = expf( -0.5f * ( i * i ) / MAX( sigma * sigma, 1e-4f ) );
		wsum += w[i + r];
	}
	for ( i = 0; i <= 2 * r; i++ ) {
		w[i] /= wsum;
	}
	for ( y = 0; y < sy; y++ ) {
		for ( x = 0; x < sx; x++ ) {
			float acc = 0.0f;
			for ( i = -r; i <= r; i++ ) {
				acc += w[i + r] * S[y * sx + MAX( 0, MIN( sx - 1, x + i ) )];
			}
			T[y * sx + x] = acc;
		}
	}
	for ( y = 0; y < sy; y++ ) {
		for ( x = 0; x < sx; x++ ) {
			float acc = 0.0f;
			for ( i = -r; i <= r; i++ ) {
				acc += w[i + r] * T[MAX( 0, MIN( sy - 1, y + i ) ) * sx + x];
			}
			S[y * sx + x] = acc;
		}
	}
	ri.Free( T );

	zr = MAX( hi + 64.0f - floorZ, 1.0f );
	pix = ri.Malloc( n * sizeof( unsigned short ) );
	for ( i = 0; i < n; i++ ) {
		pix[i] = (unsigned short)( Com_Clamp( 0.0f, 1.0f, ( S[i] - floorZ ) / zr ) * 65535.0f + 0.5f );
	}
	ri.Free( S );

	img = R_CreateImage2( "*hzmRainOccSoft", NULL, sx, sy, GL_RGBA16, 0, IMGTYPE_COLORALPHA,
	                      IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_R16 );
	qglTextureSubImage2DEXT( img->texnum, GL_TEXTURE_2D, 0, 0, 0, sx, sy, GL_RED, GL_UNSIGNED_SHORT, pix );
	ri.Free( pix );
	GL_CheckErrors();

	tr.hzmRainOccSoftImage = img;
	VectorSet4( tr.hzmRainOccSoftXform, g->x0, g->y0, 1.0f / ( sx * cell ), 1.0f / ( sy * cell ) );
	tr.hzmRainOccSoftZ[0] = floorZ;
	tr.hzmRainOccSoftZ[1] = zr;
}

static void R_HZMWetBuildOcclusion( dheader_t *header )
{
	vec3_t			mins, maxs;
	hzmOccGrid_t	g;
	float			w, h, zmin, zmax, zr;
	unsigned short	*pix;
	int				i, n;
	image_t			*img;

	ClearBounds( mins, maxs );
	HZM_OccWalk( header, NULL, mins, maxs );
	if ( mins[0] > maxs[0] ) {
		return;     // nothing shelters anything: no map (every pixel reads as open sky - the shader's default)
	}

	g.cell = HZM_OCC_CELL;
	g.x0 = mins[0] - 2.0f * g.cell;
	g.y0 = mins[1] - 2.0f * g.cell;
	w = maxs[0] - g.x0 + 2.0f * g.cell;
	h = maxs[1] - g.y0 + 2.0f * g.cell;
	if ( w * h / ( g.cell * g.cell ) > HZM_OCC_MAXTEXELS ) {
		g.cell = sqrtf( w * h / HZM_OCC_MAXTEXELS );
	}
	g.nx = ( (int)( w / g.cell ) + 2 ) & ~1;     // even: 16-bit rows stay 4-byte aligned for the upload
	g.ny = (int)( h / g.cell ) + 1;
	n = g.nx * g.ny;

	g.H = ri.Malloc( n * sizeof( float ) );
	for ( i = 0; i < n; i++ ) {
		g.H[i] = -1e9f;
	}
	HZM_OccWalk( header, &g, NULL, NULL );
	if ( tr.hzmWaterWorld ) {
		R_HZMWaterBuildSoftOcclusion( &g );
	}

	zmin = mins[2] - 64.0f;
	zmax = maxs[2] + 64.0f;
	zr = zmax - zmin;
	pix = ri.Malloc( n * sizeof( unsigned short ) );
	for ( i = 0; i < n; i++ ) {
		float v = ( g.H[i] < zmin ) ? 0.0f : ( g.H[i] - zmin ) / zr;
		pix[i] = (unsigned short)( Com_Clamp( 0.0f, 1.0f, v ) * 65535.0f + 0.5f );
	}
	ri.Free( g.H );

	// storage only (pic NULL), then a 16-bit single-channel upload: R_CreateImage2's upload path is RGBA8/RGBA16
	img = R_CreateImage2( "*hzmRainOcc", NULL, g.nx, g.ny, GL_RGBA16, 0, IMGTYPE_COLORALPHA,
	                      IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_R16 );
	qglTextureSubImage2DEXT( img->texnum, GL_TEXTURE_2D, 0, 0, 0, g.nx, g.ny, GL_RED, GL_UNSIGNED_SHORT, pix );
	ri.Free( pix );
	GL_CheckErrors();

	tr.hzmRainOccImage = img;
	VectorSet4( tr.hzmRainOccXform, g.x0, g.y0, 1.0f / ( g.nx * g.cell ), 1.0f / ( g.ny * g.cell ) );
	tr.hzmRainOccZ[0] = zmin;
	tr.hzmRainOccZ[1] = zr;

	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMWET occlusion %dx%d cell %.1f z %.0f..%.0f\n", g.nx, g.ny, g.cell, zmin, zmax );
}

static void HZM_ReadSmallestMip( image_t *img, vec3_t out )
{
	byte	px[64];
	int		w, h, level = 0;

	VectorSet( out, -1.0f, -1.0f, -1.0f );
	if ( !img || img == tr.defaultImage || !( img->flags & IMGFLAG_MIPMAP ) || !qglGetTexImage || !img->texnum ) {
		return;
	}
	w = img->uploadWidth;
	h = img->uploadHeight;
	while ( w > 1 || h > 1 ) {
		w = MAX( 1, w >> 1 );
		h = MAX( 1, h >> 1 );
		level++;
	}
	// the tr_gore.c R_GoreReadTexImage pattern: raw bind on unit 0, read, then re-sync the DSA bind cache
	qglActiveTexture( GL_TEXTURE0 );
	qglBindTexture( GL_TEXTURE_2D, img->texnum );
	qglGetTexImage( GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, px );
	GL_BindNullTextures();
	VectorSet( out, px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f );
}

static void R_HZMWetBuildSky( void )
{
	int			i, k, nh = 0;
	vec3_t		c;
	shader_t	*sky = NULL;

	// no box (caulksky / env/idontexist) = a dark neutral; the shader fogs it like the visible sky either way
	VectorSet4( tr.hzmSkyZenith, 0.25f, 0.26f, 0.28f, 0.0f );
	VectorSet4( tr.hzmSkyHorizon, 0.25f, 0.26f, 0.28f, 0.0f );

	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		shader_t *sh = tr.world->surfaces[i].shader;
		if ( sh && sh->isSky && sh->sky.outerbox[0] && sh->sky.outerbox[0] != tr.defaultImage ) {
			sky = sh;
			break;
		}
	}
	if ( !sky ) {
		return;
	}
	// ParseSkyParms suffix order rt bk lf ft up dn -> outerbox[0..5]
	HZM_ReadSmallestMip( sky->sky.outerbox[4], c );
	if ( c[0] >= 0.0f ) {
		VectorCopy( c, tr.hzmSkyZenith );
	}
	VectorClear( tr.hzmSkyHorizon );
	for ( k = 0; k < 4; k++ ) {
		HZM_ReadSmallestMip( sky->sky.outerbox[k], c );
		if ( c[0] >= 0.0f ) {
			VectorAdd( tr.hzmSkyHorizon, c, tr.hzmSkyHorizon );
			nh++;
		}
	}
	if ( nh ) {
		VectorScale( tr.hzmSkyHorizon, 1.0f / nh, tr.hzmSkyHorizon );
	} else {
		VectorCopy( tr.hzmSkyZenith, tr.hzmSkyHorizon );
	}
}

static void R_HZMWetBuildNoise( void )
{
	// band-limited, tileable: a sum of cosines with INTEGER wave vectors (3..12 cycles per tile), amplitude k^-0.9,
	// deterministic phases - the C twin of tools/ww_look.py make_puddle_noise (its FFT form, low bands cut)
	enum { N = HZM_NOISE_SIZE, WAVES = 48 };
	static const float twoPi = 6.28318530718f;
	float		kx[WAVES], ky[WAVES], amp[WAVES], ph[WAVES];
	float		*f, lo = 1e9f, hi = -1e9f;
	byte		*pic;
	unsigned int seed = 0x31u, hist[256];
	int			i, x, y, acc;

	for ( i = 0; i < WAVES; ) {
		int a, b;
		float k;
		seed = seed * 1664525u + 1013904223u; a = (int)( ( seed >> 16 ) % 25u ) - 12;
		seed = seed * 1664525u + 1013904223u; b = (int)( ( seed >> 16 ) % 25u ) - 12;
		k = sqrtf( (float)( a * a + b * b ) );
		if ( k < 3.0f || k > 12.0f ) {
			continue;
		}
		seed = seed * 1664525u + 1013904223u;
		kx[i] = (float)a; ky[i] = (float)b; amp[i] = powf( k, -0.9f );
		ph[i] = ( ( seed >> 8 ) & 0xffff ) / 65536.0f * twoPi;
		i++;
	}
	f = ri.Malloc( N * N * sizeof( float ) );
	for ( y = 0; y < N; y++ ) {
		for ( x = 0; x < N; x++ ) {
			float v = 0.0f;
			for ( i = 0; i < WAVES; i++ ) {
				v += amp[i] * cosf( twoPi * ( kx[i] * x + ky[i] * y ) / N + ph[i] );
			}
			f[y * N + x] = v;
			lo = MIN( lo, v );
			hi = MAX( hi, v );
		}
	}
	pic = ri.Malloc( N * N * 4 );
	Com_Memset( hist, 0, sizeof( hist ) );
	for ( i = 0; i < N * N; i++ ) {
		byte v = (byte)( ( f[i] - lo ) / MAX( hi - lo, 1e-6f ) * 255.0f + 0.5f );
		pic[i * 4 + 0] = pic[i * 4 + 1] = pic[i * 4 + 2] = v;
		pic[i * 4 + 3] = 255;
		hist[v]++;
	}
	ri.Free( f );
	for ( i = 0, acc = 0; i < 256; i++ ) {
		acc += hist[i];
		if ( acc >= (int)( HZM_PUDDLE_COVER * N * N ) ) {
			break;
		}
	}
	tr.hzmPuddleQ = ( i + 0.5f ) / 255.0f;
	tr.hzmWetNoiseImage = R_CreateImage( "*hzmWetNoise", pic, N, N, IMGTYPE_COLORALPHA,
	                                     IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	ri.Free( pic );
}

/*
================
R_HZMWetBuild - RE_LoadWorldMap, after every lump is loaded and BEFORE the BSP file is freed.
================
*/
void R_HZMWetBuild( dheader_t *header )
{
	extern suninfo_t s_sun;
	float m;
	int i;

	R_HZMWetCvars();

	tr.hzmWetReady = qfalse;
	tr.hzmRainOccImage = NULL;
	tr.hzmWetNoiseImage = NULL;
	tr.hzmRainOccSoftImage = NULL;
	tr.hzmWaterWorld = 0;
	tr.hzmOmahaWorld = HZM_WaterWetOmahaMap( tr.world->baseName );
	if ( tr.hzmOmahaWorld ) {
		R_HZMWaterBuild();     // registers r_hzmWater and leaves the water pass off
		ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMWET off: %s is on the Omaha list\n", tr.world->baseName );
		return;
	}
	for ( i = 0; i < tr.world->numsurfaces; i++ ) {
		if ( tr.world->surfaces[i].shader && tr.world->surfaces[i].shader->hzmWater ) {
			tr.hzmWaterWorld++;
		}
	}

	R_HZMWetBuildOcclusion( header );
	R_HZMWetBuildSky();
	R_HZMWetBuildNoise();

	VectorCopy( tr.sunDirection, tr.hzmWetSun );
	tr.hzmWetSun[3] = s_sun.exists ? 1.0f : 0.0f;
	m = MAX( tr.sunLight[0], MAX( tr.sunLight[1], tr.sunLight[2] ) );
	if ( m > 1e-4f ) {
		VectorScale( tr.sunLight, MIN( m, 1.0f ) / m, tr.hzmWetSunCol );
	} else {
		VectorClear( tr.hzmWetSunCol );
	}
	tr.hzmWetSunCol[3] = 0.0f;

	tr.hzmWetReady = ( tr.hzmRainOccImage && tr.hzmWetNoiseImage ) ? qtrue : qfalse;

	R_HZMWaterBuild();     // tr_hzm_water.c: after the sky, sun and soft occlusion above
}

/*
================
the live master fade: a switch flip eases over 1.5 s, it never pops (plan W3/W4)
================
*/
static float HZM_WetFade( void )
{
	static float	s_fade, s_last;
	float			t = backEnd.refdef.floatTime, target, dt;
	int				master;

	if ( !r_hzmWet ) {
		return 0.0f;
	}
	master = r_hzmWet->integer < 0 ? HZM_WET_AUTO : r_hzmWet->integer;
	target = ( master == 1 && tr.hzmWetReady && !tr.hzmOmahaWorld ) ? 1.0f : 0.0f;
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
	return tr.hzmWetReady ? s_fade : 0.0f;
}

void RB_HZMWetOff( shaderProgram_t *sp )
{
	static const vec4_t off = { 0.0f, 0.0f, 0.0f, 0.0f };
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWET, off );
}

/*
================
RB_HZMWetUniforms - every lightall draw in RB_IterateStagesGeneric (never the depth-fill branch).
================
*/
void RB_HZMWetUniforms( shaderProgram_t *sp, const shaderCommands_t *input, const shaderStage_t *pStage, int stage,
                        qboolean charLit )
{
	vec4_t	wet = { 0.0f, 0.0f, 0.0f, 0.0f };
	float	fade = HZM_WetFade();
	int		cls;

	if ( fade > 0.0f
	     && stage == 0
	     && !charLit
	     && backEnd.currentEntity == &tr.worldEntity
	     && !backEnd.depthFill
	     && !backEnd.projection2D
	     && !( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) )
	     && !backEnd.viewParms.isPortal
	     && !backEnd.viewParms.isPortalSky
	     && !( tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo )
	     && !( pStage->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
	     && input->shader->sort <= SS_OPAQUE
	     && !( input->shader->contentFlags & ( CONTENTS_WATER | CONTENTS_SLIME ) )
	     && ( pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK )
	     && ( pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK ) != LIGHTDEF_USE_LIGHT_VECTOR
	     && ( cls = HZM_WetClass( input->shader->hzmWetBits ) ) >= 0 )
	{
		wet[0] = Com_Clamp( 0.0f, 1.0f, r_hzmWetNow->value ) * fade;
		wet[1] = Com_Clamp( 0.0f, 1.0f, r_hzmPuddleNow->value ) * fade;
		wet[2] = (float)fmod( backEnd.refdef.floatTime, 3600.0 );
		wet[3] = ( wet[0] > 0.0005f || wet[1] > 0.0005f ) ? 1.0f : 0.0f;

		if ( wet[3] > 0.0f ) {
			vec4_t occz;

			occz[0] = tr.hzmRainOccZ[0];
			occz[1] = tr.hzmRainOccZ[1];
			occz[2] = tr.hzmPuddleQ;
			occz[3] = 0.45f;     // lightmap sheen (plan D6, kept)
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETMAT, s_hzmClassMat[cls] );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETOCC, tr.hzmRainOccXform );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETOCCZ, occz );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSKYZ, tr.hzmSkyZenith );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSKYH, tr.hzmSkyHorizon );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSUN, tr.hzmWetSun );
			GLSL_SetUniformVec4( sp, UNIFORM_HZMWETSUNCOL, tr.hzmWetSunCol );
			GL_BindToTMU( tr.hzmRainOccImage, TB_HZMRAINOCC );
			GL_BindToTMU( tr.hzmWetNoiseImage, TB_HZMWETNOISE );
		}
	}
	GLSL_SetUniformVec4( sp, UNIFORM_HZMWET, wet );
}
