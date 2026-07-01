/*
===========================================================================
HZM coop - 3D GRASS (renderergl1)

"Actual grass": deterministic scatter points are generated once at map load on
world surfaces flagged SURF_GRASS (upward-facing only), then each frame the
blades within r_grassRadius of the camera are drawn as small procedural
geometry (crossed tapered quads) with a CPU wind sway, a green base->tip
gradient, and a distance alpha-fade. No texture asset is needed.

Design notes / constraints (gl1, fixed-function):
- No GPU instancing/geometry shaders, so blades are submitted in immediate mode.
  Density x draw-distance is the framerate dial; defaults ship "light" and every
  knob is a live cvar. r_grass 0 = zero cost (the whole system is skipped).
- v1 scatters on SF_FACE / SF_TRIANGLES grass faces. Terrain heightmap patches
  (cTerraPatch) are a deliberate phase-2 follow-up.
===========================================================================
*/

#include "tr_local.h"

#ifndef VERTEXSIZE
#define VERTEXSIZE 8
#endif

#define GRASS_MAX_POINTS   60000   // hard cap on scatter points for the whole map (static budget)

typedef struct {
	vec3_t xyz;   // world position of the blade tuft base
} grassPoint_t;

static grassPoint_t s_grass[GRASS_MAX_POINTS];
static int          s_grassNum   = 0;
static qboolean     s_grassBuilt = qfalse;

// cached cvars (lazily fetched; CVAR_ARCHIVE so they persist + are console/menu tunable later)
static cvar_t *gr_on, *gr_radius, *gr_density, *gr_height, *gr_wind, *gr_maxDraw;

static void R_Grass_GetCvars( void ) {
	gr_on      = ri.Cvar_Get( "r_grass",         "0",     CVAR_ARCHIVE );   // master on/off (0 = vanilla)
	gr_radius  = ri.Cvar_Get( "r_grassRadius",   "1280",  CVAR_ARCHIVE );   // draw radius (units)
	gr_density = ri.Cvar_Get( "r_grassDensity",  "0.004", CVAR_ARCHIVE );   // blades per square unit of grass surface
	gr_height  = ri.Cvar_Get( "r_grassHeight",   "16",    CVAR_ARCHIVE );   // blade height (units)
	gr_wind    = ri.Cvar_Get( "r_grassWind",     "2.5",   CVAR_ARCHIVE );   // tip sway amplitude (units)
	gr_maxDraw = ri.Cvar_Get( "r_grassMaxDraw",  "16000", CVAR_ARCHIVE );   // safety cap on blades drawn per frame
}

// cheap deterministic hash of a world position -> [0,1), so per-blade randomness is stable frame to frame
static float R_Grass_Hash( float x, float y, float z, int salt ) {
	int   n = (int)( x * 73.0f ) ^ (int)( y * 179.0f ) ^ (int)( z * 283.0f ) ^ ( salt * 668265263 );
	n = ( n << 13 ) ^ n;
	n = ( n * ( n * n * 15731 + 789221 ) + 1376312589 ) & 0x7fffffff;
	return (float)n / 2147483648.0f;
}

// a surface is "grass" if it carries the SURF_GRASS material flag OR its shader is named *grass*
// (many MOHAA ground/terrain shaders aren't flagged but are named e.g. "grass", "field_grass").
static qboolean R_Grass_ShaderIsGrass( const shader_t *sh ) {
	if ( !sh ) {
		return qfalse;
	}
	if ( sh->surfaceFlags & SURF_GRASS ) {
		return qtrue;
	}
	if ( Q_stristr( sh->name, "grass" ) ) {
		return qtrue;
	}
	return qfalse;
}

