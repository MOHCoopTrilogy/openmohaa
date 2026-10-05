/*
===========================================================================
Copyright (C) 2026 HaZardModding coop - OpenMOHAA gl2 renderer

This file is part of OpenMOHAA source code.

OpenMOHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/
// tr_msaa.c - HZM gl2 MSAA core, plan P2a (infrastructure).
//
// Switch: r_msaaOverride (flags 0, never archived).
//   ""        today's pipeline, verbatim (FBO_Init's legacy branch, driven by r_ext_framebuffer_multisample,
//             including its write-back). This is the pre-flip default.
//   "legacy"  the same, by name (diagnostic after the flip).
//   -1        new path, Auto (tiers below).       0  new path, MSAA off (== today's MSAA-0 pipeline).
//   2/4/8     new path, explicit (clamped by render scale and the GL caps).
//
// The new path renders the 3D scene into multisample TEXTURES (tr.sceneColorMSImage / tr.sceneDepthMSImage,
// so P2b can resolve them with shaders), presents through a single-sample DISPLAY FBO (tr.displaySplit - the
// HUD is never multisampled), asks for a single-sample window, and NEVER writes a user cvar: the resolved count
// goes to tr.msaaSamples and to ROM r_msaaActive.
//
// P2b shader resolves (R_MsaaResolvesActive; otherwise every site keeps its P2a blit):
//   depth (msaa_depthresolve): the nearest sample into renderDepthImage + (min, max) into msaaDepthMinMaxImage - after
//     the prepass, after the opaque list (before sun rays and flares), at the soft-particle snapshot, at the post head;
//   colour (msaa_colorresolve, MRT) at the post head, after SSAO generation: out_Color = the tone-exact input
//     (clamp on the grade path, Karis on Hable) with AO applied per sample, out_Linear = the linear average * AO, which
//     bloom, DoF and the exposure measure read (R_MsaaLinearSource); the separate AO composite is skipped;
//   readers: soft particles (two-surface fade), underwater and the screen fog (max depth) through MSAA_DEPTH_MINMAX.
//
// r_msaaBypass (flags 0, live, the HOME key): on the new path at >= 2 samples the scene renders into the
// single-sample tr.msaaResolveFbo instead and every resolve is skipped - i.e. the MSAA-0 pipeline, in-process.
//
// Auto (ME-F14): >= 10 GB VRAM and the 8x buffers within 5% of it -> 8; 4-10 GB, unknown VRAM or over that cap
// -> 4; < 4 GB -> 2; Intel iGPU (Arc counts as discrete) -> 0; r_renderScale >= 1.5 -> 0. An explicit value is
// clamped to 4 above scale 1.0 and to 2 from scale 1.5. Everything is clamped to the GL caps (GL_MAX_SAMPLES,
// GL_MAX_COLOR/DEPTH_TEXTURE_SAMPLES). Off, with the reason printed, without ARB_texture_multisample or
// GLSL 1.50, with r_dlightMode >= 2 or r_skeldiag (ME-F18: both read the scene buffer directly), and after a
// failure (r_msaaForcedOff) until the setting changes.
//
// Failure handling: an incomplete multisample FBO halves the sample count, then falls to 0 (ME-F9); an MSAA
// program that fails to build re-initialises the renderer in place at 0 (ME-F3, R_Msaa_AfterGLSL). Either way
// to 0 records the setting in ROM r_msaaForcedOff, which holds MSAA off until the setting changes.
//
// Dev-only cvars (flags 0, default 0, for the Auto matrix and the fallback proofs): r_msaaAutoDebugVramMB
// (>0 pretend that much VRAM, -1 pretend unknown), r_msaaAutoDebugIntel 1, r_msaaAutoDebugMaxSamples N,
// r_msaaDebugFailCompile 1, r_msaaDebugFailFbo N (treat the MS scene FBO as incomplete at >= N samples).

#include "tr_local.h"
#include "tr_fbo.h"
#include "tr_msaa_decide.h"

// tr_fbo.c defines these without a header prototype
FBO_t    *FBO_Create(const char *name, int width, int height);
qboolean  R_CheckFBO(const FBO_t *fbo);

#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE           0x9100
#endif
#ifndef GL_TEXTURE_BINDING_2D_MULTISAMPLE
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE   0x9104
#endif
#ifndef GL_MAX_COLOR_TEXTURE_SAMPLES
#define GL_MAX_COLOR_TEXTURE_SAMPLES        0x910E
#endif
#ifndef GL_MAX_DEPTH_TEXTURE_SAMPLES
#define GL_MAX_DEPTH_TEXTURE_SAMPLES        0x910F
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES                      0x8D57
#endif
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#endif

// optional GL entry points, loaded by GLimp_InitExtraExtensions (NULL = feature off)
hzmTexImage2DMultisample_t          qglTexImage2DMultisample;
hzmAlphaToCoverageDitherControlNV_t qglAlphaToCoverageDitherControlNV;
hzmDrawBuffers_t                    qglDrawBuffers;
hzmBindFragDataLocation_t           qglBindFragDataLocation;

cvar_t *r_msaaOverride;
cvar_t *r_msaaBypass;
cvar_t *r_alphaToCoverage;
cvar_t *r_msaaCentroid;
cvar_t *r_msaaShadowMatch;
cvar_t *r_msaaAutoDebugVramMB;
cvar_t *r_msaaAutoDebugIntel;
cvar_t *r_msaaAutoDebugMaxSamples;
cvar_t *r_msaaDebugFailCompile;
cvar_t *r_msaaDebugFailFbo;
static cvar_t *r_msaaForcedOffCv;   // ROM: the setting that failed; MSAA stays off until it changes
static cvar_t *r_gpuVramMBCv;       // ROM: dedicated VRAM in MB, 0 = not queried yet, -1 = unknown

// renderer_reinit plan 6, rule 1 (see tr_local.h): read only at R_Init, flags 0 -> a kept renderer snapshots these.
// r_skeldiag is registered elsewhere but R_DecideMsaa reads it at init (ME-F18).
const char *const hzmMsaaInitReadCvars[] = {
	"r_msaaOverride",
	"r_msaaAutoDebugVramMB",
	"r_msaaAutoDebugIntel",
	"r_msaaAutoDebugMaxSamples",
	"r_msaaDebugFailCompile",
	"r_msaaDebugFailFbo",
	"r_skeldiag",
	"r_shadowFboDummy",   // P1b, read at FBO_Init
	"r_shadowHarden",     // P1a, read at FBO_Init (the clears) as well as per frame
	"r_msaaCentroid",     // P4b, a GLSL define
	NULL
};

// the switches that must never be saved (V3-C1): a `seta` in a config would outlive the kill switch
static const struct {
	const char *name;
	const char *def;
} s_msaaSwitches[] = {
	{ "r_msaaOverride",    ""  },
	{ "r_msaaBypass",      "0" },
	{ "r_shadowStable",    "0" },
	{ "r_shadowHarden",    "0" },
	{ "r_shadowFboDummy",  "1" },
	{ "r_alphaToCoverage", "1" },
	{ "r_msaaCentroid",    "1" },
	{ "r_msaaShadowMatch", "1" },
	{ "r_hzmPshadowCharGate",  "-1" },   // P3 E1 (bug-3009)
	{ "r_hzmSunStaticCasters", "1" },    // P3 B0 (a W re-bake trigger)
	{ "r_shadowWFaces",        "1" },    // P3 D2 dev switch
};

static struct {
	char  setting[32];   // r_msaaOverride as decided at this R_Init
	char  reason[192];   // why n is what it is (appended to by the fallbacks)
	int   want;          // parsed setting: -1 Auto, 0, or the explicit count
	int   caps;          // min(GL_MAX_SAMPLES, MAX_COLOR/DEPTH_TEXTURE_SAMPLES), after the debug override
	int   vramMB;        // VRAM used by the decision (-1 unknown)
	float scale;         // r_renderScale as R_CreateBuiltinImages will clamp it
	int   decided;       // samples chosen by R_DecideMsaa, before any fallback
} s_ms;

/*
=================
R_Msaa_Register - at every R_Init (R_Register)
=================
*/
void R_Msaa_Register(void)
{
	int i;

	r_msaaOverride            = ri.Cvar_Get("r_msaaOverride", "", 0);
	r_msaaBypass              = ri.Cvar_Get("r_msaaBypass", "0", 0);
	r_msaaAutoDebugVramMB     = ri.Cvar_Get("r_msaaAutoDebugVramMB", "0", 0);
	r_msaaAutoDebugIntel      = ri.Cvar_Get("r_msaaAutoDebugIntel", "0", 0);
	r_msaaAutoDebugMaxSamples = ri.Cvar_Get("r_msaaAutoDebugMaxSamples", "0", 0);
	r_msaaDebugFailCompile    = ri.Cvar_Get("r_msaaDebugFailCompile", "0", 0);
	r_msaaDebugFailFbo        = ri.Cvar_Get("r_msaaDebugFailFbo", "0", 0);

	// ROM, written by the renderer at every R_Init (V3-OK8); they outlive the DLL, which a vid_restart reloads
	ri.Cvar_Get("r_msaaActive", "0", CVAR_ROM);
	r_gpuVramMBCv     = ri.Cvar_Get("r_gpuVramMB", "0", CVAR_ROM);
	r_msaaForcedOffCv = ri.Cvar_Get("r_msaaForcedOff", "", CVAR_ROM);

	// V3-C1: a kill-switch cvar that carries CVAR_ARCHIVE came from a `seta` in someone's config. It would
	// silently survive the kill switch, so say so on every R_Init (G3 fails on this line). The later-phase
	// switches are registered here with their planned defaults so the check can see them.
	for (i = 0; i < (int)ARRAY_LEN(s_msaaSwitches); i++)
	{
		cvar_t *cv = ri.Cvar_Get(s_msaaSwitches[i].name, s_msaaSwitches[i].def, 0);

		if (cv && (cv->flags & CVAR_ARCHIVE))
		{
			ri.Printf(PRINT_ALL, "^~^~^ GFX WARN %s archived (value \"%s\") - a switch cvar must never be saved; remove its seta line\n",
				s_msaaSwitches[i].name, cv->string);
		}
	}

	// P4 (registered with their defaults by the loop above)
	r_alphaToCoverage = ri.Cvar_Get("r_alphaToCoverage", "1", 0);
	r_msaaCentroid    = ri.Cvar_Get("r_msaaCentroid", "1", 0);
	r_msaaShadowMatch = ri.Cvar_Get("r_msaaShadowMatch", "1", 0);
}

