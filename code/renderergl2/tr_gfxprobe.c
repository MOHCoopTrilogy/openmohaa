/*
===========================================================================
HZM gl2 - graphics probes (MSAA / stable-shadow plan, phase P0). Everything here is INERT AT DEFAULT:
  r_gfxProbe 0, r_glDebug 0, r_gfxLabel 0, r_shadowFitYaw 0, r_shadowFitOffset 0.
The only unconditional output is the one-line ^~^~^ GFXBUILD stamp at every R_Init, which proves which
renderer DLL actually loaded (a failed renderer_<name>.dll load silently falls back to opengl1).

r_gfxProbe (flags 0) is a bitmask:
  1 = log: ^~^~^ SHADOWBUDGET / SHADOWTIME / GPUTIME / VRAM / GLERR / WCRC lines
  2 = freeze renderer time (tr.refdef.time pinned) so repeated captures of a static view are comparable
  4 = auto WCRC: CRC the far cascade's depth right after every frame that renders it
r_glDebug 1 (read at window creation) asks SDL for a DEBUG GL context and installs a synchronous
KHR_debug callback; every new (source,type,id) prints once as ^~^~^ GLERR.
r_gfxLabel 1 draws the A/B label (AA / SHADOWS / fps vs the real cap) last in the frame.
===========================================================================
*/
#include "tr_local.h"

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "tr_dsa.h"
#include "tr_gfxbuild.h"

#ifndef HZM_GFX_COMMIT
#define HZM_GFX_COMMIT "unknown"
#endif
#ifndef HZM_GFX_DATE
#define HZM_GFX_DATE "unknown"
#endif

// Read by publish_release.ps1 / build.ps1 (plan section 3.5). It stays "legacy" until the flip commit.
const char hzmGfxDefaultsMarker[] = "HZM_GFX_DEFAULTS=legacy";

extern int TIKI_Skel_Bones_Index;

cvar_t *r_gfxProbe;
cvar_t *r_glDebug;
cvar_t *r_gfxLabel;
cvar_t *r_shadowFitYaw;
cvar_t *r_shadowFitOffset;
cvar_t *r_shadowHarden;
cvar_t *r_shadowFboDummy;

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_DEBUG_OUTPUT
#define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
#ifndef GL_CONTEXT_FLAGS
#define GL_CONTEXT_FLAGS 0x821E
#endif
#ifndef GL_CONTEXT_FLAG_DEBUG_BIT
#define GL_CONTEXT_FLAG_DEBUG_BIT 0x00000002
#endif
#define HZM_GL_DEBUG_TYPE_ERROR               0x824C
#define HZM_GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR  0x824E
#define HZM_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX         0x9047
#define HZM_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX   0x9048
#define HZM_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049

typedef void (APIENTRY *hzmQueryCounter_t)(GLuint id, GLenum target);
typedef void (APIENTRY *hzmGetQueryObjectui64v_t)(GLuint id, GLenum pname, unsigned long long *params);
typedef void (APIENTRY *hzmDebugProc_t)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *userParam);
typedef void (APIENTRY *hzmDebugMessageCallback_t)(hzmDebugProc_t callback, const void *userParam);

static hzmQueryCounter_t        s_QueryCounter;
static hzmGetQueryObjectui64v_t s_GetQueryObjectui64v;