// scatter blades across one terrain heightmap patch (512x512, 9x9 samples, 8x8 cells of 64u; z = z0 + h*2)
static void R_Grass_ScatterPatch( const cTerraPatchUnpacked_t *patch, unsigned *seed ) {
	float dens = gr_density ? gr_density->value : 0.004f;
	int   ci, cj, k, n;

	if ( dens <= 0.0f ) {
		return;
	}
	n = (int)( 4096.0f * dens );   // blades per 64x64 cell
	if ( n < 1 )  { n = 1; }
	if ( n > 64 ) { n = 64; }

	for ( cj = 0; cj < 8; cj++ ) {
		for ( ci = 0; ci < 8; ci++ ) {
			float h00 = (float)patch->heightmap[ cj * 9 + ci ]         * 2.0f;
			float h10 = (float)patch->heightmap[ cj * 9 + ci + 1 ]     * 2.0f;
			float h01 = (float)patch->heightmap[ ( cj + 1 ) * 9 + ci ] * 2.0f;
			float h11 = (float)patch->heightmap[ ( cj + 1 ) * 9 + ci + 1 ] * 2.0f;
			float cx  = patch->x0 + ci * 64.0f;
			float cy  = patch->y0 + cj * 64.0f;
			for ( k = 0; k < n && s_grassNum < GRASS_MAX_POINTS; k++ ) {
				float u, v, z;
				*seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5; u = (float)( *seed & 0xffff ) / 65535.0f;
				*seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5; v = (float)( *seed & 0xffff ) / 65535.0f;
				z = patch->z0 + ( h00 * ( 1 - u ) * ( 1 - v ) + h10 * u * ( 1 - v ) + h01 * ( 1 - u ) * v + h11 * u * v );
				s_grass[s_grassNum].xyz[0] = cx + u * 64.0f;
				s_grass[s_grassNum].xyz[1] = cy + v * 64.0f;
				s_grass[s_grassNum].xyz[2] = z;
				s_grassNum++;
			}
		}
	}
}

// add up to 'count' scatter points across one triangle (barycentric, deterministic per triangle)
static void R_Grass_ScatterTri( const vec3_t v0, const vec3_t v1, const vec3_t v2, int count, unsigned *seed ) {
	vec3_t e1, e2, nrm;
	int    i;

	VectorSubtract( v1, v0, e1 );
	VectorSubtract( v2, v0, e2 );
	CrossProduct( e1, e2, nrm );
	if ( VectorNormalize( nrm ) <= 0.0f ) {
		return;
	}
	if ( nrm[2] < 0.6f ) {
		return; // only roughly-upward ground gets grass (skip walls/ceilings/steep slopes)
	}

	for ( i = 0; i < count && s_grassNum < GRASS_MAX_POINTS; i++ ) {
		float r1, r2;
		// xorshift LCG for a stable, cheap per-point random
		*seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
		r1 = (float)( *seed & 0xffff ) / 65535.0f;
		*seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
		r2 = (float)( *seed & 0xffff ) / 65535.0f;
		if ( r1 + r2 > 1.0f ) { r1 = 1.0f - r1; r2 = 1.0f - r2; }

		s_grass[s_grassNum].xyz[0] = v0[0] + r1 * e1[0] + r2 * e2[0];
		s_grass[s_grassNum].xyz[1] = v0[1] + r1 * e1[1] + r2 * e2[1];
		s_grass[s_grassNum].xyz[2] = v0[2] + r1 * e1[2] + r2 * e2[2];
		s_grassNum++;
	}
}

static int R_Grass_BladesForTri( const vec3_t v0, const vec3_t v1, const vec3_t v2 ) {
	vec3_t e1, e2, cr;
	float  area, dens;

	VectorSubtract( v1, v0, e1 );
	VectorSubtract( v2, v0, e2 );
	CrossProduct( e1, e2, cr );
	area = 0.5f * VectorLength( cr );
	dens = gr_density ? gr_density->value : 0.004f;
	if ( dens <= 0.0f ) {
		return 0;
	}
	{
		int n = (int)( area * dens );
		if ( n > 400 ) { n = 400; }  // clamp per-triangle so one huge face can't eat the whole budget
		return n;
	}
}

