/*
===========================================================================
HZM coop [2026-09-27] REALISTIC LIGHTNING - the gl2 half (docs/proposals/lightning_2026-09-27/plan.md 2.4).
Contract and switches: renderercommon/hzm_lightning.h.

  R_HzmLt_Register     r_hzmLightning (-1 = HZM_LIGHTNING_AUTO, flags 0), r_hzmLtState (cgame writes it)
  R_HzmLt_FrontEnd     per main scene: parse r_hzmLtState into tr.hzmLt (stale > 2 s = off, Omaha = off)
  RB_HzmLt_SetUniforms per lightall/generic stage, from RB_SetGlobalFogUniforms' call sites:
                         u_HzmLtFog/View/Proj  - fog in-scatter toward the strike (the haze glows where the light is)
                         u_HzmLtWorld          - outdoor surfaces lit by the strike, visibility = the sun shadow mask
                       all ZERO unless a strike is lit, so with the switch off every program sees exactly today's
                       inputs (GPU uniforms start at 0 and GLSL_SetUniformVec4 caches, so zero is the inert value)
  RB_HzmLt_SkyPass     after the main opaque list: an additive full-screen quad at depth range (1,1), so it lands
                       only on sky pixels: the lit cloud lobe and the far bolt. Its program and the bolt image are
                       created on the FIRST lit frame, never at start-up, so a player who never enables the feature
                       cannot be affected by this code at all.

v1 uses the SUN shadow mask as the sky-visibility test for the surface light (a pixel the sun reaches is open to the
sky), not a strike-direction shadow map: it is already computed every frame on every rain map (all 21 carry sun
keys), costs nothing, and during a 0.1-0.4 s flash the shadow direction is not readable. Indoors therefore stays dark
except where daylight comes in. Characters get no surface flash (r_charLightShadow 0: they never receive the mask).
===========================================================================
*/

#include "tr_local.h"
#include "../renderercommon/hzm_lightning.h"

static cvar_t *r_hzmLightning;
static cvar_t *r_hzmLtState;

extern const char *fallbackShader_hzmlt_sky_vp;
extern const char *fallbackShader_hzmlt_sky_fp;

void R_HzmLt_Register(void)
{
	r_hzmLightning = ri.Cvar_Get("r_hzmLightning", "-1", 0);
	r_hzmLtState   = ri.Cvar_Get("r_hzmLtState", "0", 0);
	ri.Cvar_Set("r_hzmLtState", "0");
	// T10/T14e: proves which renderer loaded (grep -a -c LTBUILD in the DLL finds the literal)
	ri.Printf(PRINT_ALL, "^~^~^ LTBUILD gl2 %s %s auto=%d\n", __DATE__, __TIME__, HZM_LIGHTNING_AUTO);
}

static qboolean R_HzmLt_Enabled(void)
{
	if (!r_hzmLightning || !HZM_ResolveAutoSwitch(r_hzmLightning->integer, HZM_LIGHTNING_AUTO)) {
		return qfalse;
	}
	if (!tr.world || R_HZM_LightRestoreProtectedWorld()) {
		return qfalse;
	}
	return qtrue;
}

