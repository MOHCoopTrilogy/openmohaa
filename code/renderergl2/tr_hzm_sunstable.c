/*
===========================================================================
HZM gl2 - STABLE SUN SHADOWS + B0 (MSAA / stable-shadow plan P3; shadows plan B0 route 1 with vet section 7 D1-D3;
bug-3009 / E1 pshadow character gate). Switch: r_shadowStable (flags 0, live; 0 = today's legacy cascades verbatim).

With r_shadowStable 1 and every validity condition met (R_SunStable_Resolve), a frame renders:
  - W, the static world cascade: BSP + alpha-tested world surfaces + a fixed-detail terrain proxy (+ static models
    while r_hzmSunStaticCasters 1), baked ONCE into tr.sunShadowFbo[3] from a fixed per-map light basis, into its own
    exactly-sized drawsurf and static-surface arrays (it can never compete with, or wrap, the shared list). Re-baked
    only on: a world load, a changed sun direction, a change of the cvars that decide what it holds, or a switch back
    from legacy (legacy cascade 3 shares the image).
  - three camera-centred RADIAL cascades per frame (radii from r_shadowDistance / the worldspawn farplane, split by
    r_shadowSplitLambda), texel-snapped in the fixed basis, holding ENTITIES ONLY (B0: the world's shadow is already
    baked into the lightmap; casting it again was the double shadow of bug-3006).
  - the screen-space mask (shadowmask_fp USE_SHADOW_STABLE) now carries only V_entity.
  - lightall samples V_world from W per pixel with a face-normal offset (D1) and gates: world stages
    mix(1, 1 - Vw*(1-Ve), facing), light-vector stages 1 - Vw*(1 - Ve*NL) (D3), sun specular f*Vw*Ve.
Anything invalid (the list in R_SunStable_Resolve) = the legacy path, exactly today's.

Every ^~^~^ line here is PRINT_ALL and bounded (once per bake / map / state change).
===========================================================================
*/
#include "tr_local.h"
#include "tr_dsa.h"

// tr_main.c (no header prototypes; all three return void - a C4013 here would truncate nothing, but it is
// declared so the build gate stays strict)
void R_SetupProjectionOrtho(viewParms_t *dest, vec3_t viewBounds[2]);
void R_SortDrawSurfs(drawSurf_t *drawSurfs, int numDrawSurfs, drawSurf_t *spriteSurfs, int numSpriteSurfs);
void R_AddEntitySurfaces(void);

#ifndef GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#endif

cvar_t *r_shadowStable;
cvar_t *r_shadowDistance;
cvar_t *r_shadowSplitLambda;
cvar_t *r_shadowSoftness;
cvar_t *r_shadowNormalOffset;
cvar_t *r_shadowConstBias;
cvar_t *r_shadowSlopeBias;
cvar_t *r_shadowBlendBand;
cvar_t *r_shadowCasterPad;
cvar_t *r_shadowWFaces;
cvar_t *r_shadowWPush;
cvar_t *r_shadowWBias;
cvar_t *r_hzmSunStaticCasters;
cvar_t *r_hzmPshadowCharGate;

#define SS_TER_VERTS    81
#define SS_TER_INDEXES  384     // 2 base triangles x 2^6 bintree leaves x 3

typedef struct {
	int              serial;          // bumped by every world load
	qboolean         haveWorld;       // R_SunStable_WorldLoaded ran for the current world
	qboolean         protectedMap;    // Q5: Omaha stays exactly as today (m3l1a m3l1b e3l1 e3l2 obj_team3)
	float            farplane;        // worldspawn farplane (0 = none), latched at load (SH#2)
	float            fDist;           // f = min(r_shadowDistance, farplane > 0)
	float            radius[3];

	// W
	qboolean         wDirty;
	qboolean         wValid;
	qboolean         wDropped;        // W-DROP: B0 resolves off for the rest of this map
	int              wSig;
	vec3_t           wSunDir;
	float            wMvp[16];
	float            wTexel;          // world units per W texel (the larger axis)
	float            wDepthRange;     // world units covered by W's depth
	int              wSize;
	int              wBakes;
	drawSurf_t      *wSurfs;
	int              wSurfCap;
	staticSurface_t *wStatics;
	int              wStaticCap;
	int              wNumStatics;

	// terrain proxy (W only)
	srfBspSurface_t *ter;
	int              numTer;

	// per-frame resolution
	int              lastMode;        // -1 = none yet, 0 = legacy, 1 = stable
	qboolean         legacyNeedsFar;  // one-shot: legacy cascade 3 must re-render (W overwrote the image)
	char             offWhy[64];
	char             lastOffWhy[64];
	int              units;           // GL_MAX_TEXTURE_IMAGE_UNITS (0 = not queried yet)
} sunStable_t;

static sunStable_t ss;