/*
=================
P4a helpers
=================
*/
// the matte of an RGBA cutout: mean luma of its transparent texels (alpha < 128). Too few of them = not a cutout.
int R_HzmLightMatte(const byte *pic, int width, int height)
{
	size_t n = (size_t)width * (size_t)height, i, cnt = 0;
	double sum = 0.0;

	if (!pic || n == 0)
	{
		return -1;
	}
	for (i = 0; i < n; i++)
	{
		const byte *p = pic + i * 4;
		if (p[3] < 128)
		{
			cnt++;
			sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
		}
	}
	if (cnt < n / 200 + 1)
	{
		return -1;
	}
	return (sum / (double)cnt > 128.0) ? 1 : 0;
}

// P4c (ME-F4, r_msaaShadowMatch, live): lightall's shadow-mask matching inputs for one draw. The mask is resolved
// at every pixel's NEAREST sample; with the (min, max) depth image of THIS frame bound on TB_SCREENDEPTH, a fragment
// of the farther surface in an edge pixel takes the neighbour texel whose depth is its own. Uploads 0 (= off, today)
// on the bypass / legacy / MSAA-off paths, off the main scene, and whenever the image is not this frame's.
void RB_MsaaShadowMatch(shaderProgram_t *sp)
{
	vec4_t v = { 0.0f, 0.0f, 0.0f, 0.0f };

	if (r_msaaShadowMatch && r_msaaShadowMatch->integer && R_MsaaDepthMinMaxOr(NULL) != NULL
	    && backEnd.viewParms.targetFbo == NULL && !backEnd.viewParms.isPortal && !backEnd.viewParms.isPortalSky
	    && !(backEnd.refdef.rdflags & RDF_NOWORLDMODEL) && tr.sceneWidth > 0 && tr.sceneHeight > 0)
	{
		vec2_t inv;
		inv[0] = 1.0f / (float)tr.sceneWidth;
		inv[1] = 1.0f / (float)tr.sceneHeight;
		GL_BindToTMU(tr.msaaDepthMinMaxImage, TB_SCREENDEPTH);
		GLSL_SetUniformVec2(sp, UNIFORM_INVTEXRES, inv);
		v[0] = 1.0f;
		v[1] = 1.5f;
	}
	GLSL_SetUniformVec4(sp, UNIFORM_HZMSHADOWMATCH, v);
}