static double R_GfxNowMs(void)
{
	return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/*
=======================================================================
 per-registration state
=======================================================================
*/
typedef struct {
	int ds, st, ter, bones;
	double cpu;
} gfxView_t;

enum { GV_C0, GV_C1, GV_C2, GV_C3, GV_MAIN, GV_DLS, GV_COUNT };
static const char *s_viewName[GV_COUNT] = { "c0", "c1", "c2", "c3", "main", "dls" };

static struct {
	char     map[MAX_QPATH];
	int      reg;              // registration generation (map load / vid_restart)
	int      frames;           // frontend frames since registration
	int      worldFrames;      // frames that rendered a world scene
	// this frame
	gfxView_t cur[GV_COUNT];
	int      curMaxDs, curMaxBones;
	qboolean curFar;
	qboolean curWorld;
	// view in flight
	int      vDs, vSt, vBones;
	double   vT;
	// high-water since registration
	gfxView_t hwView[GV_COUNT];
	int      hwFrameDs, hwFrameDsAt, hwFrameSt, hwFrameStAt, hwBones;
	int      hwPrintedDs;
	double   hwLastPrint;
	gfxView_t farView;         // the cascade-3 render of the far frame
	int      farFrameDs, farFrameSt, farAt, farCount;
	int      wraps, wrapSurfs;
	int      drops, dropFrames;
	int      staticOverflow;
	qboolean staticOverflowPrinted;
	int      vramSettledPrinted;
	// cpu timing ring
	double   lastEndMs;
} gp;

#define GFX_RING 1024
typedef struct {
	float v[GFX_RING];
	int   n, head;
} gfxRing_t;

enum { GR_CPU_SUN, GR_CPU_FRAME, GR_GPU_FRAME, GR_GPU_SUN, GR_GPU_SHOTH, GR_GPU_MAIN, GR_GPU_POST, GR_COUNT };
static gfxRing_t s_ring[GR_COUNT];
static int s_ringSincePrint;

static void R_GfxRingPush(int which, float v)
{
	gfxRing_t *r = &s_ring[which];
	r->v[r->head] = v;
	r->head = (r->head + 1) % GFX_RING;
	if (r->n < GFX_RING) r->n++;
}

static int R_GfxCmpFloat(const void *a, const void *b)
{
	float fa = *(const float *)a, fb = *(const float *)b;
	return (fa < fb) ? -1 : (fa > fb) ? 1 : 0;
}

static void R_GfxRingPct(int which, float *p50, float *p99)
{
	static float tmp[GFX_RING];
	gfxRing_t *r = &s_ring[which];
	*p50 = *p99 = 0.0f;
	if (!r->n) return;
	Com_Memcpy(tmp, r->v, sizeof(float) * r->n);
	qsort(tmp, r->n, sizeof(float), R_GfxCmpFloat);
	*p50 = tmp[(r->n - 1) / 2];
	*p99 = tmp[(int)((r->n - 1) * 0.99f)];
}

static qboolean R_GfxLog(void)
{
	return (qboolean)(r_gfxProbe && (r_gfxProbe->integer & 1));
}

/*
=======================================================================
 GL debug output (r_glDebug)
=======================================================================
*/
#define GFX_MAX_GLKEYS 256
typedef struct { GLenum source, type, severity; GLuint id; int count; } gfxGlKey_t;
static gfxGlKey_t s_glKeys[GFX_MAX_GLKEYS];
static int s_numGlKeys;
static qboolean s_debugActive;

static void APIENTRY R_GfxDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *userParam)
{
	int i;
	char buf[200];
	(void)length; (void)userParam;

	for (i = 0; i < s_numGlKeys; i++) {
		if (s_glKeys[i].source == source && s_glKeys[i].type == type && s_glKeys[i].id == id) {
			s_glKeys[i].count++;
			return;
		}
	}
	if (s_numGlKeys < GFX_MAX_GLKEYS) {
		gfxGlKey_t *k = &s_glKeys[s_numGlKeys++];
		k->source = source; k->type = type; k->id = id; k->severity = severity; k->count = 1;
	}
	Q_strncpyz(buf, message ? message : "", sizeof(buf));
	for (i = 0; buf[i]; i++) {
		if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
	}
	ri.Printf(PRINT_ALL, "^~^~^ GLERR new src=0x%x type=0x%x id=%u sev=0x%x map=%s \"%s\"\n",
		source, type, id, severity, gp.map, buf);
}

static void R_GfxGlErrSummary(const char *when)
{
	int i, errKeys = 0, errCount = 0;
	if (!s_debugActive || !s_numGlKeys) {
		if (s_debugActive) ri.Printf(PRINT_ALL, "^~^~^ GLERR summary when=%s keys=0\n", when);
		return;
	}
	for (i = 0; i < s_numGlKeys; i++) {
		if (s_glKeys[i].type == HZM_GL_DEBUG_TYPE_ERROR || s_glKeys[i].type == HZM_GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR) {
			errKeys++;
			errCount += s_glKeys[i].count;
		}
	}
	ri.Printf(PRINT_ALL, "^~^~^ GLERR summary when=%s keys=%d error_or_ub_keys=%d error_or_ub_count=%d\n",
		when, s_numGlKeys, errKeys, errCount);
	for (i = 0; i < s_numGlKeys; i++) {
		ri.Printf(PRINT_ALL, "^~^~^ GLERR key src=0x%x type=0x%x id=%u sev=0x%x count=%d\n",
			s_glKeys[i].source, s_glKeys[i].type, s_glKeys[i].id, s_glKeys[i].severity, s_glKeys[i].count);
	}
}

/*
=======================================================================
 GPU timestamps (r_gfxProbe & 1)
=======================================================================
*/
#define GPU_FRAMES 4
#define GPU_MARKS  64
typedef struct {
	GLuint  q[GPU_MARKS];
	int     kind[GPU_MARKS];
	int     n;
	qboolean pending;
} gpuFrame_t;

static gpuFrame_t s_gpu[GPU_FRAMES];
static int        s_gpuCur;
static qboolean   s_gpuReady;
static qboolean   s_gpuNeedBegin = qtrue;

static void R_GfxGpuInit(void)
{
	int i;
	s_gpuReady = qfalse;
	// timer queries are core only from GL 3.3; the context is 3.2 core, so require the extension otherwise
	if (!QGL_VERSION_ATLEAST(3, 3) && !SDL_GL_ExtensionSupported("GL_ARB_timer_query")) {
		return;
	}
	s_QueryCounter = (hzmQueryCounter_t)SDL_GL_GetProcAddress("glQueryCounter");
	s_GetQueryObjectui64v = (hzmGetQueryObjectui64v_t)SDL_GL_GetProcAddress("glGetQueryObjectui64v");
	if (!s_QueryCounter || !s_GetQueryObjectui64v || !qglGenQueries) {
		return;
	}
	for (i = 0; i < GPU_FRAMES; i++) {
		qglGenQueries(GPU_MARKS, s_gpu[i].q);
		s_gpu[i].n = 0;
		s_gpu[i].pending = qfalse;
	}
	s_gpuCur = 0;
	s_gpuNeedBegin = qtrue;
	s_gpuReady = qtrue;
}