// scan the world for SURF_GRASS surfaces and build the scatter set (called once per map load)
void R_InitGrassWorld( void ) {
	int      i;
	int      grassSurfs = 0, grassPatches = 0;
	unsigned seed = 0x1234abcd;

	s_grassNum   = 0;
	s_grassBuilt = qfalse;
	R_Grass_GetCvars();

	// Only scatter when grass is enabled. Keeps r_grass 0 truly zero-cost AND means the (surface
	// pointer-walking) scatter never runs in a normal session. NOTE: toggling r_grass on at runtime
	// therefore needs a map reload to populate; set it in autoexec or before loading a map.
	if ( !gr_on || !gr_on->integer ) {
		return;
	}

	if ( !tr.world ) {
		return;
	}

	// (1) BSP grass faces
	for ( i = 0; tr.world->surfaces && i < tr.world->numsurfaces && s_grassNum < GRASS_MAX_POINTS; i++ ) {
		msurface_t    *surf = &tr.world->surfaces[i];
		surfaceType_t *st;

		if ( !surf->shader || !surf->data ) {
			continue;
		}
		if ( !R_Grass_ShaderIsGrass( surf->shader ) ) {
			continue;
		}
		grassSurfs++;
		st = surf->data;

		if ( *st == SF_FACE ) {
			srfSurfaceFace_t *face = (srfSurfaceFace_t *)st;
			int              *idx  = (int *)( (byte *)face + face->ofsIndices );
			int               t;
			for ( t = 0; t + 2 < face->numIndices && s_grassNum < GRASS_MAX_POINTS; t += 3 ) {
				const float *p0 = face->points[ idx[t + 0] ];
				const float *p1 = face->points[ idx[t + 1] ];
				const float *p2 = face->points[ idx[t + 2] ];
				int          n  = R_Grass_BladesForTri( p0, p1, p2 );
				if ( n > 0 ) {
					R_Grass_ScatterTri( p0, p1, p2, n, &seed );
				}
			}
		} else if ( *st == SF_TRIANGLES ) {
			srfTriangles_t *tris = (srfTriangles_t *)st;
			int             t;
			for ( t = 0; t + 2 < tris->numIndexes && s_grassNum < GRASS_MAX_POINTS; t += 3 ) {
				const float *p0 = tris->verts[ tris->indexes[t + 0] ].xyz;
				const float *p1 = tris->verts[ tris->indexes[t + 1] ].xyz;
				const float *p2 = tris->verts[ tris->indexes[t + 2] ].xyz;
				int          n  = R_Grass_BladesForTri( p0, p1, p2 );
				if ( n > 0 ) {
					R_Grass_ScatterTri( p0, p1, p2, n, &seed );
				}
			}
		}
		// SF_GRID is skipped (curved patches are rare for ground); terrain patches handled below.
	}

	// (2) terrain heightmap patches flagged/named grass
	for ( i = 0; tr.world->terraPatches && i < tr.world->numTerraPatches && s_grassNum < GRASS_MAX_POINTS; i++ ) {
		cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];
		if ( !R_Grass_ShaderIsGrass( patch->shader ) ) {
			continue;
		}
		grassPatches++;
		R_Grass_ScatterPatch( patch, &seed );
	}

	s_grassBuilt = qtrue;
	ri.Printf( PRINT_ALL, "grass: scattered %i blades (%i grass faces, %i grass terrain patches of %i total)\n",
		s_grassNum, grassSurfs, grassPatches, tr.world->numTerraPatches );
}

void R_ShutdownGrass( void ) {
	s_grassNum   = 0;
	s_grassBuilt = qfalse;
}

// emit one tapered quad (2 tris) from base..top with a base->tip green gradient and the given alpha
static void R_Grass_EmitQuad( const vec3_t base, const vec3_t top, float halfWidth, float dirx, float diry, float alpha ) {
	float bx0 = base[0] - dirx * halfWidth, by0 = base[1] - diry * halfWidth;
	float bx1 = base[0] + dirx * halfWidth, by1 = base[1] + diry * halfWidth;
	// base color (dark green), tip color (lighter/yellower green)
	// two triangles forming the tapered quad (top collapses toward a point-ish tip)
	qglColor4f( 0.12f, 0.28f, 0.06f, alpha ); qglVertex3f( bx0, by0, base[2] );
	qglColor4f( 0.12f, 0.28f, 0.06f, alpha ); qglVertex3f( bx1, by1, base[2] );
	qglColor4f( 0.40f, 0.60f, 0.20f, alpha ); qglVertex3f( top[0] + dirx * halfWidth * 0.3f, top[1] + diry * halfWidth * 0.3f, top[2] );

	qglColor4f( 0.12f, 0.28f, 0.06f, alpha ); qglVertex3f( bx0, by0, base[2] );
	qglColor4f( 0.40f, 0.60f, 0.20f, alpha ); qglVertex3f( top[0] + dirx * halfWidth * 0.3f, top[1] + diry * halfWidth * 0.3f, top[2] );
	qglColor4f( 0.40f, 0.60f, 0.20f, alpha ); qglVertex3f( top[0] - dirx * halfWidth * 0.3f, top[1] - diry * halfWidth * 0.3f, top[2] );
}

