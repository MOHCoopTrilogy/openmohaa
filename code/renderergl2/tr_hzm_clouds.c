/*
===========================================================================
HZM gl2 volumetric clouds - the africanight profile (vcl tree only until the flip).

docs/proposals/volumetric_clouds_2026-09-26/plan.md is the reference; section numbers below point into it.

  2.4 step 1  trace     vclouds_trace_fp   half-res raymarch of a spherical slab around the dome, coverage = the private
                                           HD talonclouds copy, erosion = a 64^3 Worley fBm generated here at R_Init
  2.4 step 2  tent      vclouds_tent_fp    3x3 [1 2 1]^2 at trace resolution (no history anywhere)
  2.4 step 3  composite vclouds_composite  replaces today's talonclouds stage draw in the sky pass (tr_shade.c), same
                                           vertices / texcoords / texmods; T = 1, L = 0 is exactly today's stage
  2.4 step 4  dither    tonemap_*_fp       animated cloud-masked TPDF before the RGBA8 write (tr_postprocess.c)

Contracts (plan 2.1, the keep-renderer port): all state in tr.hzmClouds (R_Init's memset zeroes it), no function
statics, permanent GPU objects created only in R_Init (R_HZM_Clouds_InitGL) and destroyed in RE_Shutdown, the per-map
profile pointer cleared at every world load. Failure policy (2.4): any missing capability or failed try-compile turns
the feature off for the session with the reason printed once - never ri.Error.
===========================================================================
*/
#include "tr_local.h"
#include "tr_dsa.h"
#include "tr_hzm_clouds_profile.h"
#include "tr_hzm_clouds_pure.h"
#include "tr_gfxbuild.h"
#include "../renderercommon/hzm_storm.h"	// v1.1: the storm raises the procedural cover (R_HZM_Storm)
#include "../renderercommon/hzm_waterwet.h"	// HZM_WaterWetOmahaMap: the shared five-map Omaha list

void GLSL_DeleteGPUShader( shaderProgram_t *program );	// tr_glsl.c

#ifndef GL_R8
#define GL_R8                               0x8229
#endif
#ifndef GL_RED
#define GL_RED                              0x1903
#endif
#ifndef GL_RG
#define GL_RG                               0x8227
#endif
#ifndef GL_RG32F
#define GL_RG32F                            0x8230
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL                0x813D
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL               0x813C
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                    0x812F
#endif

#ifndef HZM_GFX_COMMIT
#define HZM_GFX_COMMIT "unknown"
#endif
#ifndef HZM_GFX_DATE
#define HZM_GFX_DATE "unknown"
#endif

// Read by build.ps1 / publish_release.ps1 (plan 2.1 guards): a vcl-tree build must never ship under a fixed name.
// The P4 flip commit removes this marker.
// [2026-10-12] THE FLIP. The user judged the clouds from the in-engine captures ("Only for skies where they look
// good. Keep on by default", "I think african night is fine to keep") and asked for the build on their own install to
// test. The marker no longer says "test", so build.ps1 / publish_release.ps1 accept this renderer under its fixed name;
// the string stays because the VCLBUILD log line prints it.
const char hzmVclStateMarker[] = "HZM_VCL=on";

hzmTexImage3D_t qglTexImage3D;
hzmPixelStorei_t qglHzmPixelStorei;	// glPixelStorei is not in the qgl table either (tr_extensions.c loads both)

// declared locally by their other users too (tr_msaa.c)
FBO_t    *FBO_Create( const char *name, int width, int height );
qboolean R_CheckFBO( const FBO_t *fbo );
void     R_LoadImage( const char *name, byte **pic, int *width, int *height, GLenum *picFormat, int *numMips );

cvar_t *r_hzmClouds;
cvar_t *r_hzmCloudsAll;		// look-dev only: 1 = also the reviewed-and-refused sky families (read when a map loads)
cvar_t *r_hzmCloudsDebug;
cvar_t *r_hzmCloudsDither;
cvar_t *r_hzmCloudsProbe;
cvar_t *r_hzmCloudsLabel;
cvar_t *r_hzmCloudsRes;
cvar_t *r_hzmCloudsForceFail;
cvar_t *r_hzmCloudsTune;

// v1.1 GPU timer (r_hzmCloudsProbe 1 only): GL_TIMESTAMP, loaded in tr_extensions.c (NULL = no CLOUDSGPU line)
hzmQueryCounter_t qglHzmQueryCounter;
hzmGetQueryObjectui64v_t qglHzmGetQueryObjectui64v;

// v1.1 the procedural profile: its trace is the africanight source with VCL_PROC (constants -> uniforms) and these
#define VCL_PROC_COVN 512
#define HZM_VCL_PROC_DEFINES \
	"#define VCL_PROC 1\n" \
	"#define VCL_HB 470.0\n" \
	"#define VCL_HT 590.0\n" \
	"#define VCL_ALB 0.98\n" \
	"#define VCL_NZ 0.34\n" \
	"#define VCL_STEPS 16.0\n" \
	"#define VCL_STEPS_I 16\n" \
	"#define VCL_G0 0.35\n" \
	"#define VCL_G1 -0.15\n" \
	"#define VCL_GW 0.3\n" \
	"#define VCL_MSA 0.4\n" \
	"#define VCL_MSB 0.5\n" \
	"#define VCL_MSC 0.5\n" \
	"#define VCL_FADE0 1.0\n" \
	"#define VCL_FADE1 10.0\n" \
	"#define VCL_TMIN 0.01\n" \
	"#define VCL_BASE0 0.06\n" \
	"#define VCL_TOPPOW 1.0\n" \
	"#define VCL_EDGE 0.18\n" \
	"#define VCL_SHAPELOD 3.0\n" \
	"#define VCL_ELEVREF_SIN 0.5\n" \
	"#define VCL_CAPLIT 8.0\n" \
	"#define VCL_COVN 512.0\n" \
	"#define VCL_NOISEN 64.0\n" \
	"#define VCL_COVMAXLEVELS 10.0\n"

#define VCL_COVER_PATH	"env/hzmhd/clouds/sky/talonclouds.tga"
#define VCL_PROFILE_SKY	"textures/sky/africanight"
#define VCL_NOISE_N		HZM_VCL_NOISEN
#define VCL_SKY_SUBD	8

/*
==================
switches (plan 2.2: flags 0, never archived)
==================
*/
void R_HZM_Clouds_Register( void ) {
	r_hzmClouds          = ri.Cvar_Get( "r_hzmClouds",          "-1",  0 );
	r_hzmCloudsAll       = ri.Cvar_Get( "r_hzmCloudsAll",       "0",   0 );
	r_hzmCloudsDebug     = ri.Cvar_Get( "r_hzmCloudsDebug",     "0",   0 );
	r_hzmCloudsDither    = ri.Cvar_Get( "r_hzmCloudsDither",    "0.5", 0 );
	r_hzmCloudsProbe     = ri.Cvar_Get( "r_hzmCloudsProbe",     "0",   0 );
	r_hzmCloudsLabel     = ri.Cvar_Get( "r_hzmCloudsLabel",     "0",   0 );
	r_hzmCloudsRes       = ri.Cvar_Get( "r_hzmCloudsRes",       "0",   0 );
	r_hzmCloudsForceFail = ri.Cvar_Get( "r_hzmCloudsForceFail", "0",   0 );
	// v1.1: look-dev overrides of the procedural profile ("cover=0.5 sigma=0.05 sun=1.7 amb=1 tile=2400 wind=3
	// eros=0.75 lmax=1.6"), and the sky fog model (tr_shade.c). All flags 0, never archived (TRAPS T7).
	r_hzmCloudsTune      = ri.Cvar_Get( "r_hzmCloudsTune",      "",    0 );
}

// -1 or "" = HZM_CLOUDS_AUTO; 0 off; anything else on. Read per frame (live).
qboolean R_HZM_CloudsEnabled( void ) {
	if ( !r_hzmClouds || !r_hzmClouds->string[0] || r_hzmClouds->integer == -1 ) {
		return (qboolean)( HZM_CLOUDS_AUTO != 0 );
	}
	return (qboolean)( r_hzmClouds->integer != 0 );
}

static void R_HZM_CloudsSetReason( const char *why ) {
	hzmClouds_t *c = &tr.hzmClouds;

	if ( !c->reason[0] ) {
		Q_strncpyz( c->reason, why, sizeof( c->reason ) );
	}
	c->supported = qfalse;
}