static void R_GfxGpuShutdown(void)
{
	int i;
	if (!s_gpuReady) return;
	for (i = 0; i < GPU_FRAMES; i++) {
		qglDeleteQueries(GPU_MARKS, s_gpu[i].q);
	}
	s_gpuReady = qfalse;
}

static void R_GfxGpuEmit(int kind)
{
	gpuFrame_t *f = &s_gpu[s_gpuCur];
	if (f->n >= GPU_MARKS) return;
	s_QueryCounter(f->q[f->n], GL_TIMESTAMP);
	f->kind[f->n] = kind;
	f->n++;
}

void R_GfxProbe_GpuMark(int kind)
{
	if (!s_gpuReady || !R_GfxLog()) return;
	if (s_gpu[s_gpuCur].pending) return;   // results of this slot not read yet - skip this frame
	if (s_gpuNeedBegin) {
		R_GfxGpuEmit(GPUMARK_FRAME_BEGIN);
		s_gpuNeedBegin = qfalse;
	}
	R_GfxGpuEmit(kind);
}

static void R_GfxGpuResolve(gpuFrame_t *f)
{
	unsigned long long t[GPU_MARKS];
	unsigned long long open[GPUMARK_COUNT];
	double acc[GPUMARK_COUNT];
	GLint avail = 0;
	int i;

	if (!f->pending || f->n < 2) { f->pending = qfalse; f->n = 0; return; }
	qglGetQueryObjectiv(f->q[f->n - 1], GL_QUERY_RESULT_AVAILABLE, &avail);
	if (!avail) return;
	for (i = 0; i < f->n; i++) {
		s_GetQueryObjectui64v(f->q[i], GL_QUERY_RESULT, &t[i]);
	}
	Com_Memset(open, 0, sizeof(open));
	Com_Memset(acc, 0, sizeof(acc));
	for (i = 0; i < f->n; i++) {
		int k = f->kind[i];
		if (k == GPUMARK_SUN_B || k == GPUMARK_SHOTH_B || k == GPUMARK_MAIN_B || k == GPUMARK_POST_B) {
			open[k] = t[i];
		} else if (k == GPUMARK_SUN_E || k == GPUMARK_SHOTH_E || k == GPUMARK_MAIN_E || k == GPUMARK_POST_E) {
			if (open[k - 1]) acc[k - 1] += (double)(t[i] - open[k - 1]) / 1.0e6;
			open[k - 1] = 0;
		}
	}
	if (f->kind[0] == GPUMARK_FRAME_BEGIN && f->kind[f->n - 1] == GPUMARK_FRAME_END) {
		R_GfxRingPush(GR_GPU_FRAME, (float)((double)(t[f->n - 1] - t[0]) / 1.0e6));
		R_GfxRingPush(GR_GPU_SUN,   (float)acc[GPUMARK_SUN_B]);
		R_GfxRingPush(GR_GPU_SHOTH, (float)acc[GPUMARK_SHOTH_B]);
		R_GfxRingPush(GR_GPU_MAIN,  (float)acc[GPUMARK_MAIN_B]);
		R_GfxRingPush(GR_GPU_POST,  (float)acc[GPUMARK_POST_B]);
	}
	f->pending = qfalse;
	f->n = 0;
}

/*
=======================================================================
 WCRC - CRC of the far cascade's depth (legacy cascade 3 today; the static W layer in P3)
=======================================================================
*/
static int  s_wcrcRequest;       // frames to wait before reading back (0 = none)
static char s_wcrcWhy[32];