/*
=================
registration (every R_Init)
=================
*/
void R_SunStable_Register(void)
{
	// the switch. flags 0 (never archived, T7). Its default flips only in the P6 flip commit.
	r_shadowStable        = ri.Cvar_Get("r_shadowStable", "1", 0);   // P6.1 flip (was 0); B0 is part of it (route 1)
	// knobs (flags 0, plan P3)
	r_shadowDistance      = ri.Cvar_Get("r_shadowDistance", "2048", 0);
	r_shadowSplitLambda   = ri.Cvar_Get("r_shadowSplitLambda", "0.85", 0);
	r_shadowSoftness      = ri.Cvar_Get("r_shadowSoftness", "1.5", 0);
	r_shadowNormalOffset  = ri.Cvar_Get("r_shadowNormalOffset", "1.0", 0);   // x one W texel (D1)
	r_shadowConstBias     = ri.Cvar_Get("r_shadowConstBias", "0.25", 0);     // world units along the light axis
	r_shadowSlopeBias     = ri.Cvar_Get("r_shadowSlopeBias", "1.5", 0);      // glPolygonOffset factor, stable casters
	r_shadowBlendBand     = ri.Cvar_Get("r_shadowBlendBand", "0.15", 0);
	r_shadowCasterPad     = ri.Cvar_Get("r_shadowCasterPad", "512", 0);      // depth kept toward the sun, world units
	// W contents (vet section 7 D2: 1 = back faces only (R1), 2 = both faces, the sun-facing pass pushed back)
	r_shadowWFaces        = ri.Cvar_Get("r_shadowWFaces", "1", 0);
	r_shadowWPush         = ri.Cvar_Get("r_shadowWPush", "1.5", 0);          // push-back, W texels (S1, D2)
	r_shadowWBias         = ri.Cvar_Get("r_shadowWBias", "0.05", 0);         // W compare bias, world units, + = occluded
	// B0-M: 1 = static models are in W (their shadow is baked), 0 = they cast per frame like entities
	r_hzmSunStaticCasters = ri.Cvar_Get("r_hzmSunStaticCasters", "1", 0);
	// bug-3009 / E1 (plan_P3_addendum_B0 section 2): characters in every non-sun-cascade depth view. -1 = auto (0)
	r_hzmPshadowCharGate  = ri.Cvar_Get("r_hzmPshadowCharGate", "-1", 0);

	Com_Memset(&ss, 0, sizeof(ss));
	ss.lastMode = -1;
}

qboolean R_PshadowCharGate(void)
{
	int v;

	if (!r_hzmPshadowCharGate) {
		return qfalse;
	}
	v = r_hzmPshadowCharGate->integer;
	if (v < 0) {
		v = HZM_PSHADOWCHARGATE_AUTO;
	}
	return (qboolean)(v != 0);
}

/*
=================
the fixed per-map light basis: axis[0] = away from the sun (the light's view direction), up Z (Y when |z| > 0.9)
=================
*/
static qboolean R_SunStable_Basis(const vec3_t sunDir, vec3_t axis[3])
{
	vec3_t d;

	VectorCopy(sunDir, d);
	if (VectorNormalize(d) < 1e-3f) {
		return qfalse;
	}
	VectorScale(d, -1.0f, axis[0]);
	if (fabsf(axis[0][2]) > 0.9f) {
		VectorSet(axis[2], 0, 1, 0);
	} else {
		VectorSet(axis[2], 0, 0, 1);
	}
	CrossProduct(axis[2], axis[0], axis[1]);
	VectorNormalize(axis[1]);
	CrossProduct(axis[0], axis[1], axis[2]);
	VectorNormalize(axis[2]);
	return qtrue;
}

/*
=================
world load: latch f, build the terrain proxy, size W's own arrays
=================
*/
static void R_SunStable_ParseFarplane(void)
{
	const char *p;
	char        key[MAX_TOKEN_CHARS];
	char       *tok;

	ss.farplane = 0.0f;
	if (!tr.world || !tr.world->entityString) {
		return;
	}
	p = tr.world->entityString;
	tok = COM_ParseExt((char **)&p, qtrue);
	if (!tok[0] || tok[0] != '{') {
		return;
	}
	while (p) {
		tok = COM_ParseExt((char **)&p, qtrue);
		if (!tok[0] || tok[0] == '}') {
			break;
		}
		Q_strncpyz(key, tok, sizeof(key));
		tok = COM_ParseExt((char **)&p, qtrue);
		if (!tok[0] || tok[0] == '}') {
			break;
		}
		if (!Q_stricmp(key, "farplane")) {
			ss.farplane = (float)atof(tok);
		}
	}
}

static int s_terIdx;

static void R_SunStable_TerSplit(glIndex_t *out, const int *p0, const int *p1, const int *p2, int depth)
{
	int m[2];

	if (depth == 0) {
		out[s_terIdx++] = (glIndex_t)(p0[1] * 9 + p0[0]);
		out[s_terIdx++] = (glIndex_t)(p1[1] * 9 + p1[0]);
		out[s_terIdx++] = (glIndex_t)(p2[1] * 9 + p2[0]);
		return;
	}
	// the ROAM split (tr_terrain.c R_SplitTri): hypotenuse iPt[0]-iPt[1], new point at its midpoint;
	// left child (p1, p2, m), right child (p2, p0, m) - same winding as the rendered terrain
	m[0] = (p0[0] + p1[0]) / 2;
	m[1] = (p0[1] + p1[1]) / 2;
	R_SunStable_TerSplit(out, p1, p2, m, depth - 1);
	R_SunStable_TerSplit(out, p2, p0, m, depth - 1);
}

