/*
===========================================================================
HZM coop [2026-09-26] PHASE S - the SHARED cgame spot / flare facility (plan_phaseS.md section 7, vet_phaseS.md).

Managers (the headlights today - cg_view.c CG_CoopHeadlights - and later the searchlights) do not talk to the
renderer themselves. Each frame they REQUEST spots and flares here; CG_HZM_FlushSpots, called once after every manager
(cg_view.c, after CG_CoopHeadlights), sorts them, applies ONE shared budget and emits each spot as its light followed
by its carrier, and each flare as one carrier - the protocol in renderercommon/hzm_light_restore.h section 3.

  BUDGET  cg_hzmSpotMax (4, flags 0): full-strength spots per frame, nearest (lowest priority value) first, shared by
          every manager; a spot a manager is FADING OUT (it just left that manager's nearest set - vet F15) rides on
          top, at most HZM_SPOT_FADEOUT_EXTRA of them, so a convoy's pools cross-fade instead of popping. The 32
          renderer slots are first come first served and these lights are added AFTER the muzzle/blast lights
          (cg_view.c CG_AddCoopDynamicLights runs first - vet F8). Flares: nearest HZM_FLARE_MAX (32).
  GATES   spots: the renderer's handshake r_hzmSpotProtocol == this cgame's HZM_SPOT_PROTOCOL (vet F3 - never a
          cl_renderer string compare, which read the gfx test's "opengl2flip" as not-gl2), cl_renderer names a gl2
          build (so a hand-set value under gl1 can never send a spot to gl1, which would draw a sun-flare sprite on it -
          vet F6), r_hzmSpot resolved on, not an Omaha BSP. Flares: the same plus r_hzmFlares and r_flares.
          Anything unavailable is simply not requested - a manager falls back to Phase H's omni pool.
Reads nothing beyond the snapshot (spotlight.scr bug-2548 is untouched).
===========================================================================
*/

#include "cg_local.h"
#include "../renderercommon/hzm_light_restore.h"

#define HZM_SPOT_MAX_REQ        16  // requests accepted per frame (all managers)
#define HZM_SPOT_FADEOUT_EXTRA  2   // fading-out spots allowed on top of cg_hzmSpotMax
#define HZM_FLARE_MAX_REQ       ( HZM_FLARE_MAX * 2 )

static hzmSpotReq_t  s_spotReq[HZM_SPOT_MAX_REQ];
static int           s_numSpotReq;
static hzmFlareReq_t s_flareReq[HZM_FLARE_MAX_REQ];
static int           s_numFlareReq;
static int           s_lastSpots, s_lastFlares;

static cvar_t *s_hzmSpot, *s_hzmFlares, *s_hzmProtocol, *s_hzmRenderer, *s_hzmRFlares, *s_hzmSpotMax;

static void CG_HZM_RegisterCvars(void)
{
    if (s_hzmSpot) {
        return;
    }
    // the same names and defaults the renderer registers (renderergl2/tr_hzm_spot_rb.c, tr_init.c) - a second
    // Cvar_Get with a different default would BE a bug (TRAPS T3)
    s_hzmSpot     = cgi.Cvar_Get("r_hzmSpot", "-1", 0);
    s_hzmFlares   = cgi.Cvar_Get("r_hzmFlares", "-1", 0);
    s_hzmProtocol = cgi.Cvar_Get("r_hzmSpotProtocol", "0", 0);
    s_hzmRenderer = cgi.Cvar_Get("cl_renderer", "opengl1", 0);
    s_hzmRFlares  = cgi.Cvar_Get("r_flares", "0", 0);
    s_hzmSpotMax  = cgi.Cvar_Get("cg_hzmSpotMax", "4", 0);   // flags 0: never saved (TRAPS T7)
}

// a gl2 build: "opengl2", and the gfx test's "opengl2flip" (vet F3); never "opengl1"
qboolean CG_HZM_RendererIsGl2(void)
{
    CG_HZM_RegisterCvars();
    return Q_stricmpn(s_hzmRenderer->string, "opengl2", 7) ? qfalse : qtrue;
}

// the renderer that is loaded speaks THIS cgame's carrier protocol
qboolean CG_HZM_ProtocolOk(void)
{
    CG_HZM_RegisterCvars();
    return (CG_HZM_RendererIsGl2() && s_hzmProtocol->integer == HZM_SPOT_PROTOCOL) ? qtrue : qfalse;
}

int CG_HZM_RendererProtocol(void)
{
    CG_HZM_RegisterCvars();
    return s_hzmProtocol->integer;
}

qboolean CG_HZM_SpotsAvailable(void)
{
    if (!CG_HZM_ProtocolOk()) {
        return qfalse;
    }
    if (!HZM_ResolveAutoSwitch(s_hzmSpot->integer, HZM_SPOT_AUTO)) {
        return qfalse;
    }
    return HZM_LightRestoreMapProtected(cgs.mapname) ? qfalse : qtrue;
}

qboolean CG_HZM_FlaresAvailable(void)
{
    if (!CG_HZM_ProtocolOk()) {
        return qfalse;
    }
    if (!HZM_ResolveAutoSwitch(s_hzmFlares->integer, HZM_FLARES_AUTO) || !s_hzmRFlares->integer) {
        return qfalse;
    }
    return HZM_LightRestoreMapProtected(cgs.mapname) ? qfalse : qtrue;
}