// level-0 noise bytes (tr_hzm_clouds_pure.h R_HZM_VclNoiseBytes: the twin's make_noise, V2-tested)
static qboolean R_HZM_VclMakeNoise( byte *out ) {
	const int n = VCL_NOISE_N, n3 = n * n * n;
	double *w = (double *)ri.Malloc( sizeof( double ) * n3 * 3 );
	double *fp = (double *)ri.Malloc( sizeof( double ) * 16 * 16 * 16 * 3 );

	if ( !w || !fp ) {
		if ( w ) ri.Free( w );
		if ( fp ) ri.Free( fp );
		return qfalse;
	}
	R_HZM_VclNoiseBytes( n, out, w, fp );
	ri.Free( fp );
	ri.Free( w );
	return qtrue;
}

/*
==================
GL helpers: a raw bind on the ACTIVE unit, restored after, so tr_dsa.c's per-unit cache stays valid (the
R_MsaaSpecifyStorage recipe)
==================
*/
static GLuint R_HZM_VclGenTexture( GLenum target, GLenum bindingQuery, GLint *prev ) {
	GLuint t = 0;

	qglGenTextures( 1, &t );
	qglGetIntegerv( bindingQuery, prev );
	qglBindTexture( target, t );
	return t;
}

static void R_HZM_VclUploadNoise( const byte *lvl0 ) {
	hzmClouds_t *c = &tr.hzmClouds;
	const int n0 = VCL_NOISE_N;
	float *cur = (float *)ri.Malloc( sizeof( float ) * n0 * n0 * n0 );
	float *nxt = (float *)ri.Malloc( sizeof( float ) * ( n0 / 2 ) * ( n0 / 2 ) * ( n0 / 2 ) + 16 );
	byte *up = (byte *)ri.Malloc( n0 * n0 * n0 );
	GLint prev = 0;
	int n = n0, level = 0, i;

	for ( i = 0; i < n0 * n0 * n0; i++ ) {
		cur[i] = (float)( lvl0[i] / 255.0 );
	}
	c->noiseTex = R_HZM_VclGenTexture( GL_TEXTURE_3D, GL_TEXTURE_BINDING_3D, &prev );
	qglHzmPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	for ( ;; ) {
		R_HZM_VclNoiseQuantGL( cur, n, up );
		qglTexImage3D( GL_TEXTURE_3D, level, GL_R8, n, n, n, 0, GL_RED, GL_UNSIGNED_BYTE, up );
		c->vramBytes += n * n * n;
		if ( n == 1 ) {
			break;
		}
		R_HZM_VclNoiseDown( cur, n, nxt );
		memcpy( cur, nxt, sizeof( float ) * ( n / 2 ) * ( n / 2 ) * ( n / 2 ) );
		n /= 2;
		level++;
	}
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0 );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, level );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	qglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT );
	qglBindTexture( GL_TEXTURE_3D, (GLuint)prev );
	qglHzmPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	ri.Free( up );
	ri.Free( nxt );
	ri.Free( cur );
}

/*
==================
coverage: the private copy of the HD layer, by NAME (independent of r_skyHD / r_picmip / r_textureMode; vet E8).
Grey = PIL's "L" (ITU-R 601-2 luma, integer rounding) exactly as vc_assets.load_talon reads it. Box mips in float32
from the unquantised chain, each level rounded half-even to R8 (vc_render.build_mips quant8). The max chain is the
conservative bound for the empty-ray skip.
==================
*/
// v1.1: the upload of a grey coverage field (box mips + the conservative max mips), shared by the africanight layer and
// the procedural field. Returns the level count.
static int R_HZM_VclUploadGrey( const byte *grey, int w, int h, GLuint *covTex, GLuint *covMaxTex );

static qboolean R_HZM_VclUploadCoverage( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	byte *pic = NULL, *grey;
	int w = 0, h = 0, mips = 0, i;
	GLenum fmt = GL_RGBA8;

	R_LoadImage( VCL_COVER_PATH, &pic, &w, &h, &fmt, &mips );
	if ( !pic ) {
		c->africaWhy = "hd-layer-missing";
		return qfalse;
	}
	if ( fmt != GL_RGBA8 || w != HZM_VCL_COVN || h != HZM_VCL_COVN ) {
		ri.Free( pic );
		c->africaWhy = "hd-layer-format";
		return qfalse;
	}
	grey = (byte *)ri.Malloc( w * h );
	for ( i = 0; i < w * h; i++ ) {
		grey[i] = R_HZM_VclGrey( pic + i * 4 );
	}
	ri.Free( pic );
	c->covLevels = R_HZM_VclUploadGrey( grey, w, h, &c->covTex, &c->covMaxTex );
	ri.Free( grey );
	return qtrue;
}

static int R_HZM_VclUploadGrey( const byte *grey, int w, int h, GLuint *covTex, GLuint *covMaxTex ) {
	hzmClouds_t *c = &tr.hzmClouds;
	byte *mx, *mxn, *up;
	float *cur, *nxt;
	int i, x, y, level, levels;
	GLint prev = 0;

	mx = (byte *)ri.Malloc( w * h );
	mxn = (byte *)ri.Malloc( ( w / 2 ) * ( h / 2 ) + 16 );
	up = (byte *)ri.Malloc( w * h );
	cur = (float *)ri.Malloc( sizeof( float ) * w * h );
	nxt = (float *)ri.Malloc( sizeof( float ) * ( w / 2 ) * ( h / 2 ) + 16 );
	for ( i = 0; i < w * h; i++ ) {
		cur[i] = (float)grey[i] / 255.0f;
		mx[i] = grey[i];
	}

	qglHzmPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	*covTex = R_HZM_VclGenTexture( GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D, &prev );
	for ( level = 0, x = w, y = h;; level++ ) {
		for ( i = 0; i < x * y; i++ ) {
			up[i] = (byte)R_HZM_VclRintF( cur[i] * 255.0f );
		}
		qglTexImage2D( GL_TEXTURE_2D, level, GL_R8, x, y, 0, GL_RED, GL_UNSIGNED_BYTE, up );
		c->vramBytes += x * y;
		if ( x == 1 && y == 1 ) {
			break;
		}
		R_HZM_VclCovDown( cur, x, y, nxt );
		memcpy( cur, nxt, sizeof( float ) * ( x / 2 ) * ( y / 2 ) );
		x /= 2; y /= 2;
	}
	levels = level + 1;
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	qglBindTexture( GL_TEXTURE_2D, (GLuint)prev );

	*covMaxTex = R_HZM_VclGenTexture( GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D, &prev );
	for ( level = 0, x = w, y = h;; level++ ) {
		qglTexImage2D( GL_TEXTURE_2D, level, GL_R8, x, y, 0, GL_RED, GL_UNSIGNED_BYTE, mx );
		c->vramBytes += x * y;
		if ( x == 1 && y == 1 ) {
			break;
		}
		R_HZM_VclMaxDown( mx, x, y, mxn );
		memcpy( mx, mxn, ( x / 2 ) * ( y / 2 ) );
		x /= 2; y /= 2;
	}
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	qglBindTexture( GL_TEXTURE_2D, (GLuint)prev );
	qglHzmPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

	ri.Free( nxt ); ri.Free( cur ); ri.Free( up ); ri.Free( mxn ); ri.Free( mx );
	return levels;
}

// v1.1: the procedural coverage field (tr_hzm_clouds_pure.h R_HZM_VclProcCoverage), generated and uploaded at R_Init
static qboolean R_HZM_VclUploadProcCoverage( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	const int n = VCL_PROC_COVN;
	byte *out = (byte *)ri.Malloc( n * n );
	float *val = (float *)ri.Malloc( sizeof( float ) * n * n );
	unsigned int *hist = (unsigned int *)ri.Malloc( sizeof( unsigned int ) * 65536 );
	int t0 = ri.Milliseconds();

	if ( !out || !val || !hist ) {
		if ( out ) ri.Free( out );
		if ( val ) ri.Free( val );
		if ( hist ) ri.Free( hist );
		return qfalse;
	}
	R_HZM_VclProcCoverage( n, out, val, hist );
	c->procMs = (float)( ri.Milliseconds() - t0 );
	R_HZM_VclUploadGrey( out, n, n, &c->procCovTex, &c->procCovMaxTex );
	ri.Free( hist );
	ri.Free( val );
	ri.Free( out );
	return qtrue;
}

// today's dome (s,t) per box-face vertex at cloud height 512 (tr_sky.c's formula, its own copy - vet R2-1)
static void R_HZM_VclUploadVerts( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	float table[6][VCL_SKY_SUBD + 1][VCL_SKY_SUBD + 1][2];
	GLint prev = 0;

	R_HZM_SkyVertexTable( 512.0f, table );
	c->vertTex = R_HZM_VclGenTexture( GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D, &prev );
	qglHzmPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	// row r = face * 9 + t, column = s: exactly the [face][t][s] layout
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RG32F, VCL_SKY_SUBD + 1, 6 * ( VCL_SKY_SUBD + 1 ), 0, GL_RG, GL_FLOAT, table );
	c->vramBytes += (int)sizeof( table );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglBindTexture( GL_TEXTURE_2D, (GLuint)prev );
}

