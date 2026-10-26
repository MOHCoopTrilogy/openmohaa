/*
===========================================================================
HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE LENS - the gl2 side.

cgame (cg_hzmscope.c) publishes ONE state cvar, r_ppScopeState (shared with the bug-3462 pass):
    "<amount> <lens radius / screen height> <pupil x> <pupil y> <pupil radius> <glass warmth> <lens fov>"
A lens fov > 0 means "picture-in-picture": the MAIN view stays at the wide hip fov and this file adds
  1. R_HzmLensRenderView (front end, from RE_RenderScene, BEFORE the main R_RenderView): a second world view along the
     SAME ray at the magnified fov, 1:1, into the lens FBO (N x N, HDR) - shares the scene lists exactly as a mirror /
     portal sub-view does. No viewmodel, no cubemaps, no dynamic sun-shadow receive (r_ppScopeShadows 0, default).
  2. RB_HZMLensComposite (back end, top of the post chain, on the HDR scene, before bloom / tone map): inside the lens
     circle the lens picture (barrel, chromatic fringe, glass tint, rim, pupil shadow); outside it the real wide-angle
     world, blurred and darkened; so exposure, bloom and grade treat both as one picture.
A fov of 0 (or amount 0) leaves everything to the old bug-3462 pass (tr_hzm_scope.c).

Cvars (flags 0): r_ppScopePip (1; 0 = never), r_ppScopeRes (1024: lens N cap, 512..1024), r_ppScopeShadows (0),
r_ppScopeOutside (0.42: brightness of the world around the lens), r_ppScopeBlur (1: blur radius scale), r_ppScopeCA (shared).
r_ppScopePipLive is renderer-published: 1 while this path can run, so cgame never selects it when it cannot.
===========================================================================
*/
#include "tr_local.h"
#include "tr_fbo.h"
#include "tr_postprocess.h"
#ifdef _WIN32
#include <windows.h>
#endif

static cvar_t *r_ppScopeState   = NULL;
static cvar_t *r_ppScopePip     = NULL;
static cvar_t *r_ppScopePipLive = NULL;
static cvar_t *r_ppScopeRes     = NULL;
static cvar_t *r_ppScopeShadows = NULL;
static cvar_t *r_ppScopeOutside = NULL;
static cvar_t *r_ppScopeBlur    = NULL;
static cvar_t *r_ppScopeCA      = NULL;

qboolean R_CheckFBO(const FBO_t *fbo);   // tr_fbo.c (no header declares it)
FBO_t   *FBO_Create(const char *name, int width, int height);   // same: undeclared, so C would have truncated the pointer to int

#ifdef _WIN32
/*
Crash probe (r_ppScopeCrash 1/2, flags 0): a vectored exception handler that writes the faulting module+offset and the last 24 frames of a
fault to hzm_lens_crash.txt (cwd) and the console log before the process dies, plus stage markers (2) around the lens passes. Inert at the default 0.
*/
static volatile int s_lensStage = 0;
static LONG __stdcall R_HzmLensVeh(PEXCEPTION_POINTERS ep)
{
	unsigned long  code = ep->ExceptionRecord->ExceptionCode;
	void          *addr = ep->ExceptionRecord->ExceptionAddress;
	void          *st[24];
	unsigned short n, i;
	FILE          *f;
	char           nm[260];
	HMODULE        mod = NULL;

	if (code < 0xC0000000UL || code == 0xC0000194UL || code == 0xE06D7363UL) {
		return EXCEPTION_CONTINUE_SEARCH; // only hard faults
	}
	f = fopen("hzm_lens_crash.txt", "a");
	if (f) {
		fprintf(f, "FAULT code=%08lx stage=%d addr=%p\n", code, s_lensStage, addr);
		n = RtlCaptureStackBackTrace(0, 24, st, NULL);
		for (i = 0; i < n; i++) {
			nm[0] = 0; mod = NULL;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)st[i], &mod) && mod) {
				GetModuleFileNameA(mod, nm, sizeof(nm));
				fprintf(f, "  #%u %s+0x%llx\n", (unsigned)i, nm, (unsigned long long)((char *)st[i] - (char *)mod));
			} else {
				fprintf(f, "  #%u %p\n", (unsigned)i, st[i]);
			}
		}
		fclose(f);
	}
	ri.Printf(PRINT_ALL, "^~^~^ LENSFAULT code=%08lx stage=%d addr=%p\n", code, s_lensStage, addr);
	return EXCEPTION_CONTINUE_SEARCH;
}
static void R_HzmLensCrashProbe(void)
{
	static qboolean s_inst = qfalse;
	cvar_t *c = ri.Cvar_Get("r_ppScopeCrash", "0", 0);

	if (c->integer && !s_inst) {
		s_inst = qtrue;
		AddVectoredExceptionHandler(1, R_HzmLensVeh);
	}
}
#define LENS_STAGE(n) do { s_lensStage = (n); if (ri.Cvar_Get("r_ppScopeCrash", "0", 0)->integer > 1) ri.Printf(PRINT_ALL, "^~^~^ LENS stage %d\n", (n)); } while (0)
#else
#define R_HzmLensCrashProbe() ((void)0)
#define LENS_STAGE(n) ((void)0)
#endif