// P4a (MH-B2, research step 6): the u_AlphaTest mode for one stage, 0 = today's. Only keep-above-half cutouts with no
// blend and no depthFunc, drawn into the MULTISAMPLED scene (new path, >= 2 samples, the HOME bypass off - so the
// bypass restores today's per-stage 1/2/3 exactly, V3-C5), never 2D, never a depth-only pass (shadow maps and the
// z-prepass keep a plain discard). 4 = coverage ramp centred on 0.5 (dark/opaque mattes), 5 = the opaque-side
// ramp for DETECTED light (white) mattes - it never shows a texel more contaminated than today. An unknown matte
// (compressed upload) takes 4: the foliage lab measured 5 thinning distant trees to ~0.77 density (foliage plan).
int RB_MsaaA2CStageMode(const shaderStage_t *pStage)
{
	int atest = pStage->stateBits & GLS_ATEST_BITS;
	const image_t *img;

	if (!r_alphaToCoverage || !r_alphaToCoverage->integer || !R_MsaaSceneIsMultisampled())
	{
		return 0;
	}
	if (backEnd.projection2D || backEnd.depthFill || backEnd.viewParms.targetFbo != NULL
	    || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL))
	{
		return 0;
	}
	if (atest != GLS_ATEST_GE_80 && atest != GLS_ATEST_GE_FOLIAGE1 && atest != GLS_ATEST_GE_FOLIAGE2)
	{
		return 0;
	}
	if (pStage->stateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHFUNC_BITS))
	{
		return 0;
	}
	img = pStage->bundle[0].image[0];
	return (img && img->hzmLightMatte == 1) ? 5 : 4;   // unknown matte -> centred (foliage lab: mode 5 thins far trees to ~0.77 density)
}

/*
=================
R_MsaaParseSetting

"" / "legacy" -> qfalse (today's pipeline). An integer -> qtrue, *want = -1 (Auto) / 0 / the count.
Anything else -> qfalse, with a warning.
=================
*/
static qboolean R_MsaaParseSetting(const char *s, int *want, qboolean warn)
{
	const char *p = s;

	*want = 0;
	if (!s || !*s || !Q_stricmp(s, "legacy"))
	{
		return qfalse;
	}
	if (*p == '-')
	{
		p++;
	}
	if (!*p)
	{
		p = NULL;
	}
	while (p && *p)
	{
		if (*p < '0' || *p > '9')
		{
			p = NULL;
			break;
		}
		p++;
	}
	if (!p)
	{
		if (warn)
		{
			ri.Printf(PRINT_WARNING, "r_msaaOverride \"%s\" is not legacy/-1/0/2/4/8 - using today's pipeline\n", s);
		}
		return qfalse;
	}
	*want = atoi(s);
	if (*want < 0)
	{
		*want = -1;
	}
	return qtrue;
}

