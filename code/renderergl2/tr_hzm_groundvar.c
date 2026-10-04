/*
===========================================================================
HZM coop - GROUND VARIETY for world ground surfaces (r_groundVariety). docs/proposals/ground_variety_2026-09-29.

WHY. Ground textures repeat on a visible grid even after the HD upgrades: MOHAA terrain maps exactly ONE texture repeat
onto every 512 u terrain patch (m3l2's field is 140 identical copies of m3l3grass_1rough side by side), brush ground
repeats every 128-512 u, and many HD upscales are internally two copies of their own art, halving that again.

WHAT (lightall_fp.glsl HzmGvDiffuse / HzmGvTap). Two layers, both in the WORLD xy plane:
  1 MACRO  a low-frequency brightness field (two octaves of value noise, ~1400 u and ~450 u) times the albedo, plus a
           very small warm/cool shift. Continuous across every surface and every class, so it never draws a step
           where two tagged ground shaders meet.
  2 HEX    Mikkelsen's hex tiling (JCGT 2022, "Practical Real-Time Hex-Tiling"): a triangle lattice over the ground,
           each lattice vertex a random whole-texture OFFSET (no rotation - roads, furrows and normal maps keep their
           direction), three explicit-gradient fetches blended by sharpened barycentric weights modulated by texel
           luminance, so most of the ground shows one sharp copy and the thin blend zones follow the texture's own
           light/dark structure instead of a smooth cross-fade. The lattice lives in world units, NOT uv: terrain
           rebases its uv by whole repeats on every patch (tr_terrain.c), so a uv lattice would hash differently on
           both sides of every patch edge and draw the very grid this removes. The same weights and offsets drive the
           normal map (and the specular map), so relief stays glued to the albedo.
           Only for art the generator judged stationary, non-directional and non-periodic (class 2).

WHERE. Only on shaders listed for the LOADED BSP in scripts/hzm_groundvariety.txt, which is GENERATED from the BSPs and
the player pak stack by docs/proposals/ground_variety_2026-09-29/tools/gv_census.py (ground-facing area, material
bits, image metrics; opt-outs in groundvariety_optout.txt). Per draw (RB_HZM_GroundVarUniforms): the WORLD entity,
the FIRST stage, opaque, a lit lightall permutation, not a shadow/depth/cube view - everything else uploads ALL ZERO.
The dynamic-light pass gets the same values as the lit pass, so a dlight lights the same varied texture.
Per pixel the shader fades both layers out on steep faces (geometric normal z 0.45..0.70): a wall shows its own plain
texture (through textureGrad on a hex-tagged shader - same texels and mip, not guaranteed bit-identical to texture2D).

OMAHA (user hard rule 2026-09-29: "it was hard enough to get to where we are"). m3l1a, m3l1b, e3l1, e3l2 and
obj_team3 (and suffixed copies, m3l1a_sml) are refused by BSP name before the list is even read, and any shader whose
name carries an Omaha asset token (omaha ocean seabed wetsand shoreline surf wake) is never tagged on any map. The
generator also never writes those maps or names, and drops every image the m3l1a HD pak supplies.

SWITCH. r_groundVariety (flags 0, "-1" = AUTO = HZM_GROUNDVAR_AUTO): 0 off, 1 macro only, 2 macro + hex where the
list allows it. Live - read every draw, no restart. OFF is byte-identical: every lightall draw uploads zero and the
shader takes its original texture2D path. Tuning (not saved, live): r_groundVarietyMacro, r_groundVarietyCell,
r_groundVarietySharp, r_groundVarietyLuma, r_groundVarietyDebug (1 = class tint, 2 = hex weights).
gl1 is untouched (no shader stage path is changed).
===========================================================================
*/

#include "tr_local.h"

#define HZM_GROUNDVAR_AUTO   0      // r_groundVariety -1 means this. OFF until the user approves the look.
#define HZM_GV_MAX           512    // list lines for ONE map (the census peaks at ~40)
#define HZM_GV_LIST          "scripts/hzm_groundvariety.txt"

typedef struct {
	char	name[MAX_QPATH];
	int		cls;           // 1 macro, 2 macro + hex
	float	period;        // world units per texture repeat (census median)
} hzmGvEntry_t;

static cvar_t *r_groundVariety;
static cvar_t *r_groundVarietyMacro;
static cvar_t *r_groundVarietyCell;
static cvar_t *r_groundVarietySharp;
static cvar_t *r_groundVarietyLuma;
static cvar_t *r_groundVarietyDebug;

static hzmGvEntry_t	s_gv[HZM_GV_MAX];
static int			s_gvCount;
static int			s_gvWorld;        // bumped per world load; a shader tag counts only for the world it was made in
static qboolean		s_gvOmaha;