#define LENS_MARGIN 1.04f   // the lens square is this much wider than the lens circle (barrel / fringe sample past the rim)

static void R_HzmLensCvars(void)
{
	if (!r_ppScopeState) {
		r_ppScopeState   = ri.Cvar_Get("r_ppScopeState", "0", 0);
		r_ppScopePip     = ri.Cvar_Get("r_ppScopePip", "1", 0);
		r_ppScopePipLive = ri.Cvar_Get("r_ppScopePipLive", "0", 0);
		r_ppScopeRes     = ri.Cvar_Get("r_ppScopeRes", "1024", 0);
		r_ppScopeShadows = ri.Cvar_Get("r_ppScopeShadows", "0", 0);
		r_ppScopeOutside = ri.Cvar_Get("r_ppScopeOutside", "0.42", 0);
		r_ppScopeBlur    = ri.Cvar_Get("r_ppScopeBlur", "1", 0);
		r_ppScopeCA      = ri.Cvar_Get("r_ppScopeCA", "1", 0);
	}
}

// the parsed r_ppScopeState, refreshed only when its modificationCount moves
static const float *R_HzmLensState(void)
{
	static int   s_mod = -1;
	static float s_st[7];

	R_HzmLensCvars();
	if (r_ppScopeState->modificationCount != s_mod) {
		s_mod = r_ppScopeState->modificationCount;
		memset(s_st, 0, sizeof(s_st));
		sscanf(r_ppScopeState->string, "%f %f %f %f %f %f %f", &s_st[0], &s_st[1], &s_st[2], &s_st[3], &s_st[4], &s_st[5], &s_st[6]);
	}
	return s_st;
}

qboolean R_HzmLensStateWantsPip(void)
{
	const float *st = R_HzmLensState();

	return (st[0] > 0.001f && st[1] > 0.01f && st[6] > 1.0f) ? qtrue : qfalse;
}

// can this renderer run the PiP at all right now (renderer-published so cgame can read it)
static qboolean R_HzmLensCapable(void)
{
	R_HzmLensCvars();
	return (r_ppScopePip->integer && glRefConfig.framebufferObject && r_postProcess && r_postProcess->integer
	        && tr.hzmLensShader.program && (tr.sceneFbo != NULL || tr.renderFbo != NULL)) ? qtrue : qfalse;
}

void R_HzmLensPublishLive(qboolean live)
{
	R_HzmLensCvars();
	live = (live && R_HzmLensCapable()) ? qtrue : qfalse;
	if ((r_ppScopePipLive->integer != 0) != (live != qfalse)) {
		ri.Cvar_Set("r_ppScopePipLive", live ? "1" : "0");
	}
}

static int R_HzmLensSizeFor(void)
{
	float side = 2.0f * 0.465f * LENS_MARGIN * (float)glConfig.vidHeight;
	int   n    = (int)(side + 7.0f) & ~7;
	int   cap  = r_ppScopeRes->integer;

	if (cap < 512) cap = 512;
	if (cap > 1024) cap = 1024;
	if (n > cap) n = cap;
	if (n < 512) n = 512;
	return n;
}

static qboolean R_HzmLensEnsureFbo(void)
{
	int n, fmt;

	if (tr.hzmLensFbo) {
		return qtrue;
	}
	if (!glRefConfig.framebufferObject) {
		return qfalse;
	}
	n   = R_HzmLensSizeFor();
	fmt = (r_hdr->integer && glRefConfig.textureFloat) ? GL_RGBA16F_ARB : GL_RGBA8;

	tr.hzmLensColor = R_CreateImage("*hzmLens", NULL, n, n, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, fmt);
	tr.hzmLensDepth = R_CreateImage("*hzmLensDepth", NULL, n, n, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_DEPTH_COMPONENT24);
	tr.hzmLensFbo   = FBO_Create("_hzmLens", n, n);
	FBO_AttachImage(tr.hzmLensFbo, tr.hzmLensColor, GL_COLOR_ATTACHMENT0, 0);
	FBO_AttachImage(tr.hzmLensFbo, tr.hzmLensDepth, GL_DEPTH_ATTACHMENT, 0);
	R_CheckFBO(tr.hzmLensFbo);
	tr.hzmLensSize = n;
	return qtrue;
}