/*
==================
R_HZM_Clouds_InitGL - R_Init, after GLSL_InitGPUShaders / R_Msaa_AfterGLSL (the MSAA path is decided)
==================
*/
static int R_HZM_VclDecideRes( void ) {
	int force = r_hzmCloudsRes ? r_hzmCloudsRes->integer : 0;
	int vram = ri.Cvar_VariableIntegerValue( "r_gpuVramMB" );
	const char *rend = (const char *)qglGetString( GL_RENDERER );

	int res;

	if ( force >= 2 && force <= 6 ) {
		return force;
	}
	if ( glRefConfig.intelGraphics && !( rend && strstr( rend, "Arc" ) ) ) {
		return 0;	// Intel iGPU: off
	}
	// v1.1 (2026-10-05, measured in engine): the trace cost follows the RAY COUNT. At 960x540 rays (1920x1080 half-res)
	// trace + tent + composite measured p50 0.15-0.26 ms, p99 <= 0.43 ms on the user's RTX 5070 Ti over the africanight
	// sky; half-res at 3440x1440 is 2.4x the rays (~1.0 ms p99, over the 0.5 ms budget). So the divisor is the smallest
	// integer >= 2 that keeps the trace at or under ~600k rays: 1080p -> 2, 1440p and 3440x1440 -> 3, 4K -> 4. Weaker
	// cards (< 8 GB) get one step coarser. Decided once at R_Init, never mid-map (no popping).
	for ( res = 2; res < 6; res++ ) {
		if ( ( glConfig.vidWidth / res ) * ( glConfig.vidHeight / res ) <= 600000 ) {
			break;
		}
	}
	if ( vram > 0 && vram < 8192 && res < 6 ) {
		res++;
	}
	return res;
}

void R_HZM_Clouds_InitGL( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	byte *noise;
	int t0;

	c->lastLogged = -1;
	c->supported = qtrue;

	if ( qglesMajorVersion >= 1 ) {
		R_HZM_CloudsSetReason( "gles" );
	} else if ( !( glRefConfig.glslMajorVersion > 1 || ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30 ) ) ) {
		R_HZM_CloudsSetReason( "glsl<130" );
	} else if ( !qglTexImage3D || !qglHzmPixelStorei ) {
		R_HZM_CloudsSetReason( "no-tex3d" );
	} else if ( !glRefConfig.framebufferObject || !glRefConfig.textureFloat ) {
		R_HZM_CloudsSetReason( "no-float-fbo" );
	} else if ( !r_hdr->integer ) {
		R_HZM_CloudsSetReason( "no-hdr" );
	}

	if ( c->supported ) {
		c->res = R_HZM_VclDecideRes();
		if ( !c->res ) {
			R_HZM_CloudsSetReason( "intel-igpu" );
		}
	}

	if ( c->supported ) {
		c->texW = ( glConfig.vidWidth + c->res - 1 ) / c->res;
		c->texH = ( glConfig.vidHeight + c->res - 1 ) / c->res;
		if ( !GLSL_HZM_BuildClouds( &c->traceShader, &c->tentShader, &c->compositeShader, HZM_VCL_TRACE_DEFINES,
		                            (qboolean)( r_hzmCloudsForceFail && r_hzmCloudsForceFail->integer ) ) ) {
			R_HZM_CloudsSetReason( "compile-failed" );
		}
	}

	if ( c->supported ) {
		// a missing / odd HD talonclouds layer turns only africanight off (it keeps today's stage); v1.1's procedural
		// profile needs only the noise and its own generated field
		c->africaOk = R_HZM_VclUploadCoverage();
		t0 = ri.Milliseconds();
		noise = (byte *)ri.Malloc( VCL_NOISE_N * VCL_NOISE_N * VCL_NOISE_N );
		if ( !noise || !R_HZM_VclMakeNoise( noise ) ) {
			R_HZM_CloudsSetReason( "no-memory" );
		} else {
			c->noiseMs = (float)( ri.Milliseconds() - t0 );
			R_HZM_VclUploadNoise( noise );
		}
		if ( noise ) {
			ri.Free( noise );
		}
	}

	if ( c->supported ) {
		R_HZM_VclUploadVerts();
		c->traceImage = R_CreateImage( "*vclTrace", NULL, c->texW, c->texH, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F_ARB );
		c->tentImage = R_CreateImage( "*vclTent", NULL, c->texW, c->texH, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F_ARB );
		c->vramBytes += 2 * c->texW * c->texH * 8;
		c->traceFbo = FBO_Create( "_vclTrace", c->texW, c->texH );
		FBO_AttachImage( c->traceFbo, c->traceImage, GL_COLOR_ATTACHMENT0_EXT, 0 );
		c->tentFbo = FBO_Create( "_vclTent", c->texW, c->texH );
		FBO_AttachImage( c->tentFbo, c->tentImage, GL_COLOR_ATTACHMENT0_EXT, 0 );
		if ( !R_CheckFBO( c->traceFbo ) || !R_CheckFBO( c->tentFbo ) ) {
			R_HZM_CloudsSetReason( "fbo-incomplete" );
		}
		FBO_Bind( NULL );
	}

	// v1.1: the procedural profile (painted boxes) - its field and its two programs; a failure turns only it off
	if ( c->supported && R_HZM_VclUploadProcCoverage() ) {
		c->vramBytes += VCL_PROC_COVN * VCL_PROC_COVN * 2 * 4 / 3;
		c->procSupported = (qboolean)GLSL_HZM_BuildCloudsProc( &c->procTraceShader, &c->skyCompShader,
			HZM_VCL_PROC_DEFINES, (qboolean)( r_hzmCloudsForceFail && r_hzmCloudsForceFail->integer ) );
	}
	// GPU timer objects (only read with r_hzmCloudsProbe 1)
	if ( c->supported && qglHzmQueryCounter && qglHzmGetQueryObjectui64v && qglGenQueries ) {
		qglGenQueries( 16, &c->gpuQ[0][0] );
	}

	if ( qglGetError() != GL_NO_ERROR ) {
		R_HZM_CloudsSetReason( "gl-error-at-init" );
	}

	ri.Printf( PRINT_ALL, "^~^~^ VCLBUILD %s %s %s look=%s auto=%d supported=%d reason=%s res=%d target=%dx%d noise_ms=%.1f vram_kb=%d glsl=%d.%d africa=%s proc=%d proc_ms=%.1f skyfog_auto=%d\n",
		HZM_GFX_COMMIT, HZM_GFX_DATE, hzmVclStateMarker, HZM_VCL_LOOK, HZM_CLOUDS_AUTO, c->supported ? 1 : 0,
		c->supported ? "-" : c->reason, c->res, c->texW, c->texH, c->noiseMs, c->vramBytes / 1024,
		glRefConfig.glslMajorVersion, glRefConfig.glslMinorVersion,
		c->africaOk ? "ok" : ( c->africaWhy ? c->africaWhy : "off" ), c->procSupported ? 1 : 0, c->procMs, HZM_SKYFOG_AUTO );
}

void R_HZM_Clouds_Shutdown( void ) {
	hzmClouds_t *c = &tr.hzmClouds;

	if ( c->noiseTex ) qglDeleteTextures( 1, &c->noiseTex );
	if ( c->covTex ) qglDeleteTextures( 1, &c->covTex );
	if ( c->covMaxTex ) qglDeleteTextures( 1, &c->covMaxTex );
	if ( c->vertTex ) qglDeleteTextures( 1, &c->vertTex );
	if ( c->procCovTex ) qglDeleteTextures( 1, &c->procCovTex );
	if ( c->procCovMaxTex ) qglDeleteTextures( 1, &c->procCovMaxTex );
	c->noiseTex = c->covTex = c->covMaxTex = c->vertTex = c->procCovTex = c->procCovMaxTex = 0;
	if ( c->gpuQ[0][0] && qglDeleteQueries ) {
		qglDeleteQueries( 16, &c->gpuQ[0][0] );
	}
	memset( c->gpuQ, 0, sizeof( c->gpuQ ) );
	GLSL_HZM_DeleteClouds( &c->traceShader, &c->tentShader, &c->compositeShader );
	if ( c->procSupported ) {
		GLSL_DeleteGPUShader( &c->procTraceShader );
		GLSL_DeleteGPUShader( &c->skyCompShader );
	}
	c->procSupported = qfalse;
	c->africaOk = qfalse;
	c->kind = 0;
	c->family = NULL;
	// the two targets and their FBOs belong to the image / FBO lists, which R_DeleteTextures / FBO_Shutdown free
	c->traceImage = c->tentImage = NULL;
	c->traceFbo = c->tentFbo = NULL;
	c->supported = qfalse;
	c->profileShader = NULL;
}

