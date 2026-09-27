/*
===========================================================================
HZM coop [2026-09-26] PHASE S - spot cones and lamp flares for renderer_opengl2: the PURE half.

  docs/proposals/headlights_2026-09-25/plan_phaseS.md (the plan) and vet_phaseS.md (authoritative where they differ).
  The carrier protocol, its bit layout, the AUTO defines and the Omaha list: renderercommon/hzm_light_restore.h.

Every function in this file reads ONLY its arguments - no tr, no backEnd, no cvars, no GL, no engine imports - so
docs/tools/hzm_spot_selftest compiles this exact file into a console program and proves the invariants the renderer
relies on (vet_phaseS G4): a non-spot factor of exactly 1.0f, carriers never taking a slot, a carrier after a dropped
light discarded, spot-or-nothing, an all-zero uniform for every non-spot, mismatched tags discarded, flare ids that
round-trip. The renderer glue (cvars, the tr_scene.c intake, uniform uploads, flares, occlusion queries) is
tr_hzm_spot_rb.c.

THE ONE LAW. World, soldiers, props and grid-lit things all use lightall's CalcLightAttenuation with point = 1:
    att(d) = clamp(0.5 * R^2 / d^2 - 0.5, 0, 1)      (1 inside 0.577 R, 0 at R)
and one cone:
    cone   = smoothstep01((cos(angle to the axis) - cosOuter) * k),   k = 1 / (cosInner - cosOuter)
which is exactly 0 behind the apex because cosOuter is kept > 0 (an outer half-angle under 90 degrees).
===========================================================================
*/

#include "tr_local.h"

static float R_HZM_Clamp01( float x ) {
	if ( !( x > 0.0f ) ) {	// also catches NaN
		return 0.0f;
	}
	return ( x < 1.0f ) ? x : 1.0f;
}

static float R_HZM_Smooth01( float x ) {
	x = R_HZM_Clamp01( x );
	return x * x * ( 3.0f - 2.0f * x );
}

static float R_HZM_Len( const vec3_t v ) {
	return (float)sqrt( DotProduct( v, v ) );
}

/*
=============
R_HZM_SpotClearLight

Every add zeroes these (vet: the 32 slots are reused every frame). hzmOwner -1 = no owner.
=============
*/
void R_HZM_SpotClearLight( dlight_t *dl ) {
	dl->hzmSpot = HZM_SPOT_NONE;
	dl->hzmTag = 0;
	VectorClear( dl->hzmAxis );
	VectorClear( dl->hzmAxisLocal );
	dl->hzmCosOuter = 0.0f;
	dl->hzmConeK = 0.0f;
	dl->hzmOwner = -1;
}

/*
=============
R_HZM_SpotClassify

What one R_AddLightToScene call is, given whether spots / flares are live on this world. A carrier is honoured only
with intensity < 0 (it is always sent that way, and gl1 / an old gl2 drop exactly that) and a spot only with a
non-zero tag (plan R4: an author's raw TIKI integer can never become either).
=============
*/
hzmAdd_t R_HZM_SpotClassify( float intensity, int type, qboolean spotsOn, qboolean flaresOn ) {
	if ( type & HZM_DLIGHT_CARRIER ) {
		if ( !( intensity < 0.0f ) ) {
			return HZM_ADD_LIGHT;			// not ours: an ordinary light, the Phase S bits are ignored
		}
		if ( type & HZM_DLIGHT_RESERVED_MASK ) {
			return HZM_ADD_DISCARD;
		}
		switch ( type & HZM_DLIGHT_KIND_MASK ) {
		case HZM_DLIGHT_KIND_SPOT:
			if ( !HZM_DlightTagOf( type ) || !spotsOn ) {
				return HZM_ADD_DISCARD;
			}
			return HZM_ADD_SPOT_CARRIER;
		case HZM_DLIGHT_KIND_FLARE:
			return flaresOn ? HZM_ADD_FLARE : HZM_ADD_DISCARD;
		default:
			return HZM_ADD_DISCARD;
		}
	}
	if ( ( type & HZM_DLIGHT_SPOT ) && HZM_DlightTagOf( type ) && intensity > 0.0f ) {
		return spotsOn ? HZM_ADD_SPOT : HZM_ADD_DROP_SPOT;
	}
	return HZM_ADD_LIGHT;
}

