/*
===========================================================================
HZM coop [2026-09-26] PHASE S - spot cones and lamp flares for renderer_opengl2: the RENDERER GLUE.

  docs/proposals/headlights_2026-09-25/plan_phaseS.md, vet_phaseS.md (authoritative). Pure maths: tr_hzm_spot.c.
  Protocol, bit layout, AUTO defines, Omaha list: renderercommon/hzm_light_restore.h.

SWITCHES (all flags 0 - never saved, TRAPS T7; read per frame / per draw, so they toggle live; "-1" = auto):
  r_hzmSpot           -1 auto (HZM_SPOT_AUTO)  - honour spot lights + their carriers (world, soldiers, props, grid)
  r_hzmFlares         -1 auto (HZM_FLARES_AUTO) - draw lamp flares (also needs r_flares, GL_SAMPLES_PASSED queries)
  r_hzmSpotDebug      0; 1 = a ^~^~^ HZMSPOT line a second; 2 = also tint every spot-lit pixel MAGENTA, which is how
                      a uniform leak onto a character or rgbGen lightingDiffuse draw would show (plan R1)
  r_hzmSpotEntScale   1   soldiers / props / statics: one knob on the one law (a surface facing the lamp inside the
                          pool gets what the ground at its feet gets)
  r_hzmFlareSize 0.05 / r_hzmFlareIntensity 0.6 (headlights), r_hzmFlareSize1 0.04 / r_hzmFlareIntensity1 0.25
  (searchlights), r_hzmFlareNear 150 u, r_hzmFlareFade 0.12 s, r_hzmFlareBudget 0.0075 (sum of intensity x screen area)
  [2026-09-26, bug-2999] headlight intensity 0.25 -> 0.6, HZM_FLARE_PEAK 0.25 -> 0.8, budget 0.003 -> 0.0075 (x2.5 with
  the intensity, or the frame budget scales the brighter flares straight back): at 0.25 / peak 0.25 the lamp flares
  drew (r_speeds 6: 2 renders a frame) but stayed a sub-bloom 2-3 px speck - invisible on the co_lobby8 docks.
  r_hzmSpotProtocol   CVAR_ROM handshake, FORCED to HZM_SPOT_PROTOCOL here at every R_Init and to 0 at RE_Shutdown
                      (vet F3: cgame's Cvar_Get never changes an existing cvar, so only a forced set is reliable).
Omaha (m3l1a / m3l1b): both switches resolve OFF whatever they say, so spot lights are dropped, every carrier is
discarded, the flare pass returns before touching GL, and the only difference left is a compiled uniform nobody sets.
===========================================================================
*/

#include "tr_local.h"

cvar_t	*r_hzmSpot;
cvar_t	*r_hzmFlares;
cvar_t	*r_hzmSpotProtocol;
cvar_t	*r_hzmSpotDebug;
cvar_t	*r_hzmSpotEntScale;
cvar_t	*r_hzmFlareSize;
cvar_t	*r_hzmFlareIntensity;
cvar_t	*r_hzmFlareSize1;
cvar_t	*r_hzmFlareIntensity1;
cvar_t	*r_hzmFlareNear;
cvar_t	*r_hzmFlareFade;
cvar_t	*r_hzmFlareBudget;

// tr_scene.c's slot counters (plain globals there)
extern int	r_numdlights;
extern int	r_firstSceneDlight;

// this frame's flare requests, scene-relative exactly like backEndData->dlights (read by the backend before
// R_InitNextFrame resets them)
static hzmFlare_t	s_flares[HZM_FLARE_MAX];
static int			s_numFlares;
static int			s_firstSceneFlare;

// drop counters for the ^~^~^ HZMSPOT line (cumulative since the DLL loaded)
static struct {
	hzmSpotStats_t st;	// spotOff / orphan / discarded / flareFull - R_HZM_SpotIntakeCore (tr_hzm_spot.c)
	int	noCarrier;		// a spot light whose carrier never came: removed at RE_BeginScene
	int	spots;			// ready spots handed to a scene
	int	flares;			// flares handed to a scene
	int	lastSum;
	int	lastPrint;
	int	lastDebug;
	qboolean shaderNoted;
} s_hzm;

