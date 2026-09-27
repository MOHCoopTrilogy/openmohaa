/*
===========================================================================
HZM coop [user 2026-09-25] VEHICLE HEADLIGHTS + gl2 RETAIL LIGHT-GLOW RESTORE - the shared switches.

ONE header, read by BOTH halves of the feature, because they ship as a pair (TRAPS T10) and must never disagree
about what "auto" means (TRAPS T4: two constants that agree by coincidence are a bomb):
  renderer_opengl2.dll  Phase R - `deformVertexes lightglow` (every corona / muzzle glow / spark) and `rgbGen dot`
                        (the soft beam edge): r_hzmLightGlow, r_hzmLightGlowNear, r_hzmRgbGenDot (tr_init.c)
  cgame.dll             Phase H - the per-viewer headlight manager: cg_hzmHeadlights (cg_view.c CG_CoopHeadlights),
                        which also reads r_hzmLightGlow / r_hzmRgbGenDot so it never draws its glow as a flat quad
  searchlights [2026-09-26] ride r_hzmRgbGenDot too: S1 in the renderer (alphaGen dot on an additive stage -> RGB,
                        gl1 parity: the tower cone, R_HZM_AlphaDotToRgb) and S2 in cgame (the t1l1 sky-beam
                        shader swap, CG_HL_SkyBeamShader)
  Phase S [2026-09-26]  spot cones + lens flares (docs/proposals/headlights_2026-09-25/plan_phaseS.md, vet_phaseS.md):
                        r_hzmSpot / r_hzmFlares in renderer_opengl2 (tr_hzm_spot.c, tr_hzm_spot_rb.c), the shared
                        request facility in cgame (cg_hzmspot.c), and the CARRIER protocol in section 3 below.

1) THE AUTO DEFAULTS. Every switch is registered "-1" = auto with flags 0 (never saved), the r_skyHD pattern:
   -1 resolves to the define below, 0 / 1 force it. While these are 0 NOTHING changes for any player. When the
   user's A/B passes, flip them to 1 HERE and rebuild BOTH DLLs - that reaches every player, which a saved "0"
   never could (TRAPS T7).

2) THE OMAHA PROTECTION LIST (user decision "Option A": restore everywhere EXCEPT Omaha Beach). On these BSPs both
   restores are forced OFF whatever the switches say, so the maps render byte-identically to the pre-restore gl2
   (lamp coronas, muzzle glows and sparks included), and the cgame headlight manager does nothing at all.
   Keyed on the BSP base name - path and extension stripped, case-insensitive:
     renderer  R_HZM_LightRestoreProtectedWorld()  (tr_shade_calc.c)  <- tr.world->baseName, the LOADED world
     cgame     CG_CoopHeadlights()                 (cg_view.c)        <- cgs.mapname
   Monte Cassino (e3l1/e3l2) is deliberately NOT listed: the user put its lamps and trucks in scope. Omaha's
   assets, shaders and zzzzzzzzzz_coop_hd_m3l1a.pk3 are never edited; this list is the engine keeping that rule.

Header-only. Needs q_shared.h and, since Phase S, dlighttype_t (renderercommon/new/tr_types_new.h) declared first -
both includers (cgame/cg_view.c, renderergl2/tr_local.h) already have it. Neither module needs a CMake change for it.
===========================================================================
*/
#ifndef HZM_LIGHT_RESTORE_H
#define HZM_LIGHT_RESTORE_H

#define HZM_LIGHTGLOW_AUTO      1   // r_hzmLightGlow      -1 means this. ON: user passed Test A on m1l1 2026-09-26
                                    //                     ("when I hit M those headlights look worlds better").
#define HZM_LIGHTGLOWNEAR_AUTO  1   // r_hzmLightGlowNear  -1 means this. User decision: the near-shrink starts ON
                                    //                     (it only acts while lightglow itself is on).
#define HZM_RGBGENDOT_AUTO      1   // r_hzmRgbGenDot      -1 means this. ON with HZM_LIGHTGLOW_AUTO (Test A).
                                    //                     Also gates searchlights S1 + S2 (user 2026-09-26).
#define HZM_HEADLIGHTS_AUTO     1   // cg_hzmHeadlights    -1 means this. ON again 2026-09-27: user approved the bug-2999/3008 tuning ("Switch on as tuned") after the OLD|TODAY|NEW screenshot sheet. (Was 0 on 2026-09-26, bug-2990: parts too small/dim to see + flares self-occluded by the truck body.)
// Phase S [2026-09-26]. vet R20: these two and HZM_HEADLIGHTS_AUTO flip TOGETHER, here, once the user's Test S passes,
// and BOTH DLLs are rebuilt and shipped as a pair - a partial flip is a mismatched pair by another name.
#define HZM_SPOT_AUTO           1   // r_hzmSpot           -1 means this. The spot cone (world + soldiers + props). ON (Test S).
#define HZM_FLARES_AUTO         1   // r_hzmFlares         -1 means this. The lamp lens flares. ON (Test S).
// S3a [2026-09-26] cgame ONLY (cg_view.c CG_HL_BeamStop): the fog beam is shortened so it fades out on the first wall,
// brush entity or vehicle its axis meets. Flip needs a cgame rebuild only - the renderer never reads it.
#define HZM_BEAMSTOP_AUTO       0   // cg_hzmHeadlightsBeamStop -1 means this. OFF until the user's Test W passes.