/*
=============
R_HZM_SpotBeginLight

Called on the slot R_AddDynamicLightTyped just filled: zero every Phase S field, and mark a spot PENDING until its
carrier arrives. `isSpot` is R_HZM_SpotClassify's HZM_ADD_SPOT verdict for this same add.
=============
*/
void R_HZM_SpotBeginLight( dlight_t *dl, int type, qboolean isSpot ) {
	R_HZM_SpotClearLight( dl );
	if ( isSpot ) {
		dl->hzmSpot = HZM_SPOT_PENDING;
		dl->hzmTag = HZM_DlightTagOf( type );
	}
}

/*
=============
R_HZM_SpotAttachCarrier

A spot carrier fills the NEWEST light in [first, num) that is still pending with the same tag. Returns that light's
index, or -1 when there is none (its light was dropped by the 32-slot cap, or never sent): the carrier is then
discarded and can never turn some other light - a muzzle flash - into a cone (vet F15). A malformed axis leaves the
light pending, so R_HZM_SpotCompact removes it: spot-or-nothing.
=============
*/
int R_HZM_SpotAttachCarrier( dlight_t *dlights, int first, int num, int tag, const vec3_t axis, float cosInner,
                             float cosOuter, float owner ) {
	int		i;
	float	len;

	if ( tag <= 0 ) {
		return -1;
	}
	for ( i = num - 1; i >= first; i-- ) {
		dlight_t *dl = &dlights[i];

		if ( dl->hzmSpot != HZM_SPOT_PENDING || dl->hzmTag != tag ) {
			continue;
		}
		len = R_HZM_Len( axis );
		if ( !( len > 1e-6f ) || !( cosOuter == cosOuter ) || !( cosInner == cosInner ) ) {
			return -1;
		}
		// keep the whole cone in front of the apex (cosOuter > 0) and the ramp finite (vet F9: >= 1e-3 wide)
		if ( cosOuter < HZM_SPOT_MIN_COS_OUTER ) {
			cosOuter = HZM_SPOT_MIN_COS_OUTER;
		}
		if ( cosOuter > 0.9999f ) {
			cosOuter = 0.9999f;
		}
		if ( cosInner > 1.0f ) {
			cosInner = 1.0f;
		}
		if ( cosInner < cosOuter + 1e-3f ) {
			cosInner = cosOuter + 1e-3f;
		}
		VectorScale( axis, 1.0f / len, dl->hzmAxis );
		VectorCopy( dl->hzmAxis, dl->hzmAxisLocal );
		dl->hzmCosOuter = cosOuter;
		dl->hzmConeK = 1.0f / ( cosInner - cosOuter );
		dl->hzmOwner = ( owner >= 0.0f && owner < (float)MAX_GENTITIES ) ? (int)owner : -1;
		dl->hzmSpot = HZM_SPOT_READY;
		return i;
	}
	return -1;
}

/*
=============
R_HZM_SpotIntakeCore

The whole intake decision for one R_AddLightToScene call, on the arrays it is given (the renderer passes
backEndData->dlights with tr_scene.c's scene range, and its flare list). Returns qtrue when the add is CONSUMED: a
carrier (attached, turned into a flare, or discarded - it never takes one of the 32 slots) or a spot light while spots
are off (dropped: spot-or-nothing). Otherwise the caller takes a slot as it always did and then calls
R_HZM_SpotBeginLight with *verdict.
=============
*/
qboolean R_HZM_SpotIntakeCore( dlight_t *dlights, int first, int num, hzmFlare_t *flares, int *numFlares, int maxFlares,
                               const vec3_t org, float intensity, float r, float g, float b, int type,
                               qboolean spotsOn, qboolean flaresOn, hzmSpotStats_t *stats, hzmAdd_t *verdict ) {
	hzmAdd_t v = R_HZM_SpotClassify( intensity, type, spotsOn, flaresOn );

	*verdict = v;
	switch ( v ) {
	case HZM_ADD_LIGHT:
	case HZM_ADD_SPOT:
		return qfalse;
	case HZM_ADD_DROP_SPOT:
		stats->spotOff++;
		return qtrue;
	case HZM_ADD_SPOT_CARRIER:
		if ( R_HZM_SpotAttachCarrier( dlights, first, num, HZM_DlightTagOf( type ), org, r, g, b ) < 0 ) {
			stats->orphan++;
		}
		return qtrue;
	case HZM_ADD_FLARE:
		if ( *numFlares >= maxFlares ) {
			stats->flareFull++;
		} else if ( R_HZM_FlareFromCarrier( org, intensity, r, g, b, type, &flares[*numFlares] ) ) {
			( *numFlares )++;
		} else {
			stats->discarded++;
		}
		return qtrue;
	default:
		stats->discarded++;
		return qtrue;
	}
}