/*
=============
R_HZM_SpotRegister - from R_Register (every R_Init)
=============
*/
void R_HZM_SpotRegister( void ) {
	r_hzmSpot            = ri.Cvar_Get( "r_hzmSpot",            "-1",    0 );
	r_hzmFlares          = ri.Cvar_Get( "r_hzmFlares",          "-1",    0 );
	r_hzmSpotDebug       = ri.Cvar_Get( "r_hzmSpotDebug",       "0",     0 );
	r_hzmSpotEntScale    = ri.Cvar_Get( "r_hzmSpotEntScale",    "1",     0 );
	r_hzmFlareSize       = ri.Cvar_Get( "r_hzmFlareSize",       "0.05",  0 );
	r_hzmFlareIntensity  = ri.Cvar_Get( "r_hzmFlareIntensity",  "0.6",   0 );
	r_hzmFlareSize1      = ri.Cvar_Get( "r_hzmFlareSize1",      "0.04",  0 );
	r_hzmFlareIntensity1 = ri.Cvar_Get( "r_hzmFlareIntensity1", "0.25",  0 );
	r_hzmFlareNear       = ri.Cvar_Get( "r_hzmFlareNear",       "150",   0 );
	r_hzmFlareFade       = ri.Cvar_Get( "r_hzmFlareFade",       "0.12",  0 );
	r_hzmFlareBudget     = ri.Cvar_Get( "r_hzmFlareBudget",     "0.0075", 0 );
	// the handshake (vet F3): ROM so nobody sets it by hand, FORCED so a stale value from a previous renderer can
	// never survive (Cvar_Set is Cvar_Set2(force) - qcommon/cvar.c)
	r_hzmSpotProtocol    = ri.Cvar_Get( "r_hzmSpotProtocol",    "0",     CVAR_ROM );
	ri.Cvar_Set( "r_hzmSpotProtocol", va( "%d", HZM_SPOT_PROTOCOL ) );
}

/*
=============
R_HZM_SpotShutdown - from RE_Shutdown: this gl2 is going away (vid_restart, a renderer switch, quit)
=============
*/
void R_HZM_SpotShutdown( void ) {
	ri.Cvar_Set( "r_hzmSpotProtocol", "0" );
}

qboolean R_HZM_SpotsOn( void ) {
	if ( !HZM_ResolveAutoSwitch( r_hzmSpot ? r_hzmSpot->integer : -1, HZM_SPOT_AUTO ) ) {
		return qfalse;
	}
	return R_HZM_LightRestoreProtectedWorld() ? qfalse : qtrue;
}

qboolean R_HZM_FlaresOn( void ) {
	if ( !HZM_ResolveAutoSwitch( r_hzmFlares ? r_hzmFlares->integer : -1, HZM_FLARES_AUTO ) ) {
		return qfalse;
	}
	if ( !r_flares || !r_flares->integer ) {
		return qfalse;
	}
	// the ratio needs a SAMPLE COUNT; GLES only offers a boolean (vet F16)
	if ( !glRefConfig.occlusionQuery || glRefConfig.occlusionQueryTarget != GL_SAMPLES_PASSED ) {
		return qfalse;
	}
	return R_HZM_LightRestoreProtectedWorld() ? qfalse : qtrue;
}

// soldiers / props / statics: the colour a surface facing the lamp receives per unit of (attenuation x cone), in
// the 0..255 vertex-colour space. The world pass adds dl->color x albedo x att x N.L with NO overbright (ForwardDlight
// uploads dl->color as u_DirectedLight), and a vertex colour of 255 is x1 in the shader (CGEN_LIGHTING_SPHERICAL
// passes it through), so 255 x the knob is "as bright as the ground at his feet".
float R_HZM_SpotEntityK( void ) {
	float s = r_hzmSpotEntScale ? r_hzmSpotEntScale->value : 1.0f;

	if ( !( s > 0.0f ) ) {
		return 0.0f;
	}
	return 255.0f * s;
}

/*
=============
R_HZM_SpotIntake

The first thing R_AddDynamicLightTyped does. Returns qtrue when the add is CONSUMED - a carrier (used or discarded:
it never takes one of the 32 slots) or a spot light while spots are off (spot-or-nothing). *verdict is what
R_HZM_SpotBeginLight needs for the slot the caller fills otherwise. Every existing light pays one bit test.
=============
*/
qboolean R_HZM_SpotIntake( const vec3_t org, float intensity, float r, float g, float b, int type, hzmAdd_t *verdict ) {
	*verdict = HZM_ADD_LIGHT;
	if ( !( type & ( HZM_DLIGHT_SPOT | HZM_DLIGHT_CARRIER ) ) ) {
		return qfalse;
	}
	return R_HZM_SpotIntakeCore( backEndData->dlights, r_firstSceneDlight, r_numdlights, s_flares, &s_numFlares,
	                             HZM_FLARE_MAX, org, intensity, r, g, b, type, R_HZM_SpotsOn(), R_HZM_FlaresOn(),
	                             &s_hzm.st, verdict );
}

// scene bookkeeping for the flare list, mirroring the dlight counters in tr_scene.c
void R_HZM_SpotNextFrame( void ) {
	s_numFlares = 0;
	s_firstSceneFlare = 0;
}