static unsigned int R_GfxCrc32(const byte *p, size_t n, unsigned int crc)
{
	static unsigned int table[256];
	static qboolean init;
	size_t i;
	if (!init) {
		unsigned int c, k, j;
		for (k = 0; k < 256; k++) {
			c = k;
			for (j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[k] = c;
		}
		init = qtrue;
	}
	crc = ~crc;
	for (i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void R_GfxWcrcNow(void)
{
	image_t *img = tr.sunShadowDepthImage[3];
	unsigned int *buf;
	size_t n, i, covered = 0;
	unsigned int crc;

	if (!img || !qglGetTexImage) {
		ri.Printf(PRINT_ALL, "^~^~^ WCRC unavailable (no far cascade image)\n");
		return;
	}
	n = (size_t)img->uploadWidth * (size_t)img->uploadHeight;
	if (!n) n = (size_t)img->width * (size_t)img->height;
	buf = (unsigned int *)malloc(n * sizeof(unsigned int));
	if (!buf) {
		ri.Printf(PRINT_ALL, "^~^~^ WCRC malloc failed\n");
		return;
	}
	qglActiveTexture(GL_TEXTURE0);
	qglBindTexture(GL_TEXTURE_2D, img->texnum);
	qglGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, buf);
	GL_BindNullTextures();
	for (i = 0; i < n; i++) {
		buf[i] &= 0xffffff00u;          // D24 lives in the top 24 bits
		if (buf[i] < 0xffffff00u) covered++;
	}
	crc = R_GfxCrc32((const byte *)buf, n * sizeof(unsigned int), 0);
	free(buf);
	ri.Printf(PRINT_ALL, "^~^~^ WCRC layer=%s why=%s map=%s reg=%d frame=%d crc=%08x covered=%.4f\n",
		R_SunStable_FarIsW() ? "W" : "c3", s_wcrcWhy, gp.map, gp.reg, gp.frames, crc, (double)covered / (double)n);
}

// HZM gl2 P3: a W bake counts as the frame's far layer for SHADOWBUDGET far and the bit-4 auto WCRC
void R_GfxProbe_WBaked(void)
{
	gp.curFar = qtrue;
}

static void R_GfxWcrc_f(void)
{
	s_wcrcRequest = 1;
	Q_strncpyz(s_wcrcWhy, "cmd", sizeof(s_wcrcWhy));
}

/*
=======================================================================
 VRAM
=======================================================================
*/
static void R_GfxVram(const char *when)
{
	GLint total = 0, avail = 0, dedicated = 0;
	if (!SDL_GL_ExtensionSupported("GL_NVX_gpu_memory_info")) {
		ri.Printf(PRINT_ALL, "^~^~^ VRAM when=%s unsupported (no GL_NVX_gpu_memory_info)\n", when);
		return;
	}
	qglGetIntegerv(HZM_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated);
	qglGetIntegerv(HZM_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
	qglGetIntegerv(HZM_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
	ri.Printf(PRINT_ALL, "^~^~^ VRAM when=%s map=%s reg=%d dedicated=%dMB total=%dMB avail=%dMB used=%dMB\n",
		when, gp.map, gp.reg, dedicated / 1024, total / 1024, avail / 1024, (total - avail) / 1024);
}

/*
=======================================================================
 summaries
=======================================================================
*/
static void R_GfxPrintView(const char *tag, const gfxView_t *v)
{
	ri.Printf(PRINT_ALL, " %s ds=%d st=%d ter=%d bones=%d cpu=%.2f", tag, v->ds, v->st, v->ter, v->bones, v->cpu);
}

static void R_GfxBudgetSummary(const char *when)
{
	int i;
	if (!R_GfxLog() || !gp.frames) return;
	ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET summary when=%s map=%s reg=%d frames=%d world=%d frame_hw ds=%d@%d st=%d@%d bones=%d max_drawsurfs=%d max_static=%d wraps=%d wrap_surfs=%d drops=%d drop_frames=%d static_overflow=%d harden=%d",
		when, gp.map, gp.reg, gp.frames, gp.worldFrames, gp.hwFrameDs, gp.hwFrameDsAt, gp.hwFrameSt, gp.hwFrameStAt, gp.hwBones,
		MAX_DRAWSURFS, R_StaticModelSurfCapacity(), gp.wraps, gp.wrapSurfs, gp.drops, gp.dropFrames, gp.staticOverflow,
		r_shadowHarden ? r_shadowHarden->integer : 0);
	ri.Printf(PRINT_ALL, " |");
	for (i = 0; i < GV_COUNT; i++) {
		R_GfxPrintView(s_viewName[i], &gp.hwView[i]);
	}
	ri.Printf(PRINT_ALL, " | far n=%d first@%d frame_ds=%d frame_st=%d", gp.farCount, gp.farAt, gp.farFrameDs, gp.farFrameSt);
	R_GfxPrintView("c3", &gp.farView);
	ri.Printf(PRINT_ALL, "\n");
}

static void R_GfxTimeSummary(const char *when)
{
	float a50, a99, b50, b99, c50, c99, d50, d99, e50, e99, f50, f99, g50, g99;
	if (!R_GfxLog()) return;
	R_GfxRingPct(GR_CPU_SUN, &a50, &a99);
	R_GfxRingPct(GR_CPU_FRAME, &b50, &b99);
	ri.Printf(PRINT_ALL, "^~^~^ SHADOWTIME when=%s map=%s n=%d cpu_sun p50=%.3f p99=%.3f wall_frame p50=%.3f p99=%.3f (ms)\n",
		when, gp.map, s_ring[GR_CPU_SUN].n, a50, a99, b50, b99);
	if (!s_gpuReady) {
		ri.Printf(PRINT_ALL, "^~^~^ GPUTIME unavailable (no timer queries)\n");
		return;
	}
	R_GfxRingPct(GR_GPU_FRAME, &c50, &c99);
	R_GfxRingPct(GR_GPU_SUN, &d50, &d99);
	R_GfxRingPct(GR_GPU_SHOTH, &e50, &e99);
	R_GfxRingPct(GR_GPU_MAIN, &f50, &f99);
	R_GfxRingPct(GR_GPU_POST, &g50, &g99);
	ri.Printf(PRINT_ALL, "^~^~^ GPUTIME when=%s map=%s n=%d frame p50=%.3f p99=%.3f sun p50=%.3f p99=%.3f shadow_other p50=%.3f p99=%.3f main p50=%.3f p99=%.3f post p50=%.3f p99=%.3f (ms)\n",
		when, gp.map, s_ring[GR_GPU_FRAME].n, c50, c99, d50, d99, e50, e99, f50, f99, g50, g99);
}

static void R_GfxProbe_f(void)
{
	R_GfxBudgetSummary("cmd");
	R_GfxTimeSummary("cmd");
	R_GfxVram("cmd");
	R_GfxGlErrSummary("cmd");
}

static void R_GfxResetTiming_f(void)
{
	Com_Memset(s_ring, 0, sizeof(s_ring));
	s_ringSincePrint = 0;
	ri.Printf(PRINT_ALL, "^~^~^ GFXPROBE timing reset map=%s\n", gp.map);
}

/*
=======================================================================
 frontend hooks
=======================================================================
*/
// glReadBuffer is GL 1.0 but not in the qgl table; P1b needs it to make a colour-less FBO complete.
void R_GfxReadBuffer(GLenum mode)
{
	typedef void (APIENTRY *hzmReadBuffer_t)(GLenum mode);
	static hzmReadBuffer_t fn;
	static qboolean tried;
	if (!tried) {
		fn = (hzmReadBuffer_t)SDL_GL_GetProcAddress("glReadBuffer");
		tried = qtrue;
	}
	if (fn) {
		fn(mode);
	}
}

void R_GfxProbe_Register(void)
{
	r_gfxProbe        = ri.Cvar_Get("r_gfxProbe", "0", 0);
	r_glDebug         = ri.Cvar_Get("r_glDebug", "0", 0);
	r_gfxLabel        = ri.Cvar_Get("r_gfxLabel", "0", 0);
	r_shadowFitYaw    = ri.Cvar_Get("r_shadowFitYaw", "0", 0);
	r_shadowFitOffset = ri.Cvar_Get("r_shadowFitOffset", "0", 0);
	// plan P1a/P1b switches. flags 0 (never archived - T7), defaults = today's behaviour until the flip.
	r_shadowHarden    = ri.Cvar_Get("r_shadowHarden", "0", 0);
	r_shadowFboDummy  = ri.Cvar_Get("r_shadowFboDummy", "1", 0);
	ri.Cvar_Get("r_displayHz", "0", CVAR_ROM);
	ri.Cvar_Get("r_vsyncActive", "0", CVAR_ROM);

	// R_Register runs at every R_Init (every map load), while RE_Shutdown only runs on vid_restart/quit: remove
	// first so a map load neither warns "already defined" nor keeps a stale handler
	ri.Cmd_RemoveCommand("gfxprobe");
	ri.Cmd_RemoveCommand("gfxwcrc");
	ri.Cmd_RemoveCommand("gfxresettiming");
	ri.Cmd_AddCommand("gfxprobe", R_GfxProbe_f);
	ri.Cmd_AddCommand("gfxwcrc", R_GfxWcrc_f);
	ri.Cmd_AddCommand("gfxresettiming", R_GfxResetTiming_f);
}

void R_GfxProbe_Reregister(void)
{
	// the previous registration ends here (map change or disconnect); print what it saw
	R_GfxBudgetSummary("reregister");
	R_GfxTimeSummary("reregister");
	R_GfxGlErrSummary("reregister");

	// registration bookkeeping. It lives HERE, not in R_GfxProbe_InitGL, because RE_BeginRegistration calls this
	// on every registration before anything else, while R_Init may one day not run per map (renderer_reinit
	// plan 6, rule 2): reg=, the high-water marks and the once-per-map prints stay per map in both lifecycles.
	// Today it is the same order - this always runs right before R_Init.
	gp.reg++;
	gp.frames = 0;
	gp.worldFrames = 0;
	Com_Memset(gp.hwView, 0, sizeof(gp.hwView));
	Com_Memset(&gp.farView, 0, sizeof(gp.farView));
	gp.hwFrameDs = gp.hwFrameDsAt = gp.hwFrameSt = gp.hwFrameStAt = gp.hwBones = 0;
	gp.hwPrintedDs = 0;
	gp.farFrameDs = gp.farFrameSt = gp.farAt = gp.farCount = 0;
	gp.wraps = gp.wrapSurfs = 0;
	gp.drops = gp.dropFrames = 0;
	gp.staticOverflow = 0;
	gp.staticOverflowPrinted = qfalse;
	gp.vramSettledPrinted = 0;
	Com_Memset(s_ring, 0, sizeof(s_ring));
	s_ringSincePrint = 0;
	s_wcrcRequest = 0;
}

void R_GfxProbe_InitGL(void)
{
	GLint flags = 0;
	hzmDebugMessageCallback_t cb;

	// GL objects of this context/R_Init (the per-registration counters were reset by R_GfxProbe_Reregister)
	R_GfxGpuShutdown();
	R_GfxGpuInit();

	s_debugActive = qfalse;
	// GL_CONTEXT_FLAGS is a GL 3.0 query; on the GL 2.0 fallback context it would raise INVALID_ENUM, which a
	// fail-fast run (r_ignoreGLErrors 0) turns into a fatal error in RE_BeginFrame
	if (QGL_VERSION_ATLEAST(3, 0)) {
		qglGetIntegerv(GL_CONTEXT_FLAGS, &flags);
	}
	if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
		cb = (hzmDebugMessageCallback_t)SDL_GL_GetProcAddress("glDebugMessageCallback");
		if (!cb) cb = (hzmDebugMessageCallback_t)SDL_GL_GetProcAddress("glDebugMessageCallbackARB");
		if (cb) {
			qglEnable(GL_DEBUG_OUTPUT);
			qglEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
			cb(R_GfxDebugCallback, NULL);
			s_debugActive = qtrue;
		}
	}
	// the context state outlives R_Init on a map load; report it at every registration
	if (r_glDebug->integer || s_debugActive) {
		ri.Printf(PRINT_ALL, "^~^~^ GLERR context debug=%d callback=%d\n", (flags & GL_CONTEXT_FLAG_DEBUG_BIT) ? 1 : 0, s_debugActive ? 1 : 0);
	}
	{
		// r_vsyncActive is the driver's truth, not r_swapInterval: the interval is applied only at window
		// creation (sdl_glimp.c), so a latched r_swapInterval can disagree until the next vid_restart.
		int si = SDL_GL_GetSwapInterval();
		ri.Cvar_Set("r_vsyncActive", si != 0 ? "1" : "0");
	}
}

void R_GfxProbe_AfterInit(void)
{
	ri.Printf(PRINT_ALL, "^~^~^ GFXBUILD %s %s %s probe=%d glDebug=%d label=%d displayHz=%d vsyncActive=%d\n",
		HZM_GFX_COMMIT, HZM_GFX_DATE, hzmGfxDefaultsMarker,
		r_gfxProbe->integer, r_glDebug->integer, r_gfxLabel->integer,
		ri.Cvar_VariableIntegerValue("r_displayHz"), ri.Cvar_VariableIntegerValue("r_vsyncActive"));
	if (R_GfxLog()) {
		R_GfxVram("init");
	}
}

void R_GfxProbe_Shutdown(void)
{
	R_GfxBudgetSummary("shutdown");
	R_GfxTimeSummary("shutdown");
	R_GfxGlErrSummary("shutdown");
	ri.Cmd_RemoveCommand("gfxprobe");
	ri.Cmd_RemoveCommand("gfxwcrc");
	ri.Cmd_RemoveCommand("gfxresettiming");
}

void R_GfxProbe_WorldLoaded(const char *name)
{
	const char *base = strrchr(name, '/');
	Q_strncpyz(gp.map, base ? base + 1 : name, sizeof(gp.map));
	{
		char *dot = strrchr(gp.map, '.');
		if (dot) *dot = 0;
	}
	if (R_GfxLog()) {
		R_GfxVram("world");
	}
}

void R_GfxProbe_StaticOverflow(const char *model)
{
	gp.staticOverflow++;
	if (!gp.staticOverflowPrinted) {
		gp.staticOverflowPrinted = qtrue;
		ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET STATIC-OVERFLOW map=%s view=%s cap=%d first='%s' (static-model surfaces SKIPPED this frame; printed once per map)\n",
			gp.map, (tr.viewParms.flags & VPF_DEPTHSHADOW) ? "shadow" : "main", R_StaticModelSurfCapacity(), model ? model : "?");
	}
}

void R_GfxProbe_ViewBegin(int kind)
{
	(void)kind;
	gp.vDs = tr.refdef.numDrawSurfs;
	gp.vSt = g_nStaticSurfaces;
	gp.vBones = TIKI_Skel_Bones_Index;
	gp.vT = R_GfxNowMs();
}

void R_GfxProbe_ViewEnd(int kind)
{
	gfxView_t v;
	int i;

	if (kind < 0 || kind >= GV_COUNT) return;
	v.ds = tr.refdef.numDrawSurfs - gp.vDs;
	v.st = g_nStaticSurfaces - gp.vSt;
	v.bones = TIKI_Skel_Bones_Index - gp.vBones;
	v.cpu = R_GfxNowMs() - gp.vT;
	v.ter = 0;
	if (R_GfxLog()) {
		for (i = gp.vDs; i < tr.refdef.numDrawSurfs; i++) {
			const drawSurf_t *d = &tr.refdef.drawSurfs[i & DRAWSURF_MASK];
			if (d->surface && *d->surface == SF_TERRAIN_PATCH) v.ter++;
		}
	}
	gp.cur[kind].ds += v.ds;
	gp.cur[kind].st += v.st;
	gp.cur[kind].bones += v.bones;
	gp.cur[kind].ter += v.ter;
	gp.cur[kind].cpu += v.cpu;
	if (tr.refdef.numDrawSurfs > gp.curMaxDs) gp.curMaxDs = tr.refdef.numDrawSurfs;
	if (TIKI_Skel_Bones_Index > gp.curMaxBones) gp.curMaxBones = TIKI_Skel_Bones_Index;
	if (kind == GV_C3) gp.curFar = qtrue;
	if (kind == GV_MAIN) gp.curWorld = qtrue;
}

// r_shadowFitYaw / r_shadowFitOffset: perturb ONLY the view handed to the sun-cascade fit (T1). The camera,
// the main view and every other consumer are untouched, so any change in the mask is the fit's own response.
const refdef_t *R_GfxProbe_FitRefdef(const refdef_t *fd, refdef_t *scratch)
{
	float yaw, off;
	if (!r_shadowFitYaw || !r_shadowFitOffset) return fd;
	yaw = r_shadowFitYaw->value;
	off = r_shadowFitOffset->value;
	if (yaw == 0.0f && off == 0.0f) return fd;
	*scratch = *fd;
	if (yaw != 0.0f) {
		vec3_t up = { 0, 0, 1 };
		int i;
		for (i = 0; i < 3; i++) {
			RotatePointAroundVector(scratch->viewaxis[i], up, fd->viewaxis[i], yaw);
		}
	}
	if (off != 0.0f) {
		VectorMA(fd->vieworg, off, fd->viewaxis[1], scratch->vieworg);
	}
	return scratch;
}

int R_GfxProbe_FreezeTime(int t)
{
	if (r_gfxProbe && (r_gfxProbe->integer & 2)) {
		return 100000;
	}
	return t;
}

void R_GfxProbe_EndFrame(void)
{
	int i, fst;
	double now;
	float cpuSun;

	gp.frames++;
	if (gp.curWorld) gp.worldFrames++;

	fst = g_nStaticSurfaces;
	if (gp.curMaxDs > MAX_DRAWSURFS) {
		gp.wraps++;
		gp.wrapSurfs += gp.curMaxDs - MAX_DRAWSURFS;
	}
	if (r_drawSurfDrops) {
		// never silent: the first refusal of every registration prints even with the probe off
		if (!gp.drops) {
			ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET DROP map=%s reg=%d frame=%d dropped=%d cap=%d (drawsurf list full; printed once per map)\n",
				gp.map, gp.reg, gp.frames, r_drawSurfDrops, MAX_DRAWSURFS);
		}
		gp.drops += r_drawSurfDrops;
		gp.dropFrames++;
		r_drawSurfDrops = 0;
	}
	if (gp.curMaxDs > gp.hwFrameDs) { gp.hwFrameDs = gp.curMaxDs; gp.hwFrameDsAt = gp.frames; }
	if (fst > gp.hwFrameSt) { gp.hwFrameSt = fst; gp.hwFrameStAt = gp.frames; }
	if (gp.curMaxBones > gp.hwBones) gp.hwBones = gp.curMaxBones;
	for (i = 0; i < GV_COUNT; i++) {
		if (gp.cur[i].ds > gp.hwView[i].ds) gp.hwView[i].ds = gp.cur[i].ds;
		if (gp.cur[i].st > gp.hwView[i].st) gp.hwView[i].st = gp.cur[i].st;
		if (gp.cur[i].ter > gp.hwView[i].ter) gp.hwView[i].ter = gp.cur[i].ter;
		if (gp.cur[i].bones > gp.hwView[i].bones) gp.hwView[i].bones = gp.cur[i].bones;
		if (gp.cur[i].cpu > gp.hwView[i].cpu) gp.hwView[i].cpu = gp.cur[i].cpu;
	}

	if (gp.curFar) {
		gp.farCount++;
		if (gp.farCount == 1) {
			gp.farAt = gp.frames;
			gp.farView = gp.cur[GV_C3];
			gp.farFrameDs = gp.curMaxDs;
			gp.farFrameSt = fst;
		}
		if (R_GfxLog()) {
			ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET far map=%s reg=%d frame=%d frame_ds=%d frame_st=%d bones=%d", gp.map, gp.reg, gp.frames, gp.curMaxDs, fst, gp.curMaxBones);
			for (i = 0; i < GV_COUNT; i++) R_GfxPrintView(s_viewName[i], &gp.cur[i]);
			ri.Printf(PRINT_ALL, "\n");
		}
		if (r_gfxProbe && (r_gfxProbe->integer & 4)) {
			s_wcrcRequest = 1;
			Q_strncpyz(s_wcrcWhy, "far", sizeof(s_wcrcWhy));
		}
	}

	now = R_GfxNowMs();
	if (R_GfxLog()) {
		if (gp.curMaxDs > gp.hwPrintedDs && now - gp.hwLastPrint > 2000.0) {
			gp.hwPrintedDs = gp.curMaxDs;
			gp.hwLastPrint = now;
			ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET hw map=%s reg=%d frame=%d frame_ds=%d frame_st=%d bones=%d", gp.map, gp.reg, gp.frames, gp.curMaxDs, fst, gp.curMaxBones);
			for (i = 0; i < GV_COUNT; i++) R_GfxPrintView(s_viewName[i], &gp.cur[i]);
			ri.Printf(PRINT_ALL, "\n");
		}
		cpuSun = (float)(gp.cur[GV_C0].cpu + gp.cur[GV_C1].cpu + gp.cur[GV_C2].cpu + gp.cur[GV_C3].cpu);
		if (gp.curWorld) {
			R_GfxRingPush(GR_CPU_SUN, cpuSun);
			if (gp.lastEndMs > 0.0) R_GfxRingPush(GR_CPU_FRAME, (float)(now - gp.lastEndMs));
			if (++s_ringSincePrint >= 1000) {
				s_ringSincePrint = 0;
				R_GfxTimeSummary("periodic");
			}
		}
		if (gp.worldFrames == 300 && !gp.vramSettledPrinted) {
			gp.vramSettledPrinted = 1;
			R_GfxVram("settled");
		}
	}
	gp.lastEndMs = now;

	Com_Memset(gp.cur, 0, sizeof(gp.cur));
	gp.curMaxDs = gp.curMaxBones = 0;
	gp.curFar = qfalse;
	gp.curWorld = qfalse;
}