/*
=============
R_HZM_SpotCompact

Spot-or-nothing: remove every light in [first, num) still PENDING (its carrier never came). Order is kept. Returns
the new count; *dropped (if given) is increased by the number removed.
=============
*/
int R_HZM_SpotCompact( dlight_t *dlights, int first, int num, int *dropped ) {
	int i, out = first;

	for ( i = first; i < num; i++ ) {
		if ( dlights[i].hzmSpot == HZM_SPOT_PENDING ) {
			if ( dropped ) {
				( *dropped )++;
			}
			continue;
		}
		if ( out != i ) {
			dlights[out] = dlights[i];
		}
		out++;
	}
	return out;
}

/*
=============
R_HZM_SpotAttenuation

lightall CalcLightAttenuation(point = 1, R^2/d^2), on the CPU. d is clamped to 1 u.
=============
*/
float R_HZM_SpotAttenuation( float distSq, float radius ) {
	if ( !( radius > 0.0f ) ) {
		return 0.0f;
	}
	if ( distSq < 1.0f ) {
		distSq = 1.0f;
	}
	return R_HZM_Clamp01( 0.5f * radius * radius / distSq - 0.5f );
}

/*
=============
R_HZM_SpotConeDir

The cone for one direction from the apex (not normalised). At the apex itself: 1.
=============
*/
float R_HZM_SpotConeDir( const vec3_t axis, float cosOuter, float coneK, const vec3_t toPoint ) {
	float len = R_HZM_Len( toPoint );

	if ( !( len > 1e-6f ) ) {
		return 1.0f;
	}
	return R_HZM_Smooth01( ( DotProduct( toPoint, axis ) / len - cosOuter ) * coneK );
}

/*
=============
R_HZM_SpotConeSphere

The cone for a SPHERE: the angle to the axis is reduced by the sphere's angular radius asin(r/d), so a soldier
fades in as soon as his sphere touches the cone's edge (and a lamp inside the sphere counts as a full hit).
=============
*/
float R_HZM_SpotConeSphere( const vec3_t apex, const vec3_t axis, float cosOuter, float coneK, const vec3_t center,
                            float radius ) {
	vec3_t	v;
	float	d, c, s, sinA, cosA, cosEff;

	VectorSubtract( center, apex, v );
	d = R_HZM_Len( v );
	if ( !( d > radius ) || !( d > 1e-6f ) ) {
		return 1.0f;
	}
	c = DotProduct( v, axis ) / d;
	if ( c > 1.0f ) {
		c = 1.0f;
	} else if ( c < -1.0f ) {
		c = -1.0f;
	}
	sinA = ( radius > 0.0f ) ? radius / d : 0.0f;
	cosA = (float)sqrt( 1.0f - sinA * sinA );
	if ( c >= cosA ) {
		cosEff = 1.0f;						// the axis passes through the sphere
	} else {
		// cos(theta - alpha) = cos(theta) cos(alpha) + sin(theta) sin(alpha)
		s = (float)sqrt( 1.0f - c * c );
		cosEff = c * cosA + s * sinA;
	}
	return R_HZM_Smooth01( ( cosEff - cosOuter ) * coneK );
}

/*
=============
R_HZM_SpotSphereTouches

Conservative cull for the world pass: can a sphere (a surface's bounds) receive any light from this cone? True when
the lamp is inside it, or the angle to its centre is within the outer angle plus its angular radius. The range test
(the light radius) is the caller's existing one.
=============
*/
qboolean R_HZM_SpotSphereTouches( const vec3_t apex, const vec3_t axis, float cosOuter, const vec3_t center,
                                  float radius ) {
	vec3_t	v;
	float	d, c, sinA, cosA, sinO;

	VectorSubtract( center, apex, v );
	d = R_HZM_Len( v );
	if ( !( d > radius ) ) {
		return qtrue;						// the lamp sits inside the bound (vet F27: asin(r/d) breaks there)
	}
	c = DotProduct( v, axis ) / d;
	sinA = radius / d;
	cosA = (float)sqrt( 1.0f - sinA * sinA );
	sinO = ( cosOuter < 1.0f ) ? (float)sqrt( 1.0f - cosOuter * cosOuter ) : 0.0f;
	// outer < 90 deg and alpha < 90 deg, so their sum is under 180 deg and cos() is monotonic there
	return ( c >= cosOuter * cosA - sinO * sinA ) ? qtrue : qfalse;
}

