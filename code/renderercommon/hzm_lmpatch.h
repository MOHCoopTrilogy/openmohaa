/*
 * hzm_lmpatch.h - HZM: apply a per-map baked-lightmap patch (maps/<map>.hzmlm) before the lightmaps are uploaded.
 * Header-only, shared by renderergl1 and renderergl2 (included from tr_bsp.c; gl2 also from tr_staticmodels.cpp for
 * the struct only). STAGED (docs/proposals/foliage_shadows_2026-09-28) - the main tree is frozen for v1.10.3.
 *
 * Why: the user's "square shadows" on m3l3. The retail compiler baked NO shadow for static-model foliage (bush_full,
 * tree_oak, the pines ...) and baked the hedgerow PATCHES solid (the transparent part of the card shadows the ground);
 * gl2's per-frame sun cascades skip the alpha-tested leaves (r_shadowCastFoliage 0), so what the ground got was the
 * hard, straight-edged shadow of each tree's low-poly trunk and branches with no canopy around it.
 * docs/proposals/foliage_shadows_2026-09-28/tools/gen_foliage_lm.py re-bakes, offline, from the real alpha-tested
 * cards (canopy + trunk) with the map's own sun and shade level, and opens up the solid-baked hedgerow patches. The
 * patch also lists the static models it baked; gl2 keeps exactly those out of the per-frame shadow maps
 * (tr_staticmodels.cpp) and the scripted foliage entities it baked (tr_main.c), so nothing is cast twice and the trunk
 * bars cannot come back.
 *
 * File format (little endian):
 *   "HZLM"  u32 version (1|2)  u32 fnv1a32(original LUMP_LIGHTMAPS)  u32 lump length  u32 count
 *   count x { u16 page, u8 s, u8 t, u8 r, u8 g, u8 b }          (7 bytes; page = 128x128 lightmap index)
 *   v2+: u32 n, n x u16 static-model lump index (the models this patch baked)
 *   v3:  u32 m, m x float[3] origin of a scripted (build-mode) foliage script_model it baked (spawned at map load)
 * Applied ONLY when the lump length and FNV-1a hash match exactly, so a patch can never land on a different compile
 * of the map. Every failure is silent and leaves the retail lightmap (and the retail casters) untouched.
 *
 * r_hzmFoliageShadows (CVAR_ARCHIVE | CVAR_LATCH, default 1): 0 = retail bake and retail casters. Read at map load.
 * Omaha (m3l1a/m3l1b) is refused here as well as in the generator.
 */
#ifndef HZM_LMPATCH_H
#define HZM_LMPATCH_H

#define HZM_LMPATCH_PAGE 128

static unsigned int HZM_LmPatch_Fnv1a( const byte *p, int len )
{
	unsigned int h = 2166136261u;
	int i;

	for ( i = 0; i < len; i++ ) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static unsigned int HZM_LmPatch_U32( const byte *p )
{
	return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ) | ( (unsigned int)p[2] << 16 ) | ( (unsigned int)p[3] << 24 );
}

/* buf/len = the raw LUMP_LIGHTMAPS bytes (128*128*3 per page), writable, before any upload or colour shift.
 * bakedSM / maxBaked / numBaked, bakedEnt / maxEnt / numEnt (gl2; gl1 passes NULLs): receive the static-model lump
 * indices and the scripted-entity origins the patch baked. Returns the number of texels written (0 = retail bake kept,
 * and both counts 0). */