/*
Front end, once per RE_RenderScene with a world: refresh tr.hzmLt from r_hzmLtState.
*/
void R_HzmLt_FrontEnd(int refdefTime)
{
	hzmLtState_t *lt = &tr.hzmLt;
	int           ver = 0, t = 0, seed = 0;
	float         eSky = 0, eWorld = 0, eHaze = 0, eBolt = 0, sx = 0, sy = 0, sz = 0, byaw = 0, btop = 0;

	lt->on = qfalse;
	if (!r_hzmLtState || !R_HzmLt_Enabled()) {
		return;
	}
	if (!r_hzmLtState->string[0] || r_hzmLtState->string[0] == '0' && !r_hzmLtState->string[1]) {
		return;
	}
	if (sscanf(r_hzmLtState->string, "%d %d %f %f %f %f %f %f %f %d %f %f", &ver, &t, &eSky, &eWorld, &eHaze, &eBolt,
			   &sx, &sy, &sz, &seed, &byaw, &btop)
		!= 12 || ver != HZM_LT_STATE_VERSION) {
		return;
	}
	// vet V10: a state the cgame stopped refreshing (snapshot loss, cinematic, disconnect) must not stay lit
	if (refdefTime - t > HZM_LT_STALE_MS || t - refdefTime > HZM_LT_STALE_MS) {
		return;
	}
	lt->eSky   = eSky > 4.0f ? 4.0f : (eSky < 0.0f ? 0.0f : eSky);
	lt->eWorld = eWorld > 4.0f ? 4.0f : (eWorld < 0.0f ? 0.0f : eWorld);
	lt->eHaze  = eHaze > 4.0f ? 4.0f : (eHaze < 0.0f ? 0.0f : eHaze);
	lt->eBolt  = eBolt > 4.0f ? 4.0f : (eBolt < 0.0f ? 0.0f : eBolt);
	VectorSet(lt->dir, sx, sy, sz);
	if (VectorNormalize(lt->dir) < 0.5f) {
		return;
	}
	lt->boltSeed  = seed;
	lt->boltYaw   = byaw;
	lt->boltTopEl = btop;
	lt->on        = (lt->eSky > 0.0f || lt->eWorld > 0.0f || lt->eHaze > 0.0f || lt->eBolt > 0.0f) ? qtrue : qfalse;
}

// ---------------------------------------------------------------- per-stage uniforms
static qboolean RB_HzmLt_MainView(void)
{
	if (!tr.hzmLt.on || backEnd.projection2D || backEnd.depthFill) {
		return qfalse;
	}
	if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) {
		return qfalse;
	}
	if (backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky
		|| (backEnd.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW))) {
		return qfalse;
	}
	if (tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo) {
		return qfalse;
	}
	if (tr.sunRaysFbo && glState.currentFBO == tr.sunRaysFbo) {
		return qfalse;
	}
	return qtrue;
}

void RB_HzmLt_ZeroUniforms(shaderProgram_t *sp)
{
	vec4_t zero = {0.0f, 0.0f, 0.0f, 0.0f};

	GLSL_SetUniformVec4(sp, UNIFORM_HZMLTFOG, zero);
	GLSL_SetUniformVec4(sp, UNIFORM_HZMLTWORLD, zero);
}

/*
stateBits: only OPAQUE and ALPHA-BLENDED stages take the light. An additive stage fogs toward black and a modulate stage
toward white (RB_SetGlobalFogUniforms), so an in-scatter colour or a surface light there would brighten the wrong
thing. fogAsSky: the sky shell never takes the haze term - the sky pass lights the sky.
*/
void RB_HzmLt_SetUniforms(shaderProgram_t *sp, int stateBits, qboolean fogAsSky)
{
	vec4_t   v;
	int      src = stateBits & GLS_SRCBLEND_BITS, dst = stateBits & GLS_DSTBLEND_BITS;
	qboolean opaque, alphaBlend;
	const float *m = backEnd.viewParms.world.modelMatrix;
	const float *p = backEnd.viewParms.projectionMatrix;

	if (!RB_HzmLt_MainView()) {
		RB_HzmLt_ZeroUniforms(sp);
		return;
	}
	opaque     = (!src && !dst) ? qtrue : qfalse;
	alphaBlend = (src == GLS_SRCBLEND_SRC_ALPHA && dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA) ? qtrue : qfalse;
	if (!opaque && !alphaBlend) {
		RB_HzmLt_ZeroUniforms(sp);
		return;
	}

	// fog in-scatter: the strike direction in EYE space, so the fragment rebuilds its ray from gl_FragCoord alone
	if (!fogAsSky && tr.hzmLt.eHaze > 0.0f && p[0] != 0.0f && p[5] != 0.0f) {
		const vec_t *d = tr.hzmLt.dir;
		v[0] = m[0] * d[0] + m[4] * d[1] + m[8] * d[2];
		v[1] = m[1] * d[0] + m[5] * d[1] + m[9] * d[2];
		v[2] = m[2] * d[0] + m[6] * d[1] + m[10] * d[2];
		v[3] = tr.hzmLt.eHaze;
		GLSL_SetUniformVec4(sp, UNIFORM_HZMLTFOG, v);
		VectorSet4(v, (float)backEnd.viewParms.viewportX, (float)backEnd.viewParms.viewportY,
				   1.0f / (float)backEnd.viewParms.viewportWidth, 1.0f / (float)backEnd.viewParms.viewportHeight);
		GLSL_SetUniformVec4(sp, UNIFORM_HZMLTVIEW, v);
		VectorSet4(v, 1.0f / p[0], 1.0f / p[5], 0.0f, 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_HZMLTPROJ, v);
	} else {
		VectorSet4(v, 0.0f, 0.0f, 0.0f, 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_HZMLTFOG, v);
	}

	// outdoor surface light (world space; the shader multiplies by the sun mask, so it only reaches open sky)
	if (!fogAsSky && opaque && tr.hzmLt.eWorld > 0.0f) {
		VectorSet4(v, tr.hzmLt.dir[0], tr.hzmLt.dir[1], tr.hzmLt.dir[2], tr.hzmLt.eWorld);
	} else {
		VectorSet4(v, 0.0f, 0.0f, 0.0f, 0.0f);
	}
	GLSL_SetUniformVec4(sp, UNIFORM_HZMLTWORLD, v);
}