/*
==================
scope (plan 2.3): the fingerprint of the PARSED sky text (FinishShader, before the r_skyHD swap and any collapse),
the deny list, then the world's sky shader
==================
*/
void R_HZM_CloudsFingerprint( shader_t *sh, const shaderStage_t *st ) {
	char name[MAX_QPATH];
	const textureBundle_t *b;

	sh->hzmVclFp = 0;
	sh->hzmVclFpWhy = NULL;
	if ( !sh->isSky || Q_stricmp( sh->name, VCL_PROFILE_SKY ) ) {
		return;
	}
	sh->hzmVclFp = -1;
	if ( Q_stricmp( sh->sky.hzmBoxName, "env/mohnight1" ) ) {
		sh->hzmVclFpWhy = "box";
		return;
	}
	if ( sh->sky.cloudHeight != 512.0f ) {
		sh->hzmVclFpWhy = "cloudheight";
		return;
	}
	if ( !st[0].active || st[1].active ) {
		sh->hzmVclFpWhy = "stages";
		return;
	}
	b = &st[0].bundle[0];
	if ( !b->image[0] || b->numImageAnimations > 1 || b->isLightmap || b->isVideoMap ) {
		sh->hzmVclFpWhy = "map";
		return;
	}
	COM_StripExtension( b->image[0]->imgName, name, sizeof( name ) );
	if ( Q_stricmp( name, "textures/sky/talonclouds" ) ) {
		sh->hzmVclFpWhy = "map";
		return;
	}
	if ( ( st[0].stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) != ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE ) ) {
		sh->hzmVclFpWhy = "blendfunc";
		return;
	}
	if ( st[0].rgbGen != CGEN_IDENTITY && st[0].rgbGen != CGEN_IDENTITY_LIGHTING ) {
		sh->hzmVclFpWhy = "rgbgen";
		return;
	}
	// FinishShader resolves the default tcGen (TCGEN_BAD -> TCGEN_TEXTURE) only AFTER this runs, so a stage without an
	// explicit tcGen reads BAD here (v1.1, found in engine: every africanight map printed fingerprint:tcmod)
	if ( ( b->tcGen != TCGEN_TEXTURE && b->tcGen != TCGEN_BAD ) || b->numTexMods != 2
		|| b->texMods[0].type != TMOD_SCROLL || b->texMods[1].type != TMOD_SCALE
		|| b->texMods[0].scroll[0] != (float)HZM_VCL_SPS || b->texMods[0].scroll[1] != (float)HZM_VCL_SPT
		|| b->texMods[1].scale[0] != HZM_VCL_SCS || b->texMods[1].scale[1] != HZM_VCL_SCT ) {
		sh->hzmVclFpWhy = "tcmod";
		return;
	}
	sh->hzmVclFp = 1;
}


void R_HZM_Clouds_MapStart( void ) {
	hzmClouds_t *c = &tr.hzmClouds;

	c->profileShader = NULL;
	c->kind = 0;
	c->family = NULL;
	c->compositedSerial = -1;
	c->mapReason = "no-world";
	c->mapName[0] = 0;
	c->tracedSerial = -1;
	c->fogGateOff = qfalse;
	c->lastLogged = -1;
}

static void R_HZM_VclLogState( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	qboolean on = (qboolean)( c->supported && c->profileShader && R_HZM_CloudsEnabled() );
	const char *reason;

	if ( !c->supported ) {
		reason = c->reason[0] ? c->reason : "unsupported";
	} else if ( !c->profileShader ) {
		reason = c->mapReason ? c->mapReason : "no-profile-sky";
	} else if ( !R_HZM_CloudsEnabled() ) {
		reason = "off-switch";
	} else {
		reason = "-";
	}
	ri.Printf( PRINT_ALL, "^~^~^ CLOUDS map=%s sky=%s state=%s reason=%s res=%d glsl=%d.%d look=%s noise_ms=%.1f kind=%s family=%s skyfog=%s sun=%.3f,%.3f,%.3f zen=%.3f,%.3f,%.3f hor=%.3f,%.3f,%.3f\n",
		c->mapName[0] ? c->mapName : "-", c->profileShader ? c->profileShader->name : "-", on ? "on" : "off", reason,
		c->res, glRefConfig.glslMajorVersion, glRefConfig.glslMinorVersion,
		c->kind == 2 ? ( c->family ? c->family->label : "-" ) : HZM_VCL_LOOK, c->noiseMs,
		c->kind == 1 ? "africanight" : ( c->kind == 2 ? "procedural" : "none" ),
		c->family ? c->family->box : "-", R_HZM_SkyFogModelOn() ? "model" : ( tr.hzmSkyFogDeny ? "legacy-deny" : "legacy" ),
		tr.sunDirection[0], tr.sunDirection[1], tr.sunDirection[2], tr.hzmSkyZenith[0], tr.hzmSkyZenith[1],
		tr.hzmSkyZenith[2], tr.hzmSkyHorizon[0], tr.hzmSkyHorizon[1], tr.hzmSkyHorizon[2] );
	c->lastLogged = on ? 1 : 0;
}

void R_HZM_Clouds_MapLoaded( const char *baseName ) {
	hzmClouds_t *c = &tr.hzmClouds;
	shader_t *sky = NULL, *any = NULL, *proc = NULL, *refused = NULL;
	const char *deny;
	int i;
	int all = ( r_hzmCloudsAll && r_hzmCloudsAll->integer ) ? 1 : 0;

	Q_strncpyz( c->mapName, baseName ? baseName : "", sizeof( c->mapName ) );
	c->profileShader = NULL;
	if ( tr.world ) {
		for ( i = 0; i < tr.world->numsurfaces; i++ ) {
			shader_t *s = tr.world->surfaces[i].shader;
			if ( s && s->isSky ) {
				if ( !any ) {
					any = s;
				}
				if ( !Q_stricmp( s->name, VCL_PROFILE_SKY ) ) {
					sky = s;
					break;
				}
			}
		}
	}
	deny = R_HZM_VclDeny( c->mapName, sky ? sky->name : ( any ? any->name : NULL ),
		sky ? sky->sky.hzmBoxName : ( any ? any->sky.hzmBoxName : NULL ) );
	// v1.1: any OTHER world sky whose box is a reviewed family gets the procedural profile (africanight wins)
	if ( !sky && tr.world ) {
		for ( i = 0; i < tr.world->numsurfaces && !proc; i++ ) {
			shader_t *s = tr.world->surfaces[i].shader;
			if ( s && s->isSky && R_HZM_VclFamily( s->sky.hzmBoxName, all ) ) {
				proc = s;
			} else if ( s && s->isSky && !refused && R_HZM_VclFamily( s->sky.hzmBoxName, 1 ) ) {
				refused = s;	// a reviewed sky that is NOT on the allow list: named in the CLOUDS line
			}
		}
		if ( proc && !deny ) {
			deny = R_HZM_VclDeny( c->mapName, proc->name, proc->sky.hzmBoxName );
		}
	}
	// OMAHA IS NEVER TOUCHED (user hard rule). On top of the cloud deny list above: the shared five-map list the water,
	// wet and footprint passes refuse on (renderercommon/hzm_waterwet.h - m3l1a, m3l1b, e3l1, e3l2, obj_team3 and their
	// _suffix twins) and the world flag built from it. No profile shader is attached, so no trace and no composite can
	// run there whatever r_hzmClouds says; RB_HZM_CloudsTrace checks the flag again per view.
	if ( HZM_WaterWetOmahaMap( c->mapName ) || tr.hzmOmahaWorld ) {
		deny = "deny:omaha";
	}
	if ( deny ) {
		c->mapReason = deny;
	} else if ( r_skyHDCompare && r_skyHDCompare->integer ) {
		c->mapReason = "sky-compare";
	} else if ( r_fastsky && r_fastsky->integer ) {
		c->mapReason = "r_fastsky";
	} else if ( sky && !HZM_VCL_ALLOW_AFRICANIGHT && !all ) {
		c->mapReason = "sky-not-allowed:africanight";
	} else if ( sky ) {
		if ( sky->hzmVclFp != 1 ) {
			Com_sprintf( c->mapReasonBuf, sizeof( c->mapReasonBuf ), "fingerprint:%s",
				sky->hzmVclFpWhy ? sky->hzmVclFpWhy : "unchecked" );
			c->mapReason = c->mapReasonBuf;
		} else if ( !c->africaOk ) {
			c->mapReason = c->africaWhy ? c->africaWhy : "hd-layer-missing";
		} else {
			c->mapReason = NULL;
			c->profileShader = sky;
			c->kind = 1;
		}
	} else if ( proc ) {
		if ( !c->procSupported ) {
			c->mapReason = "proc-unsupported";
		} else {
			c->mapReason = NULL;
			c->profileShader = proc;
			c->family = R_HZM_VclFamily( proc->sky.hzmBoxName, all );
			c->kind = 2;
		}
	} else if ( refused ) {
		Com_sprintf( c->mapReasonBuf, sizeof( c->mapReasonBuf ), "sky-not-allowed:%s", refused->sky.hzmBoxName );
		c->mapReason = c->mapReasonBuf;
	} else {
		c->mapReason = "no-profile-sky";
	}
	R_HZM_VclLogState();
}