/*
Front end. Called from RE_RenderScene with the MAIN view's parms (already complete), right before the main
R_RenderView, so tr.viewParms ends the frame as the main view. A world scene only.
*/
void R_HzmLensRenderView(const viewParms_t *mainParms)
{
	const float *st;
	viewParms_t  lens;
	float        tanHalf, fov;

	tr.hzmLensValid = qfalse;
	R_HzmLensCvars();
	R_HzmLensCrashProbe();
	if (!R_HzmLensCapable() || !R_HzmLensStateWantsPip()) {
		return;
	}
	LENS_STAGE(1);
	if (!R_HzmLensEnsureFbo()) {
		return;
	}
	LENS_STAGE(2);
	st  = R_HzmLensState();
	fov = st[6];
	if (fov < 2.0f || fov > 120.0f) {
		return;
	}
	tanHalf = (float)tan(fov * (M_PI / 360.0));

	lens = *mainParms;
	lens.viewportX      = 0;
	lens.viewportY      = 0;
	lens.viewportWidth  = tr.hzmLensSize;
	lens.viewportHeight = tr.hzmLensSize;
	lens.fovX           = fov;
	lens.fovY           = fov;
	(void)tanHalf;
	lens.targetFbo      = tr.hzmLensFbo;
	lens.targetFboLayer = 0;
	lens.targetFboCubemapIndex = 0;
	lens.isPortal       = qfalse;
	lens.flags          = VPF_NOVIEWMODEL | VPF_NOCUBEMAPS;
	if (r_ppScopeShadows->integer) {
		lens.flags |= (mainParms->flags & VPF_USESUNLIGHT);
	}

	LENS_STAGE(10);
	R_RenderView(&lens);
	LENS_STAGE(11);
	tr.hzmLensValid = qtrue;
}

static qboolean R_HzmLensEnsureScratch(void)
{
	int w = tr.sceneWidth, h = tr.sceneHeight, fmt;

	if (tr.hzmLensScratchFbo && tr.hzmLensScratchW == w && tr.hzmLensScratchH == h) {
		return qtrue;
	}
	if (tr.hzmLensScratchFbo) {
		return qfalse; // size changed without a vid_restart: not expected; skip rather than leak another FBO
	}
	fmt = (r_hdr->integer && glRefConfig.textureFloat) ? GL_RGBA16F_ARB : GL_RGBA8;
	{
		image_t *img = R_CreateImage("*hzmLensScratch", NULL, w, h, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, fmt);
		tr.hzmLensScratchFbo = FBO_Create("_hzmLensScratch", w, h);
		FBO_AttachImage(tr.hzmLensScratchFbo, img, GL_COLOR_ATTACHMENT0, 0);
		R_CheckFBO(tr.hzmLensScratchFbo);
	}
	tr.hzmLensScratchW = w;
	tr.hzmLensScratchH = h;
	return qtrue;
}

/*
Back end. Top of RB_PostProcess, on the resolved HDR scene (srcFbo), before bloom / tone map.
*/
void RB_HZMLensComposite(FBO_t *srcFbo, ivec4_t srcBox)
{
	const float *st;
	vec4_t       color, params;

	if (!tr.hzmLensValid) {
		return;
	}
	tr.hzmLensValid = qfalse;
	R_HzmLensCvars();
	LENS_STAGE(19);
	if (!srcFbo || !tr.hzmLensShader.program || !tr.hzmLensColor || !R_HzmLensEnsureScratch()) {
		return;
	}
	if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) {
		return;
	}
	st = R_HzmLensState();
	if (st[0] <= 0.001f || st[1] <= 0.01f) {
		return;
	}

	VectorSet4(color, st[0] > 1.0f ? 1.0f : st[0], st[1], st[2], st[3]);
	VectorSet4(params, st[4], st[5], r_ppScopeCA->value, r_ppScopeOutside->value);

	LENS_STAGE(20);
	GL_BindToTMU(tr.hzmLensColor, TB_LEVELSMAP);
	FBO_SetHzmParams(params);
	FBO_Blit(srcFbo, srcBox, NULL, tr.hzmLensScratchFbo, srcBox, &tr.hzmLensShader, color, 0);
	FBO_SetHzmParams(NULL);
	LENS_STAGE(21);
	FBO_FastBlit(tr.hzmLensScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	LENS_STAGE(22);
}