static void R_SunStable_BuildTerrainProxy(void)
{
	static const int g00[2] = { 0, 0 }, g01[2] = { 0, 8 }, g10[2] = { 8, 0 }, g11[2] = { 8, 8 };
	int i, x, y;

	ss.ter = NULL;
	ss.numTer = 0;
	if (!tr.world || tr.world->numTerraPatches <= 0) {
		return;
	}
	ss.ter = ri.Hunk_Alloc(tr.world->numTerraPatches * sizeof(srfBspSurface_t), h_low);
	for (i = 0; i < tr.world->numTerraPatches; i++) {
		const cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];
		srfBspSurface_t *s = &ss.ter[i];
		srfVert_t *v = ri.Hunk_Alloc(SS_TER_VERTS * sizeof(srfVert_t), h_low);
		glIndex_t *ix = ri.Hunk_Alloc(SS_TER_INDEXES * sizeof(glIndex_t), h_low);

		Com_Memset(s, 0, sizeof(*s));
		Com_Memset(v, 0, SS_TER_VERTS * sizeof(srfVert_t));
		// full-detail heights: the geometry the lightmap compiler lit (ROAM at max LOD draws exactly this)
		for (y = 0; y < 9; y++) {
			for (x = 0; x < 9; x++) {
				srfVert_t *sv = &v[y * 9 + x];
				sv->xyz[0] = patch->x0 + 64.0f * x;
				sv->xyz[1] = patch->y0 + 64.0f * y;
				sv->xyz[2] = (float)(patch->heightmap[y * 9 + x] * 2) + patch->z0;
			}
		}
		// the two base triangles exactly as R_PreTessellateTerrain orders them (TERPATCH_NEIGHBOR 0x80 picks the
		// diagonal, TERPATCH_FLIP 0x40 the winding)
		s_terIdx = 0;
		if ((patch->flags & 0x80u) == 0) {
			if (patch->flags & 0x40) {
				R_SunStable_TerSplit(ix, g00, g11, g01, 6);
				R_SunStable_TerSplit(ix, g11, g00, g10, 6);
			} else {
				R_SunStable_TerSplit(ix, g11, g00, g01, 6);
				R_SunStable_TerSplit(ix, g00, g11, g10, 6);
			}
		} else {
			if (patch->flags & 0x40) {
				R_SunStable_TerSplit(ix, g01, g10, g11, 6);
				R_SunStable_TerSplit(ix, g10, g01, g00, 6);
			} else {
				R_SunStable_TerSplit(ix, g10, g01, g11, 6);
				R_SunStable_TerSplit(ix, g01, g10, g00, 6);
			}
		}
		s->surfaceType = SF_TRIANGLES;
		s->numVerts = SS_TER_VERTS;
		s->verts = v;
		s->numIndexes = s_terIdx;
		s->indexes = ix;
		VectorSet(s->cullBounds[0], patch->x0, patch->y0, patch->z0);
		VectorSet(s->cullBounds[1], patch->x0 + 512.0f, patch->y0 + 512.0f, patch->z0 + patch->zmax);
		ss.numTer++;
	}
}

static int R_SunStable_CountStaticSurfaces(void)
{
	int i, mesh, n = 0;

	if (!tr.world) {
		return 0;
	}
	for (i = 0; i < tr.world->numStaticModels; i++) {
		dtiki_t *tiki = tr.world->staticModels[i].tiki;
		if (!tiki) {
			continue;
		}
		for (mesh = 0; mesh < tiki->numMeshes; mesh++) {
			skelHeaderGame_t *skelmodel = ri.TIKI_GetSkel(tiki->mesh[mesh]);
			if (skelmodel) {
				n += skelmodel->numSurfaces;
			}
		}
	}
	return n;
}

void R_SunStable_WorldLoaded(const char *name)
{
	int  numStatic;
	char base[MAX_QPATH];

	ss.serial++;
	ss.haveWorld = (qboolean)(tr.world != NULL);
	ss.wDirty = qtrue;
	ss.wValid = qfalse;
	ss.wDropped = qfalse;
	ss.wBakes = 0;
	ss.lastMode = -1;
	ss.legacyNeedsFar = qfalse;
	ss.lastOffWhy[0] = 0;
	if (!tr.world) {
		return;
	}

	COM_StripExtension(COM_SkipPath((char *)name), base, sizeof(base));
	// Omaha = the same five maps ground variety excludes (tr_hzm_groundvar.c R_HZM_GroundVarOmahaMap), as the BSP base
	// name or a suffixed copy (m3l1a_sml); 2026-10-04 was m3l1a/m3l1b only
	{
		static const char *const omaha[] = { "m3l1a", "m3l1b", "e3l1", "e3l2", "obj_team3" };
		size_t len = strlen(base), n;
		int    k;

		ss.protectedMap = qfalse;
		for (k = 0; k < (int)(sizeof(omaha) / sizeof(omaha[0])); k++) {
			n = strlen(omaha[k]);
			if (len >= n && !Q_stricmpn(base, omaha[k], (int)n) && (len == n || base[n] == '_' || base[n] == '.')) {
				ss.protectedMap = qtrue;
			}
		}
	}

	R_SunStable_ParseFarplane();
	R_SunStable_BuildTerrainProxy();

	numStatic = R_SunStable_CountStaticSurfaces();
	ss.wStaticCap = numStatic + 16;
	ss.wStatics = ri.Hunk_Alloc(ss.wStaticCap * sizeof(staticSurface_t), h_low);
	// exactly sized: every world surface + every terrain proxy + every static surface (+ a margin). A W larger than
	// the radix sort's scratch is refused up front (W-DROP) instead of truncated.
	ss.wSurfCap = tr.world->numWorldSurfaces + ss.numTer + numStatic + 64;
	if (ss.wSurfCap > MAX_DRAWSURFS) {
		ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET W-DROP map=%s reason=capacity need=%d max=%d (stable shadows resolve to legacy on this map)\n",
			base, ss.wSurfCap, MAX_DRAWSURFS);
		ss.wDropped = qtrue;
		ss.wSurfCap = MAX_DRAWSURFS;
	}
	ss.wSurfs = ri.Hunk_Alloc(ss.wSurfCap * sizeof(drawSurf_t), h_low);

	ss.fDist = r_shadowDistance->value;
	if (ss.fDist < 256.0f) {
		ss.fDist = 256.0f;
	}
	if (ss.farplane > 0.0f && ss.farplane < ss.fDist) {
		ss.fDist = ss.farplane;
	}

	// quiet while the switch is off (console quiet, 2026-09-27): only with r_shadowStable or the probes on
	if (r_shadowStable->integer || (r_gfxProbe && r_gfxProbe->integer))
	ri.Printf(PRINT_ALL, "^~^~^ SHADOWFIT map=%s f=%.0f farplane=%.0f distance=%.0f terrain_proxy=%d static_surfs=%d w_cap=%d protected=%d\n",
		base, ss.fDist, ss.farplane, r_shadowDistance->value, ss.numTer, numStatic, ss.wSurfCap, (int)ss.protectedMap);
}

