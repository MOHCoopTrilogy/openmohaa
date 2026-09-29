/*
===========================================================================
HZM coop [2026-09-27] FOG-OWNED LOD for renderer_opengl2: the GLUE half (cvars, view checks, the Omaha list).
The maths and its reasoning are in tr_hzm_lodfog.c; the plan is docs/proposals/fog_lod_pop_2026-09-27/plan.md.

  r_hzmLodFog       0  piece B: tree/prop distance fades wait until the model is fully inside the fog. 0 = retail.
  r_hzmLodFogCap    3500  B's perf guard: a model governed only while floor + radius <= this.
  r_hzmLodFogOmaha  0  1 = also govern the Omaha list below (user decision pending - default excluded).
  r_hzmFogAO        0  piece A: SSAO faded by the forward fog fraction (tr_postprocess.c RB_HZMSsao, ssao_fp.glsl).
  r_hzmLodFogDebug  0  2 = REFERENCE mode for the headless dolly (plan.md 5.3): no distance LOD at all on static
                       models - every fade-out held fully visible, every fade-in (impostor) culled - on every map, fog
                       or not. Only fog and the far-plane cull still act, so (capture - reference) is the LOD alone.

All flags 0 (TRAPS T7: tuning knobs are not archived, so a default change ships), all default OFF, and with
every one of them 0 the renderer output is byte-identical to before this change.
Registered from R_Register (every R_Init): renderer statics die with the DLL on vid_restart (ENGINE.md), so every
value this file caches is re-derived after a restart.
===========================================================================
*/

#include "tr_local.h"

cvar_t *r_hzmLodFog;
cvar_t *r_hzmLodFogCap;
cvar_t *r_hzmLodFogOmaha;
cvar_t *r_hzmFogAO;
cvar_t *r_hzmLodFogDebug;

#define HZM_LODFOG_REFERENCE_DIST 1.0e9f

/*
==============
R_HZM_LodFogRegister
==============
*/
void R_HZM_LodFogRegister( void )
{
	r_hzmLodFog      = ri.Cvar_Get( "r_hzmLodFog",      "0",    0 );
	r_hzmLodFogCap   = ri.Cvar_Get( "r_hzmLodFogCap",   "3500", 0 );
	r_hzmLodFogOmaha = ri.Cvar_Get( "r_hzmLodFogOmaha", "0",    0 );
	r_hzmFogAO       = ri.Cvar_Get( "r_hzmFogAO",       "0",    0 );
	r_hzmLodFogDebug = ri.Cvar_Get( "r_hzmLodFogDebug", "0",    0 );
}

/*
==============
R_HZM_LodFogMapExcluded

The Omaha list decision (tr_hzm_lodfog.c R_HZM_LodFogMapNameExcluded) for the LOADED world, cached per BSP NAME.
FIX [2026-09-28, renderer-keep review]: the first version cached against the tr.world POINTER, but tr.world is always
&s_worldData (tr_bsp.c RE_LoadWorldMap), so the first map of the DLL's lifetime decided the exclusion for every map
after it - m3l1a first left the feature off everywhere, any other map first applied it to Omaha. Now keyed on
tr.world->baseName, like R_HZM_LightRestoreProtectedWorld. The static dies with the DLL on vid_restart, which only
means one re-derivation. One developer line per change, so a test log proves the decision per map.
==============
*/
static qboolean R_HZM_LodFogMapExcluded( void )
{
	static hzmLodFogMapCache_t cache;
	qboolean                   changed, excluded;

	if ( !tr.world ) {
		return qfalse;
	}
	excluded = R_HZM_LodFogExcludedCached( tr.world->baseName, &cache, &changed );
	if ( changed ) {
		ri.Printf( PRINT_DEVELOPER, "^~^~^ LODFOG world %s: omaha-excluded %d\n", tr.world->baseName, (int)excluded );
	}
	return excluded;
}

/*
==============
R_HZM_LodFogFloorForView

The floor for one view, or 0 = retail behaviour. 0 whenever the fog cannot be trusted to hide anything:
the feature off, a portal / sky-portal / shadow view, no farplane, the global fog off or scaled below 100%
(r_globalFogScale < 1 never reaches full fog), r_farplane_nofog, or an excluded map.
The floor is planar-conservative: r_globalFogRadial / a future radial forward fog only fog MORE at a given radial
distance, so the planar edge factor stays a valid bound on every fog path.
==============
*/
float R_HZM_LodFogFloorForView( const viewParms_t *vp )
{
	if ( vp->isPortal || vp->isPortalSky || ( vp->flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW | VPF_PSHADOW ) ) ) {
		return 0.0f;
	}
	if ( r_hzmLodFogDebug && r_hzmLodFogDebug->integer == 2 ) {
		return HZM_LODFOG_REFERENCE_DIST;	// R_HZM_LodFogStaticBand turns this into the no-LOD reference band
	}
	if ( !r_hzmLodFog || !r_hzmLodFog->integer ) {
		return 0.0f;
	}
	if ( !( vp->farplane_distance > 0.0f ) ) {
		return 0.0f;
	}
	if ( ( r_globalFog && !r_globalFog->integer ) || ( r_farplane_nofog && r_farplane_nofog->integer ) ) {
		return 0.0f;
	}
	if ( r_globalFogScale && r_globalFogScale->value < 1.0f ) {
		return 0.0f;
	}
	if ( R_HZM_LodFogMapExcluded() && !( r_hzmLodFogOmaha && r_hzmLodFogOmaha->integer ) ) {
		return 0.0f;
	}

	return R_HZM_LodFogFloor( vp->farplane_distance, r_globalFogEndScale ? r_globalFogEndScale->value : 1.0f,
	                          vp->fovX, vp->fovY );
}

/*
==============
R_HZM_LodFogStaticBand

The band a distance-fade shader on static model SM uses in this view. Returns HZM_LODBAND_* (AUTHORED = retail,
out untouched from the shader values). Static models only: TIKI entities keep the retail behaviour (no coarse cull
path exists for them, and the shipped entity trees are two omnitrees on e3l1, which is excluded anyway).
==============
*/
int R_HZM_LodFogStaticBand( float floorDist, const shader_t *sh, const cStaticModelUnpacked_t *SM, hzmLodBand_t *out )
{
	float modelScale = 1.0f;

	if ( SM && SM->tiki ) {
		modelScale = SM->tiki->load_scale * SM->scale;
	}

	if ( SM && r_hzmLodFogDebug && r_hzmLodFogDebug->integer == 2 ) {
		// REFERENCE: a band that never starts - fade-outs stay fully visible (and are never coarse-culled), fade-ins
		// (the impostor cards) stay at alpha 0 and are always coarse-culled
		out->nearDist     = HZM_LODFOG_REFERENCE_DIST;
		out->range        = sh->fDistRange;
		out->distScale    = modelScale;
		out->gateDist     = HZM_LODFOG_REFERENCE_DIST;
		out->trueDistance = qtrue;
		return HZM_LODBAND_GOVERNED;
	}

	return R_HZM_LodFogBand( SM ? floorDist : 0.0f, r_hzmLodFogCap ? r_hzmLodFogCap->value : 0.0f,
	                         SM ? SM->cull_radius : 0.0f, sh->fDistNear, sh->fDistRange, modelScale, out );
}