/*
==================
backend: the trace (RB_DrawSurfs, main list, !shadow view) and the composite gate (tr_shade.c)
==================
*/
static qboolean RB_HZM_VclListHasProfile( const drawSurf_t *drawSurfs, int numDrawSurfs, const shader_t *profile ) {
	int i;

	for ( i = 0; i < numDrawSurfs; i++ ) {
		shader_t *shader;
		int entityNum, fogNum, dlighted, pshadowed;
		qboolean bStatic;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed, &bStatic );
		if ( shader == profile ) {
			return qtrue;
		}
		if ( shader && shader->sort > profile->sort ) {
			return qfalse;	// sorted: the sky comes first
		}
	}
	return qfalse;
}

static void RB_HZM_VclQuad( void ) {
	vec4_t quadVerts[4];
	vec2_t texCoords[4];

	VectorSet4( quadVerts[0], -1.0f,  1.0f, 0.0f, 1.0f );
	VectorSet4( quadVerts[1],  1.0f,  1.0f, 0.0f, 1.0f );
	VectorSet4( quadVerts[2],  1.0f, -1.0f, 0.0f, 1.0f );
	VectorSet4( quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f );
	VectorSet2( texCoords[0], 0.0f, 1.0f );
	VectorSet2( texCoords[1], 1.0f, 1.0f );
	VectorSet2( texCoords[2], 1.0f, 0.0f );
	VectorSet2( texCoords[3], 0.0f, 0.0f );
	RB_InstantQuad2( quadVerts, texCoords );
}

/*
==================
v1.1 GPU timer (r_hzmCloudsProbe 1): GL_TIMESTAMP marks 0/1 around the trace + tent, 2/3 around the composite, in a ring
of 4 frames read back 3 frames late (no stall). ^~^~^ CLOUDSGPU every 5 s: p50 / p99 / max of trace+tent+composite.
==================
*/
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif

static void RB_HZM_VclGpuMark( int k ) {
	hzmClouds_t *c = &tr.hzmClouds;
	int slot = c->gpuFrame & 3;

	if ( !r_hzmCloudsProbe || !r_hzmCloudsProbe->integer || !c->gpuQ[0][0] || !qglHzmQueryCounter ) {
		return;
	}
	qglHzmQueryCounter( c->gpuQ[slot][k], GL_TIMESTAMP );
	c->gpuQUsed[slot] |= 1 << k;
}

static void RB_HZM_VclGpuFrameEnd( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	unsigned long long t[4] = { 0, 0, 0, 0 };
	int slot, used, k;
	float ms;

	if ( !c->gpuQ[0][0] || !qglHzmGetQueryObjectui64v ) {
		return;
	}
	c->gpuFrame++;
	slot = c->gpuFrame & 3;		// the slot written 3 frames ago, about to be reused
	used = c->gpuQUsed[slot];
	if ( ( used & 3 ) == 3 ) {
		for ( k = 0; k < 4; k++ ) {
			if ( used & ( 1 << k ) ) {
				qglHzmGetQueryObjectui64v( c->gpuQ[slot][k], GL_QUERY_RESULT, &t[k] );
			}
		}
		ms = (float)( (double)( t[1] - t[0] ) / 1.0e6 );
		if ( ( used & 12 ) == 12 ) {
			ms += (float)( (double)( t[3] - t[2] ) / 1.0e6 );
		}
		if ( c->gpuN < (int)ARRAY_LEN( c->gpuMs ) ) {
			c->gpuMs[c->gpuN++] = ms;
		}
	}
	c->gpuQUsed[slot] = 0;
}

static int RB_HZM_VclCmpF( const void *a, const void *b ) {
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : ( x > y ? 1 : 0 );
}

static void RB_HZM_VclGpuReport( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	float s[512];
	int n = c->gpuN;

	if ( n <= 0 ) {
		return;
	}
	memcpy( s, c->gpuMs, sizeof( float ) * n );
	qsort( s, n, sizeof( float ), RB_HZM_VclCmpF );
	ri.Printf( PRINT_ALL, "^~^~^ CLOUDSGPU map=%s kind=%d n=%d p50=%.3f p99=%.3f max=%.3f res=%d trace=%dx%d\n",
		c->mapName[0] ? c->mapName : "-", c->kind, n, s[n / 2], s[( n * 99 ) / 100], s[n - 1], c->res, c->tw, c->th );
	c->gpuN = 0;
}

/*
==================
v1.1 THE PROCEDURAL PROFILE - per frame. The family's rest state (tr_hzm_clouds_pure.h hzmCloudFamilies), the dev
overrides (r_hzmCloudsTune), the storm (R_HZM_Storm sunFade raises cover toward overcast, dims the sun, tints the sky
light), and the map's own palette: sky light from the box's 1x1 mips (tr.hzmSkyZenith / tr.hzmSkyHorizon, built per
map by tr_hzm_wet.c), the sun / moon direction and hue from the world's sun (tr.sunDirection / tr.sunLight), its
brightness from the box (so a night box lights its clouds by moonlight and a day box by day).
==================
*/
// look-dev v4 (2026-10-12): what the v3 light model could not reach from r_hzmCloudsTune. Each sky family carries its
// own rest values (tr_hzm_clouds_pure.h); r_hzmCloudsTune overrides them per frame. The v3 values are in brackets.
//   deep      weight of the third multiple-scatter octave (0 = v3: a thick base keeps ~4 % of its top)
//   silver    forward-scatter rim on thin cloud toward the sun (0 = v3)
//   shrink    how much the outline narrows from base to top (v3: 0.1)
//   powder    0..1 darkening of the first skin of the cloud (0 = v3)
//   white     0..1 how far the sky light on the cloud is pulled from the box colour toward neutral (0 = v3)
//   ambbot    light from below / the side as a share of the light from above (v3: 0.8)
//   sunw      0..1 how far the sun's hue is pulled toward white (0 = v3)
//   lumfloor  the least box brightness the light is scaled from (v3: 0.02)
typedef struct {
	float deep, silver, shrink, powder, white, ambBot, sunWhite, lumFloor;
} hzmVclLook_t;
#define HZM_VCL_LOOK_V3 { 0.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.8f, 0.0f, 0.02f }
static hzmVclLook_t hzmVclLook = HZM_VCL_LOOK_V3;