// per-frame: draw the blades within radius of the camera. Called from RB_DrawSurfs after the world is drawn,
// with the world modelview re-loaded so the (world-space) blade verts project correctly.
void R_DrawGrass( void ) {
	vec3_t cam;
	float  radius, radius2, fadeStart, hScale, windAmp, t, windDirX, windDirY;
	int    i, drawn, maxDraw;

	// only the real world view: skip the weapon-HUD pass and any no-world (UI/model) views
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_HUD ) ) {
		return;
	}
	R_Grass_GetCvars();
	if ( !gr_on || !gr_on->integer ) {
		return;
	}
	// lazy-build so toggling r_grass on at runtime (the settings checkbox) populates WITHOUT a map reload
	if ( !s_grassBuilt ) {
		R_InitGrassWorld();
	}
	if ( s_grassNum <= 0 ) {
		return;
	}

	radius    = gr_radius ? gr_radius->value : 512.0f;
	if ( radius < 32.0f ) { radius = 32.0f; }
	radius2   = radius * radius;
	fadeStart = radius * 0.65f;           // fully opaque inside this, ramp to 0 at the radius edge
	hScale    = gr_height ? gr_height->value : 16.0f;
	windAmp   = gr_wind ? gr_wind->value : 2.5f;
	maxDraw   = gr_maxDraw ? gr_maxDraw->integer : 8000;
	t         = backEnd.refdef.time * 0.001f;
	windDirX  = 0.7071f; windDirY = 0.7071f;  // fixed wind heading (diagonal); fine for v1

	VectorCopy( backEnd.viewParms.ori.origin, cam );

	// world-space draw: load the world modelview, depth-test on, alpha blend, double-sided, untextured (white)
	qglMatrixMode( GL_MODELVIEW );
	qglLoadMatrixf( backEnd.viewParms.world.modelMatrix );
	GL_State( GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	qglDisable( GL_CULL_FACE );
	GL_Bind( tr.whiteImage );

	drawn = 0;
	qglBegin( GL_TRIANGLES );
	for ( i = 0; i < s_grassNum && drawn < maxDraw; i++ ) {
		float  dx = s_grass[i].xyz[0] - cam[0];
		float  dy = s_grass[i].xyz[1] - cam[1];
		float  d2 = dx * dx + dy * dy;
		float  dist, alpha, h, sway, hgt;
		vec3_t base, top;

		if ( d2 > radius2 ) {
			continue;
		}
		dist  = sqrt( d2 );
		alpha = ( dist <= fadeStart ) ? 1.0f : ( 1.0f - ( dist - fadeStart ) / ( radius - fadeStart ) );
		if ( alpha <= 0.02f ) {
			continue;
		}

		hgt  = R_Grass_Hash( s_grass[i].xyz[0], s_grass[i].xyz[1], s_grass[i].xyz[2], 1 );
		h    = hScale * ( 0.7f + 0.6f * hgt );                                   // per-blade height variation
		sway = windAmp * (float)sin( t * 1.7f + hgt * 6.2831f );                 // tip sway (phase from hash)

		VectorCopy( s_grass[i].xyz, base );
		top[0] = base[0] + windDirX * sway;
		top[1] = base[1] + windDirY * sway;
		top[2] = base[2] + h;

		// crossed quads (two perpendicular blades) so the tuft reads as 3D from any angle
		R_Grass_EmitQuad( base, top, 1.6f, 1.0f, 0.0f, alpha );
		R_Grass_EmitQuad( base, top, 1.6f, 0.0f, 1.0f, alpha );
		drawn++;
	}
	qglEnd();

	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	qglEnable( GL_CULL_FACE );
}