/*
=================
validity (plan section 3.0 "B0 resolves off", vet R7, addendum 1b D1)
=================
*/
static qboolean R_SunStable_Off(const char *why)
{
	Q_strncpyz(ss.offWhy, why, sizeof(ss.offWhy));
	return qfalse;
}

qboolean R_SunStable_Resolve(void)
{
	float l;

	ss.offWhy[0] = 0;
	if (!r_shadowStable || !r_shadowStable->integer) {
		return R_SunStable_Off("r_shadowStable 0");
	}
	if (!tr.world || !ss.haveWorld) {
		return R_SunStable_Off("no world");
	}
	if (!glRefConfig.framebufferObject || !tr.sunShadowFbo[0] || !tr.sunShadowFbo[1] || !tr.sunShadowFbo[2]
	    || !tr.sunShadowFbo[3]) {
		return R_SunStable_Off("no shadow targets");
	}
	if (!tr.shadowmaskStableShader.program) {
		return R_SunStable_Off("stable mask shader not built");
	}
	if (r_sunlightMode->integer != 1) {
		return R_SunStable_Off("r_sunlightMode not 1");
	}
	if (r_forceSun->integer == 2) {
		return R_SunStable_Off("r_forceSun 2");
	}
	if (r_charLightShadow && r_charLightShadow->integer) {
		return R_SunStable_Off("r_charLightShadow 1");
	}
	if (ss.protectedMap) {
		return R_SunStable_Off("protected map (Omaha)");
	}
	if (ss.wDropped) {
		return R_SunStable_Off("W-DROP");
	}
	l = VectorLength(tr.refdef.sunDir);
	if (l < 1e-3f) {
		return R_SunStable_Off("|sunDir| < 1e-3");
	}
	if (!ss.units) {
		GLint u = 0;
		qglGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &u);
		ss.units = u > 0 ? u : -1;
	}
	if (ss.units < TB_SUNWORLD + 1) {
		return R_SunStable_Off("fewer than 9 texture units");
	}
	return qtrue;
}

qboolean R_SunStable_LegacyNeedsFar(void)
{
	qboolean r = ss.legacyNeedsFar;
	ss.legacyNeedsFar = qfalse;
	return r;
}

