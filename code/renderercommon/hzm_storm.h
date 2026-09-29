/*
===========================================================================
HZM coop [2026-09-28] STORM DARKNESS - the shared switch, the mapping and the cgame -> renderer state contract.
docs/proposals/storm_darkness_2026-09-28/plan.md (research.md, vet.md).

ONE header, read by cgame.dll and renderer_opengl2.dll, so the two halves can never disagree about the limits.

  server  coop_mod/stormlight.scr: per rain storm a kind (light / moderate / dark) x the rain pattern (+ passing cells)
          = the storm darkness D, sent as KEYFRAMES to every player:
            stufftext "set coop_storm 1 <tok> <seq> <from x1000> <to x1000> <durMs> <elapsedMs>"
  cgame   cg_hzmstorm.c: evaluates the same smootherstep curve every frame (every client renders the same D), eases
          any correction, maps D to the terms below, multiplies the refdef fog colour and publishes r_hzmStormNow
  gl2     tr_postprocess.c R_HZM_Storm(): the tone-grade storm layer (after the night and map layers; it never switches
          the tone path); tr_scene.c the sun: its runtime shadow/visibility term fades (lightall SHADOWMAP_MODULATE reads
          u_PrimaryLightAmbient.b / .r as the sun's visibility - vet F1: taking the ambient toward 1 instead would have
          BRIGHTENED every shadow); tr_sky.c + tr_shade.c the skybox and its cloud stages take the fog tint; the wet film
          and the water pass lose the sun glint and reflect the darker sky
  gl1     NOTHING (vet F5): gl1 does not fog the sky, so any fog darkening draws the far terrain darker than the sky above
          it - a horizon band. gl1 players see today's look.

1) THE AUTO DEFAULT. cg_hzmStorm is registered "-1" = auto, flags 0 (never saved; a cg_ name, so a server cannot set it -
   the SEC2 filter). The client half is READY (1): what stages the feature is the SERVER switch coop_stormLight, whose
   empty value is off until the user has seen the GIFs (coop_mod/stormlight.scr coop_storm_on). 0 = the player opts out.
2) OMAHA. cgame refuses the five-map list (HZM_WaterWetOmahaMap, hzm_waterwet.h); the renderer refuses it again
   (tr.hzmOmahaWorld); the server never publishes there.
3) THE STATE STRING (cgame writes, renderer reads; "" = identity):
     r_hzmStormNow "<expo> <cont> <sat> <temp> <sunFade> <fogR> <fogG> <fogB> <gain>"
   All multipliers except temp (additive) and sunFade (0..1, the share of the sun's runtime term removed). gain is a
   DISPLAY-referred multiplier applied after the ACES curve (tonemap_hzm_fp via u_HzmParams.x = 1 - gain): an exposure
   cut alone crushes the ACES toe (m4l2 capture: tree shade 12.6 -> 6.2 LSB, rock 3.7 -> 1.8 at x0.86), while a display
   gain keeps shadow detail in proportion. The renderer clamps every field to the HZM_STORM_* limits below.
4) THE NO-SUN COMPENSATION. Where the runtime sun term is not live (r_sunShadows 0, no sun on the map, r_sunlightMode
   not 1), removing the sun darkens nothing, so the renderer multiplies the exposure by 1 - HZM_STORM_NOSUN_K x sunFade
   instead: every player gets about the same storm whatever their shadow settings.
===========================================================================
*/
#ifndef HZM_STORM_H
#define HZM_STORM_H

#define HZM_STORM_AUTO          1   // cg_hzmStorm -1 means this (the server switch stages the feature, see 1)
#define HZM_STORM_VERSION       1   // first token of coop_storm
#define HZM_STORM_NOSUN_K       0.25f

// renderer-side clamps (a wider band than the mapping ever produces)
#define HZM_STORM_EXPO_MIN      0.50f
#define HZM_STORM_CONT_MIN      0.90f
#define HZM_STORM_CONT_MAX      1.10f
#define HZM_STORM_SAT_MIN       0.60f
#define HZM_STORM_TEMP_MAX      0.06f
#define HZM_STORM_FOG_MIN       0.60f
#define HZM_STORM_FOG_MAX       1.10f
#define HZM_STORM_GAIN_MIN      0.60f

typedef struct {
	float	expo, cont, sat, temp;	// grade layer: identity 1 1 1 0
	float	sunFade;				// 0 = the map's sun, 1 = its runtime term gone (shadow edges, lit side, specular)
	float	fog[3];					// fog colour + skybox multiplier: identity 1 1 1
	float	gain;					// display-referred multiplier after the tone curve: identity 1
	int		active;					// any field away from identity
} hzmStormState_t;

// the look, at full darkness (x = 1), for a day map and for a night map; interpolated by "day"
typedef struct {
	float	expoDay, expoNight;		// exposure multiplier
	float	satDay, satNight;		// saturation multiplier
	float	tempDay, tempNight;		// additive (negative = cooler)
	float	fadeDay, fadeNight;		// sun term removed at x >= HZM_STORM_FADE_FULL
	float	fogDay, fogNight;		// fog colour + skybox multiplier
	float	gainDay, gainNight;		// display-referred gain (keeps the ACES toe)
	float	fogPortal;				// share of the fog change kept on a sky-portal view (its portal sky is not fogged).
									// 1.0 = none: cg.sky_portal (CS_SKYINFO) read 1 on m3l2, which has no portal view,
									// and halved its sky darkening in the first capture (ingame/INGAME.md)
} hzmStormTune_t;