/*
=================
R_MsaaNewPathRequested - from cvars only (InitOpenGL asks before the window, and so the context, exists)
=================
*/
qboolean R_MsaaNewPathRequested(void)
{
	int want;

	return R_MsaaParseSetting(r_msaaOverride ? r_msaaOverride->string : "", &want, qfalse);
}

// the dedicated VRAM, cached in ROM r_gpuVramMB once per process (the cvar outlives the DLL). -1 = unknown.
static int R_MsaaVramMB(void)
{
	int mb = r_gpuVramMBCv ? r_gpuVramMBCv->integer : 0;

	if (mb == 0)
	{
		GLint kb = 0;

		mb = -1;
		if (glRefConfig.memInfo == MI_NVX)
		{
			qglGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kb);
			if (kb > 0)
			{
				mb = kb / 1024;
			}
		}
		// GL_ATI_meminfo only reports FREE memory, not the size of the card: leave it unknown (Auto -> 4)
		ri.Cvar_Set("r_gpuVramMB", va("%d", mb));
	}
	return mb;
}

static qboolean R_MsaaIntelIGpu(void)
{
	const char *renderer;

	if (r_msaaAutoDebugIntel && r_msaaAutoDebugIntel->integer)
	{
		return qtrue;
	}
	if (!glRefConfig.intelGraphics)
	{
		return qfalse;
	}
	renderer = (const char *)qglGetString(GL_RENDERER);
	return (qboolean)!(renderer && strstr(renderer, "Arc"));   // Arc is a discrete card
}

static void R_MsaaReason(const char *fmt, ...)
{
	va_list argptr;
	char    text[128];
	size_t  len = strlen(s_ms.reason);

	va_start(argptr, fmt);
	Q_vsnprintf(text, sizeof(text), fmt, argptr);
	va_end(argptr);

	if (len && len + 1 < sizeof(s_ms.reason))
	{
		Q_strcat(s_ms.reason, sizeof(s_ms.reason), ",");
	}
	Q_strcat(s_ms.reason, sizeof(s_ms.reason), text);
}

