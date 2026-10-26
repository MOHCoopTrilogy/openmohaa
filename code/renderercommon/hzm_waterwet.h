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
#define HZM_WATER_NUM_ALLOW 22

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
		"textures/central_europe_winter/frozenriver",     // [2026-10-25] the ICE preset (part A of ice_1025/DESIGN.md)
		// [2026-10-25, user: Omaha WATER is allowed now - beach, sand wash and sky stay untouched] the open sea only; the
		// *_shoreline wash shaders (alpha ramps over sand) are NOT here
		"textures/misc_outside/deepbluesea", "textures/misc_outside/deepbluesea_runup", "textures/misc_outside/deepbluesea_runup_nowave",
		// [2026-10-25, user: Omaha WATER layers incl. the shoreline sheet are in scope; sand, seabed and sky are not]
		"textures/misc_outside/deepbluesea_shoreline", "textures/misc_outside/northafrika_shoreline",
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

// ---------------------------------------------------------------------------------------------------------------------
// WATER PRESETS (docs/proposals/kickoff_2026-10-18/ice_1025/DESIGN.md part B, user-approved 2026-10-25). A preset replaces
// the milky retail water of an allowlisted shader, on named maps, with a body colour + swell + chop + fresnel sky/planar
// reflection (+ ice floes at the quay walls for COLD). It is a mode of THIS pass, not a second system: same allowlist, same
// master (r_hzmWater), same fade, same Omaha rule. Preset 0 = the shipped reflection layer only.
// ---------------------------------------------------------------------------------------------------------------------
#define HZM_WPRESET_NONE 0
#define HZM_WPRESET_COLD 1      // cold northern harbour: Norway U-boat pens + the cutscene fjord (m2l2a/b/c, co_lobby7)
#define HZM_WPRESET_ICE  2      // frozen river ice (frozenriver: m6l2a/b, mp_verschneit_dm, mohdm5): glossy cracked ice + frost
#define HZM_WPRESET_CHANNEL 3   // open Channel sea (Omaha m3l1a/m3l1b/obj_team3 deepbluesea, Anzio): heavy swell, whitecaps
#define HZM_WPRESET_HARBOUR 4   // Mediterranean harbour (Bizerte choppywater): clear teal, light chop
#define HZM_WPRESET_RIVER   5   // European canals and rivers: muddy green, calm, partly see-through
#define HZM_WPRESET_POND    6   // ponds and pools: calm, dark green
#define HZM_WPRESET_COUNT 7

typedef struct {
	float	cover;          // 1 - cover = how much of the retail stages shows through
	float	swell;          // swell slope gain (1 = the three-sine set of the prototype)
	float	chop;           // ripple-map chop gain
	float	floe;           // 0 = no ice, 1 = the prototype amount at the quay walls
	float	planar;         // planar-reflection mix (0 = sky probe only)
	float	deep[3], shallow[3];
	float	drift;          // world units from a quay wall over which crust / floes thicken
	float	lmgain;         // ICE: lightmap rgb gain for the diffuse term (retail multiplies texture x lightmap x ~2)
	float	whitecap;       // crest foam amount (0 = none)
	float	yaw;            // rotation (rad) of the swell directions: waves run toward the shore (Omaha: the beach is +y)
	float	shore;          // foam bands along the shoreline ramp (shoreline sheets only)
	float	flow;           // 1 = river: ripples stretch along the water's own long axis and drift downstream
	float	contact;        // contact foam strength around solid things at the waterline (0 = none)
} hzmWaterPreset_t;

static const hzmWaterPreset_t hzmWaterPresets[] = {
	{ 0.0f },
	// COLD: dark green-grey-black (tools/water_look.py: deep 0.012 0.034 0.036, shallow 0.045 0.100 0.095)
	{ 0.97f, 1.0f, 1.0f, 1.0f, 1.0f, { 0.012f, 0.034f, 0.036f }, { 0.045f, 0.100f, 0.095f }, 330.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f , 0.0f },
	// ICE: deep = clear ice (dark cold blue), shallow = white (bubbles / cracks), cover 1 (frost patches cut it away in the shader)
	{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, { 0.035f, 0.110f, 0.200f }, { 0.640f, 0.740f, 0.860f }, 170.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f , 0.0f },
	// the water presets below are defined at body gain 1; r_hzmWaterBody (default 2.0) scales them all
	// CHANNEL: coherent grey-green-blue sea. cover is UNIFORM (no blotchy mix with the retail stages); the retail foam and wash
	// come back only where the shoreline sheet's own alpha ramp fades our coverage out. Swell runs at the beach (+y, yaw 63.7 deg).
	{ 0.93f, 2.6f, 1.0f, 0.0f, 0.0f, { 0.013f, 0.030f, 0.036f }, { 0.040f, 0.085f, 0.095f }, 0.0f, 0.0f, 0.80f, 1.112f, 0.6f, 0.0f , 1.0f },
	// HARBOUR: clear Mediterranean teal, light chop, a few whitecaps
	{ 0.92f, 1.0f, 2.8f, 0.0f, 0.0f, { 0.005f, 0.035f, 0.045f }, { 0.025f, 0.100f, 0.108f }, 0.0f, 0.0f, 0.30f, 0.0f, 0.0f, 0.0f , 0.6f },
	// RIVER: muddy green with a downstream drift; 35 % of the retail stages (the riverbed) shows - shallow water must stay readable
	{ 0.65f, 0.15f, 1.6f, 0.0f, 0.0f, { 0.035f, 0.055f, 0.030f }, { 0.095f, 0.140f, 0.080f }, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f , 0.5f },
	// POND: calm dark green
	{ 0.60f, 0.08f, 0.70f, 0.0f, 0.0f, { 0.014f, 0.030f, 0.024f }, { 0.040f, 0.075f, 0.055f }, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f , 0.5f },
};