#define HZM_STORM_FADE_FULL     0.60f
// plan.md section 2 (tuned in-engine, ingame/INGAME.md)
#define HZM_STORM_TUNE_DEFAULT  { 0.95f, 0.97f, 0.80f, 0.90f, -0.025f, -0.01f, 0.40f, 0.15f, 0.58f, 0.85f, 0.86f, 0.92f, 1.00f }

static ID_INLINE float HZM_StormLerp( float a, float b, float t ) {
	return a + ( b - a ) * t;
}

static ID_INLINE float HZM_StormSat01( float x ) {
	return x < 0.0f ? 0.0f : ( x > 1.0f ? 1.0f : x );
}

static ID_INLINE float HZM_StormSmoothstep( float e0, float e1, float x ) {
	float t = HZM_StormSat01( ( x - e0 ) / ( e1 - e0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

// the ease of every keyframe (0..1 -> 0..1, zero velocity and acceleration at both ends)
static ID_INLINE float HZM_StormSmootherstep( float t ) {
	t = HZM_StormSat01( t );
	return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f );
}

// how "day" a map is, from the luminance of its RAW fog colour (the headlight test's own measure): m3l2 .63 -> 1,
// m5l1 .39 -> 1, e2l3 .34 -> 0.95, m4l1/m4l2 .20 -> 0 (the headlight test calls them dark too), t1l1 .02 -> 0
static ID_INLINE float HZM_StormDayness( float fogLum ) {
	return HZM_StormSmoothstep( 0.22f, 0.36f, fogLum );
}

// THE MAPPING. x = storm darkness 0..1, day = HZM_StormDayness, gl2 = the renderer that draws it (gl1: identity),
// portal = the view has a sky portal (its portal sky is not fogged: keep the fog change small, vet F16).
static ID_INLINE void HZM_StormMap( float x, float day, int gl2, int portal, const hzmStormTune_t *t,
                                    hzmStormState_t *s ) {
	float k;

	x   = HZM_StormSat01( x );
	day = HZM_StormSat01( day );

	s->expo = s->cont = s->sat = 1.0f;
	s->temp = s->sunFade = 0.0f;
	s->fog[0] = s->fog[1] = s->fog[2] = 1.0f;
	s->gain = 1.0f;
	s->active = 0;
	if ( !gl2 || x <= 0.0005f ) {
		return;
	}
	s->expo    = 1.0f - x * ( 1.0f - HZM_StormLerp( t->expoNight, t->expoDay, day ) );
	s->sat     = 1.0f - x * ( 1.0f - HZM_StormLerp( t->satNight, t->satDay, day ) );
	s->gain    = 1.0f - x * ( 1.0f - HZM_StormLerp( t->gainNight, t->gainDay, day ) );
	s->temp    = x * HZM_StormLerp( t->tempNight, t->tempDay, day );
	s->sunFade = HZM_StormSat01( x / HZM_STORM_FADE_FULL ) * HZM_StormLerp( t->fadeNight, t->fadeDay, day );
	k = x * ( 1.0f - HZM_StormLerp( t->fogNight, t->fogDay, day ) );
	if ( portal ) {
		k *= t->fogPortal;
	}
	s->fog[0] = ( 1.0f - k ) * ( 1.0f - 0.03f * x * day );		// slate: a little less red, a little more blue
	s->fog[1] = ( 1.0f - k );
	s->fog[2] = ( 1.0f - k ) * ( 1.0f + 0.05f * x * day );
	s->active = 1;
}

// headlight pools + flares on a DAY map in a dark storm only (a night map is already "dark": dayScale 1)
static ID_INLINE float HZM_StormHeadlightDay( float x, float day ) {
	return HZM_StormSmoothstep( 0.70f, 1.0f, x ) * HZM_StormSat01( day );
}

// cgame (cg_hzmstorm.c)
void	CG_HzmStorm_Init( void );				// CG_Init, after the gamestate: registers, restores a vid_restart park
void	CG_HzmStorm_Shutdown( void );			// CG_Shutdown: parks the live keyframe, clears the renderer state
void	CG_HzmStorm_Frame( void );				// CG_DrawActiveFrame, once per frame
void	CG_HzmStorm_FogColor( float *rgb );		// CG_SetupFog: the refdef copy only
float	CG_HzmStorm_SunFade( void );			// CG_EntityShadow: the contact decal takes over from the fading sun shadow
float	CG_HzmStorm_HeadlightDay( void );		// CG_HL_UpdateFrame: dayScale floor

// renderer_opengl2 (tr_postprocess.c): r_hzmStormNow parsed once per change, clamped; identity when empty or on Omaha
const hzmStormState_t *R_HZM_Storm( void );
void	R_HZM_StormSetSunLive( int live );	// RE_RenderScene (world views): the runtime sun term runs this frame
float	R_HZM_StormExposure( int withGain );	// the exposure multiplier incl. the no-sun compensation (4); withGain folds the
											// display gain in (paths with no display-referred stage: Hable, no tonemap)

#endif