static int HZM_ApplyLightmapPatch( const char *baseName, byte *buf, int len, int *bakedSM, int maxBaked, int *numBaked,
                                   float ( *bakedEnt )[3], int maxEnt, int *numEnt )
{
	static cvar_t *s_enable = NULL;
	char          path[MAX_QPATH];
	byte         *file = NULL;
	int           flen, count, i, applied = 0, pages, version, tail;

	if ( numBaked ) {
		*numBaked = 0;
	}
	if ( numEnt ) {
		*numEnt = 0;
	}
	if ( !s_enable ) {
		s_enable = ri.Cvar_Get( "r_hzmFoliageShadows", "1", CVAR_ARCHIVE | CVAR_LATCH );
	}
	if ( !s_enable->integer || !baseName || !baseName[0] || !buf || len <= 0 ) {
		return 0;
	}
	if ( !Q_stricmp( baseName, "m3l1a" ) || !Q_stricmp( baseName, "m3l1b" ) ) {
		return 0;   /* Omaha: never touched */
	}

	Com_sprintf( path, sizeof( path ), "maps/%s.hzmlm", baseName );
	flen = ri.FS_ReadFile( path, (void **)&file );
	if ( flen <= 0 || !file ) {
		return 0;
	}
	version = flen >= 20 ? (int)HZM_LmPatch_U32( file + 4 ) : 0;
	if ( flen < 20 || memcmp( file, "HZLM", 4 ) || ( version < 1 || version > 3 )
	     || HZM_LmPatch_U32( file + 12 ) != (unsigned int)len
	     || HZM_LmPatch_U32( file + 8 ) != HZM_LmPatch_Fnv1a( buf, len ) ) {
		ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMLM %s: patch does not match this compile - ignored\n", path );
		ri.FS_FreeFile( file );
		return 0;
	}
	count = (int)HZM_LmPatch_U32( file + 16 );
	if ( count < 0 || count > ( flen - 20 ) / 7 ) {
		ri.FS_FreeFile( file );
		return 0;
	}
	pages = len / ( HZM_LMPATCH_PAGE * HZM_LMPATCH_PAGE * 3 );
	for ( i = 0; i < count; i++ ) {
		const byte *r = file + 20 + i * 7;
		int         page = r[0] | ( r[1] << 8 );
		int         s = r[2], t = r[3];
		byte       *px;

		if ( page >= pages || s >= HZM_LMPATCH_PAGE || t >= HZM_LMPATCH_PAGE ) {
			continue;
		}
		px = buf + ( ( page * HZM_LMPATCH_PAGE + t ) * HZM_LMPATCH_PAGE + s ) * 3;
		px[0] = r[4];
		px[1] = r[5];
		px[2] = r[6];
		applied++;
	}
	tail = 20 + count * 7;
	if ( version >= 2 && applied > 0 && tail + 4 <= flen ) {
		int n = (int)HZM_LmPatch_U32( file + tail );

		if ( n < 0 || n > ( flen - tail - 4 ) / 2 ) {
			n = 0;
		}
		for ( i = 0; bakedSM && numBaked && i < n && *numBaked < maxBaked; i++ ) {
			bakedSM[( *numBaked )++] = file[tail + 4 + i * 2] | ( file[tail + 5 + i * 2] << 8 );
		}
		tail += 4 + n * 2;
		if ( version >= 3 && tail + 4 <= flen ) {
			int m = (int)HZM_LmPatch_U32( file + tail );

			if ( m > 0 && m <= ( flen - tail - 4 ) / 12 ) {
				for ( i = 0; bakedEnt && numEnt && i < m && *numEnt < maxEnt; i++ ) {
					int k;

					for ( k = 0; k < 3; k++ ) {
						unsigned int u = HZM_LmPatch_U32( file + tail + 4 + i * 12 + k * 4 );
						float        f;

						memcpy( &f, &u, 4 );
						bakedEnt[*numEnt][k] = f;
					}
					( *numEnt )++;
				}
			}
		}
	}
	ri.Printf( PRINT_DEVELOPER, "^~^~^ HZMLM %s: %d texels reshaped, %d static models + %d scripted foliage baked\n", path,
	           applied, numBaked ? *numBaked : 0, numEnt ? *numEnt : 0 );
	ri.FS_FreeFile( file );
	return applied;
}

#endif
