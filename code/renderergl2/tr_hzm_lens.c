/*
===========================================================================
HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE - the gl2 side.

cgame (cg_hzmscope.c) publishes ONE state cvar, r_ppScopeState (shared with the bug-3462 pass):
    "<amount> <lens radius / screen height> <pupil x> <pupil y> <pupil radius> <glass warmth> <wide fov>"
A wide fov > 0 means "picture-in-picture". ROLE SWAP: the MAIN view is the magnified scope picture (cgame gives it the optic's fov, so it
gets MSAA, sun shadows, SSAO - everything the bug-3462 scope picture always had) and this file adds, before it in RE_RenderScene:
  1. the WIDE view (R_HzmLensRenderView): the same ray at the hip fov, into its own FBO (scene size x r_ppScopeWide). It shares the scene
     lists exactly as a mirror / portal sub-view does. No viewmodel, no cubemaps, no dynamic sun-shadow receive. It is blurred and dimmed
     in the composite, so it needs no MSAA and no extras.
  2. the RIFLE LAYER: the view weapon and arms alone (VPF_ONLYVIEWMODEL) at the wide weapon projection into a second FBO (colour + depth).
  The main view then leaves the rifle out.
  RB_HZMLensComposite (back end, top of the post chain, on the HDR scene, before bloom / tone map): inside the lens circle the main (scope)
  picture with barrel / fringe / tint / inner shadow; outside it the wide world, blurred and darkened; the rifle layer crisp over it - the
  real scope body is the frame around the lens. Exposure, bloom and grade treat it all as one picture.
A wide fov of 0 (or amount 0) leaves everything to the old bug-3462 pass (tr_hzm_scope.c).

Cvars (flags 0): r_ppScopePip (1; 0 = never), r_ppScopeWide (1: wide view size relative to the scene), r_ppScopeShadows (0),
r_ppScopeOutside (0.75: brightness of the world around the lens), r_ppScopeCA (shared; 10 / 20 / 30 = debug views).
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
static cvar_t *r_ppScopeWide    = NULL;   // size of the wide view relative to the scene (1 = full)
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
		r_ppScopeWide    = ri.Cvar_Get("r_ppScopeWide", "1", 0);
		r_ppScopeShadows = ri.Cvar_Get("r_ppScopeShadows", "0", 0);
		r_ppScopeOutside = ri.Cvar_Get("r_ppScopeOutside", "0.9", 0);
		r_ppScopeBlur    = ri.Cvar_Get("r_ppScopeBlur", "1", 0);
		r_ppScopeCA      = ri.Cvar_Get("r_ppScopeCA", "0.5", 0);
	}
}

// the parsed r_ppScopeState, refreshed only when its modificationCount moves
static const float *R_HzmLensState(void)
{
	static int   s_mod = -1;
	static float s_st[9];

	R_HzmLensCvars();
	if (r_ppScopeState->modificationCount != s_mod) {
		s_mod = r_ppScopeState->modificationCount;
		memset(s_st, 0, sizeof(s_st));
		sscanf(r_ppScopeState->string, "%f %f %f %f %f %f %f %f %f", &s_st[0], &s_st[1], &s_st[2], &s_st[3], &s_st[4], &s_st[5], &s_st[6], &s_st[7], &s_st[8]);
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

static qboolean R_HzmLensEnsureGun(void)
{
	int fmt;

	if (tr.hzmGunFbo) {
		return (tr.hzmGunW == tr.sceneWidth && tr.hzmGunH == tr.sceneHeight) ? qtrue : qfalse;
	}
	if (!glRefConfig.framebufferObject || tr.sceneWidth <= 0) {
		return qfalse;
	}
	fmt = (r_hdr->integer && glRefConfig.textureFloat) ? GL_RGBA16F_ARB : GL_RGBA8;
	tr.hzmGunW = tr.sceneWidth;
	tr.hzmGunH = tr.sceneHeight;
	tr.hzmGunColor = R_CreateImage("*hzmGun", NULL, tr.hzmGunW, tr.hzmGunH, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, fmt);
	tr.hzmGunDepth = R_CreateImage("*hzmGunDepth", NULL, tr.hzmGunW, tr.hzmGunH, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_DEPTH_COMPONENT24);
	tr.hzmGunFbo   = FBO_Create("_hzmGun", tr.hzmGunW, tr.hzmGunH);
	FBO_AttachImage(tr.hzmGunFbo, tr.hzmGunColor, GL_COLOR_ATTACHMENT0, 0);
	FBO_AttachImage(tr.hzmGunFbo, tr.hzmGunDepth, GL_DEPTH_ATTACHMENT, 0);
	R_CheckFBO(tr.hzmGunFbo);
	return qtrue;
}

static qboolean R_HzmLensEnsureFbo(void)
{
	int w, h, fmt;

	if (tr.hzmLensFbo) {
		return qtrue;
	}
	if (!glRefConfig.framebufferObject || tr.sceneWidth <= 0) {
		return qfalse;
	}
	w = (int)(tr.sceneWidth  * r_ppScopeWide->value + 0.5f) & ~1;
	h = (int)(tr.sceneHeight * r_ppScopeWide->value + 0.5f) & ~1;
	if (w < 256 || h < 144) {
		w = tr.sceneWidth;
		h = tr.sceneHeight;
	}
	fmt = (r_hdr->integer && glRefConfig.textureFloat) ? GL_RGBA16F_ARB : GL_RGBA8;

	tr.hzmLensColor = R_CreateImage("*hzmWide", NULL, w, h, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, fmt);
	tr.hzmLensDepth = R_CreateImage("*hzmWideDepth", NULL, w, h, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_DEPTH_COMPONENT24);
	tr.hzmLensFbo   = FBO_Create("_hzmWide", w, h);
	FBO_AttachImage(tr.hzmLensFbo, tr.hzmLensColor, GL_COLOR_ATTACHMENT0, 0);
	FBO_AttachImage(tr.hzmLensFbo, tr.hzmLensDepth, GL_DEPTH_ATTACHMENT, 0);
	R_CheckFBO(tr.hzmLensFbo);
	tr.hzmLensW = w;
	tr.hzmLensH = h;
	tr.hzmLensSize = w;
	return qtrue;
}

/*
Front end. Called from RE_RenderScene with the MAIN view's parms (already complete), right before the main
R_RenderView, so tr.viewParms ends the frame as the main view. A world scene only.
*/
void R_HzmLensRenderView(viewParms_t *mainParms)
{
	const float *st;
	viewParms_t  lens;
	float        wideTanY, aspect, fov;

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
	if (fov < 20.0f || fov > 130.0f) {
		return;
	}
	// ROLE SWAP: the MAIN view is the magnified scope picture (cgame gives it the optic's fov); this second view is the WIDE hip-fov world
	// around it, along the same ray, same aspect. st[6] is the pre-aspect hip fov (cgame fov_x convention: the 4:3 reference, vertical
	// half-tan = 0.75 x tan(fov / 2)); the main view's own fovX / fovY give the aspect.
	aspect   = (float)tan(mainParms->fovX * (M_PI / 360.0)) / (float)tan(mainParms->fovY * (M_PI / 360.0));
	wideTanY = 0.75f * (float)tan(fov * (M_PI / 360.0));

	lens = *mainParms;
	lens.viewportX      = 0;
	lens.viewportY      = 0;
	lens.viewportWidth  = tr.hzmLensW;
	lens.viewportHeight = tr.hzmLensH;
	lens.fovX           = 2.0f * (float)atan(wideTanY * aspect) * (180.0f / (float)M_PI);
	lens.fovY           = 2.0f * (float)atan(wideTanY) * (180.0f / (float)M_PI);
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

	// the RIFLE LAYER: the view weapon and arms alone, at the main view's size and projection, into their own target - the composite keeps them
	// crisp over the blurred world (the real scope body around the lens). The main view then leaves them out.
	tr.hzmGunValid = qfalse;
	if (R_HzmLensEnsureGun()) {
		viewParms_t gun = *mainParms;

		gun.viewportX = 0;
		gun.viewportY = 0;
		gun.viewportWidth  = tr.hzmGunW;
		gun.viewportHeight = tr.hzmGunH;
		gun.targetFbo      = tr.hzmGunFbo;
		gun.targetFboLayer = 0;
		gun.targetFboCubemapIndex = 0;
		gun.isPortal       = qfalse;
		gun.flags          = VPF_ONLYVIEWMODEL | VPF_NOCUBEMAPS;
		LENS_STAGE(12);
		R_RenderView(&gun);
		LENS_STAGE(13);
		tr.hzmGunValid = qtrue;
		mainParms->flags |= VPF_NOVIEWMODEL;
	}
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
	{
		vec4_t vi;
		VectorSet4(vi, st[7], st[8], 0.0f, 0.0f);   // the lens circle centre (it follows the rifle's eyepiece), screen-height units, +y down
		GLSL_BindProgram(&tr.hzmLensShader);
		GLSL_SetUniformVec4(&tr.hzmLensShader, UNIFORM_VIEWINFO, vi);
	}
	GL_BindToTMU(tr.hzmLensColor, TB_LEVELSMAP);
	if (tr.hzmGunValid && tr.hzmGunColor && tr.hzmGunDepth) {   // the rifle layer (colour + depth = its coverage mask)
		GL_BindToTMU(tr.hzmGunColor, TB_NORMALMAP);
		GL_BindToTMU(tr.hzmGunDepth, TB_DELUXEMAP);
	} else {
		GL_BindToTMU(tr.whiteImage, TB_NORMALMAP);
		GL_BindToTMU(tr.whiteImage, TB_DELUXEMAP);   // depth 1 everywhere = no rifle
	}
	tr.hzmGunValid = qfalse;
	FBO_SetHzmParams(params);
	FBO_Blit(srcFbo, srcBox, NULL, tr.hzmLensScratchFbo, srcBox, &tr.hzmLensShader, color, 0);
	FBO_SetHzmParams(NULL);
	LENS_STAGE(21);
	FBO_FastBlit(tr.hzmLensScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	LENS_STAGE(22);
}
