/*
===========================================================================
HZM coop [2026-09-27] FOG-OWNED LOD for renderer_opengl2: the PURE half.

  docs/proposals/fog_lod_pop_2026-09-27/ - research.md (why), plan.md piece B (what), vet.md (the review that
  shaped it), lookdev/rubric.md (the measured pass). The glue (cvars, the view checks, the Omaha list) is
  tr_hzm_lodfog_rb.c. Every function here reads ONLY its arguments - no tr, no backEnd, no cvars, no GL - so
  docs/tools/hzm_lodfog_selftest compiles this exact file into a console program.

THE PROBLEM. MOHAA trees fade their cards with alphaGen distFade / oneMinusTikiDistFade into an alphaFunc GE128 test,
at absolute distances tuned for retail fog. The alpha test turns each linear ramp into a one-frame step (the shipped
HD leaf alpha has a spike at exactly 1.0, so 24-57% of a card's coverage goes at once), and the coop fog profiles
moved the fog on 44 maps, so those steps now happen in clear air: "distant objects pop instead of fading".

THE RULE. A distance fade may not start until the whole model is inside 100% fog at every screen position.
  - The fog is linear in PLANAR eye depth and reaches 100% at farEff = farplane x r_globalFogEndScale.
  - Inside the frustum a point at radial distance D has planar depth >= D / edge, where
        edge = sqrt(1 + tan^2(fovX/2) + tan^2(fovY/2))      (the corner ray; 1.91 at 3440x1440, cg_fov 80)
    so D >= floor = farEff x edge guarantees 100% fog anywhere on screen.
  - Every vertex of an instance lies within its cull radius r of the origin, so once the ORIGIN is at
    floor + r, every fragment of the model is past the floor.
  GOVERNED (floor + r <= cap): the band starts at max(near, floor + r); per-vertex fades hold their near-side value
  until the origin itself reaches floor + r (the gate); distances are TRUE (the retail per-vertex fade measures
  unscaled model-space vertices, ~2x off on a scale-0.52 TIKI); coarse culls use the true 3-D distance (the retail
  R_DistanceCullPointAndRadius counts dz twice and culls early when the eye is above or below the model).
  Above the cap the band hands back to the authored one CONTINUOUSLY over the next 25% of the cap, so a fog that
  moves (dust storm, script ramp, the 2 s profile keeper) can never make a band jump; past that it is the retail
  behaviour exactly.
===========================================================================
*/

#include "tr_local.h"

/*
==============
R_HZM_LodFogFloor

0 = no floor (no farplane, degenerate fov).
==============
*/
float R_HZM_LodFogFloor( float farplane, float endScale, float fovXdeg, float fovYdeg )
{
	float farEff, tx, ty;

	farEff = farplane * endScale;
	if ( !( farEff > 0.0f ) ) {		// also catches NaN
		return 0.0f;
	}
	if ( !( fovXdeg > 0.0f && fovXdeg < 179.0f && fovYdeg > 0.0f && fovYdeg < 179.0f ) ) {
		return 0.0f;
	}

	tx = (float)tan( fovXdeg * ( M_PI / 360.0 ) );
	ty = (float)tan( fovYdeg * ( M_PI / 360.0 ) );

	return farEff * (float)sqrt( 1.0 + tx * tx + ty * ty );
}

/*
==============
R_HZM_LodFogBand

floorDist  - R_HZM_LodFogFloor (0 = off)
cap        - r_hzmLodFogCap (<= 0 = off)
instRadius - the instance's cull radius (cStaticModelUnpacked_t::cull_radius, world units)
nearIn/rangeIn - the shader's authored fDistNear / fDistRange
modelScale - tiki->load_scale x the instance scale (what the GL matrix applies to tess.xyz)

Fills out for every return value; HZM_LODBAND_AUTHORED leaves it exactly the retail band (distScale 1, no gate,
retail culls).
==============
*/
int R_HZM_LodFogBand( float floorDist, float cap, float instRadius, float nearIn, float rangeIn, float modelScale,
                      hzmLodBand_t *out )
{
	float want, k, nearH;

	out->nearDist     = nearIn;
	out->range        = rangeIn;
	out->distScale    = 1.0f;
	out->gateDist     = 0.0f;
	out->trueDistance = qfalse;

	if ( !( floorDist > 0.0f ) || !( cap > 0.0f ) ) {
		return HZM_LODBAND_AUTHORED;
	}
	if ( !( instRadius >= 0.0f ) ) {
		instRadius = 0.0f;
	}
	if ( !( modelScale > 0.0f ) ) {
		modelScale = 1.0f;
	}

	want = floorDist + instRadius;

	if ( want <= cap ) {
		out->nearDist     = ( nearIn > want ) ? nearIn : want;
		out->distScale    = modelScale;
		out->gateDist     = want;
		out->trueDistance = qtrue;
		return HZM_LODBAND_GOVERNED;
	}

	k = ( want - cap ) / ( 0.25f * cap );
	if ( k >= 1.0f ) {
		return HZM_LODBAND_AUTHORED;
	}

	// continuous hand-back: the start slides from the cap to the authored near, the distance measure from true to
	// the retail unscaled one; no gate (the model is no longer guaranteed hidden in this zone anyway)
	nearH             = cap * ( 1.0f - k ) + nearIn * k;
	out->nearDist     = ( nearIn > nearH ) ? nearIn : nearH;
	out->distScale    = modelScale * ( 1.0f - k ) + k;
	out->trueDistance = qtrue;
	return HZM_LODBAND_HANDBACK;
}