// -1 (any negative) = auto, 0 = off, anything else = on
static ID_INLINE qboolean HZM_ResolveAutoSwitch( int value, int autoValue ) {
	if ( value < 0 ) {
		return autoValue ? qtrue : qfalse;
	}
	return value ? qtrue : qfalse;
}

static ID_INLINE qboolean HZM_LightRestoreMapProtected( const char *mapName ) {
	static const char *const protectedMaps[] = {
		"m3l1a",	// Omaha Beach - the landing
		"m3l1b",	// Omaha Beach - the bluffs
	};
	const char	*base;
	size_t		len;
	int			i;

	if ( !mapName || !mapName[0] ) {
		return qfalse;
	}

	// strip the path ...
	base = mapName + strlen( mapName );
	while ( base > mapName && base[-1] != '/' && base[-1] != '\\' ) {
		base--;
	}
	// ... and the extension
	len = strcspn( base, "." );

	for ( i = 0; i < (int)( sizeof( protectedMaps ) / sizeof( protectedMaps[0] ) ); i++ ) {
		if ( strlen( protectedMaps[i] ) == len && !Q_stricmpn( base, protectedMaps[i], (int)len ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
===========================================================================
3) THE SPOT / FLARE CARRIER (Phase S, vet_phaseS.md F1-F4). cgame reaches the renderer through the one call the exe
   already forwards untouched: cgi.R_AddLightToScene(org, intensity, r, g, b, type) -> re.AddLightToScene
   (client/cl_cgame.cpp) -> RE_AddLightToScene2 -> R_AddDynamicLightTyped (renderergl2/tr_scene.c). So NO exe change
   and NO refexport/cgame-import append - an append would have to ship exe + cgame + gl2 as a set, and a mismatch
   reads past the shorter table. This degrades instead: a pair that does not agree falls back to Phase H's omni pool.

   The type is an int. Bits 0-4 are dlighttype_t (renderercommon/new/tr_types_new.h). Phase S owns:
     bit  5     HZM_DLIGHT_SPOT      this light is a cone. The renderer DROPS it unless its carrier arrives in the
                                     same scene (spot-or-nothing: an omni light at the lamp lights behind the truck)
     bit  6     HZM_DLIGHT_FOGGED    the world pass fogs it the way it fogs an additive stage (toward black)
     bit  7     HZM_DLIGHT_CARRIER   NOT a light: the parameters of the spot / flare its tag names. Always sent with
                                     intensity < 0, which gl1 (tr_scene.c) and every pre-Phase-S gl2 drop before it
                                     can take one of the 32 slots
     bits 8-9   kind                 0 = spot parameters, 1 = a lens flare
     bits 10-23 tag / id             spot: 1..N, the light it belongs to (0 = no tag). flare: HZM_FlareIdPack()
     bit  24    flare class          0 = headlight, 1 = searchlight
     bits 25-31                      must stay 0 (bit 31 is the sign bit)
   A SPOT is two adds, light then carrier, with the same tag n:
       light    (apex, range,  r, g, b,                       additive | ... | HZM_DLIGHT_SPOT | HZM_DlightTagBits(n))
       carrier  (axis,  -1,    cosInner, cosOuter, (float)ownerEntity, HZM_DLIGHT_CARRIER | HZM_DLIGHT_KIND_SPOT |
                                                                        HZM_DlightTagBits(n))
   A FLARE is one add:
       carrier  (lamp, -brightness, facing.x, facing.y, facing.z, HZM_DlightFlareBits(id, class))

   HANDSHAKE (vet F3): renderer_opengl2 registers r_hzmSpotProtocol CVAR_ROM, FORCES it to HZM_SPOT_PROTOCOL in
   R_Register (every R_Init) and back to 0 in RE_Shutdown. cgame sends spots / flares only while it equals cgame's OWN
   HZM_SPOT_PROTOCOL and cl_renderer names a gl2 build. gl1 never sets it; RE_Shutdown clears it when gl2 goes away.

   TIKI `dlight` passes an author's raw integer into the type (cgame/cg_commands.cpp DynamicLight): cgame masks it to
   HZM_DLIGHT_RETAIL_BITS, and the renderer honours bits 5-24 only with a non-zero tag and (carriers) intensity < 0.
===========================================================================
*/
#define HZM_SPOT_PROTOCOL           1                  // bump when the carrier layout changes; both DLLs ship together

#define HZM_DLIGHT_RETAIL_BITS      ( 1 | 2 | 4 )      // lensflare | viewlensflare | additive - all a TIKI may set
#define HZM_DLIGHT_SPOT             ( 1 << 5 )
#define HZM_DLIGHT_FOGGED           ( 1 << 6 )
#define HZM_DLIGHT_CARRIER          ( 1 << 7 )
#define HZM_DLIGHT_KIND_SHIFT       8
#define HZM_DLIGHT_KIND_MASK        ( 3 << HZM_DLIGHT_KIND_SHIFT )
#define HZM_DLIGHT_KIND_SPOT        ( 0 << HZM_DLIGHT_KIND_SHIFT )
#define HZM_DLIGHT_KIND_FLARE       ( 1 << HZM_DLIGHT_KIND_SHIFT )
#define HZM_DLIGHT_TAG_SHIFT        10
#define HZM_DLIGHT_TAG_BITS         14
#define HZM_DLIGHT_TAG_MAX          ( ( 1 << HZM_DLIGHT_TAG_BITS ) - 1 )
#define HZM_DLIGHT_TAG_MASK         ( HZM_DLIGHT_TAG_MAX << HZM_DLIGHT_TAG_SHIFT )
#define HZM_DLIGHT_CLASS_SHIFT      24
#define HZM_DLIGHT_CLASS_MASK       ( 1 << HZM_DLIGHT_CLASS_SHIFT )
#define HZM_DLIGHT_RESERVED_MASK    ( ~( ( 1 << 25 ) - 1 ) )

#define HZM_FLARE_MAX               32                 // flares per frame, both sides (renderer list, cgame cap)
#define HZM_FLARE_CLASS_HEADLIGHT   0
#define HZM_FLARE_CLASS_SEARCHLIGHT 1

// vet F2: a flare id is (entnum << 2 | lamp). It must fit the tag field or it overwrites the class bit / kind bits.
#if GENTITYNUM_BITS + 2 > HZM_DLIGHT_TAG_BITS
#error "hzm_light_restore.h: a flare id (entnum << 2 | lamp) no longer fits the 14-bit carrier tag (bits 10-23) - GENTITYNUM_BITS grew; widen the tag and move the class bit (vet_phaseS F2)"
#endif
#if HZM_DLIGHT_CLASS_SHIFT < HZM_DLIGHT_TAG_SHIFT + HZM_DLIGHT_TAG_BITS
#error "hzm_light_restore.h: the flare class bit overlaps the carrier tag"
#endif
// every Phase S bit is clear of dlighttype_t and of the reserved top bits
typedef char hzm_dlight_bits_are_disjoint_t[
	( ( ( HZM_DLIGHT_SPOT | HZM_DLIGHT_FOGGED | HZM_DLIGHT_CARRIER | HZM_DLIGHT_KIND_MASK | HZM_DLIGHT_TAG_MASK
	      | HZM_DLIGHT_CLASS_MASK ) & ( lensflare | viewlensflare | additive | hzm_dlight_noshadow | hzm_dlight_edgefade ) ) == 0
	  && ( ( HZM_DLIGHT_TAG_MASK | HZM_DLIGHT_CLASS_MASK ) & HZM_DLIGHT_RESERVED_MASK ) == 0
	  && HZM_DLIGHT_RETAIL_BITS == ( lensflare | viewlensflare | additive ) ) ? 1 : -1 ];

static ID_INLINE int HZM_DlightTagBits( int tag ) {
	return ( tag & HZM_DLIGHT_TAG_MAX ) << HZM_DLIGHT_TAG_SHIFT;
}

static ID_INLINE int HZM_DlightTagOf( int type ) {
	return ( type & HZM_DLIGHT_TAG_MASK ) >> HZM_DLIGHT_TAG_SHIFT;
}

static ID_INLINE int HZM_FlareIdPack( int entnum, int lamp ) {
	return ( ( entnum & ( MAX_GENTITIES - 1 ) ) << 2 ) | ( lamp & 3 );
}

static ID_INLINE int HZM_DlightFlareBits( int id, int flareClass ) {
	return HZM_DLIGHT_CARRIER | HZM_DLIGHT_KIND_FLARE | HZM_DlightTagBits( id )
	     | ( flareClass ? HZM_DLIGHT_CLASS_MASK : 0 );
}

static ID_INLINE int HZM_DlightFlareClassOf( int type ) {
	return ( type & HZM_DLIGHT_CLASS_MASK ) ? HZM_FLARE_CLASS_SEARCHLIGHT : HZM_FLARE_CLASS_HEADLIGHT;
}

#endif // HZM_LIGHT_RESTORE_H