// the BSP base names a preset applies to: the exact name or "<name>_..." (_sml copies), like HZM_WaterWetOmahaMap
static ID_INLINE qboolean HZM_WaterMapIn( const char *mapName, const char *const *list, int n ) {
	const char	*base;
	size_t		len, l;
	int			i;

	if ( !mapName || !mapName[0] ) {
		return qfalse;
	}
	base = mapName + strlen( mapName );
	while ( base > mapName && base[-1] != '/' && base[-1] != '\\' ) {
		base--;
	}
	len = strcspn( base, "." );
	for ( i = 0; i < n; i++ ) {
		l = strlen( list[i] );
		if ( len >= l && !Q_stricmpn( base, list[i], (int)l ) && ( len == l || base[l] == '_' ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

// the retail first stage of the shoreline sheet fades ITSELF into the sand: alphaGen tCoord 1.8 -0.01 0 1, i.e.
// alpha = clamp((max - min) * T + min, 0, 1) on the RAW texcoord (zz_coop_shoreline.shader). The water pass has no access to
// that stage, so it re-applies the same ramp to its own coverage. axis 0 = none, 1 = S, 2 = T.
static ID_INLINE int HZM_WaterRampFor( const char *shaderName, float *rmin, float *rmax ) {
	*rmin = 0.0f;
	*rmax = 0.0f;
	if ( shaderName && !Q_stricmp( shaderName, "textures/misc_outside/deepbluesea_shoreline" ) ) {
		*rmin = 1.8f;
		*rmax = -0.01f;
		return 2;
	}
	return 0;
}

// preset for (shader, map): HZM_WPRESET_*; only allowlisted shader names are ever asked
static ID_INLINE int HZM_WaterPresetFor( const char *shaderName, const char *mapName ) {
	static const char *const cold[] = { "m2l2a", "m2l2b", "m2l2c", "co_lobby7" };
	static const char *const channel[] = { "textures/misc_outside/deepbluesea", "textures/misc_outside/deepbluesea_runup",
	                                       "textures/misc_outside/deepbluesea_runup_nowave", "textures/misc_outside/deepbluesea_shoreline" };
	static const char *const harbour[] = { "textures/choppywater/tehao_choppywater", "textures/choppywater/tehao_indoorwater",
	                                       "textures/choppywater/subpen_opaque_chop", "textures/choppywater/mp_stuckguter_lib",
	                                       "textures/misc_outside/northafrika_shoreline" };
	static const char *const river[] = { "textures/water/canal_water", "textures/water/subpen_clear", "textures/water/subpen_opaque",
	                                     "textures/water/subpen_transition", "textures/water/subpen_water_clear",
	                                     "textures/misc_outside/canal_sludge", "textures/misc_outside/subpen_water4",
	                                     "textures/misc_outside/subpen_water2", "textures/misc_outside/riversf" };
	static const char *const pond[] = { "textures/misc_outside/pond", "textures/jon/ex_pool", "textures/misc_outside/water_nodraw" };
	int i;

	if ( !shaderName ) {
		return HZM_WPRESET_NONE;
	}
	if ( !Q_stricmp( shaderName, "textures/central_europe_winter/frozenriver" ) ) {
		return HZM_WPRESET_ICE;
	}
	if ( ( !Q_stricmp( shaderName, "textures/misc_outside/subpen_water4" )
	       || !Q_stricmp( shaderName, "textures/misc_outside/canal_sludge" ) )
	     && HZM_WaterMapIn( mapName, cold, (int)( sizeof( cold ) / sizeof( cold[0] ) ) ) ) {
		return HZM_WPRESET_COLD;
	}
	for ( i = 0; i < (int)( sizeof( channel ) / sizeof( channel[0] ) ); i++ ) {
		if ( !Q_stricmp( shaderName, channel[i] ) ) return HZM_WPRESET_CHANNEL;
	}
	for ( i = 0; i < (int)( sizeof( harbour ) / sizeof( harbour[0] ) ); i++ ) {
		if ( !Q_stricmp( shaderName, harbour[i] ) ) return HZM_WPRESET_HARBOUR;
	}
	// RIVER stays OFF (plain reflection layer) until its look is approved: the t13 captures read as a flat dark sheet
	// (no flow, no reflections at bank angles). Set HZM_RIVER_PRESET_ON to 1 to try it.
#define HZM_RIVER_PRESET_ON 0     // off until a river view is captured: m5l2b's teleport box keeps cameras off the water
#if HZM_RIVER_PRESET_ON
	for ( i = 0; i < (int)( sizeof( river ) / sizeof( river[0] ) ); i++ ) {
		if ( !Q_stricmp( shaderName, river[i] ) ) return HZM_WPRESET_RIVER;
	}
#else
	(void)river;
#endif
	for ( i = 0; i < (int)( sizeof( pond ) / sizeof( pond[0] ) ); i++ ) {
		if ( !Q_stricmp( shaderName, pond[i] ) ) return HZM_WPRESET_POND;
	}
	return HZM_WPRESET_NONE;
}

#endif // HZM_WATERWET_H