/* self-contained string helpers: this file links on its own into docs/tools/hzm_lodfog_selftest (no q_shared.c) */
static int R_HZM_LodFogIEq( const char *a, const char *b )
{
	for ( ;; a++, b++ ) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if ( ca >= 'A' && ca <= 'Z' ) ca += 'a' - 'A';
		if ( cb >= 'A' && cb <= 'Z' ) cb += 'a' - 'A';
		if ( ca != cb ) return 0;
		if ( !ca ) return 1;
	}
}

static void R_HZM_LodFogCopy( char *dst, const char *src, int size )
{
	int i;
	for ( i = 0; i < size - 1 && src[i]; i++ ) {
		dst[i] = src[i];
	}
	dst[i] = 0;
}

/*
==============
R_HZM_LodFogMapNameExcluded

The Omaha list for r_hzmLodFog (user rule 2026-09-27: any visual change on these maps is the user's decision).
Deliberately NOT renderercommon/hzm_light_restore.h's list, which excludes only m3l1a/m3l1b from the light restores.
m3l1a/m3l1b/obj_team3 carry distance-faded barbed wire, e3l1/e3l2 oaks and bushes (research.md section 6).
Compared on the last path component of the world name, case-insensitively, with a trailing _sml stripped (the
server-side small bsp shares the static-model lump).
==============
*/
qboolean R_HZM_LodFogMapNameExcluded( const char *worldBaseName )
{
	static const char *omaha[] = { "m3l1a", "m3l1b", "e3l1", "e3l2", "obj_team3" };
	char        base[MAX_QPATH];
	const char *p, *s;
	int         i, len;

	if ( !worldBaseName || !worldBaseName[0] ) {
		return qfalse;
	}
	p = worldBaseName;
	for ( s = worldBaseName; *s; s++ ) {
		if ( *s == '/' || *s == '\\' ) {
			p = s + 1;
		}
	}
	R_HZM_LodFogCopy( base, p, sizeof( base ) );
	len = (int)strlen( base );
	if ( len > 4 && R_HZM_LodFogIEq( base + len - 4, ".bsp" ) ) {
		base[len - 4] = 0;
		len -= 4;
	}
	if ( len > 4 && R_HZM_LodFogIEq( base + len - 4, "_sml" ) ) {
		base[len - 4] = 0;
	}
	for ( i = 0; i < (int)ARRAY_LEN( omaha ); i++ ) {
		if ( R_HZM_LodFogIEq( base, omaha[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
==============
R_HZM_LodFogExcludedCached

The decision for the LOADED world, re-derived whenever the world's NAME changes (the same keying as
R_HZM_LightRestoreProtectedWorld, tr_shade_calc.c). *changed (optional) is set when this call re-derived it.
==============
*/
qboolean R_HZM_LodFogExcludedCached( const char *worldBaseName, hzmLodFogMapCache_t *cache, qboolean *changed )
{
	const char *name = worldBaseName ? worldBaseName : "";

	if ( changed ) {
		*changed = qfalse;
	}
	if ( cache->valid && !strcmp( cache->name, name ) ) {
		return cache->excluded;
	}
	R_HZM_LodFogCopy( cache->name, name, sizeof( cache->name ) );
	cache->excluded = R_HZM_LodFogMapNameExcluded( name );
	cache->valid    = qtrue;
	if ( changed ) {
		*changed = qtrue;
	}
	return cache->excluded;
}