static void RB_HZM_VclTune( hzmCloudFamily_t *f ) {
	char buf[256], *p, *eq;

	hzmVclLook.deep = f->deep;
	hzmVclLook.silver = f->silver;
	hzmVclLook.shrink = f->shrink;
	hzmVclLook.powder = f->powder;
	hzmVclLook.white = f->white;
	hzmVclLook.ambBot = f->ambBot;
	hzmVclLook.sunWhite = f->sunWhite;
	hzmVclLook.lumFloor = f->lumFloor;
	if ( !r_hzmCloudsTune || !r_hzmCloudsTune->string[0] ) {
		return;
	}
	Q_strncpyz( buf, r_hzmCloudsTune->string, sizeof( buf ) );
	for ( p = strtok( buf, " " ); p; p = strtok( NULL, " " ) ) {
		float v;
		if ( !( eq = strchr( p, '=' ) ) ) {
			continue;
		}
		*eq = 0;
		v = (float)atof( eq + 1 );
		if ( !Q_stricmp( p, "cover" ) ) f->cover = v;
		else if ( !Q_stricmp( p, "sigma" ) ) f->sigma = v;
		else if ( !Q_stricmp( p, "sun" ) ) f->sun = v;
		else if ( !Q_stricmp( p, "amb" ) ) f->amb = v;
		else if ( !Q_stricmp( p, "tile" ) && v > 64.0f ) f->tile = v;
		else if ( !Q_stricmp( p, "wind" ) ) f->windSpeed = v;
		else if ( !Q_stricmp( p, "winddeg" ) ) f->windDeg = v;
		else if ( !Q_stricmp( p, "eros" ) ) f->eros = v;
		else if ( !Q_stricmp( p, "lmax" ) ) f->lmax = v;
		else if ( !Q_stricmp( p, "ramp" ) && v > 0.01f ) f->ramp = v;
		else if ( !Q_stricmp( p, "nfreq" ) && v >= 1.0f ) f->nfreq = (float)floor( v + 0.5 );	// integer: seamless wrap
		else if ( !Q_stricmp( p, "detail" ) ) f->detail = v;
		else if ( !Q_stricmp( p, "dfreq" ) && v >= 1.0f ) f->dfreq = (float)floor( v + 0.5 );	// integer: seamless wrap
		else if ( !Q_stricmp( p, "basesharp" ) ) f->basesharp = v;
		else if ( !Q_stricmp( p, "base1" ) ) f->base1 = v;
		else if ( !Q_stricmp( p, "topk" ) ) f->topk = v;
		// look-dev v4 knobs (file statics: one sky at a time is tuned, and r_hzmCloudsTune "" restores the family)
		else if ( !Q_stricmp( p, "deep" ) ) hzmVclLook.deep = v;
		else if ( !Q_stricmp( p, "silver" ) ) hzmVclLook.silver = v;
		else if ( !Q_stricmp( p, "shrink" ) ) hzmVclLook.shrink = v;
		else if ( !Q_stricmp( p, "powder" ) ) hzmVclLook.powder = v;
		else if ( !Q_stricmp( p, "white" ) ) hzmVclLook.white = v;
		else if ( !Q_stricmp( p, "ambbot" ) ) hzmVclLook.ambBot = v;
		else if ( !Q_stricmp( p, "sunw" ) ) hzmVclLook.sunWhite = v;
		else if ( !Q_stricmp( p, "lumfloor" ) ) hzmVclLook.lumFloor = v;
	}
}

static float RB_HZM_VclLuma( const float *c ) {
	return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static void RB_HZM_CloudsProcTrace( const shader_t *prof ) {
	hzmClouds_t *c = &tr.hzmClouds;
	shaderProgram_t *sp = &c->procTraceShader;
	hzmCloudFamily_t fam = *c->family;
	const hzmStormState_t *hs = R_HZM_Storm();
	FBO_t *oldFbo;
	mat4_t ident;
	vec4_t v;
	vec3_t chroma, sunDir;
	double t, a;
	float tanX, tanY, storm, cover, lum, mx, cs, sn;
	int tw, th, sceneW, sceneH, k;

	RB_HZM_VclTune( &fam );
	storm = ( hs && hs->active ) ? hs->sunFade : 0.0f;
	storm = storm < 0.0f ? 0.0f : ( storm > 1.0f ? 1.0f : storm );
	cover = fam.cover + ( 0.9f - fam.cover ) * storm;
	cover = cover < 0.02f ? 0.02f : ( cover > 0.98f ? 0.98f : cover );

	// coverage ramp: threshold 1 - cover, full density 0.22 above it; extinction; top erosion
	VectorSet4( c->procA, 1.0f - cover, 1.0f - cover + fam.ramp, fam.sigma * ( 1.0f + 0.6f * storm ), fam.eros );
	// the palette
	lum = RB_HZM_VclLuma( tr.hzmSkyZenith );
	if ( RB_HZM_VclLuma( tr.hzmSkyHorizon ) > lum ) {
		lum = RB_HZM_VclLuma( tr.hzmSkyHorizon );
	}
	lum = lum < hzmVclLook.lumFloor ? hzmVclLook.lumFloor : lum;
	mx = tr.sunLight[0] > tr.sunLight[1] ? tr.sunLight[0] : tr.sunLight[1];
	mx = tr.sunLight[2] > mx ? tr.sunLight[2] : mx;
	if ( mx > 1e-4f ) {
		VectorScale( tr.sunLight, 1.0f / mx, chroma );
	} else {
		VectorSet( chroma, 1.0f, 1.0f, 1.0f );
	}
	for ( k = 0; k < 3; k++ ) {
		chroma[k] += ( 1.0f - chroma[k] ) * hzmVclLook.sunWhite;
	}
	VectorSet4( c->lcol, chroma[0], chroma[1], chroma[2], 0.0f );
	VectorScale( c->lcol, fam.sun * lum * ( 1.0f - 0.75f * storm ), c->lcol );
	{
		float zl = RB_HZM_VclLuma( tr.hzmSkyZenith ), hl = RB_HZM_VclLuma( tr.hzmSkyHorizon );
		for ( k = 0; k < 3; k++ ) {
			float tint = ( hs && hs->active ) ? hs->fog[k] : 1.0f;
			float zc = tr.hzmSkyZenith[k] + ( zl - tr.hzmSkyZenith[k] ) * hzmVclLook.white;
			float hc = tr.hzmSkyHorizon[k] + ( hl - tr.hzmSkyHorizon[k] ) * hzmVclLook.white;
			c->ambTop[k] = zc * fam.amb * tint * ( 1.0f - 0.3f * storm );
			c->ambBot[k] = hc * fam.amb * hzmVclLook.ambBot * tint * ( 1.0f - 0.3f * storm );
		}
	}
	c->lcol[3] = hzmVclLook.deep;
	c->ambTop[3] = hzmVclLook.shrink;
	c->ambBot[3] = hzmVclLook.powder;
	VectorCopy( tr.sunDirection, sunDir );
	if ( VectorNormalize( sunDir ) < 1e-4f ) {
		VectorSet( sunDir, 0.3f, 0.3f, 0.9f );
		VectorNormalize( sunDir );
	}
	VectorSet4( c->ldir, sunDir[0], sunDir[1], sunDir[2], hzmVclLook.silver );

	// wind: the field moves TOWARD the heading; offsets wrapped in double (the field and the noise both tile at 1)
	t = backEnd.refdef.floatTime - prof->timeOffset;
	cs = (float)cos( DEG2RAD( fam.windDeg ) );
	sn = (float)sin( DEG2RAD( fam.windDeg ) );
	VectorSet4( c->procB, fam.lmax * lum, 1.0f / fam.tile, fam.nfreq, fam.detail );
	VectorSet4( c->procC, fam.dfreq, fam.basesharp, fam.base1, fam.topk );

	sceneW = tr.sceneWidth > 0 ? tr.sceneWidth : glConfig.vidWidth;
	sceneH = tr.sceneHeight > 0 ? tr.sceneHeight : glConfig.vidHeight;
	tw = (int)ceil( backEnd.viewParms.viewportWidth  * ( glConfig.vidWidth  / (double)sceneW ) / c->res );
	th = (int)ceil( backEnd.viewParms.viewportHeight * ( glConfig.vidHeight / (double)sceneH ) / c->res );
	tw = tw < 1 ? 1 : ( tw > c->texW ? c->texW : tw );
	th = th < 1 ? 1 : ( th > c->texH ? c->texH : th );
	tanX = (float)tan( DEG2RAD( backEnd.viewParms.fovX * 0.5 ) );
	tanY = (float)tan( DEG2RAD( backEnd.viewParms.fovY * 0.5 ) );

	oldFbo = glState.currentFBO;
	Mat4Identity( ident );
	GL_Cull( CT_TWO_SIDED );
	GL_State( GLS_DEPTHTEST_DISABLE );

	FBO_Bind( c->traceFbo );
	qglViewport( 0, 0, tw, th );
	qglScissor( 0, 0, tw, th );
	GLSL_BindProgram( sp );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, ident );
	VectorSet4( v, -backEnd.viewParms.or.axis[1][0], -backEnd.viewParms.or.axis[1][1], -backEnd.viewParms.or.axis[1][2], tanX );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLRIGHT, v );
	VectorSet4( v, backEnd.viewParms.or.axis[2][0], backEnd.viewParms.or.axis[2][1], backEnd.viewParms.or.axis[2][2], tanY );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLUP, v );
	VectorSet4( v, backEnd.viewParms.or.axis[0][0], backEnd.viewParms.or.axis[0][1], backEnd.viewParms.or.axis[0][2], 2.0f * tanY / th );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLFWD, v );
	a = t * fam.windSpeed / fam.tile;
	VectorSet4( v, (float)-( a * cs - floor( a * cs ) ), (float)-( a * sn - floor( a * sn ) ),
		(float)( r_hzmCloudsDebug ? r_hzmCloudsDebug->integer : 0 ), 1.0f / th );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLSCROLL, v );
	VectorSet4( v, (float)tw, (float)th, 1.0f / tw, 0.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLVIEW, v );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLPROCA, c->procA );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLPROCB, c->procB );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLPROCC, c->procC );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLLDIR, c->ldir );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLLCOL, c->lcol );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLAMBTOP, c->ambTop );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLAMBBOT, c->ambBot );
	// the sky fog model for the per-pixel early exit (fully fogged sky: nothing to march)
	VectorSet4( v, 0.0f, 0.0f, 0.0f, 0.0f );
	if ( R_HZM_SkyFogModelOn() && R_UseForwardGlobalFog() && rb_globalFog.active && rb_globalFog.end > rb_globalFog.start
		&& !( r_globalFogSky && !r_globalFogSky->integer ) ) {
		VectorSet4( v, r_hzmSkyFogHeight->value, rb_globalFog.start, 1.0f / ( rb_globalFog.end - rb_globalFog.start ),
			r_globalFogScale ? r_globalFogScale->value : 1.0f );
	}
	GLSL_SetUniformVec4( sp, UNIFORM_VCLFOG, v );
	GL_BindMultiTexture( GL_TEXTURE1, GL_TEXTURE_2D, c->procCovTex );
	GL_BindMultiTexture( GL_TEXTURE2, GL_TEXTURE_2D, c->procCovMaxTex );
	GL_BindMultiTexture( GL_TEXTURE3, GL_TEXTURE_3D, c->noiseTex );
	GL_BindMultiTexture( GL_TEXTURE4, GL_TEXTURE_2D, c->vertTex );
	RB_HZM_VclQuad();

	FBO_Bind( c->tentFbo );
	qglViewport( 0, 0, tw, th );
	qglScissor( 0, 0, tw, th );
	GLSL_BindProgram( &c->tentShader );
	GLSL_SetUniformMat4( &c->tentShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, ident );
	VectorSet4( v, (float)tw, (float)th, 1.0f / tw, 0.0f );
	GLSL_SetUniformVec4( &c->tentShader, UNIFORM_VCLVIEW, v );
	GL_BindToTMU( c->traceImage, TB_COLORMAP );
	RB_HZM_VclQuad();

	FBO_Bind( oldFbo );
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );

	c->tw = tw;
	c->th = th;
	c->vpX = backEnd.viewParms.viewportX;
	c->vpY = backEnd.viewParms.viewportY;
	c->vpW = backEnd.viewParms.viewportWidth;
	c->vpH = backEnd.viewParms.viewportHeight;
	c->tracedSerial = c->serial;
	c->nTraced++;
}