/*
=================
R_DecideMsaa

Between InitOpenGL and R_InitImages (ME-F7): every image, FBO and MSAA-only GLSL define depends on it.
Sets tr.msaaNewPath, tr.msaaSamples and tr.displaySplit. Writes no user cvar.
=================
*/
void R_DecideMsaa(void)
{
	int         n = 0;
	GLint       v;
	const char *forced;

	Com_Memset(&s_ms, 0, sizeof(s_ms));
	tr.msaaNewPath      = qfalse;
	tr.msaaSamples      = 0;
	tr.displaySplit     = qfalse;
	tr.msaaBypassActive = qfalse;

	Q_strncpyz(s_ms.setting, r_msaaOverride->string, sizeof(s_ms.setting));
	s_ms.vramMB = -1;

	if (!R_MsaaParseSetting(s_ms.setting, &s_ms.want, qtrue))
	{
		R_MsaaReason(s_ms.setting[0] ? "override-legacy" : "pre-flip-default");
		return;   // today's pipeline: FBO_Init's legacy branch decides everything, exactly as before
	}
	tr.msaaNewPath = qtrue;

	// the render scale exactly as R_CreateBuiltinImages will clamp it
	s_ms.scale = r_renderScale ? r_renderScale->value : 1.0f;
	if (s_ms.scale < 0.5f) s_ms.scale = 0.5f;
	if (s_ms.scale > 2.0f) s_ms.scale = 2.0f;

	// the GL caps, for every value (a debug cap lowers them)
	s_ms.caps = 0;
	if (glRefConfig.framebufferMultisample)
	{
		v = 0; qglGetIntegerv(GL_MAX_SAMPLES, &v); s_ms.caps = v;
	}
	if (glRefConfig.textureMultisample)
	{
		v = 0; qglGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &v); if (v < s_ms.caps) s_ms.caps = v;
		v = 0; qglGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &v); if (v < s_ms.caps) s_ms.caps = v;
	}
	if (r_msaaAutoDebugMaxSamples->integer > 0 && r_msaaAutoDebugMaxSamples->integer < s_ms.caps)
	{
		s_ms.caps = r_msaaAutoDebugMaxSamples->integer;
	}

	// hard requirements: any one of these keeps MSAA off on the new path (which is then today's MSAA-0 image)
	if (!glRefConfig.framebufferObject || !glRefConfig.framebufferBlit || !glRefConfig.framebufferMultisample)
	{
		R_MsaaReason("no-fbo-multisample");
		goto decided;
	}
	if (qglesMajorVersion)
	{
		R_MsaaReason("gles");
		goto decided;
	}
	if (!glRefConfig.textureMultisample)
	{
		R_MsaaReason("no-ARB_texture_multisample");
		goto decided;
	}
	if (glRefConfig.glslMajorVersion < 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion < 50))
	{
		R_MsaaReason("glsl<1.50");
		goto decided;
	}
	if (r_dlightMode && r_dlightMode->integer >= 2)
	{
		R_MsaaReason("dlightMode>=2");   // ME-F18: its cubemaps CopyTexSubImage from the scene buffer
		goto decided;
	}
	if (ri.Cvar_VariableIntegerValue("r_skeldiag") > 0)
	{
		R_MsaaReason("skeldiag");        // ME-F18: SKELPIX glReadPixels the bound scene buffer
		goto decided;
	}

	// a failure earlier in this process holds MSAA off until the setting changes; then it is retried once
	forced = r_msaaForcedOffCv ? r_msaaForcedOffCv->string : "";
	if (forced && *forced)
	{
		if (!strcmp(forced, s_ms.setting))
		{
			R_MsaaReason("forced-off-after-failure(r_msaaForcedOff)");
			goto decided;
		}
		ri.Cvar_Set("r_msaaForcedOff", "");
		R_MsaaReason("retry-after-setting-change");
	}

	{
		// the pure decision (tr_msaa_decide.h, unit-tested offline): Auto tiers, explicit scale clamp, caps clamp
		hzmMsaaIn_t  in;
		hzmMsaaOut_t out;

		if (s_ms.want < 0)
		{
			if (r_msaaAutoDebugVramMB->integer > 0)
				s_ms.vramMB = r_msaaAutoDebugVramMB->integer;
			else if (r_msaaAutoDebugVramMB->integer < 0)
				s_ms.vramMB = -1;
			else
				s_ms.vramMB = R_MsaaVramMB();
		}

		in.want      = s_ms.want;
		in.scale     = s_ms.scale;
		in.caps      = s_ms.caps;
		in.vramMB    = s_ms.vramMB;
		in.sceneW    = (int)(glConfig.vidWidth  * s_ms.scale + 0.5f);
		in.sceneH    = (int)(glConfig.vidHeight * s_ms.scale + 0.5f);
		in.colorBpp  = (r_hdr->integer && glRefConfig.textureFloat) ? 8 : 4;
		in.intelIGpu = (s_ms.want < 0) ? (int)R_MsaaIntelIGpu() : 0;
		HZM_MsaaDecide(&in, &out);

		n = out.n;
		if (s_ms.want < 0)
			R_MsaaReason("%s(8x=%dMB)", out.tier, out.mb8x);
		else
			R_MsaaReason("%s", out.tier);
		if (out.clamp)
			R_MsaaReason("%s", out.clamp);
		if (out.capped)
			R_MsaaReason("caps:%d", s_ms.caps);
	}

decided:
	s_ms.decided          = n;
	tr.msaaSamples      = n;
	tr.displaySplit     = (qboolean)(n >= 2);
}

// the multisample scene images (R_CreateImageMS, tr_image.c) - made once per R_Init, and re-entrant: a second
// FBO_Init in the same lifetime (the compile fallback, or a renderer kept across map loads) reuses the slots,
// regenerates a released texture name, and re-sizes them to the current scene before the storage is specified
static void R_MsaaEnsureImage(image_t **slot, const char *name, int w, int h, int internalFormat)
{
	image_t *image = *slot;

	if (!image)
	{
		*slot = R_CreateImageMS(name, w, h, internalFormat);
		return;
	}
	if (!image->texnum)
	{
		qglGenTextures(1, &image->texnum);
	}
	image->width          = image->uploadWidth  = w;
	image->height         = image->uploadHeight = h;
	image->internalFormat = internalFormat;
}

// (re)specify a multisample texture's storage. Raw bind on the active unit's MULTISAMPLE target, restored after,
// so the renderer's per-unit 2D binding cache (tr_dsa.c) stays valid. No texture parameters: MS textures have none.
static void R_MsaaSpecifyStorage(image_t *image, int samples)
{
	GLint prev = 0;

	qglGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &prev);
	qglBindTexture(GL_TEXTURE_2D_MULTISAMPLE, image->texnum);
	qglTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, image->internalFormat,
	                         image->uploadWidth, image->uploadHeight, GL_TRUE);
	qglBindTexture(GL_TEXTURE_2D_MULTISAMPLE, (GLuint)prev);
}

static void R_MsaaDrainErrors(void)
{
	int i;

	for (i = 0; i < 16 && qglGetError() != GL_NO_ERROR; i++)
	{
	}
}

static void R_MsaaReleaseImages(void)
{
	image_t *img[2];
	int      i;

	img[0] = tr.sceneColorMSImage;
	img[1] = tr.sceneDepthMSImage;
	for (i = 0; i < 2; i++)
	{
		if (img[i] && img[i]->texnum)
		{
			qglDeleteTextures(1, &img[i]->texnum);
			img[i]->texnum = 0;
		}
	}
}