// SE#21: depth-compare filtering LINEAR (hardware 2x2 PCF) while stable, NEAREST (today) for legacy
static void R_SunStable_SetFilters(qboolean linear)
{
	int i;
	GLenum f = linear ? GL_LINEAR : GL_NEAREST;

	for (i = 0; i < 4; i++) {
		if (tr.sunShadowDepthImage[i]) {
			qglTextureParameterfEXT(tr.sunShadowDepthImage[i]->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
			qglTextureParameterfEXT(tr.sunShadowDepthImage[i]->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		}
	}
}

static int R_SunStable_WSignature(void)
{
	return r_shadowWFaces->modificationCount
	     + 7 * r_hzmSunStaticCasters->modificationCount
	     + 31 * r_drawstaticmodels->modificationCount
	     + 127 * r_shadowWPush->modificationCount
	     + 509 * r_shadowMapBiasFactor->modificationCount
	     + 1021 * r_shadowMapBiasUnits->modificationCount;
}

/*
=================
W bake
=================
*/
static void R_SunStable_ExtendBounds(vec3_t mins, vec3_t maxs, const vec3_t lo, const vec3_t hi)
{
	AddPointToBounds(lo, mins, maxs);
	AddPointToBounds(hi, mins, maxs);
}

static void R_SunWorld_Bake(const char *why)
{
	viewParms_t parms;
	vec3_t      axis[3], mins, maxs, center, lb[2];
	mat4_t      lightViewMatrix;
	int         i, n, drops, savedNum, savedCap, savedDrops;
	qboolean    savedWrap;
	drawSurf_t *savedSurfs;
	FBO_t      *fbo = tr.sunShadowFbo[3];
	float       ext1, ext2;

	if (!R_SunStable_Basis(tr.refdef.sunDir, axis)) {
		return;
	}

	// the whole static world: BSP, terrain, static models
	ClearBounds(mins, maxs);
	if (tr.world->bmodels) {
		R_SunStable_ExtendBounds(mins, maxs, tr.world->bmodels[0].bounds[0], tr.world->bmodels[0].bounds[1]);
	}
	for (i = 0; i < ss.numTer; i++) {
		R_SunStable_ExtendBounds(mins, maxs, ss.ter[i].cullBounds[0], ss.ter[i].cullBounds[1]);
	}
	for (i = 0; i < tr.world->numStaticModels; i++) {
		const cStaticModelUnpacked_t *SM = &tr.world->staticModels[i];
		vec3_t lo, hi;
		VectorSet(lo, SM->origin[0] - SM->cull_radius, SM->origin[1] - SM->cull_radius, SM->origin[2] - SM->cull_radius);
		VectorSet(hi, SM->origin[0] + SM->cull_radius, SM->origin[1] + SM->cull_radius, SM->origin[2] + SM->cull_radius);
		R_SunStable_ExtendBounds(mins, maxs, lo, hi);
	}
	if (mins[0] > maxs[0]) {
		return;
	}
	VectorAdd(mins, maxs, center);
	VectorScale(center, 0.5f, center);

	Mat4View(axis, center, lightViewMatrix);
	ClearBounds(lb[0], lb[1]);
	for (i = 0; i < 8; i++) {
		vec4_t p, lp;
		p[0] = (i & 1) ? maxs[0] : mins[0];
		p[1] = (i & 2) ? maxs[1] : mins[1];
		p[2] = (i & 4) ? maxs[2] : mins[2];
		p[3] = 1.0f;
		Mat4Transform(lightViewMatrix, p, lp);
		AddPointToBounds(lp, lb[0], lb[1]);
	}
	for (i = 0; i < 3; i++) {
		lb[0][i] -= 16.0f;
		lb[1][i] += 16.0f;
	}

	ss.wSize = fbo->width;
	ext1 = lb[1][1] - lb[0][1];
	ext2 = lb[1][2] - lb[0][2];
	ss.wTexel = MAX(ext1, ext2) / (float)fbo->width;
	ss.wDepthRange = lb[1][0] - lb[0][0];

	Com_Memset(&parms, 0, sizeof(parms));
	parms.viewportX = 0;
	parms.viewportY = 0;
	parms.viewportWidth = fbo->width;
	parms.viewportHeight = fbo->height;
	parms.fovX = 90;
	parms.fovY = 90;
	parms.targetFbo = fbo;
	parms.flags = VPF_DEPTHSHADOW | VPF_ORTHOGRAPHIC | VPF_NOVIEWMODEL | VPF_SUNWORLD;
	if (glRefConfig.depthClamp) {
		parms.flags |= VPF_DEPTHCLAMP;
	}
	parms.zFar = lb[1][0];
	parms.shadowCascade = 4;
	parms.hzmWFaces = (r_shadowWFaces->integer == 2) ? 2 : 1;
	// push-back (S1, D2): slope part in texels, plus a constant of the same size in world units, in depth units
	parms.hzmWPush[0] = r_shadowWPush->value;
	parms.hzmWPush[1] = (ss.wDepthRange > 0.0f) ? (r_shadowWPush->value * ss.wTexel) * 16777216.0f / ss.wDepthRange : 0.0f;
	VectorCopy(center, parms.or.origin);
	VectorCopy(axis[0], parms.or.axis[0]);
	VectorCopy(axis[1], parms.or.axis[1]);
	VectorCopy(axis[2], parms.or.axis[2]);
	VectorCopy(center, parms.pvsOrigin);

	tr.viewCount++;
	tr.viewParms = parms;
	tr.viewParms.frameSceneNum = tr.frameSceneNum;
	tr.viewParms.frameCount = tr.frameCount;
	tr.viewCount++;

	// W's own arrays: never the shared list, never the per-frame static pool
	savedSurfs = tr.refdef.drawSurfs;
	savedNum = tr.refdef.numDrawSurfs;
	savedWrap = r_drawSurfNoWrap;
	savedCap = r_drawSurfCap;
	savedDrops = r_drawSurfDrops;
	tr.refdef.drawSurfs = ss.wSurfs;
	tr.refdef.numDrawSurfs = 0;
	r_drawSurfNoWrap = qtrue;
	r_drawSurfCap = ss.wSurfCap;
	ss.wNumStatics = 0;

	R_RotateForViewer();
	R_SetupProjectionOrtho(&tr.viewParms, lb);
	R_AddWorldSurfaces();   // VPF_SUNWORLD: tr_world.c swaps ROAM terrain for the proxy; static models per B0-M

	n = tr.refdef.numDrawSurfs;
	drops = r_drawSurfDrops - savedDrops;
	R_SortDrawSurfs(ss.wSurfs, n, tr.refdef.spriteSurfs + tr.refdef.numSpriteSurfs, 0);

	tr.refdef.drawSurfs = savedSurfs;
	tr.refdef.numDrawSurfs = savedNum;
	r_drawSurfNoWrap = savedWrap;
	r_drawSurfCap = savedCap;
	r_drawSurfDrops = savedDrops;

	Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, ss.wMvp);
	VectorCopy(tr.refdef.sunDir, ss.wSunDir);
	ss.wSig = R_SunStable_WSignature();
	ss.wDirty = qfalse;
	ss.wBakes++;

	if (drops || ss.wNumStatics >= ss.wStaticCap) {
		ss.wDropped = qtrue;
		ss.wValid = qfalse;
		ss.legacyNeedsFar = qtrue;   // the partial W is in the legacy far cascade's image
		ri.Printf(PRINT_ALL, "^~^~^ SHADOWBUDGET W-DROP why=%s surfs=%d dropped=%d cap=%d statics=%d/%d (stable shadows resolve to legacy on this map)\n",
			why, n, drops, ss.wSurfCap, ss.wNumStatics, ss.wStaticCap);
	} else {
		ss.wValid = qtrue;
	}
	ri.Printf(PRINT_ALL, "^~^~^ SHADOWFIT W-BAKE why=%s n=%d surfs=%d cap=%d statics=%d terrain=%d faces=%d size=%d texel=%.2f depth=%.0f sun=%.3f %.3f %.3f\n",
		why, ss.wBakes, n, ss.wSurfCap, ss.wNumStatics, ss.numTer, parms.hzmWFaces, ss.wSize, ss.wTexel, ss.wDepthRange,
		tr.refdef.sunDir[0], tr.refdef.sunDir[1], tr.refdef.sunDir[2]);
	R_GfxProbe_WBaked();
}

// tr_staticmodels.cpp: the W bake's static surfaces come from W's own pool
staticSurface_t *R_SunWorld_StaticSlot(void)
{
	if (!ss.wStatics || ss.wNumStatics >= ss.wStaticCap) {
		ss.wNumStatics = ss.wStaticCap;   // flags W-DROP
		return NULL;
	}
	return &ss.wStatics[ss.wNumStatics++];
}

// tr_world.c: the terrain proxy into the W bake
void R_SunWorld_AddTerrainProxy(void)
{
	int i;

	for (i = 0; i < ss.numTer; i++) {
		const cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];
		if (patch->shader) {
			R_AddDrawSurf((surfaceType_t *)&ss.ter[i], patch->shader, 0, 0, 0, 0);
		}
	}
}

