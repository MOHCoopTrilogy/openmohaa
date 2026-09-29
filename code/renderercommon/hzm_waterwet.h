/*
===========================================================================
HZM coop - RAIN WETNESS (+ the later water pass): constants shared by cgame and renderergl2.
docs/proposals/water_wetness_2026-09-27 (research.md, plan.md, vet.md, lookdev/RUBRIC.md).

1) THE SWITCHES. r_hzmWet / r_hzmWater are flags 0 and default "-1" = the AUTO define below. Both AUTO values are 1
   (ON) since v1.10.3: the user saw the in-engine comparison (ingame/INGAME.md, 42/42 on v1103d) and chose it
   (plan D7, 2026-09-28). The menu rows and the DEFAULTS cfgs write -1, so a later change of these defines still
   reaches every player; a saved 0 or 1 is the player's own choice (TRAPS T7).

2) THE OMAHA EXCLUSION (user hard rule, 2026-09-27: "OMAHA IS OFF-LIMITS"). m3l1a, m3l1b, e3l1, e3l2, obj_team3, as
   the BSP base name or a suffixed copy of it (m3l1a_sml) - the tr_surface.c R_HZM_TerrainLightMapProtected rule.
   Both features return early on it: the renderer from tr.world->baseName at load, cgame from cgs.mapname.
   The multiplayer weather rains on all five BSPs today (plan.md section 0), so this is load-bearing, not belt and
   braces.
   Deliberately NOT hzm_light_restore.h's list (m3l1a/m3l1b only) - that one answers a different question.

3) THE EASING (plan W1, lookdev/05_transition_curve.png). Stage 1 is asymmetric (wet fast, dry slow, puddles
   slowest - Lagarde, "Water drop 3b"); stage 2 has ONE fixed time constant so the published value's derivative stays
   continuous when stage 1 changes direction, and the 2 s configstring staircase (global/weather.scr) is smoothed
   twice. docs/proposals/water_wetness_2026-09-27/tools/ww_motion.py ease() is the reference implementation.

Header-only; needs q_shared.h first (Q_stricmpn).
===========================================================================
*/
#ifndef HZM_WATERWET_H
#define HZM_WATERWET_H

#define HZM_WET_AUTO    1   // r_hzmWet   -1 means this. ON since v1.10.3 (plan D7, the user's decision 2026-09-28)
#define HZM_WATER_AUTO  1   // r_hzmWater -1 means this. ON since v1.10.3 (plan D7, the user's decision 2026-09-28)

// W1 easing, seconds (cgame)
#define HZM_WET_TAU_UP      12.0f
#define HZM_WET_TAU_DOWN    55.0f
#define HZM_WET_TAU_S2       8.0f
#define HZM_PUDDLE_TAU_UP   40.0f
#define HZM_PUDDLE_TAU_DOWN 170.0f
#define HZM_PUDDLE_TAU_S2   12.0f

static ID_INLINE qboolean HZM_WaterWetOmahaMap( const char *mapName ) {
	static const char *const omaha[] = { "m3l1a", "m3l1b", "e3l1", "e3l2", "obj_team3" };
	const char	*base;
	size_t		len, n;
	int			i;

	if ( !mapName || !mapName[0] ) {
		return qfalse;
	}
	base = mapName + strlen( mapName );
	while ( base > mapName && base[-1] != '/' && base[-1] != '\\' ) {
		base--;
	}
	len = strcspn( base, "." );
	for ( i = 0; i < (int)( sizeof( omaha ) / sizeof( omaha[0] ) ); i++ ) {
		n = strlen( omaha[i] );
		if ( len >= n && !Q_stricmpn( base, omaha[i], (int)n ) && ( len == n || base[n] == '_' ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

// W3 the water pass draws ONLY on these shaders (exact names, case-insensitive): real water surfaces, none of the
// Omaha family, none of the brief's off-limits tokens (omaha ocean seabed wetsand shoreline surf wake). NOT listed on
// purpose (plan D3 / docs/proposals/water_wetness_2026-09-27/research.md 1.1): deepbluesea*, northafrika_shoreline,
// omaha_set4_shoreline, frozenriver (ice), t2fogplane (a fog plane), river floors, waterclip. tools/ww_scope.py ALLOW
// is the same list; lookdev/data/scope.md is the per-map result (33 maps).
#define HZM_WATER_NUM_ALLOW 16

// the allowlist slot of a shader name, -1 = not water we touch (tr_hzm_water.c keys its per-shader lightmap reference on it)
static ID_INLINE int HZM_WaterAllowIndex( const char *shaderName ) {
	static const char *const allow[HZM_WATER_NUM_ALLOW] = {
		"textures/choppywater/tehao_choppywater", "textures/choppywater/tehao_indoorwater",
		"textures/choppywater/subpen_opaque_chop", "textures/choppywater/mp_stuckguter_lib",
		"textures/water/canal_water", "textures/water/subpen_clear", "textures/water/subpen_opaque",
		"textures/water/subpen_transition", "textures/water/subpen_water_clear",
		"textures/misc_outside/canal_sludge", "textures/misc_outside/subpen_water4", "textures/misc_outside/subpen_water2",
		"textures/misc_outside/pond", "textures/misc_outside/riversf", "textures/misc_outside/water_nodraw",
		"textures/jon/ex_pool",
	};
	int i;

	if ( !shaderName ) {
		return -1;
	}
	for ( i = 0; i < HZM_WATER_NUM_ALLOW; i++ ) {
		if ( !Q_stricmp( shaderName, allow[i] ) ) {
			return i;
		}
	}
	return -1;
}

static ID_INLINE qboolean HZM_WaterAllowlisted( const char *shaderName ) {
	return HZM_WaterAllowIndex( shaderName ) >= 0 ? qtrue : qfalse;
}

// W1 targets from the networked rain density (-1..10, cg.rain.density): drizzle 0.8 -> film 0.53, storm 3 -> 1;
// puddles only in real rain.
static ID_INLINE float HZM_WetFilmTarget( float density ) {
	float x;
	if ( density <= 0.0f ) {
		return 0.0f;
	}
	x = density * 0.5f;
	if ( x > 1.0f ) {
		x = 1.0f;
	}
	return (float)pow( x, 0.7 );
}

static ID_INLINE float HZM_WetPuddleTarget( float density ) {
	float x;
	if ( density <= 0.0f ) {
		return 0.0f;
	}
	x = ( density - 1.0f ) * 0.25f;
	return x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
}

#endif // HZM_WATERWET_H