/*
=================
R_MsaaInitSceneFbos - FBO_Init, new path at >= 2 samples

The scene target from multisample textures, with the ME-F9 fallback: a GL error or an incomplete FBO halves the
sample count, and below 2 the new path runs at 0 (FBO_Init then builds today's single-sample scene target).
Also builds msaaResolveFbo (renderImage + renderDepthImage, as the legacy path does) and the depth-resolve target
P2b draws into (colour msaaDepthMinMaxImage, depth renderDepthImage).
Returns qtrue when tr.sceneFbo is the multisample target.
=================
*/
qboolean R_MsaaInitSceneFbos(int hdrFormat)
{
	int    n = tr.msaaSamples;
	int    w = tr.renderImage->width;
	int    h = tr.renderImage->height;
	FBO_t *fbo;

	if (!tr.msaaNewPath || n < 2 || !qglTexImage2DMultisample)
	{
		return qfalse;
	}

	R_MsaaEnsureImage(&tr.sceneColorMSImage, "*sceneColorMS", w, h, hdrFormat);
	R_MsaaEnsureImage(&tr.sceneDepthMSImage, "*sceneDepthMS", w, h, GL_DEPTH_COMPONENT24);

	fbo = FBO_Create("_sceneMS", w, h);

	while (n >= 2)
	{
		GLenum   err;
		qboolean complete, forced;

		R_MsaaDrainErrors();
		R_MsaaSpecifyStorage(tr.sceneColorMSImage, n);
		R_MsaaSpecifyStorage(tr.sceneDepthMSImage, n);
		err = qglGetError();

		FBO_AttachImage(fbo, tr.sceneColorMSImage, GL_COLOR_ATTACHMENT0, 0);
		FBO_AttachImage(fbo, tr.sceneDepthMSImage, GL_DEPTH_ATTACHMENT, 0);

		forced   = (qboolean)(r_msaaDebugFailFbo->integer >= 2 && n >= r_msaaDebugFailFbo->integer);
		complete = (qboolean)(!forced && err == GL_NO_ERROR && R_CheckFBO(fbo));
		if (complete)
		{
			break;
		}

		ri.Printf(PRINT_ALL, "^~^~^ MSAA F9 n=%d unusable (glError 0x%x forced=%d) - %s\n",
			n, err, forced, n >= 4 ? "halving" : "falling to 0");
		R_MsaaReason("fbo-fail@%d", n);
		n >>= 1;
	}

	if (n < 2)
	{
		// nothing usable: detach, free the storage, and let FBO_Init build today's MSAA-0 scene target
		qglNamedFramebufferTexture2DEXT(fbo->frameBuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, 0, 0);
		qglNamedFramebufferTexture2DEXT(fbo->frameBuffer, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D_MULTISAMPLE, 0, 0);
		R_MsaaReleaseImages();
		tr.msaaSamples  = 0;
		tr.displaySplit = qfalse;
		ri.Cvar_Set("r_msaaForcedOff", s_ms.setting);
		return qfalse;
	}

	tr.msaaSamples  = n;
	tr.sceneFbo     = fbo;
	tr.msaaSceneFbo = fbo;

	tr.msaaResolveFbo = FBO_Create("_msaaResolve", w, h);
	FBO_AttachImage(tr.msaaResolveFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0);
	FBO_AttachImage(tr.msaaResolveFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0);
	R_CheckFBO(tr.msaaResolveFbo);
	tr.msaaResolveSSFbo = tr.msaaResolveFbo;

	if (tr.msaaDepthMinMaxImage)
	{
		tr.msaaDepthResolveFbo = FBO_Create("_msaaDepthResolve", w, h);
		FBO_AttachImage(tr.msaaDepthResolveFbo, tr.msaaDepthMinMaxImage, GL_COLOR_ATTACHMENT0, 0);
		FBO_AttachImage(tr.msaaDepthResolveFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0);
		if (!R_CheckFBO(tr.msaaDepthResolveFbo))
		{
			tr.msaaDepthResolveFbo = NULL;   // no shader depth resolve: the blit resolves stay (P2a behaviour)
		}
	}

	// P2b: the MRT colour-resolve target (out_Color -> renderImage, out_Linear -> sceneLinearImage) and the
	// sceneLinear source view that bloom / DoF / the exposure measure read. The draw-buffer list is FBO state, set once.
	if (tr.sceneLinearImage && qglDrawBuffers)
	{
		static const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

		tr.msaaColorResolveFbo = FBO_Create("_msaaColorResolve", w, h);
		FBO_AttachImage(tr.msaaColorResolveFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0);
		FBO_AttachImage(tr.msaaColorResolveFbo, tr.sceneLinearImage, GL_COLOR_ATTACHMENT1, 0);
		FBO_Bind(tr.msaaColorResolveFbo);
		qglDrawBuffers(2, bufs);
		if (!R_CheckFBO(tr.msaaColorResolveFbo))
		{
			tr.msaaColorResolveFbo = NULL;
		}
		FBO_Bind(NULL);

		tr.msaaLinearFbo = FBO_Create("_msaaLinear", w, h);
		FBO_AttachImage(tr.msaaLinearFbo, tr.sceneLinearImage, GL_COLOR_ATTACHMENT0, 0);
		if (!R_CheckFBO(tr.msaaLinearFbo))
		{
			tr.msaaLinearFbo = NULL;
		}
	}

	return qtrue;
}