qboolean R_SunStable_StaticInWorld(void)
{
	return (qboolean)(!r_hzmSunStaticCasters || r_hzmSunStaticCasters->integer != 0);
}

/*
=================
W membership per static model (vet R2: "W is exactly what the lightmap baked").
FOLIAGE static models (the fshadow track's name pattern) are baked by retail only as thin box slabs or not at all
(fshadow offline B0-M, m4l3: bush_full = a bbox-wide slab, tree_oak ~ nothing), so they stay OUT of W unless the
fshadow lightmap patch re-baked their alpha-shaped canopy on this map (then they go in: near mesh, alpha-tested).
Every other static model follows r_hzmSunStaticCasters (B0-M, measured per class by p3_gate b0m).
=================
*/
static const char *const s_foliageTokens[] = {
	"tree", "bush", "shrub", "plant", "hedge", "fern", "foliage", "pine", "palm", "ivy", "vine", "bamboo", "hay", NULL
};

static qboolean R_SunStable_IsFoliagePath(const char *path)
{
	char low[128];
	int  i;

	Q_strncpyz(low, path ? path : "", sizeof(low));
	Q_strlwr(low);
	for (i = 0; s_foliageTokens[i]; i++) {
		if (strstr(low, s_foliageTokens[i])) {
			return qtrue;
		}
	}
	return qfalse;
}

qboolean R_SunStable_IsFoliageStatic(int staticIndex)
{
	if (!tr.world || staticIndex < 0 || staticIndex >= tr.world->numStaticModels) {
		return qfalse;
	}
	return R_SunStable_IsFoliagePath(tr.world->staticModels[staticIndex].model);
}

qboolean R_SunWorld_StaticWanted(int staticIndex)
{
	const cStaticModelUnpacked_t *SM;

	if (!tr.world || staticIndex < 0 || staticIndex >= tr.world->numStaticModels) {
		return qfalse;
	}
	SM = &tr.world->staticModels[staticIndex];
	// main-snap-10 (fb4abb43, bug-3224): the fshadow lightmap patch (maps/<map>.hzmlm) marks every static model whose
	// canopy + trunk it re-baked (SM->hzmFoliage, set in R_InitStaticModels). Those shadows ARE in the lightmap now, so
	// the model is in W (near mesh, alpha-tested); any other foliage-named model keeps retail's bake = out of W.
	if (SM->hzmFoliage) {
		return qtrue;
	}
	if (R_SunStable_IsFoliagePath(SM->model)) {
		return qfalse;
	}
	return R_SunStable_StaticInWorld();
}

/*
=================
one stable cascade: camera-centred radial, snapped in the fixed basis, entities only (B0)
=================
*/
static void R_SunStable_RenderCascade(const refdef_t *fd, int level)
{
	viewParms_t parms;
	vec3_t      axis[3], origin, lb[2];
	double      c0, c1, c2, t, s1, s2;
	float       R = ss.radius[level];
	FBO_t      *fbo = tr.sunShadowFbo[level];
	int         firstDrawSurf, firstSpriteSurf, i;

	if (!R_SunStable_Basis(tr.refdef.sunDir, axis)) {
		return;
	}
	t = 2.0 * R / (double)fbo->width;
	c0 = (double)fd->vieworg[0] * axis[0][0] + (double)fd->vieworg[1] * axis[0][1] + (double)fd->vieworg[2] * axis[0][2];
	c1 = (double)fd->vieworg[0] * axis[1][0] + (double)fd->vieworg[1] * axis[1][1] + (double)fd->vieworg[2] * axis[1][2];
	c2 = (double)fd->vieworg[0] * axis[2][0] + (double)fd->vieworg[1] * axis[2][1] + (double)fd->vieworg[2] * axis[2][2];
	s1 = floor(c1 / t + 0.5) * t;
	s2 = floor(c2 / t + 0.5) * t;
	for (i = 0; i < 3; i++) {
		origin[i] = (float)(axis[0][i] * c0 + axis[1][i] * s1 + axis[2][i] * s2);
	}

	// light view space relative to the snapped origin: x along the light, y/z lateral
	VectorSet(lb[0], -R - r_shadowCasterPad->value, -R, -R);
	VectorSet(lb[1], R, R, R);
	if (!glRefConfig.depthClamp) {
		lb[0][0] = lb[1][0] - 8192.0f;   // SE#8 fallback: the legacy push-out
	}

	Com_Memset(&parms, 0, sizeof(parms));
	parms.viewportWidth = fbo->width;
	parms.viewportHeight = fbo->height;
	parms.fovX = 90;
	parms.fovY = 90;
	parms.targetFbo = fbo;
	parms.flags = VPF_DEPTHSHADOW | VPF_DEPTHCLAMP | VPF_ORTHOGRAPHIC | VPF_NOVIEWMODEL | VPF_SUNSTABLE;
	parms.zFar = lb[1][0];
	parms.shadowCascade = level + 1;
	VectorCopy(origin, parms.or.origin);
	VectorCopy(axis[0], parms.or.axis[0]);
	VectorCopy(axis[1], parms.or.axis[1]);
	VectorCopy(axis[2], parms.or.axis[2]);
	VectorCopy(origin, parms.pvsOrigin);

	tr.viewCount++;
	tr.viewParms = parms;
	tr.viewParms.frameSceneNum = tr.frameSceneNum;
	tr.viewParms.frameCount = tr.frameCount;
	firstDrawSurf = tr.refdef.numDrawSurfs;
	firstSpriteSurf = tr.refdef.numSpriteSurfs;
	tr.viewCount++;

	R_RotateForViewer();
	R_SetupProjectionOrtho(&tr.viewParms, lb);

	// B0 (route 1): no R_AddWorldSurfaces, no R_AddPolygonSurfaces - the world casts only through W
	// (per-frame static casters, B0-M "not baked" - foliage included only with r_shadowCastFoliage for its leaf cards)
	if (!R_SunStable_StaticInWorld() && r_drawstaticmodels->integer && tr.world) {
		tr.currentEntityNum = REFENTITYNUM_WORLD;
		tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;
		R_AddStaticModelSurfaces();
	}
	R_AddEntitySurfaces();

	{
		int lastDrawSurf = r_drawSurfNoWrap ? MIN(tr.refdef.numDrawSurfs, MAX_DRAWSURFS) : tr.refdef.numDrawSurfs;
		if (firstDrawSurf > lastDrawSurf) {
			firstDrawSurf = lastDrawSurf;
		}
		R_SortDrawSurfs(tr.refdef.drawSurfs + firstDrawSurf, lastDrawSurf - firstDrawSurf,
			tr.refdef.spriteSurfs + firstSpriteSurf, tr.refdef.numSpriteSurfs - firstSpriteSurf);
	}

	Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, tr.refdef.sunShadowMvp[level]);
	tr.refdef.hzmSunDepthRange[level] = lb[1][0] - lb[0][0];
}