void R_HZM_SpotClearScene( void ) {
	s_firstSceneFlare = s_numFlares;
}

void R_HZM_SpotEndScene( void ) {
	s_firstSceneFlare = s_numFlares;
}

static void R_HZM_SpotReport( int numSpots ) {
	int now = ri.Milliseconds();
	int sum = s_hzm.st.spotOff + s_hzm.st.orphan + s_hzm.noCarrier + s_hzm.st.discarded + s_hzm.st.flareFull;

	// drops: one line when the counts moved, at most every 5 s - silent in normal play (none of these should happen)
	if ( sum != s_hzm.lastSum && ( now - s_hzm.lastPrint >= 5000 || now < s_hzm.lastPrint ) ) {
		s_hzm.lastSum = sum;
		s_hzm.lastPrint = now;
		ri.Printf( PRINT_ALL, "^~^~^ HZMSPOT drops: spotOff=%d orphanCarrier=%d noCarrier=%d discarded=%d flareFull=%d "
		           "(spots=%d flares=%d so far; r_hzmSpot %s, r_hzmFlares %s)\n", s_hzm.st.spotOff, s_hzm.st.orphan,
		           s_hzm.noCarrier, s_hzm.st.discarded, s_hzm.st.flareFull, s_hzm.spots, s_hzm.flares,
		           r_hzmSpot ? r_hzmSpot->string : "?", r_hzmFlares ? r_hzmFlares->string : "?" );
	}
	if ( r_hzmSpotDebug && r_hzmSpotDebug->integer && ( now - s_hzm.lastDebug >= 1000 || now < s_hzm.lastDebug ) ) {
		s_hzm.lastDebug = now;
		ri.Printf( PRINT_ALL, "^~^~^ HZMSPOT scene: dlights=%d spots=%d flares=%d spotsOn=%d flaresOn=%d protocol=%d\n",
		           r_numdlights - r_firstSceneDlight, numSpots, s_numFlares - s_firstSceneFlare,
		           R_HZM_SpotsOn() ? 1 : 0, R_HZM_FlaresOn() ? 1 : 0, HZM_SPOT_PROTOCOL );
	}
}

/*
=============
R_HZM_SpotFinishScene

From RE_BeginScene, BEFORE tr.refdef.num_dlights is taken: spot-or-nothing (a spot whose carrier never came is
removed - an omni light at the lamp would light behind the truck), then this scene's flare list.
=============
*/
void R_HZM_SpotFinishScene( void ) {
	int i, spots = 0;

	r_numdlights = R_HZM_SpotCompact( backEndData->dlights, r_firstSceneDlight, r_numdlights, &s_hzm.noCarrier );
	for ( i = r_firstSceneDlight; i < r_numdlights; i++ ) {
		if ( backEndData->dlights[i].hzmSpot == HZM_SPOT_READY ) {
			spots++;
		}
	}
	s_hzm.spots += spots;
	tr.refdef.numHzmFlares = s_numFlares - s_firstSceneFlare;
	tr.refdef.hzmFlares = &s_flares[s_firstSceneFlare];
	s_hzm.flares += tr.refdef.numHzmFlares;
	R_HZM_SpotReport( spots );
}

/*
=============
uniform uploads (tr_shade.c)

ForwardDlight uploads for EVERY light (zero for an omni one) and RB_IterateStagesGeneric zeroes it for every lightall
draw, because the USE_LIGHT_VECTOR programs also serve character lighting and rgbGen lightingDiffuse (plan R1, vet F8):
the last headlight must never leak its cone onto a soldier. GLSL_SetUniformVec4 skips an unchanged value, so the
zeroing costs a compare.
=============
*/
static qboolean R_HZM_SpotDebugTint( void ) {
	return ( r_hzmSpotDebug && r_hzmSpotDebug->integer >= 2 ) ? qtrue : qfalse;
}

void RB_HZM_SpotForwardUniform( shaderProgram_t *sp, const dlight_t *dl ) {
	vec4_t v;

	R_HZM_SpotUniformVec( dl, dl->hzmAxis, R_HZM_SpotDebugTint(), v );	// world space, like u_LightOrigin
	GLSL_SetUniformVec4( sp, UNIFORM_HZMLIGHTSPOT, v );
}

void RB_HZM_SpotProjectUniform( shaderProgram_t *sp, const dlight_t *dl ) {
	vec4_t v;

	R_HZM_SpotUniformVec( dl, dl->hzmAxisLocal, R_HZM_SpotDebugTint(), v );	// the frame of u_DlightInfo
	GLSL_SetUniformVec4( sp, UNIFORM_HZMLIGHTSPOT, v );
}