/*
=================
P2b: the shader resolves (tr_backend.c call sites; each keeps its blit when this is qfalse)
=================
*/
qboolean R_MsaaResolvesActive(void)
{
	return (qboolean)(R_MsaaSceneIsMultisampled()
	                  && tr.msaaResolveSSFbo && tr.msaaDepthResolveFbo && tr.msaaColorResolveFbo && tr.msaaLinearFbo
	                  && tr.sceneColorMSImage && tr.sceneDepthMSImage
	                  && tr.msaaResolveDepthShader.program && tr.msaaResolveColorShader.program);
}

// depth: the nearest sample into tr.renderDepthImage (gl_FragDepth, depth test ALWAYS) and (min, max) into
// tr.msaaDepthMinMaxImage. Every existing depth consumer keeps reading renderDepthImage / its hdrDepthImage copies.
void RB_MSAAResolveDepth(void)
{
	if (!R_MsaaResolvesActive())
	{
		return;
	}
	FBO_BlitFromTexture(tr.sceneDepthMSImage, NULL, NULL, tr.msaaDepthResolveFbo, NULL, &tr.msaaResolveDepthShader,
		NULL, GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK_TRUE);
	backEnd.msaaMinMaxFrame = tr.frameCount + 1;
}

// the tone stage's resolve mode: 1 = clamp (the grade path, and no tone stage at all), 2 = Karis (the Hable path),
// 0 = plain box (no tone stage but a camera-exposure gain follows, which a clamp before it would distort)
int R_MsaaToneResolveMode(void)
{
	if (r_hdr->integer && (r_toneMap->integer || r_forceToneMap->integer))
	{
		return RB_HZMToneUsesGrade() ? 1 : 2;
	}
	return (r_cameraExposure->value != 0.0f) ? 0 : 1;
}

// colour: one MRT pass - out_Color (renderImage) the tone-exact input, out_Linear (sceneLinearImage) the linear box
// average; AO applied per sample (viewRect = the view in window pixels, x y w h; NULL = no AO rectangle)
void RB_MSAAResolveColor(qboolean withAO, int mode, const int *viewRect)
{
	vec4_t color, params;

	if (!R_MsaaResolvesActive())
	{
		return;
	}

	VectorSet4(color, (float)mode, withAO ? 1.0f : 0.0f, 0.0f, 0.0f);
	if (viewRect)
	{
		VectorSet4(params, (float)viewRect[0], (float)viewRect[1],
			(float)(viewRect[0] + viewRect[2]), (float)(viewRect[1] + viewRect[3]));
	}
	else
	{
		VectorSet4(params, 0.0f, 0.0f, 0.0f, 0.0f);
	}

	GL_BindToTMU((withAO && tr.screenSsaoImage) ? tr.screenSsaoImage : tr.whiteImage, TB_NORMALMAP);
	if (mode == 2)
	{
		GL_BindToTMU((r_autoExposure->integer || r_forceAutoExposure->integer) ? tr.calcLevelsImage : tr.fixedLevelsImage,
			TB_LEVELSMAP);
	}
	else
	{
		GL_BindToTMU(tr.whiteImage, TB_LEVELSMAP);
	}

	FBO_SetHzmParams(params);
	FBO_BlitFromTexture(tr.sceneColorMSImage, NULL, NULL, tr.msaaColorResolveFbo, NULL, &tr.msaaResolveColorShader,
		color, 0);
	FBO_SetHzmParams(NULL);

	backEnd.msaaLinearValid = qtrue;
}

// bloom's bright pass, the DoF blur source and the exposure measure read the linear resolve when this post chain
// made one (the composites still go onto the tone input, srcFbo)
FBO_t *R_MsaaLinearSource(FBO_t *src)
{
	if (backEnd.msaaLinearValid && tr.msaaLinearFbo && src && src == tr.msaaResolveSSFbo)
	{
		return tr.msaaLinearFbo;
	}
	return src;
}

// the (min, max) depth for the MSAA_DEPTH_MINMAX readers, when a shader depth resolve ran this frame
image_t *R_MsaaDepthMinMaxOr(image_t *fallback)
{
	if (R_MsaaResolvesActive() && tr.msaaDepthMinMaxImage && backEnd.msaaMinMaxFrame == tr.frameCount + 1)
	{
		return tr.msaaDepthMinMaxImage;
	}
	return fallback;
}

/*
=================
R_MsaaBeginFrame - RE_BeginFrame: apply r_msaaBypass (live) by pointing the scene at the single-sample buffer

With the bypass on, tr.sceneFbo is msaaResolveFbo's single-sample target (renderImage + renderDepthImage - the
very attachments of today's MSAA-0 scene FBO) and tr.msaaResolveFbo is NULL, so every "if (tr.msaaResolveFbo)"
resolve in the pipeline is skipped exactly as at MSAA 0. The display split stays, so the HUD path is identical.
=================
*/
void R_MsaaBeginFrame(void)
{
	qboolean want;

	if (!tr.msaaNewPath || !tr.msaaSceneFbo || !r_msaaBypass)
	{
		return;
	}
	want = (qboolean)(r_msaaBypass->integer != 0);
	if (want == tr.msaaBypassActive)
	{
		return;
	}

	R_IssuePendingRenderCommands();
	tr.msaaBypassActive = want;
	if (want)
	{
		tr.sceneFbo       = tr.msaaResolveSSFbo;
		tr.msaaResolveFbo = NULL;
	}
	else
	{
		tr.sceneFbo       = tr.msaaSceneFbo;
		tr.msaaResolveFbo = tr.msaaResolveSSFbo;
	}
	ri.Printf(PRINT_ALL, "^~^~^ MSAA bypass=%d n=%d\n", want ? 1 : 0, tr.msaaSamples);
}