// ---------------------------------------------------------------- the bolt image (built per strike from its seed)
#define LT_BOLT_W 128
#define LT_BOLT_H 256

static unsigned lt_rng;
static float    LT_BRand(void)
{
	lt_rng = lt_rng * 1664525u + 1013904223u;
	return (float)((lt_rng >> 8) & 0xFFFFFF) / 16777216.0f;
}

static float LT_BGauss(void)
{
	return (LT_BRand() + LT_BRand() + LT_BRand() + LT_BRand() - 2.0f) * 0.85f;
}

static void LT_Segment(float *buf, float x0, float y0, float x1, float y1, float inten)
{
	float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
	int   n  = (int)(len * 1.5f) + 2, i;

	for (i = 0; i <= n; i++) {
		float t = (float)i / n;
		int   x = (int)(x0 + dx * t), y = (int)(y0 + dy * t);
		if (x >= 0 && x < LT_BOLT_W && y >= 0 && y < LT_BOLT_H && buf[y * LT_BOLT_W + x] < inten) {
			buf[y * LT_BOLT_W + x] = inten;
		}
	}
}

// midpoint displacement between two points, depth levels, amplitude relative to the segment length
static void LT_Channel(float *buf, float x0, float y0, float x1, float y1, int depth, float amp, float inten)
{
	float mx, my, dx, dy, len;

	if (depth == 0) {
		LT_Segment(buf, x0, y0, x1, y1, inten);
		return;
	}
	dx  = x1 - x0;
	dy  = y1 - y0;
	len = sqrtf(dx * dx + dy * dy);
	mx  = (x0 + x1) * 0.5f;
	my  = (y0 + y1) * 0.5f;
	if (len > 0.001f) {
		float o = LT_BGauss() * amp * len;
		mx += -dy / len * o;
		my += dx / len * o;
	}
	LT_Channel(buf, x0, y0, mx, my, depth - 1, amp * 0.62f, inten);
	LT_Channel(buf, mx, my, x1, y1, depth - 1, amp * 0.62f, inten);
}

static void LT_BoxBlur(const float *src, float *dst, int r)
{
	int x, y, k;
	static float tmp[LT_BOLT_W * LT_BOLT_H];

	for (y = 0; y < LT_BOLT_H; y++) {
		for (x = 0; x < LT_BOLT_W; x++) {
			float s = 0.0f;
			for (k = -r; k <= r; k++) {
				int xx = x + k;
				if (xx >= 0 && xx < LT_BOLT_W) {
					s += src[y * LT_BOLT_W + xx];
				}
			}
			tmp[y * LT_BOLT_W + x] = s / (2 * r + 1);
		}
	}
	for (y = 0; y < LT_BOLT_H; y++) {
		for (x = 0; x < LT_BOLT_W; x++) {
			float s = 0.0f;
			for (k = -r; k <= r; k++) {
				int yy = y + k;
				if (yy >= 0 && yy < LT_BOLT_H) {
					s += tmp[yy * LT_BOLT_W + x];
				}
			}
			dst[y * LT_BOLT_W + x] = s / (2 * r + 1);
		}
	}
}