void RB_HZM_SpotZeroUniform( shaderProgram_t *sp ) {
	vec4_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };

	GLSL_SetUniformVec4( sp, UNIFORM_HZMLIGHTSPOT, zero );
}

/*
=============
RB_HZM_SpotSphereLight

Soldiers, vehicles, the view weapon: a spot reaches a TIKI only through the light sphere (tr_sphere_shade.cpp). The
cone is judged ONCE at the sphere (attenuation x the sphere-dilated cone) and the result is emitted as a DIRECTIONAL
light toward the lamp: per vertex that is colour x N.L with no 1/d term, so it cannot saturate at the bumper the way a
pre-scaled point light would (vet F13), and under r_charLighting 1 the fold (tr_backend.c) takes a directional light
as is. The vehicle carrying the lamp is skipped. False = contributes nothing (do not spend a MAX_REAL_LIGHTS slot).
=============
*/
qboolean RB_HZM_SpotSphereLight( const dlight_t *dl, const sphereor_t *sph, const trRefEntity_t *ent,
                                 reallightinfo_t *out ) {
	vec3_t	dir, local;
	float	f, k, dist;

	if ( dl->hzmOwner >= 0 && ent && ent->e.entityNumber == dl->hzmOwner ) {
		return qfalse;
	}
	f = R_HZM_DlightFactorAt( dl, sph->worldOrigin, sph->radius );
	k = R_HZM_SpotEntityK() * f;
	if ( !( k > 0.0f ) ) {
		return qfalse;
	}
	VectorSubtract( dl->origin, sph->worldOrigin, dir );
	dist = VectorNormalize( dir );
	if ( !( dist > 0.0f ) ) {
		return qfalse;
	}
	MatrixTransformVectorRight( ent->e.axis, dir, local );
	if ( VectorNormalize( local ) <= 0.0f ) {
		return qfalse;
	}
	VectorScale( dl->color, k, out->color );
	VectorCopy( local, out->vDirection );
	VectorSubtract( dl->origin, ent->e.origin, dir );
	MatrixTransformVectorRight( ent->e.axis, dir, out->vOrigin );
	out->eType = LIGHT_DIRECTIONAL;
	out->fDist = dist;
	out->fIntensity = out->color[0] * 0.299f + out->color[1] * 0.587f + out->color[2] * 0.114f;
	out->fSpotSlope = 0.0f;
	out->fSpotConst = 0.0f;
	out->fSpotScale = 0.0f;
	return qtrue;
}

/*
===========================================================================
S2 LAMP FLARES (plan section 5, vet F16-F20)

OCCLUSION: per flare a small world-space billboard - at the lamp, pulled HZM_FLARE_PULL (24 u) toward the eye so the lamp's own housing
cannot hide it (plan R19) - drawn twice into the CURRENTLY BOUND scene FBO after the opaque list, inside
GL_SAMPLES_PASSED queries: (a) depth-tested, (b) depth test off. Visible fraction = a / b. Same colour-neutral blend
(ZERO, ONE), no depth write, colour alpha 1, no alpha-to-coverage. The ratio is sample-count agnostic, so it is the
same at MSAA 0/2/4/8 (legacy or P2) and at any r_renderScale - no resolve, no glReadPixels, no sync (vet F18).
Results are read ONE+ frames later and only when GL_QUERY_RESULT_AVAILABLE says so; a 3-deep ring per flare, and a
slot is never restarted while its result is unread (vet F19). The names live in tr and are created / deleted in
R_InitQueries / R_ShutDownQueries unconditionally - never behind a cvar that changes live (vet F17).

DRAWING: in the scene before the tonemap, RB_RenderFlare's ortho recipe with its own shader hzmLampFlare
(scripts/coop_headlights.shader, textures/coop_fx/lampflare.tga). Intensity = r_hzmFlareIntensity x facing^lobe
(4 headlight, 16 searchlight) x brightness x visibility x presence x global-fog transmittance x a near fade; each
flare capped at HZM_FLARE_PEAK additive and 0.3% of the screen, and the frame's sum(intensity x area) capped by
r_hzmFlareBudget - no white-outs (bloom bright-pass, bug-1156).

MAIN VIEW ONLY (vet F20): not portals, sky portals, cubemap captures, HUD / RDF_NOWORLDMODEL scenes. With nothing to
draw it returns before ANY GL call.
===========================================================================
*/
// [2026-09-26, bug-2999] 8 -> 24: measured on the co_lobby8 Opel trucks, the lamp tag sits deeper than 8 u behind the
// lamp glass, so the truck's own body failed every occlusion query (visibility ~0: no flare at any intensity, while
// r_drawentities 0 showed all six). lightglow clears the housing because it pulls by its radius (11-34 u).
#define HZM_FLARE_PULL			24.0f	// u toward the eye (plan R19)
#define HZM_FLARE_MIN_RADIUS	4.0f	// u; also never under 2 px
#define HZM_FLARE_PEAK			0.8f	// max additive per flare (bug-2999: was 0.25 - a sub-bloom speck)
#define HZM_FLARE_MAX_AREA		0.003f	// max fraction of the screen per flare
#define HZM_FLARE_STALE_MS		500		// forget a flare this long after its id stopped arriving (once faded)