// the scene buffer this frame is multisampled on the new path (runtime gates for the ME-F18 readers)
qboolean R_MsaaSceneIsMultisampled(void)
{
	return (qboolean)(tr.msaaNewPath && tr.msaaSamples >= 2 && !tr.msaaBypassActive);
}

// label text for the A/B label (tr_gfxprobe.c)
const char *R_MsaaLabel(void)
{
	if (!tr.msaaNewPath)
	{
		return tr.msaaResolveFbo ? va("%dx (old)", r_ext_framebuffer_multisample->integer) : "OFF (old)";
	}
	if (tr.msaaSamples < 2)
	{
		return "OFF (new)";
	}
	if (tr.msaaBypassActive)
	{
		return va("OFF (HOME bypass of %dx)", tr.msaaSamples);
	}
	return va("%dx (new)", tr.msaaSamples);
}

static void R_MsaaPublish(void)
{
	int active;

	if (tr.msaaNewPath)
	{
		active = tr.msaaSamples;
	}
	else
	{
		active = tr.msaaResolveFbo ? r_ext_framebuffer_multisample->integer : 0;
	}
	ri.Cvar_Set("r_msaaActive", va("%d", active));

	ri.Printf(PRINT_ALL, "^~^~^ MSAA path=%s n=%d reason=%s setting=\"%s\" decided=%d caps=%d vramMB=%d scale=%.2f split=%d forcedOff=\"%s\" resolve=%s\n",
		tr.msaaNewPath ? "new" : "legacy", active, s_ms.reason[0] ? s_ms.reason : "-", s_ms.setting, s_ms.decided,
		s_ms.caps, tr.msaaNewPath ? s_ms.vramMB : ri.Cvar_VariableIntegerValue("r_gpuVramMB"), s_ms.scale,
		tr.displaySplit ? 1 : 0, r_msaaForcedOffCv ? r_msaaForcedOffCv->string : "",
		(tr.msaaNewPath && tr.msaaSamples >= 2)
			? ((tr.msaaDepthResolveFbo && tr.msaaColorResolveFbo && tr.msaaLinearFbo && tr.msaaResolveDepthShader.program
			    && tr.msaaResolveColorShader.program) ? "shader" : "blit")
			: "-");
}

/*
=================
R_Msaa_AfterGLSL - R_Init, right after GLSL_InitGPUShaders

ME-F3: an MSAA program that failed to build re-initialises in place at 0 - shaders and FBOs are rebuilt, the
images are kept (the multisample textures are freed) - so there is no ri.Error and no drop to the menu.
Then publishes ROM r_msaaActive and prints the one ^~^~^ MSAA line of this R_Init.
=================
*/
void R_Msaa_AfterGLSL(void)
{
	if (tr.msaaCompileFailed)
	{
		int i;

		ri.Printf(PRINT_ALL, "^~^~^ MSAA compile failure at n=%d - re-initialising in place at 0\n", tr.msaaSamples);
		R_MsaaReason("compile-fail@%d", tr.msaaSamples);
		ri.Cvar_Set("r_msaaForcedOff", s_ms.setting);

		GLSL_ShutdownGPUShaders();
		FBO_Shutdown();
		R_MsaaReleaseImages();

		// every FBO pointer FBO_Init may leave unassigned must not survive into the rebuild
		tr.sceneFbo = tr.renderFbo = tr.msaaResolveFbo = tr.displayScratchFbo = NULL;
		tr.msaaSceneFbo = tr.msaaResolveSSFbo = tr.msaaDepthResolveFbo = NULL;
		tr.msaaColorResolveFbo = tr.msaaLinearFbo = NULL;
		tr.screenScratchFbo = tr.globalFogFbo = tr.sunRaysFbo = tr.screenShadowFbo = NULL;
		tr.calcLevelsFbo = tr.targetLevelsFbo = tr.hdrDepthFbo = tr.screenSsaoFbo = tr.renderCubeFbo = NULL;
		for (i = 0; i < MAX_DRAWN_PSHADOWS; i++) tr.pshadowFbos[i] = NULL;
		for (i = 0; i < 4; i++) tr.sunShadowFbo[i] = NULL;
		for (i = 0; i < 2; i++) tr.textureScratchFbo[i] = tr.quarterFbo[i] = tr.bloomFbo[i] = NULL;

		tr.msaaSamples       = 0;
		tr.displaySplit      = qfalse;
		tr.msaaCompileFailed = qfalse;

		FBO_Init();
		GLSL_InitGPUShaders();
	}

	R_MsaaPublish();
}