/*
=============
R_HZM_DlightFactorAt

The spot's share at a sphere (radius 0 = a point): attenuation at its centre x the sphere-dilated cone. EXACTLY 1.0f
for every light that is not a ready spot - callers multiply only when dl->hzmSpot == HZM_SPOT_READY anyway, so every
existing light stays bit-identical.
=============
*/
float R_HZM_DlightFactorAt( const dlight_t *dl, const vec3_t point, float radius ) {
	vec3_t v;

	if ( dl->hzmSpot != HZM_SPOT_READY ) {
		return 1.0f;
	}
	VectorSubtract( point, dl->origin, v );
	return R_HZM_SpotAttenuation( DotProduct( v, v ), dl->radius )
	     * R_HZM_SpotConeSphere( dl->origin, dl->hzmAxis, dl->hzmCosOuter, dl->hzmConeK, point, radius );
}

/*
=============
R_HZM_SpotUniformVec

u_HzmLightSpot for one light: (axis * k, cosOuter), cosOuter + 4 when the r_hzmSpotDebug tint is on. ALL ZERO for
every light that is not a ready spot - GPU uniforms start at 0 and GLSL_SetUniformVec4 caches from 0, so zero is the
value a program already holds and must mean "omni" (plan finding 5).
=============
*/
void R_HZM_SpotUniformVec( const dlight_t *dl, const vec3_t axis, qboolean debugTint, vec4_t out ) {
	if ( !dl || dl->hzmSpot != HZM_SPOT_READY ) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
	out[0] = axis[0] * dl->hzmConeK;
	out[1] = axis[1] * dl->hzmConeK;
	out[2] = axis[2] * dl->hzmConeK;
	out[3] = dl->hzmCosOuter + ( debugTint ? HZM_SPOT_DEBUG_OFFSET : 0.0f );
}

/*
=============
R_HZM_FlareFromCarrier

Decode one flare carrier (lamp, -brightness, facing.xyz, bits). False = malformed (discard).
=============
*/
qboolean R_HZM_FlareFromCarrier( const vec3_t org, float intensity, float r, float g, float b, int type,
                                 hzmFlare_t *out ) {
	vec3_t	f;
	float	len;

	if ( !( intensity < 0.0f ) ) {
		return qfalse;
	}
	f[0] = r;
	f[1] = g;
	f[2] = b;
	len = R_HZM_Len( f );
	if ( !( len > 1e-6f ) ) {
		return qfalse;
	}
	VectorCopy( org, out->origin );
	VectorScale( f, 1.0f / len, out->facing );
	out->brightness = -intensity;
	out->id = HZM_DlightTagOf( type );
	out->flareClass = HZM_DlightFlareClassOf( type );
	return qtrue;
}

/*
=============
R_HZM_FlareShape

Screen size and raw intensity of one flare seen from `eye` (plan section 5):
  half size (px) = 0.5 x size x viewport height x (0.5 + 0.5 facing) x clamp(600 / dist, 0.35, 1)
  intensity      = intensity x facing^lobe x brightness x smoothstep01(dist / near)
facing = the lamp's forward . the direction to the eye, 0 behind the lamp. Visibility, presence, fog and the frame
budget are applied by the caller.
=============
*/
void R_HZM_FlareShape( const hzmFlare_t *f, const vec3_t eye, float viewportHeight, float size, float intensity,
                       float nearDist, float lobe, float *outHalfPx, float *outIntensity, float *outDist ) {
	vec3_t	toEye;
	float	dist, facing, sizeK, nearK, bright;

	VectorSubtract( eye, f->origin, toEye );
	dist = R_HZM_Len( toEye );
	*outDist = dist;
	if ( !( dist > 1e-3f ) ) {
		*outHalfPx = 0.0f;
		*outIntensity = 0.0f;
		return;
	}
	facing = R_HZM_Clamp01( DotProduct( toEye, f->facing ) / dist );
	sizeK = 600.0f / dist;
	if ( sizeK > 1.0f ) {
		sizeK = 1.0f;
	} else if ( sizeK < 0.35f ) {
		sizeK = 0.35f;
	}
	*outHalfPx = 0.5f * size * viewportHeight * ( 0.5f + 0.5f * facing ) * sizeK;
	nearK = ( nearDist > 0.0f ) ? R_HZM_Smooth01( dist / nearDist ) : 1.0f;
	bright = R_HZM_Clamp01( f->brightness );
	*outIntensity = intensity * (float)pow( facing, lobe ) * bright * nearK;
}