static void RB_HzmLt_BuildBolt(int seed)
{
	static float core[LT_BOLT_W * LT_BOLT_H], g1[LT_BOLT_W * LT_BOLT_H], g2[LT_BOLT_W * LT_BOLT_H];
	static byte  rgba[LT_BOLT_W * LT_BOLT_H * 4];
	int          i, b;

	memset(core, 0, sizeof(core));
	lt_rng = (unsigned)seed * 2246822519u + 777u;

	// the main channel from the cloud (row 0) to the ground end (last row), then 2-4 branches off its upper half
	{
		float x0 = LT_BOLT_W * (0.35f + 0.3f * LT_BRand()), x1 = LT_BOLT_W * (0.3f + 0.4f * LT_BRand());
		LT_Channel(core, x0, 2.0f, x1, LT_BOLT_H - 3.0f, 8, 0.22f, 1.0f);
		for (b = 0; b < 2 + (int)(LT_BRand() * 3.0f); b++) {
			float t   = 0.12f + 0.45f * LT_BRand();
			float sx  = x0 + (x1 - x0) * t + LT_BGauss() * 6.0f, sy = t * LT_BOLT_H;
			float ang = (LT_BRand() * 2.0f - 1.0f) * 1.0f, len = LT_BOLT_H * (0.08f + 0.14f * LT_BRand());
			LT_Channel(core, sx, sy, sx + sinf(ang) * len, sy + (cosf(ang) * 0.9f + 0.3f) * len, 6, 0.25f,
					   0.35f + 0.25f * LT_BRand());
		}
	}
	LT_BoxBlur(core, g1, 2);
	LT_BoxBlur(g1, g2, 6);
	for (i = 0; i < LT_BOLT_W * LT_BOLT_H; i++) {
		float v = core[i] * 0.85f + g1[i] * 2.2f + g2[i] * 2.5f;
		byte  c;
		// fade the ground end into the haze (the lowest fifth of the channel)
		float row = (float)(i / LT_BOLT_W) / LT_BOLT_H;
		if (row > 0.8f) {
			v *= 1.0f - (row - 0.8f) / 0.2f * 0.7f;
		}
		if (v > 1.0f) {
			v = 1.0f;
		}
		c               = (byte)(v * 255.0f);
		rgba[i * 4 + 0] = c;
		rgba[i * 4 + 1] = c;
		rgba[i * 4 + 2] = c;
		rgba[i * 4 + 3] = 255;
	}
	if (!tr.hzmLtBoltImage) {
		tr.hzmLtBoltImage = R_CreateImage("*hzmLtBolt", rgba, LT_BOLT_W, LT_BOLT_H, IMGTYPE_COLORALPHA,
										  IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_RGBA8);
	} else {
		R_UpdateSubImage(tr.hzmLtBoltImage, rgba, 0, 0, LT_BOLT_W, LT_BOLT_H, GL_RGBA8);
	}
	tr.hzmLt.builtSeed = seed;
}

