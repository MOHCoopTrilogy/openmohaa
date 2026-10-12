/*
===========================================================================
HZM coop [bug-3462] MODERN SCOPE VIEW - the gl2 optics pass.

cgame (cg_hzmscope.c) owns every decision: which optic, the eased scope-in amount, the lens radius, where the eye's
exit pupil sits (the scope shadow). It publishes them in ONE state cvar, r_ppScopeState, which this pass parses only
when its modificationCount moves:
    "<amount> <lens radius / screen height> <pupil x> <pupil y> <pupil radius> <glass warmth>"
An amount of 0 (or the bare "0" cgame writes when the scope is down) skips the pass entirely - no cost unscoped.

r_ppScopeLive tells cgame whether this pass can run (the FBO post chain is up). cgame draws its own 2D aperture and
shadow masks only when it is not, i.e. on gl1 or with r_postProcess 0, so the two never stack. It is written only on
change, from the post chain's own entry (RB_HZMScopeLive), never per frame.

Tuning (flags 0 - TRAPS T7, defaults ship): r_ppScopeCA (chromatic fringe, 1 = a touch), r_ppScopeOuter (how dark
the housing outside the lens is, 0..1; 1 = flat black, the default).
===========================================================================
*/
#include "tr_local.h"
#include "tr_fbo.h"
#include "tr_postprocess.h"

static cvar_t *r_ppScopeState = NULL;
static cvar_t *r_ppScopeLive  = NULL;
static cvar_t *r_ppScopeCA    = NULL;
static cvar_t *r_ppScopeOuter = NULL;

static void RB_HZMScopeCvars(void)
{
	if (!r_ppScopeState) {
		r_ppScopeState = ri.Cvar_Get("r_ppScopeState", "0", 0);    // cgame-published state, never archived
		r_ppScopeLive  = ri.Cvar_Get("r_ppScopeLive",  "0", 0);    // renderer-published
		r_ppScopeCA    = ri.Cvar_Get("r_ppScopeCA",    "1", 0);
		r_ppScopeOuter = ri.Cvar_Get("r_ppScopeOuter", "1", 0);      // 1 = flat black housing. 0.97 (3% of the blurred frame) still read as a grey smudge on bright maps
	}
}

void RB_HZMScopeLive(qboolean live)
{
	RB_HZMScopeCvars();
	live = (live && tr.hzmScopeShader.program && tr.screenScratchFbo) ? qtrue : qfalse;
	if ((r_ppScopeLive->integer != 0) != (live != qfalse)) {
		ri.Cvar_Set("r_ppScopeLive", live ? "1" : "0");
	}
}

void RB_HZMScope(FBO_t *srcFbo, ivec4_t srcBox)
{
	static int   s_mod = -1;
	static float s_st[6];
	vec4_t       color, params;

	RB_HZMScopeCvars();
	if (!srcFbo || !tr.screenScratchFbo || !tr.hzmScopeShader.program) {
		return;
	}
	if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) {
		return;
	}
	if (r_ppScopeState->modificationCount != s_mod) {
		s_mod = r_ppScopeState->modificationCount;
		memset(s_st, 0, sizeof(s_st));
		sscanf(r_ppScopeState->string, "%f %f %f %f %f %f", &s_st[0], &s_st[1], &s_st[2], &s_st[3], &s_st[4], &s_st[5]);
	}
	if (s_st[0] <= 0.001f || s_st[1] <= 0.01f) {
		return;
	}

	VectorSet4(color, s_st[0] > 1.0f ? 1.0f : s_st[0], s_st[1], s_st[2], s_st[3]);
	VectorSet4(params, s_st[4], s_st[5], r_ppScopeCA->value, r_ppScopeOuter->value);

	FBO_SetHzmParams(params);
	FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.hzmScopeShader, color, 0);
	FBO_SetHzmParams(NULL);
	FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