/*
=================
R_SunStable_Frame - called from RE_RenderScene where the legacy cascades would run. qfalse = run legacy.
=================
*/
qboolean R_SunStable_Frame(const refdef_t *fitFd)
{
	qboolean active = R_SunStable_Resolve();
	int      mode = active ? 1 : 0;
	float    lambda, n, f;
	int      i;

	if (mode != ss.lastMode) {
		if (ss.lastMode != -1 || mode == 1) {
			R_SunStable_SetFilters(active);
		}
		if (mode == 1) {
			ss.wDirty = qtrue;           // legacy cascade 3 may have written the shared image
		} else if (ss.lastMode == 1) {
			ss.legacyNeedsFar = qtrue;   // W occupied the legacy far cascade's image
		}
		if (r_shadowStable->integer || ss.lastMode == 1)   // silent at the default 0 (console quiet)
			ri.Printf(PRINT_ALL, "^~^~^ SHADOWSTABLE mode=%s why=%s\n", active ? "stable" : "legacy",
				active ? "valid" : ss.offWhy);
		ss.lastMode = mode;
		Q_strncpyz(ss.lastOffWhy, ss.offWhy, sizeof(ss.lastOffWhy));
	} else if (!active && r_shadowStable->integer && Q_stricmp(ss.offWhy, ss.lastOffWhy)) {
		ri.Printf(PRINT_ALL, "^~^~^ SHADOWSTABLE mode=legacy why=%s\n", ss.offWhy);
		Q_strncpyz(ss.lastOffWhy, ss.offWhy, sizeof(ss.lastOffWhy));
	}
	if (!active) {
		return qfalse;
	}

	// W: re-bake only on its triggers
	{
		const char *why = NULL;
		if (ss.wDirty) {
			why = ss.wBakes ? "restore" : "load";
		} else if (!VectorCompare(tr.refdef.sunDir, ss.wSunDir)) {
			why = "sundir";
		} else if (R_SunStable_WSignature() != ss.wSig) {
			why = "cvar";
		}
		if (why) {
			R_GfxProbe_ViewBegin(GFXVIEW_C3);
			R_SunWorld_Bake(why);
			R_GfxProbe_ViewEnd(GFXVIEW_C3);
			if (!ss.wValid) {
				return qfalse;   // W-DROP: legacy from this frame on (the next Resolve says why)
			}
		}
	}

	// split radii (plan: lambda 0.85, n = r_shadowCascadeZNear, f latched at load): 146 / 479 / 2048 at f 2048
	lambda = r_shadowSplitLambda->value;
	n = MAX(r_shadowCascadeZNear->value, 1.0f);
	f = MAX(ss.fDist, n * 2.0f);
	for (i = 0; i < 3; i++) {
		float x = (float)(i + 1) / 3.0f;
		float lg = n * powf(f / n, x);
		float un = n + (f - n) * x;
		ss.radius[i] = lambda * lg + (1.0f - lambda) * un;
	}
	ss.radius[2] = f;

	R_GfxProbe_ViewBegin(GFXVIEW_C0);
	R_SunStable_RenderCascade(fitFd, 0);
	R_GfxProbe_ViewEnd(GFXVIEW_C0);
	R_GfxProbe_ViewBegin(GFXVIEW_C1);
	R_SunStable_RenderCascade(fitFd, 1);
	R_GfxProbe_ViewEnd(GFXVIEW_C1);
	R_GfxProbe_ViewBegin(GFXVIEW_C2);
	R_SunStable_RenderCascade(fitFd, 2);
	R_GfxProbe_ViewEnd(GFXVIEW_C2);

	Mat4Copy(ss.wMvp, tr.refdef.sunShadowMvp[3]);
	tr.refdef.hzmSunActive = 1;
	VectorSet4(tr.refdef.hzmSunSplits, ss.radius[0], ss.radius[1], ss.radius[2], r_shadowBlendBand->value);
	for (i = 0; i < 3; i++) {
		static const float kernel[3] = { 7.0f, 7.0f, 5.0f };   // plan SE#22: 7 / 7 / 5 (W: 3, in lightall)
		float size = (float)tr.sunShadowFbo[i]->width;
		tr.refdef.hzmSunKernel[i] = 0.25f * kernel[i] * (r_shadowSoftness->value / 1.5f) / size;
		tr.refdef.hzmSunBias[i] = (tr.refdef.hzmSunDepthRange[i] > 0.0f) ? r_shadowConstBias->value / tr.refdef.hzmSunDepthRange[i] : 0.0f;
	}
	tr.refdef.hzmSunKernel[3] = (r_shadowDebug && r_shadowDebug->integer == 5) ? 5.0f : 0.0f;
	tr.refdef.hzmSunBias[3] = (ss.wDepthRange > 0.0f) ? r_shadowWBias->value / ss.wDepthRange : 0.0f;
	// lightall (D1): on, normal offset (world units, ~1 W texel), W bias (depth units, + = occluded), half a W texel (uv)
	VectorSet4(tr.refdef.hzmSunMaskOnly, 1.0f, r_shadowNormalOffset->value * ss.wTexel, tr.refdef.hzmSunBias[3],
		ss.wSize > 0 ? 0.5f / (float)ss.wSize : 0.0f);
	return qtrue;
}