/*
=======================================================================
 backend hooks
=======================================================================
*/
void R_GfxProbe_BackendFrameEnd(void)
{
	int i;

	if (s_wcrcRequest) {
		s_wcrcRequest = 0;
		R_GfxWcrcNow();
	}

	if (R_GfxLog() && r_ignoreGLErrors->integer && !s_debugActive) {
		// fail-fast runs (r_ignoreGLErrors 0) must see their own errors in RE_BeginFrame, so only drain here
		// when errors are being ignored anyway
		static int seen[8];
		static int seenReg;
		int err;
		if (seenReg != gp.reg) { Com_Memset(seen, 0, sizeof(seen)); seenReg = gp.reg; }
		int guard = 0;
		while (guard++ < 8 && (err = qglGetError()) != GL_NO_ERROR) {   // bounded: a lost context repeats
			int slot = err & 7;
			if (seen[slot] != err) {
				seen[slot] = err;
				ri.Printf(PRINT_ALL, "^~^~^ GLERR glGetError=0x%x map=%s reg=%d frame=%d\n", err, gp.map, gp.reg, gp.frames);
			}
		}
	}

	if (!s_gpuReady || !R_GfxLog()) {
		s_gpuNeedBegin = qtrue;
		return;
	}
	if (!s_gpuNeedBegin) {
		R_GfxGpuEmit(GPUMARK_FRAME_END);
		s_gpu[s_gpuCur].pending = qtrue;
		s_gpuCur = (s_gpuCur + 1) % GPU_FRAMES;
	}
	s_gpuNeedBegin = qtrue;
	// read back every finished slot. A slot still pending when its turn comes round again makes the next
	// frame skip its marks (R_GfxProbe_GpuMark), so a slow readback costs samples, never a stall.
	for (i = 0; i < GPU_FRAMES; i++) {
		if (s_gpu[i].pending) {
			R_GfxGpuResolve(&s_gpu[i]);
		}
	}
}