/*
==================
v1.1 the procedural composite: tr_sky.c RB_StageIteratorSky calls this at the end of the profile sky's pass, still at
depth range (1,1). A full-screen triangle, depth test on (LEQUAL against the 1.0 clear), no depth write, blend ONE,
SRC_ALPHA, alpha write-masked: it lands only on pixels nothing nearer has covered - the box and its stages, already
fogged, keep T of their light. Once per view serial; a view whose trace did not run keeps today's sky.
==================
*/
void RB_HZM_CloudsSkyComposite( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	shaderProgram_t *sp = &c->skyCompShader;
	mat4_t ident;

	if ( c->kind != 2 || !c->procSupported || !c->profileShader || tess.shader != c->profileShader
		|| backEnd.depthFill || r_showsky->integer ) {
		return;
	}
	if ( !c->supported || !R_HZM_CloudsEnabled() || c->tracedSerial != c->serial ) {
		c->nFallback++;
		return;
	}
	if ( c->compositedSerial == c->serial ) {
		return;
	}
	GLSL_BindProgram( sp );
	Mat4Identity( ident );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, ident );
	RB_SetGlobalFogUniforms( sp, 0, qtrue );
	RB_HZM_CloudsCompositeUniforms( sp );
	GL_Cull( CT_TWO_SIDED );
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_SRC_ALPHA );
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], GL_FALSE );
	RB_HZM_VclQuad();
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
	c->compositedSerial = c->serial;
	RB_HZM_CloudsCompositeDone();
}

// the sky fog model's fraction at an elevation (main view; 0 when the sky is not fogged) - the SKYFOG probe line
float RB_HZM_SkyFogAt( float sinElev ) {
	float d, f, scale;

	if ( !R_UseForwardGlobalFog() || !rb_globalFog.active || rb_globalFog.end <= rb_globalFog.start
		|| ( r_globalFogSky && !r_globalFogSky->integer ) ) {
		return 0.0f;
	}
	scale = r_globalFogScale ? r_globalFogScale->value : 1.0f;
	if ( R_HZM_SkyFogModelOn() ) {
		d = r_hzmSkyFogHeight->value / ( sinElev > 1e-4f ? sinElev : 1e-4f );
	} else {
		d = rb_globalFog.zFar;
	}
	f = ( d - rb_globalFog.start ) / ( rb_globalFog.end - rb_globalFog.start );
	f = f < 0.0f ? 0.0f : ( f > 1.0f ? 1.0f : f );
	f *= scale;
	return f > 1.0f ? 1.0f : f;
}

void RB_HZM_CloudsTrace( const drawSurf_t *drawSurfs, int numDrawSurfs ) {
	hzmClouds_t *c = &tr.hzmClouds;
	const shader_t *prof = c->profileShader;
	FBO_t *oldFbo;
	mat4_t ident;
	vec4_t v;
	double t, so, to;
	float tanX, tanY, f;
	int tw, th, sceneW, sceneH;

	c->serial++;

	if ( c->lastLogged >= 0 && c->lastLogged != ( ( c->supported && prof && R_HZM_CloudsEnabled() ) ? 1 : 0 ) ) {
		R_HZM_VclLogState();	// the switch changed: one CLOUDS line (plan 2.2)
	}
	if ( !c->supported || !prof || !R_HZM_CloudsEnabled() || tr.hzmOmahaWorld ) {
		return;
	}
	if ( r_fastsky->integer || backEnd.projection2D || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL )
		|| backEnd.viewParms.isPortal || backEnd.viewParms.isPortalSky || backEnd.viewParms.targetFbo
		|| ( backEnd.viewParms.flags & ( VPF_SHADOWMAP | VPF_DEPTHSHADOW ) ) ) {
		return;
	}
	if ( !RB_HZM_VclListHasProfile( drawSurfs, numDrawSurfs, prof ) ) {
		return;
	}
	// the fog gate, with hysteresis (plan 2.3): fully fogged sky = today's stage draw
	f = RB_HZM_SkyFogFraction();
	if ( c->fogGateOff ) {
		if ( f < 0.998f ) {
			c->fogGateOff = qfalse;
		}
	} else if ( f >= 0.999f ) {
		c->fogGateOff = qtrue;
	}
	if ( c->fogGateOff ) {
		return;
	}

	if ( c->kind == 2 ) {
		RB_HZM_VclGpuMark( 0 );
		RB_HZM_CloudsProcTrace( prof );
		RB_HZM_VclGpuMark( 1 );
		return;
	}
	RB_HZM_VclGpuMark( 0 );

	sceneW = tr.sceneWidth > 0 ? tr.sceneWidth : glConfig.vidWidth;
	sceneH = tr.sceneHeight > 0 ? tr.sceneHeight : glConfig.vidHeight;
	tw = (int)ceil( backEnd.viewParms.viewportWidth  * ( glConfig.vidWidth  / (double)sceneW ) / c->res );
	th = (int)ceil( backEnd.viewParms.viewportHeight * ( glConfig.vidHeight / (double)sceneH ) / c->res );
	tw = tw < 1 ? 1 : ( tw > c->texW ? c->texW : tw );
	th = th < 1 ? 1 : ( th > c->texH ? c->texH : th );

	tanX = (float)tan( DEG2RAD( backEnd.viewParms.fovX * 0.5 ) );
	tanY = (float)tan( DEG2RAD( backEnd.viewParms.fovY * 0.5 ) );

	// one clock (refdef) for the trace and the composite: RB_BeginSurface's shaderTime for the profile shader
	t = backEnd.refdef.floatTime - prof->timeOffset;
	if ( prof->clampTime && t >= prof->clampTime ) {
		t = prof->clampTime;
	}
	so = R_HZM_VclScroll( t, HZM_VCL_SPS, HZM_VCL_SCS );	// RB_CalcScrollTexMatrix's wrap, then the scale
	to = R_HZM_VclScroll( t, HZM_VCL_SPT, HZM_VCL_SCT );

	oldFbo = glState.currentFBO;
	Mat4Identity( ident );
	GL_Cull( CT_TWO_SIDED );
	GL_State( GLS_DEPTHTEST_DISABLE );

	FBO_Bind( c->traceFbo );
	qglViewport( 0, 0, tw, th );
	qglScissor( 0, 0, tw, th );
	GLSL_BindProgram( &c->traceShader );
	GLSL_SetUniformMat4( &c->traceShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, ident );
	VectorSet4( v, -backEnd.viewParms.or.axis[1][0], -backEnd.viewParms.or.axis[1][1], -backEnd.viewParms.or.axis[1][2], tanX );
	GLSL_SetUniformVec4( &c->traceShader, UNIFORM_VCLRIGHT, v );
	VectorSet4( v, backEnd.viewParms.or.axis[2][0], backEnd.viewParms.or.axis[2][1], backEnd.viewParms.or.axis[2][2], tanY );
	GLSL_SetUniformVec4( &c->traceShader, UNIFORM_VCLUP, v );
	VectorSet4( v, backEnd.viewParms.or.axis[0][0], backEnd.viewParms.or.axis[0][1], backEnd.viewParms.or.axis[0][2], 2.0f * tanY / th );
	GLSL_SetUniformVec4( &c->traceShader, UNIFORM_VCLFWD, v );
	VectorSet4( v, (float)so, (float)to, (float)( r_hzmCloudsDebug ? r_hzmCloudsDebug->integer : 0 ), 1.0f / th );
	GLSL_SetUniformVec4( &c->traceShader, UNIFORM_VCLSCROLL, v );
	VectorSet4( v, (float)tw, (float)th, 1.0f / tw, 0.0f );
	GLSL_SetUniformVec4( &c->traceShader, UNIFORM_VCLVIEW, v );
	GL_BindMultiTexture( GL_TEXTURE1, GL_TEXTURE_2D, c->covTex );
	GL_BindMultiTexture( GL_TEXTURE2, GL_TEXTURE_2D, c->covMaxTex );
	GL_BindMultiTexture( GL_TEXTURE3, GL_TEXTURE_3D, c->noiseTex );
	GL_BindMultiTexture( GL_TEXTURE4, GL_TEXTURE_2D, c->vertTex );
	RB_HZM_VclQuad();

	FBO_Bind( c->tentFbo );
	qglViewport( 0, 0, tw, th );
	qglScissor( 0, 0, tw, th );
	GLSL_BindProgram( &c->tentShader );
	GLSL_SetUniformMat4( &c->tentShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, ident );
	GLSL_SetUniformVec4( &c->tentShader, UNIFORM_VCLVIEW, v );
	GL_BindToTMU( c->traceImage, TB_COLORMAP );
	RB_HZM_VclQuad();

	FBO_Bind( oldFbo );
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );

	c->tw = tw;
	c->th = th;
	c->vpX = backEnd.viewParms.viewportX;
	c->vpY = backEnd.viewParms.viewportY;
	c->vpW = backEnd.viewParms.viewportWidth;
	c->vpH = backEnd.viewParms.viewportHeight;
	c->tracedSerial = c->serial;
	c->nTraced++;
	RB_HZM_VclGpuMark( 1 );
}