qboolean CG_HZM_SpotRequest(const hzmSpotReq_t *req)
{
    if (!req || s_numSpotReq >= HZM_SPOT_MAX_REQ) {
        return qfalse;
    }
    if (!(req->range > 0.0f) || !(req->color[0] + req->color[1] + req->color[2] > 0.001f)
        || !(req->outerDeg > 0.0f && req->outerDeg < 89.0f)) {
        return qfalse;
    }
    s_spotReq[s_numSpotReq++] = *req;
    return qtrue;
}

qboolean CG_HZM_FlareRequest(const hzmFlareReq_t *req)
{
    if (!req || s_numFlareReq >= HZM_FLARE_MAX_REQ || !(req->brightness > 0.0f)) {
        return qfalse;
    }
    s_flareReq[s_numFlareReq++] = *req;
    return qtrue;
}

void CG_HZM_LastCounts(int *spots, int *flares)
{
    if (spots) {
        *spots = s_lastSpots;
    }
    if (flares) {
        *flares = s_lastFlares;
    }
}

static float CG_HZM_FlareDistSq(const hzmFlareReq_t *f)
{
    vec3_t d;

    VectorSubtract(f->origin, cg.refdef.vieworg, d);
    return DotProduct(d, d);
}

/*
=============
CG_HZM_FlushSpots

Once per frame, after every manager. Requests made while a feature is unavailable are dropped here too, so a manager
that got it wrong can never send a spot to a renderer that would light it as an omni light.
=============
*/
void CG_HZM_FlushSpots(void)
{
    int order[HZM_FLARE_MAX_REQ];
    int i, j, n, full = 0, extra = 0, tag = 1, maxFull;

    CG_HZM_RegisterCvars();
    s_lastSpots  = 0;
    s_lastFlares = 0;

    if (s_numSpotReq && CG_HZM_SpotsAvailable()) {
        n = s_numSpotReq;
        for (i = 0; i < n; i++) {
            order[i] = i;
        }
        for (i = 1; i < n; i++) {   // insertion sort by priority (a handful of requests)
            int o = order[i];

            for (j = i - 1; j >= 0 && s_spotReq[order[j]].priority > s_spotReq[o].priority; j--) {
                order[j + 1] = order[j];
            }
            order[j + 1] = o;
        }
        maxFull = s_hzmSpotMax->integer;
        if (maxFull < 0) {
            maxFull = 0;
        } else if (maxFull > 8) {
            maxFull = 8;
        }
        for (i = 0; i < n && tag <= HZM_DLIGHT_TAG_MAX; i++) {
            const hzmSpotReq_t *r = &s_spotReq[order[i]];
            int                 type;
            float               cosInner, cosOuter;

            if (r->flags & HZM_SPOTREQ_FADEOUT) {
                if (extra >= HZM_SPOT_FADEOUT_EXTRA) {
                    continue;
                }
                extra++;
            } else {
                if (full >= maxFull) {
                    continue;
                }
                full++;
            }
            cosOuter = (float)cos(DEG2RAD(r->outerDeg));
            cosInner = (float)cos(DEG2RAD(r->innerDeg < r->outerDeg ? r->innerDeg : r->outerDeg * 0.5f));
            type     = additive | HZM_DLIGHT_SPOT | HZM_DlightTagBits(tag);
            if (r->flags & HZM_SPOTREQ_NOSHADOW) {
                type |= hzm_dlight_noshadow;
            }
            if (r->flags & HZM_SPOTREQ_FOGGED) {
                type |= HZM_DLIGHT_FOGGED;
            }
            // the light, then its carrier with the same tag - adjacent, so nothing can come between them
            cgi.R_AddLightToScene(r->origin, r->range, r->color[0], r->color[1], r->color[2], type);
            cgi.R_AddLightToScene(
                r->axis,
                -1.0f,
                cosInner,
                cosOuter,
                (float)r->owner,
                HZM_DLIGHT_CARRIER | HZM_DLIGHT_KIND_SPOT | HZM_DlightTagBits(tag)
            );
            tag++;
            s_lastSpots++;
        }
    }

    if (s_numFlareReq && CG_HZM_FlaresAvailable()) {
        n = s_numFlareReq;
        for (i = 0; i < n; i++) {
            order[i] = i;
        }
        for (i = 1; i < n; i++) {   // nearest first
            int   o  = order[i];
            float dO = CG_HZM_FlareDistSq(&s_flareReq[o]);

            for (j = i - 1; j >= 0 && CG_HZM_FlareDistSq(&s_flareReq[order[j]]) > dO; j--) {
                order[j + 1] = order[j];
            }
            order[j + 1] = o;
        }
        for (i = 0; i < n && s_lastFlares < HZM_FLARE_MAX; i++) {
            const hzmFlareReq_t *f = &s_flareReq[order[i]];
            float                b = f->brightness > 1.0f ? 1.0f : f->brightness;

            cgi.R_AddLightToScene(
                f->origin, -b, f->axis[0], f->axis[1], f->axis[2], HZM_DlightFlareBits(f->id, f->cls)
            );
            s_lastFlares++;
        }
    }

    s_numSpotReq  = 0;
    s_numFlareReq = 0;
}