/*
=======================================================================
 the A/B label (r_gfxLabel), drawn last in the frame with the renderer's own font
=======================================================================
*/
static struct {
	int    frames;
	double t0;
	int    fps;
} s_lbl;

void R_GfxLabelDraw(void)
{
	char text[256];
	int maxfps, cap, hz, vsync, ms;
	float scale;
	vec4_t white = { 1, 1, 1, 1 };
	vec4_t red = { 1, 0.25f, 0.2f, 1 };
	float virt[2];
	double now = R_GfxNowMs();

	s_lbl.frames++;
	if (s_lbl.t0 <= 0.0) s_lbl.t0 = now;
	if (now - s_lbl.t0 >= 500.0) {
		s_lbl.fps = (int)(s_lbl.frames * 1000.0 / (now - s_lbl.t0) + 0.5);
		s_lbl.frames = 0;
		s_lbl.t0 = now;
	}

	// visible with r_gfxLabel 1, or whenever the HOME bypass is on (V3-C4: the viewer must see which image is up)
	if (!((r_gfxLabel && r_gfxLabel->integer) || (r_msaaBypass && r_msaaBypass->integer))) return;
	if (!tr.pFontDebugStrings) return;

	// the limiter's real rate: com_maxfps paces in whole milliseconds (common.c), so 125 -> 8 ms -> 125 fps,
	// 180 -> 5 ms -> 200 fps; with vsync live the display refresh caps it again.
	maxfps = ri.Cvar_VariableIntegerValue("com_maxfps");
	hz = ri.Cvar_VariableIntegerValue("r_displayHz");
	vsync = ri.Cvar_VariableIntegerValue("r_vsyncActive");
	cap = 0;
	if (maxfps > 0) {
		ms = 1000 / maxfps;
		cap = ms > 0 ? 1000 / ms : 1000;
	}
	if (vsync && hz > 0 && (cap == 0 || hz < cap)) cap = hz;

	Com_sprintf(text, sizeof(text), "AA: %s   SHADOWS: %s   fps %d / %d%s",
		R_MsaaLabel(),
		"OLD",
		s_lbl.fps, cap, vsync ? "  vsync" : "");

	scale = glConfig.vidHeight / 720.0f;
	if (scale < 1.0f) scale = 1.0f;
	virt[0] = virt[1] = scale;

	RE_SetColor((cap > 0 && s_lbl.fps < cap * 0.95f) ? red : white);
	R_IssuePendingRenderCommands();
	if (!backEnd.projection2D) {
		RB_SetGL2D();
	}
	R_DrawString(tr.pFontDebugStrings, text, 8.0f, 8.0f, -1, virt);
	RE_SetColor(NULL);
}