/*
=================
backend: the stable mask pass (V_entity only; r_shadowDebug 5 = V_world, for the freeze captures - vet R8)
=================
*/
void RB_SunStable_Mask(vec4_t quadVerts[4], vec2_t texCoords[4], const vec4_t viewInfo)
{
	shaderProgram_t *sp = &tr.shadowmaskStableShader;
	vec3_t viewVector;
	float zmax = backEnd.viewParms.zFar;
	float ymax = zmax * tan(backEnd.viewParms.fovY * M_PI / 360.0f);
	float xmax = zmax * tan(backEnd.viewParms.fovX * M_PI / 360.0f);

	GLSL_BindProgram(sp);
	GL_BindToTMU(tr.renderDepthImage, TB_COLORMAP);
	GL_BindToTMU(tr.sunShadowDepthImage[0], TB_SHADOWMAP);
	GL_BindToTMU(tr.sunShadowDepthImage[1], TB_SHADOWMAP2);
	GL_BindToTMU(tr.sunShadowDepthImage[2], TB_SHADOWMAP3);
	GL_BindToTMU(tr.sunShadowDepthImage[3], TB_SHADOWMAP4);

	GLSL_SetUniformMat4(sp, UNIFORM_SHADOWMVP, backEnd.refdef.sunShadowMvp[0]);
	GLSL_SetUniformMat4(sp, UNIFORM_SHADOWMVP2, backEnd.refdef.sunShadowMvp[1]);
	GLSL_SetUniformMat4(sp, UNIFORM_SHADOWMVP3, backEnd.refdef.sunShadowMvp[2]);
	GLSL_SetUniformMat4(sp, UNIFORM_SHADOWMVP4, backEnd.refdef.sunShadowMvp[3]);
	GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.refdef.vieworg);
	VectorScale(backEnd.refdef.viewaxis[0], zmax, viewVector);
	GLSL_SetUniformVec3(sp, UNIFORM_VIEWFORWARD, viewVector);
	VectorScale(backEnd.refdef.viewaxis[1], xmax, viewVector);
	GLSL_SetUniformVec3(sp, UNIFORM_VIEWLEFT, viewVector);
	VectorScale(backEnd.refdef.viewaxis[2], ymax, viewVector);
	GLSL_SetUniformVec3(sp, UNIFORM_VIEWUP, viewVector);
	GLSL_SetUniformVec4(sp, UNIFORM_VIEWINFO, viewInfo);
	GLSL_SetUniformVec4(sp, UNIFORM_HZMSHADOWSPLITS, backEnd.refdef.hzmSunSplits);
	GLSL_SetUniformVec4(sp, UNIFORM_HZMSHADOWKERNEL, backEnd.refdef.hzmSunKernel);
	GLSL_SetUniformVec4(sp, UNIFORM_HZMSHADOWBIAS, backEnd.refdef.hzmSunBias);

	RB_InstantQuad2(quadVerts, texCoords);
	// r_shadowBlur is ignored while stable (plan P3 backend): the kernel is the softness
}

/*
=================
backend: lightall's B0 inputs for one draw (tr_shade.c, only where the program has USE_SHADOWMAP)
  x 0 = today, 1 = B0, 2 = B0 but not the main view (portal sky / mirror / cube bake: no sun shadow, L4),
  3 = B0 but the mask does not describe this surface (not in the z-prepass: alpha-tested, blended, depth-hack)
=================
*/
void RB_SunStable_Lightall(shaderProgram_t *sp, const shaderCommands_t *input)
{
	vec4_t v;

	if (!backEnd.refdef.hzmSunActive) {
		VectorSet4(v, 0.0f, 0.0f, 0.0f, 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_HZMSUNMASKONLY, v);
		return;
	}
	Vector4Copy(backEnd.refdef.hzmSunMaskOnly, v);
	if (backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky || backEnd.viewParms.isMirror
	    || backEnd.viewParms.targetFbo != NULL || !(backEnd.viewParms.flags & VPF_USESUNLIGHT)) {
		v[0] = 2.0f;
	} else if (input->shader->sort != SS_OPAQUE || input->shader->hasAlphaTest
	           || (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity
	               && (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK))) {
		v[0] = 3.0f;
	}
	GL_BindToTMU(tr.sunShadowDepthImage[3], TB_SUNWORLD);
	GLSL_SetUniformMat4(sp, UNIFORM_HZMSUNWORLDMVP, backEnd.refdef.sunShadowMvp[3]);
	GLSL_SetUniformVec4(sp, UNIFORM_HZMSUNMASKONLY, v);
}

// tr_gfxprobe.c: which layer the far image holds right now
qboolean R_SunStable_FarIsW(void)
{
	return (qboolean)(ss.lastMode == 1);
}