// ---------------------------------------------------------------- the sky pass
void RB_HzmLt_SkyPass(void)
{
	vec4_t quadVerts[4];
	vec2_t texCoords[4];
	vec4_t v;
	vec3_t viewVector;
	float  xmax, ymax;
	float  eBolt = tr.hzmLt.eBolt;

	if (!RB_HzmLt_MainView() || (tr.hzmLt.eSky <= 0.0f && eBolt <= 0.0f)) {
		return;
	}
	if (!tr.hzmLtSkyShaderTried) {
		tr.hzmLtSkyShaderTried = qtrue;
		tr.hzmLtSkyShaderOk    = GLSL_HzmLtInitSkyShader();
		ri.Printf(PRINT_ALL, "^~^~^ LT r1=%s map=%s\n", tr.hzmLtSkyShaderOk ? "on" : "off:shader",
				  tr.world ? tr.world->baseName : "?");
	}
	if (!tr.hzmLtSkyShaderOk) {
		return;
	}

	if (eBolt > 0.0f && tr.hzmLt.boltTopEl - HZM_LT_BOLT_MIN_EL_DEG < 2.0f) {
		eBolt = 0.0f;   // too low to draw above the 2-degree floor
	}
	if (eBolt > 0.0f && (!tr.hzmLtBoltImage || tr.hzmLt.builtSeed != tr.hzmLt.boltSeed)) {
		RB_HzmLt_BuildBolt(tr.hzmLt.boltSeed);
	}

	GLSL_BindProgram(&tr.hzmLtSkyShader);
	GL_BindToTMU((eBolt > 0.0f && tr.hzmLtBoltImage) ? tr.hzmLtBoltImage : tr.whiteImage, TB_DIFFUSEMAP);

	xmax = tan(backEnd.viewParms.fovX * M_PI / 360.0f);
	ymax = tan(backEnd.viewParms.fovY * M_PI / 360.0f);
	GLSL_SetUniformVec3(&tr.hzmLtSkyShader, UNIFORM_VIEWFORWARD, backEnd.refdef.viewaxis[0]);
	VectorScale(backEnd.refdef.viewaxis[1], xmax, viewVector);
	GLSL_SetUniformVec3(&tr.hzmLtSkyShader, UNIFORM_VIEWLEFT, viewVector);
	VectorScale(backEnd.refdef.viewaxis[2], ymax, viewVector);
	GLSL_SetUniformVec3(&tr.hzmLtSkyShader, UNIFORM_VIEWUP, viewVector);

	VectorSet4(v, tr.hzmLt.dir[0], tr.hzmLt.dir[1], tr.hzmLt.dir[2], tr.hzmLt.eSky);
	GLSL_SetUniformVec4(&tr.hzmLtSkyShader, UNIFORM_HZMLTWORLD, v);

	VectorSet4(v, 0.0f, 0.0f, 0.0f, 0.0f);
	if (eBolt > 0.0f) {
		// the bolt spans elevation [HZM_LT_BOLT_MIN_EL_DEG, top] at the strike's azimuth; it is drawn on the plane
		// tangent to the sky at its centre, twice as tall as wide (the texture's aspect)
		float lo  = (float)DEG2RAD(HZM_LT_BOLT_MIN_EL_DEG), hi = (float)DEG2RAD(tr.hzmLt.boltTopEl);
		float mid = 0.5f * (lo + hi), yaw = (float)DEG2RAD(tr.hzmLt.boltYaw);
		float hh  = tan(0.5f * (hi - lo));
		vec4_t prm;
		VectorSet4(v, cos(mid) * cos(yaw), cos(mid) * sin(yaw), sin(mid), eBolt);
		VectorSet4(prm, hh * 0.5f, hh, sin(lo), 0.0f);
		GLSL_SetUniformVec4(&tr.hzmLtSkyShader, UNIFORM_HZMLTPARAMS, prm);
	}
	GLSL_SetUniformVec4(&tr.hzmLtSkyShader, UNIFORM_HZMLTBOLT, v);

	VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet2(texCoords[0], 0.0f, 1.0f);
	VectorSet2(texCoords[1], 1.0f, 1.0f);
	VectorSet2(texCoords[2], 1.0f, 0.0f);
	VectorSet2(texCoords[3], 0.0f, 0.0f);

	// depth test ON (LEQUAL), no depth write, additive; at depth 1 only pixels nothing opaque covered pass
	GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	GL_Cull(CT_TWO_SIDED);   // the quad's winding must not matter (the last surface may have left culling on)
	qglDepthRange(1.0, 1.0);
	RB_InstantQuad2(quadVerts, texCoords);
	qglDepthRange(0.0, 1.0);
}