void R_HZM_FlareInitQueries( void ) {
	tr.hzmFlareQueriesValid = qfalse;
	Com_Memset( tr.hzmFlareState, 0, sizeof( tr.hzmFlareState ) );
	if ( !glRefConfig.occlusionQuery || glRefConfig.occlusionQueryTarget != GL_SAMPLES_PASSED ) {
		return;
	}
	qglGenQueries( HZM_FLARE_SLOTS * HZM_FLARE_RING * 2, &tr.hzmFlareQuery[0][0][0] );
	tr.hzmFlareQueriesValid = qtrue;
}

void R_HZM_FlareShutdownQueries( void ) {
	if ( tr.hzmFlareQueriesValid ) {
		qglDeleteQueries( HZM_FLARE_SLOTS * HZM_FLARE_RING * 2, &tr.hzmFlareQuery[0][0][0] );
		tr.hzmFlareQueriesValid = qfalse;
	}
	Com_Memset( tr.hzmFlareState, 0, sizeof( tr.hzmFlareState ) );
}

static qboolean RB_HZM_FlareMainView( void ) {
	if ( backEnd.projection2D ) {
		return qfalse;
	}
	if ( backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky ) {
		return qfalse;
	}
	if ( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) {
		return qfalse;
	}
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_HUD ) ) {
		return qfalse;
	}
	if ( tr.renderCubeFbo && ( backEnd.viewParms.targetFbo == tr.renderCubeFbo || glState.currentFBO == tr.renderCubeFbo ) ) {
		return qfalse;
	}
	return qtrue;
}

static qboolean RB_HZM_FlareHasPending( const hzmFlareState_t *s ) {
	int k;

	for ( k = 0; k < HZM_FLARE_RING; k++ ) {
		if ( s->pending[k] ) {
			return qtrue;
		}
	}
	return qfalse;
}

// the global farplane fog between the eye and the flare (rb_globalFog was latched for THIS main view)
static float RB_HZM_FlareFog( float depth ) {
	float frac;

	if ( !rb_globalFog.active || !( rb_globalFog.end > rb_globalFog.start ) ) {
		return 1.0f;
	}
	frac = ( depth - rb_globalFog.start ) / ( rb_globalFog.end - rb_globalFog.start );
	if ( frac < 0.0f ) {
		frac = 0.0f;
	} else if ( frac > 1.0f ) {
		frac = 1.0f;
	}
	frac *= r_globalFogScale ? r_globalFogScale->value : 1.0f;
	if ( frac > 1.0f ) {
		frac = 1.0f;
	}
	return 1.0f - frac;
}

typedef struct {
	float	x, y, half;
	float	color[3];
} hzmFlareDraw_t;