// the sky pass asks, per view, whether the profile stage is drawn by the composite (else today's stage draw)
qboolean RB_HZM_CloudsCompositeReady( const shader_t *shader ) {
	hzmClouds_t *c = &tr.hzmClouds;

	if ( c->kind != 1 || !c->profileShader || shader != c->profileShader || backEnd.depthFill ) {
		return qfalse;
	}
	if ( !c->supported || !R_HZM_CloudsEnabled() || c->tracedSerial != c->serial ) {
		c->nFallback++;
		return qfalse;
	}
	return qtrue;
}

// the composite's own uniforms and the tent texture on TMU 1 (tr_shade.c draws the stage geometry)
void RB_HZM_CloudsCompositeUniforms( shaderProgram_t *sp ) {
	hzmClouds_t *c = &tr.hzmClouds;
	vec4_t v;

	VectorSet4( v, (float)c->vpX, (float)c->vpY, 1.0f / c->vpW, 1.0f / c->vpH );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLVP, v );
	VectorSet4( v, HZM_VCL_CLO, (float)( r_hzmCloudsDebug ? r_hzmCloudsDebug->integer : 0 ), (float)c->tw, (float)c->th );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLMISC, v );
	VectorSet4( v, 1.0f / c->texW, 1.0f / c->texH, 0.0f, 0.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLVIEW, v );
	GL_BindToTMU( c->tentImage, TB_LIGHTMAP );
	RB_HZM_VclGpuMark( 2 );
}

void RB_HZM_CloudsCompositeDone( void ) {
	tr.hzmClouds.nComposited++;
	tr.hzmClouds.frameHasClouds = qtrue;
	RB_HZM_VclGpuMark( 3 );
}

/*
==================
the animated cloud-masked tone-pass dither (plan 2.4 step 4). Returns qtrue when this frame dithers; the caller then
writes through the depth-less alias of screenScratch (tr.globalFogFbo), because the mask samples tr.renderDepthImage.
A = 0 (or no cloud this frame) leaves the uniform at 0 and the pass byte-identical.
==================
*/
qboolean RB_HZM_CloudsToneDither( shaderProgram_t *sp ) {
	hzmClouds_t *c = &tr.hzmClouds;
	float a = r_hzmCloudsDither ? r_hzmCloudsDither->value : 0.0f;
	vec4_t v;
	qboolean on = (qboolean)( c->frameHasClouds && a > 0.0f && c->tracedSerial == c->serial && tr.globalFogFbo
		&& tr.renderDepthImage );

	if ( !on ) {
		VectorSet4( v, 0.0f, 0.0f, 0.0f, 0.0f );
		GLSL_SetUniformVec4( sp, UNIFORM_VCLDITHER, v );
		return qfalse;
	}
	if ( a > 2.0f ) {
		a = 2.0f;
	}
	VectorSet4( v, a / 255.0f, 5.588238f * (float)( tr.frameCount % 64 ), 1.0f - RB_HZM_SkyFogFraction(), 0.0f );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLDITHER, v );
	VectorSet4( v, (float)c->vpX, (float)c->vpY, 1.0f / c->vpW, 1.0f / c->vpH );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLVP, v );
	VectorSet4( v, HZM_VCL_CLO, 0.0f, (float)c->tw, (float)c->th );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLMISC, v );
	VectorSet4( v, 1.0f / c->texW, 1.0f / c->texH, 1.0f / tr.renderDepthImage->width, 1.0f / tr.renderDepthImage->height );
	GLSL_SetUniformVec4( sp, UNIFORM_VCLVIEW, v );
	GL_BindToTMU( c->tentImage, TB_NORMALMAP );
	GL_BindToTMU( tr.renderDepthImage, TB_SCREENDEPTH );
	return qtrue;
}

// once per frame, after the post chain: probe lines, and clear the per-frame flag
void RB_HZM_CloudsFrameEnd( void ) {
	hzmClouds_t *c = &tr.hzmClouds;
	int now;

	c->frameHasClouds = qfalse;
	c->nFrames++;
	if ( !r_hzmCloudsProbe || !r_hzmCloudsProbe->integer ) {
		return;
	}
	RB_HZM_VclGpuFrameEnd();
	now = ri.Milliseconds();
	if ( now < c->probeNextMs ) {
		return;
	}
	c->probeNextMs = now + 5000;
	RB_HZM_VclGpuReport();
	ri.Printf( PRINT_ALL, "^~^~^ SKYFOG map=%s model=%d H=%.0f fog=%d start=%.0f end=%.0f zfar=%.0f f_zenith=%.3f f_30=%.3f f_10=%.3f\n",
		c->mapName[0] ? c->mapName : "-", R_HZM_SkyFogModelOn() ? 1 : 0, r_hzmSkyFogHeight ? r_hzmSkyFogHeight->value : 0.0f,
		rb_globalFog.active ? 1 : 0, rb_globalFog.start, rb_globalFog.end, rb_globalFog.zFar,
		RB_HZM_SkyFogAt( 1.0f ), RB_HZM_SkyFogAt( 0.5f ), RB_HZM_SkyFogAt( 0.1736f ) );
	ri.Printf( PRINT_ALL, "^~^~^ CLOUDSFRAMES map=%s frames=%d traced=%d composited=%d fallback=%d fog_gate_off=%d res=%d trace=%dx%d\n",
		c->mapName[0] ? c->mapName : "-", c->nFrames, c->nTraced, c->nComposited, c->nFallback, c->fogGateOff ? 1 : 0,
		c->res, c->tw, c->th );
	c->nFrames = c->nTraced = c->nComposited = c->nFallback = 0;
}