static void R_HZM_GroundVarCvars( void )
{
	if ( r_groundVariety ) {
		return;
	}
	// registered here, once, by the only module that reads them. A renderer DLL reload zeroes these statics and the
	// next world load registers them again (the engine keeps the values).
	r_groundVariety      = ri.Cvar_Get( "r_groundVariety",      "-1",   0 );
	r_groundVarietyMacro = ri.Cvar_Get( "r_groundVarietyMacro", "0.10", 0 );
	r_groundVarietyCell  = ri.Cvar_Get( "r_groundVarietyCell",  "0.50", 0 );
	r_groundVarietySharp = ri.Cvar_Get( "r_groundVarietySharp", "10",   0 );
	r_groundVarietyLuma  = ri.Cvar_Get( "r_groundVarietyLuma",  "0.60", 0 );
	r_groundVarietyDebug = ri.Cvar_Get( "r_groundVarietyDebug", "0",    0 );
}

// the Omaha maps, as the BSP base name or a suffixed copy of it (m3l1a_sml) - hzm_waterwet.h's rule, kept separate
// on purpose: this feature's exclusion must not change when another feature's list does
static qboolean R_HZM_GroundVarOmahaMap( const char *base )
{
	static const char *const omaha[] = { "m3l1a", "m3l1b", "e3l1", "e3l2", "obj_team3" };
	size_t	len, n;
	int		i;

	if ( !base || !base[0] ) {
		return qtrue;          // no name = no decision = off
	}
	len = strlen( base );
	for ( i = 0; i < (int)( sizeof( omaha ) / sizeof( omaha[0] ) ); i++ ) {
		n = strlen( omaha[i] );
		if ( len >= n && !Q_stricmpn( base, omaha[i], (int)n ) && ( len == n || base[n] == '_' || base[n] == '.' ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static qboolean R_HZM_GroundVarOmahaName( const char *name )
{
	static const char *const tok[] = { "omaha", "ocean", "seabed", "wetsand", "shoreline", "surf", "wake" };
	int i;

	for ( i = 0; i < (int)( sizeof( tok ) / sizeof( tok[0] ) ); i++ ) {
		if ( Q_stristr( name, tok[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
R_HZM_GroundVarWorldBegin - RE_LoadWorldMap, once the BSP base name is known and BEFORE any shader is looked up.
Reads the block of scripts/hzm_groundvariety.txt for this map only. Omaha never reads it.
================
*/
void R_HZM_GroundVarWorldBegin( const char *baseName )
{
	union { char *c; void *v; } buf;
	char		*p, *tok;
	qboolean	inMap = qfalse;
	int			len;

	R_HZM_GroundVarCvars();

	s_gvWorld++;
	s_gvCount = 0;
	s_gvOmaha = R_HZM_GroundVarOmahaMap( baseName );
	if ( s_gvOmaha ) {
		ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMGV off: %s is on the Omaha list\n", baseName ? baseName : "?" );
		return;
	}

	buf.v = NULL;
	len = ri.FS_ReadFile( HZM_GV_LIST, &buf.v );
	if ( len <= 0 || !buf.c ) {
		if ( buf.v ) {
			ri.FS_FreeFile( buf.v );    // an EMPTY file still comes back as an allocated buffer
		}
		ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMGV no %s\n", HZM_GV_LIST );
		return;
	}

	// line format (generated): "map <base>" starts a block, then "<shader> <class> <period>"; "//" comments
	p = buf.c;
	while ( 1 ) {
		tok = COM_ParseExt( &p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		if ( !Q_stricmp( tok, "map" ) ) {
			tok = COM_ParseExt( &p, qfalse );
			if ( inMap ) {
				break;              // our block is over
			}
			inMap = ( tok[0] && !Q_stricmp( tok, baseName ) ) ? qtrue : qfalse;
			continue;
		}
		if ( !inMap ) {
			if ( p ) {        // COM_ParseExt NULLs p at end of file (a truncated last line)
				SkipRestOfLine( &p );
			}
			continue;
		}
		if ( s_gvCount < HZM_GV_MAX ) {
			hzmGvEntry_t *e = &s_gv[s_gvCount];

			Q_strncpyz( e->name, tok, sizeof( e->name ) );
			COM_StripExtension( e->name, e->name, sizeof( e->name ) );
			tok = COM_ParseExt( &p, qfalse );
			e->cls = atoi( tok );
			tok = COM_ParseExt( &p, qfalse );
			e->period = (float)atof( tok );
			if ( p ) {        // COM_ParseExt NULLs p at end of file (a truncated last line)
				SkipRestOfLine( &p );
			}
			if ( e->cls >= 1 && e->cls <= 2 && e->period >= 8.0f && !R_HZM_GroundVarOmahaName( e->name ) ) {
				s_gvCount++;
			}
		} else {
			if ( p ) {        // COM_ParseExt NULLs p at end of file (a truncated last line)
				SkipRestOfLine( &p );
			}
		}
	}
	ri.FS_FreeFile( buf.v );
	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMGV %s: %d ground shaders listed\n", baseName, s_gvCount );
}

/*
================
R_HZM_GroundVarTagShader - ShaderForShaderNum (tr_bsp.c), for every BSP world surface and terrain patch.
================
*/
void R_HZM_GroundVarTagShader( shader_t *sh )
{
	char	base[MAX_QPATH];
	int		i;

	if ( !sh || sh == tr.defaultShader || s_gvOmaha || !s_gvCount ) {
		return;
	}
	COM_StripExtension( sh->name, base, sizeof( base ) );
	for ( i = 0; i < s_gvCount; i++ ) {
		if ( !Q_stricmp( base, s_gv[i].name ) ) {
			if ( R_HZM_GroundVarOmahaName( base ) ) {
				return;
			}
			sh->hzmGvClass  = s_gv[i].cls;
			sh->hzmGvPeriod = s_gv[i].period;
			sh->hzmGvWorld  = s_gvWorld;
			return;
		}
	}
}

// -1 (any negative) = auto
static int R_HZM_GroundVarMode( void )
{
	int v;

	if ( !r_groundVariety || s_gvOmaha || !s_gvCount ) {
		return 0;
	}
	v = r_groundVariety->integer;
	if ( v < 0 ) {
		v = HZM_GROUNDVAR_AUTO;
	}
	return v > 2 ? 2 : v;
}

/*
================
RB_HZM_GroundVarUniforms - every lightall draw in RB_IterateStagesGeneric and every ForwardDlight pass (never the
depth-fill branch). Uploads the ground-variety values for a qualifying draw and ZERO for every other one: uniforms
are per-program state shared by every draw of that permutation.
Deliberately INCLUDED: portal and mirror views (a reflected field must match the field), vertex-lit and light-vector
permutations (the ForwardDlight pass is LIGHT_VECTOR and must light the same varied texture as the lit pass).
Deliberately EXCLUDED: entities, characters, 2D, the depth prepass, shadow and cube-bake views, blended and
alpha-tested stages, stages after the first, water/slime, r_lightmap. Sky surfaces are never listed (the generator
drops SURF_SKY / SURF_NODRAW faces).
================
*/
void RB_HZM_GroundVarUniforms( shaderProgram_t *sp, const shaderCommands_t *input, const shaderStage_t *pStage,
                               int stage, qboolean charLit )
{
	vec4_t		gv  = { 0.0f, 0.0f, 0.0f, 0.0f };
	vec4_t		gv2 = { 0.0f, 0.0f, 0.0f, 0.0f };
	const shader_t *sh = input->shader;
	int			mode = R_HZM_GroundVarMode();

	if ( mode > 0
	     && sh && sh->hzmGvClass > 0 && sh->hzmGvWorld == s_gvWorld
	     && stage == 0
	     && !charLit
	     && backEnd.currentEntity == &tr.worldEntity
	     && !backEnd.depthFill
	     && !backEnd.projection2D
	     && !( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) )
	     && !( tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo )
	     && !( pStage->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
	     && !( pStage->stateBits & GLS_ATEST_BITS )     // cutouts: the depth prepass / shadow maps read the plain alpha
	     && !r_lightmap->integer                          // lightmap-only debug view: the diffuse is not drawn
	     && sh->sort <= SS_OPAQUE
	     && !( sh->contentFlags & ( CONTENTS_WATER | CONTENTS_SLIME ) )
	     && ( pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK ) )
	{
		float cell = sh->hzmGvPeriod * Com_Clamp( 0.15f, 4.0f, r_groundVarietyCell->value );
		int   eff  = sh->hzmGvClass < mode ? sh->hzmGvClass : mode;

		// parallax offset mapping marches the UNTILED height map; a hex-tiled albedo on top of it would slide against
		// its own relief, so a parallax permutation gets the macro layer only
		if ( ( pStage->glslShaderIndex & LIGHTDEF_USE_PARALLAXMAP ) && r_parallaxMapping->integer && eff > 1 ) {
			eff = 1;
		}
		cell = Com_Clamp( 24.0f, 2048.0f, cell );
		gv[0] = (float)eff;
		gv[1] = Com_Clamp( 0.0f, 0.5f, r_groundVarietyMacro->value );
		gv[2] = 1.0f / cell;
		gv[3] = Com_Clamp( 1.0f, 32.0f, r_groundVarietySharp->value );
		gv2[0] = Com_Clamp( 0.0f, 1.0f, r_groundVarietyLuma->value );
		gv2[1] = 0.45f;       // steep-face gate: nothing below this geometric normal z ...
		gv2[2] = 0.70f;       // ... everything above it
		gv2[3] = (float)r_groundVarietyDebug->integer;
	}
	GLSL_SetUniformVec4( sp, UNIFORM_HZMGROUNDVAR, gv );
	GLSL_SetUniformVec4( sp, UNIFORM_HZMGROUNDVAR2, gv2 );
}