void RB_HZM_SpotFlares( void ) {
	const hzmFlare_t	*req = backEnd.refdef.hzmFlares;
	int					nreq = backEnd.refdef.numHzmFlares;
	int					now = backEnd.refdef.time;
	int					i, k, ndraw = 0;
	qboolean			any = qfalse, glSet = qfalse;
	hzmFlareDraw_t		draw[HZM_FLARE_SLOTS];
	mat4_t				oldmodelview, oldprojection, matrix;
	float				tau, budgetSum = 0.0f, vpW, vpH;
	const float			*eye = backEnd.viewParms.or.origin;
	GLenum				target = glRefConfig.occlusionQueryTarget;

	if ( !req ) {
		nreq = 0;
	}
	if ( !tr.hzmFlareQueriesValid || !RB_HZM_FlareMainView() ) {
		return;
	}
	for ( i = 0; i < HZM_FLARE_SLOTS; i++ ) {
		if ( tr.hzmFlareState[i].inUse ) {
			any = qtrue;
			break;
		}
	}
	if ( !nreq && !any ) {
		return;		// nothing to do: not one GL call (this is what keeps Omaha and the feature-off state identical)
	}
	// once per frame: a second non-portal world scene in the same frame must not re-measure the flares against its
	// own depth buffer, nor draw them again
	if ( tr.hzmFlareFrame == backEnd.viewParms.frameCount ) {
		return;
	}
	tr.hzmFlareFrame = backEnd.viewParms.frameCount;
	if ( !tr.hzmLampFlareShader || tr.hzmLampFlareShader->defaultShader ) {
		// an older pk3 without hzmLampFlare: no flares at all rather than white squares (vet F7)
		if ( !s_hzm.shaderNoted ) {
			s_hzm.shaderNoted = qtrue;
			ri.Printf( PRINT_ALL, "^~^~^ HZMSPOT flare shader hzmLampFlare MISSING - no lamp flares (is the pk3 current?)\n" );
		}
		return;
	}

	vpW = (float)backEnd.viewParms.viewportWidth;
	vpH = (float)backEnd.viewParms.viewportHeight;
	if ( vpW < 1.0f || vpH < 1.0f ) {
		return;
	}

	// 1. requests -> states (by id and class)
	for ( i = 0; i < nreq; i++ ) {
		hzmFlareState_t *s = NULL;
		int				freeSlot = -1;

		for ( k = 0; k < HZM_FLARE_SLOTS; k++ ) {
			hzmFlareState_t *t = &tr.hzmFlareState[k];

			if ( t->inUse ) {
				if ( t->req.id == req[i].id && t->req.flareClass == req[i].flareClass ) {
					s = t;
					break;
				}
			} else if ( freeSlot < 0 && !RB_HZM_FlareHasPending( t ) ) {
				freeSlot = k;
			}
		}
		if ( !s ) {
			if ( freeSlot < 0 ) {
				s_hzm.st.flareFull++;
				continue;
			}
			s = &tr.hzmFlareState[freeSlot];
			Com_Memset( s, 0, sizeof( *s ) );
			s->inUse = qtrue;
			s->lastUpdate = now;
		}
		s->req = req[i];
		s->lastSeen = now;
	}

	// 2. read every result that is READY (never wait), oldest ring slot first so the newest wins
	for ( i = 0; i < HZM_FLARE_SLOTS; i++ ) {
		hzmFlareState_t *s = &tr.hzmFlareState[i];

		for ( k = 0; k < HZM_FLARE_RING; k++ ) {
			int		slot = ( s->nextRing + k ) % HZM_FLARE_RING;
			GLint	availA = 0, availB = 0;
			GLuint	a = 0, b = 0;

			if ( !s->pending[slot] ) {
				continue;
			}
			qglGetQueryObjectiv( tr.hzmFlareQuery[i][slot][0], GL_QUERY_RESULT_AVAILABLE, &availA );
			qglGetQueryObjectiv( tr.hzmFlareQuery[i][slot][1], GL_QUERY_RESULT_AVAILABLE, &availB );
			if ( !availA || !availB ) {
				continue;
			}
			qglGetQueryObjectuiv( tr.hzmFlareQuery[i][slot][0], GL_QUERY_RESULT, &a );
			qglGetQueryObjectuiv( tr.hzmFlareQuery[i][slot][1], GL_QUERY_RESULT, &b );
			s->pending[slot] = qfalse;
			s->target = ( b > 0 ) ? ( ( a >= b ) ? 1.0f : (float)a / (float)b ) : 0.0f;
			s->measured = qtrue;
		}
	}

	// 3. new measurements for the flares asked for this frame
	for ( i = 0; i < HZM_FLARE_SLOTS; i++ ) {
		hzmFlareState_t *s = &tr.hzmFlareState[i];
		vec3_t			toEye, center, left, up;
		vec4_t			quad[4];
		vec2_t			tc[4];
		float			dist, depth, pull, rad, pxWorld;
		int				slot;

		if ( !s->inUse || s->lastSeen != now ) {
			continue;
		}
		slot = s->nextRing;
		if ( s->pending[slot] ) {
			continue;	// all three slots in flight: skip a frame rather than restart an unread query
		}
		VectorSubtract( eye, s->req.origin, toEye );
		dist = VectorLength( toEye );
		depth = DotProduct( s->req.origin, backEnd.viewParms.or.axis[0] ) - DotProduct( eye, backEnd.viewParms.or.axis[0] );
		if ( !( dist > HZM_FLARE_PULL + 1.0f ) || depth <= 1.0f ) {
			s->target = 0.0f;	// behind the eye or inside the lamp: nothing to see
			s->measured = qtrue;
			continue;
		}
		pull = HZM_FLARE_PULL / dist;
		VectorMA( s->req.origin, pull, toEye, center );
		depth -= HZM_FLARE_PULL;
		pxWorld = ( backEnd.viewParms.projectionMatrix[5] > 0.0f )
		        ? 2.0f * depth / ( backEnd.viewParms.projectionMatrix[5] * vpH ) : 1.0f;
		rad = 2.0f * pxWorld;
		if ( rad < HZM_FLARE_MIN_RADIUS ) {
			rad = HZM_FLARE_MIN_RADIUS;
		}
		VectorScale( backEnd.viewParms.or.axis[1], rad, left );
		VectorScale( backEnd.viewParms.or.axis[2], rad, up );
		VectorSet4( quad[0], center[0] + left[0] + up[0], center[1] + left[1] + up[1], center[2] + left[2] + up[2], 1.0f );
		VectorSet4( quad[1], center[0] - left[0] + up[0], center[1] - left[1] + up[1], center[2] - left[2] + up[2], 1.0f );
		VectorSet4( quad[2], center[0] - left[0] - up[0], center[1] - left[1] - up[1], center[2] - left[2] - up[2], 1.0f );
		VectorSet4( quad[3], center[0] + left[0] - up[0], center[1] + left[1] - up[1], center[2] + left[2] - up[2], 1.0f );
		VectorSet2( tc[0], 0.0f, 0.0f );
		VectorSet2( tc[1], 1.0f, 0.0f );
		VectorSet2( tc[2], 1.0f, 1.0f );
		VectorSet2( tc[3], 0.0f, 1.0f );

		if ( !glSet ) {
			glSet = qtrue;
			Mat4Copy( glState.projection, oldprojection );
			Mat4Copy( glState.modelview, oldmodelview );
			GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
			GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );
			GL_Cull( CT_TWO_SIDED );
			GLSL_BindProgram( &tr.textureColorShader );
			GLSL_SetUniformMat4( &tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
			GLSL_SetUniformVec4( &tr.textureColorShader, UNIFORM_COLOR, colorWhite );
			GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
		}
		// (a) what the scene lets through, (b) the whole quad - identical draws but for the depth test
		GL_State( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE );
		qglBeginQuery( target, tr.hzmFlareQuery[i][slot][0] );
		RB_InstantQuad2( quad, tc );
		qglEndQuery( target );
		GL_State( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE | GLS_DEPTHTEST_DISABLE );
		qglBeginQuery( target, tr.hzmFlareQuery[i][slot][1] );
		RB_InstantQuad2( quad, tc );
		qglEndQuery( target );
		s->pending[slot] = qtrue;
		s->nextRing = ( slot + 1 ) % HZM_FLARE_RING;
	}
	if ( glSet ) {
		GL_SetProjectionMatrix( oldprojection );
		GL_SetModelviewMatrix( oldmodelview );
	}

	// 4. time-based fades (plan: exponential smoothing), ageing out, and what to draw
	tau = r_hzmFlareFade ? r_hzmFlareFade->value : 0.12f;
	if ( tau < 0.01f ) {
		tau = 0.01f;
	}
	for ( i = 0; i < HZM_FLARE_SLOTS; i++ ) {
		hzmFlareState_t *s = &tr.hzmFlareState[i];
		float			dt, a, half, inten, dist, area, size, base, lobe;
		vec4_t			eyeV, clip, normalized, window;
		const float		*tint;
		static const float warm[3] = { 1.0f, 0.86f, 0.66f };
		static const float cool[3] = { 0.92f, 0.96f, 1.0f };

		if ( !s->inUse ) {
			continue;
		}
		if ( now < s->lastUpdate || now < s->lastSeen ) {	// time went backwards (map restart, demo seek)
			s->lastUpdate = now;
			if ( s->lastSeen > now ) {
				s->lastSeen = now - HZM_FLARE_STALE_MS - 1;
			}
		}
		dt = ( now - s->lastUpdate ) * 0.001f;
		if ( dt > 0.1f ) {
			dt = 0.1f;
		}
		s->lastUpdate = now;
		a = 1.0f - (float)exp( -dt / tau );
		s->presence += ( ( s->lastSeen == now ? 1.0f : 0.0f ) - s->presence ) * a;
		if ( s->measured ) {
			s->vis += ( s->target - s->vis ) * a;
		}
		if ( s->lastSeen != now && now - s->lastSeen > HZM_FLARE_STALE_MS && s->presence < 0.004f ) {
			s->inUse = qfalse;		// its query names stay in tr; pending slots are drained before reuse
			continue;
		}
		if ( s->presence * s->vis < 0.004f ) {
			continue;
		}

		if ( s->req.flareClass == HZM_FLARE_CLASS_SEARCHLIGHT ) {
			size = r_hzmFlareSize1 ? r_hzmFlareSize1->value : 0.04f;
			base = r_hzmFlareIntensity1 ? r_hzmFlareIntensity1->value : 0.25f;
			lobe = 16.0f;
			tint = cool;
		} else {
			size = r_hzmFlareSize ? r_hzmFlareSize->value : 0.05f;
			base = r_hzmFlareIntensity ? r_hzmFlareIntensity->value : 0.6f;
			lobe = 4.0f;
			tint = warm;
		}
		R_HZM_FlareShape( &s->req, eye, vpH, size, base, r_hzmFlareNear ? r_hzmFlareNear->value : 150.0f, lobe, &half,
		                  &inten, &dist );
		inten *= s->vis * s->presence;
		inten *= RB_HZM_FlareFog( DotProduct( s->req.origin, backEnd.viewParms.or.axis[0] )
		                          - DotProduct( eye, backEnd.viewParms.or.axis[0] ) );
		if ( inten > HZM_FLARE_PEAK ) {
			inten = HZM_FLARE_PEAK;
		}
		if ( !( inten > 0.002f ) || !( half >= 0.5f ) ) {
			continue;
		}

		R_TransformModelToClip( s->req.origin, backEnd.viewParms.world.modelMatrix, backEnd.viewParms.projectionMatrix,
		                        eyeV, clip );
		if ( clip[3] <= 0.0f || clip[0] <= -clip[3] || clip[0] >= clip[3] || clip[1] <= -clip[3] || clip[1] >= clip[3] ) {
			continue;
		}
		R_TransformClipToWindow( clip, &backEnd.viewParms, normalized, window );

		area = ( 2.0f * half ) * ( 2.0f * half ) / ( vpW * vpH );
		if ( area > HZM_FLARE_MAX_AREA ) {
			half *= (float)sqrt( HZM_FLARE_MAX_AREA / area );
			area = HZM_FLARE_MAX_AREA;
		}
		budgetSum += inten * area;

		draw[ndraw].x = backEnd.viewParms.viewportX + window[0];
		draw[ndraw].y = backEnd.viewParms.viewportY + window[1];
		draw[ndraw].half = half;
		draw[ndraw].color[0] = inten * tint[0];
		draw[ndraw].color[1] = inten * tint[1];
		draw[ndraw].color[2] = inten * tint[2];
		ndraw++;
	}
	if ( !ndraw ) {
		return;
	}
	// the frame budget: many flares at once dim together instead of pumping the exposure
	if ( r_hzmFlareBudget && r_hzmFlareBudget->value > 0.0f && budgetSum > r_hzmFlareBudget->value ) {
		float scale = r_hzmFlareBudget->value / budgetSum;

		for ( i = 0; i < ndraw; i++ ) {
			VectorScale( draw[i].color, scale, draw[i].color );
		}
	}

	// 5. the sprites, RB_RenderFlare's ortho recipe with our own shader
	Mat4Copy( glState.projection, oldprojection );
	Mat4Copy( glState.modelview, oldmodelview );
	Mat4Identity( matrix );
	GL_SetModelviewMatrix( matrix );
	Mat4Ortho( backEnd.viewParms.viewportX, backEnd.viewParms.viewportX + backEnd.viewParms.viewportWidth,
	           backEnd.viewParms.viewportY, backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight,
	           -99999, 99999, matrix );
	GL_SetProjectionMatrix( matrix );

	for ( i = 0; i < ndraw; i++ ) {
		const hzmFlareDraw_t	*d = &draw[i];
		unsigned short			c[3];
		int						v;
		static const float		corner[4][2] = { { -1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f } };

		for ( k = 0; k < 3; k++ ) {
			float x = d->color[k];

			c[k] = (unsigned short)( ( x <= 0.0f ? 0.0f : ( x >= 1.0f ? 1.0f : x ) ) * 65535.0f );
		}
		RB_BeginSurface( tr.hzmLampFlareShader, 0, 0 );
		for ( v = 0; v < 4; v++ ) {
			tess.xyz[tess.numVertexes][0] = d->x + corner[v][0] * d->half;
			tess.xyz[tess.numVertexes][1] = d->y + corner[v][1] * d->half;
			tess.xyz[tess.numVertexes][2] = 0.0f;
			tess.texCoords[tess.numVertexes][0] = corner[v][0] > 0.0f ? 1.0f : 0.0f;
			tess.texCoords[tess.numVertexes][1] = corner[v][1] > 0.0f ? 1.0f : 0.0f;
			tess.color[tess.numVertexes][0] = c[0];
			tess.color[tess.numVertexes][1] = c[1];
			tess.color[tess.numVertexes][2] = c[2];
			tess.color[tess.numVertexes][3] = 65535;
			tess.numVertexes++;
		}
		tess.indexes[tess.numIndexes++] = 0;
		tess.indexes[tess.numIndexes++] = 1;
		tess.indexes[tess.numIndexes++] = 2;
		tess.indexes[tess.numIndexes++] = 0;
		tess.indexes[tess.numIndexes++] = 2;
		tess.indexes[tess.numIndexes++] = 3;
		RB_EndSurface();
		backEnd.pc.c_flareRenders++;
	}

	GL_SetProjectionMatrix( oldprojection );
	GL_SetModelviewMatrix( oldmodelview );
}
