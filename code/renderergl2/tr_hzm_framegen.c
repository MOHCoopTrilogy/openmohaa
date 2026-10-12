/*
===========================================================================
tr_hzm_framegen.c - HZM frame generation prototype (r_hzmFrameGen, default 0)

The game is CPU-bound: the main thread needs a fixed time per real frame however idle the GPU is. This shows MORE
frames than the main thread makes:

  r_hzmFrameGen 1   a present thread shows, for every real frame N+1, first a GENERATED picture for the camera
                    halfway between N and N+1, and the real picture half a frame later (interpolation: half a real
                    frame of added delay, but the camera is never shown anywhere it was not - extrapolating a mouse
                    overshoots on every stop and reversal)
  r_hzmFrameGen 2   the present thread alone (real frames only) - isolates what moving the swap off the main thread buys
  r_hzmFrameGen 3   single-threaded picture test: fg_capture 0 / fg_capture 1 keep two frames, r_hzmFrameGenDebug
                    1 = show the frame generated from them for the CURRENT camera, 2 = for the camera halfway between
                    them, 3 / 4 = the same with every pixel tinted by where it came from

How a picture is generated. The CAMERA is reprojected for everything that stands still: every output pixel searches
the source frame's depth buffer for the surface that lands on it (a few fixed-point steps), in the newer frame first
and in the older one where that fails (screen edge, something uncovered). Two kinds of pixel are never warped and
are taken from the newer real frame as they are: the first-person weapon (drawn with glDepthRange 0..0.3, so its
depth gives it away) and everything 2D (any pixel the HUD pass changed after the 3D picture was complete).

What moves by itself (r_hzmFrameGenVectors 1, the default; 0 = the first version, where it kept the newer frame's
place and so stood half a real frame ahead of the world in every generated picture - "characters vibrate"): the
main thread draws every numbered entity's opaque surfaces twice more per frame, see "motion vectors" below -
  M  where it is in the frame, with how far each point moved in the world since the frame before (the entity's
     placement then and now, and for a skinned model each vertex then and now), so the search above knows such a
     pixel is not where it will be in the generated picture;
  C  where it is HALFWAY, seen from the halfway camera - the generated picture's own view of it, thin parts
     included - with where each of those pixels is in the newer frame. The generator takes them from there (or, if
     the newer frame hides that point, from as far the other way in the older frame), unless something that stands
     still is nearer. What a mover uncovers comes from the other frame, or from the background beside it.
Not moved (they keep the newer frame's place): blended surfaces and sprites - smoke, fire, rain and snow, beams,
muzzle flashes, decals, shadows on the ground - and entities without a number (client-side effect models).

A translucent box over the whole picture (damage flash, fade, tint) is a uniform change, final = scene * K + B; the
generated picture gets the same K and B.

A frame is not generated at all - the real one is simply shown - when there was no world scene (menu, loading screen),
when a 2D quad covering a tenth of the screen or more was drawn over the scene (menu, scope, an opaque fade) or
when the camera jumped. The whole feature stays off in stereo and on the legacy r_ext_framebuffer_multisample path at
render scale 1 (there the HUD is drawn into multisample renderbuffers). The display-split MSAA path and render scale
are handled: the scene depth is then smaller than the display, and is sampled by position.

Measured (2026-10-11, one PC only: NVIDIA 16 GB, windowed 1280x720, MSAA 8x, vsync off): Omaha 44-47 real frames a second
are shown as 87, intervals 11.5 ms +- 0.9; the main thread's frame time does not change by more than the map's own
drift (the swap was 0.3 ms of it here). Costs: four extra sets of display-sized textures (with vectors two more
half-float ones per set), three copies per real frame, and half a real frame of delay on what is shown.

Commands: fg_stats [reset] (present-time record), fg_capture 0|1 (mode 3), fg_record <n> (the next n pictures as the
present thread shows them -> screenshots/fgrec_*.tga). r_hzmFrameGenDebug 3 = every generated pixel tinted by where
it came from (none = newer frame, cyan = a mover, green = older frame, light blue = a mover from the older frame,
orange = background beside a mover, red / yellow = nothing fitted), 5 = the newer frame's vectors.

Threading. The present thread owns a second GL context that shares textures with the main one, and is the only
thread that touches the window's back buffer and swaps. The main thread copies each finished frame into one of four
texture sets, plants a fence and publishes it; it never waits for the swap. Everything GL the present thread does is
raw qgl* on its own context - the renderer's state cache (glState) belongs to the main thread alone.
The renderer DLL is unloaded on every vid_restart: R_FG_Shutdown joins the thread (RE_Shutdown calls it first).
===========================================================================
*/
#include "tr_local.h"
#include "tr_dsa.h"
#include "tr_hzm_framegen.h"

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

// tr_fbo.c (not in tr_fbo.h)
FBO_t   *FBO_Create( const char *name, int width, int height );
void     FBO_CreateBuffer( FBO_t *fbo, int format, int index, int multisample );
qboolean R_CheckFBO( const FBO_t *fbo );

#define FG_SETS			4		// held by the present thread: 2, waiting for it: 1, being written: 1
#define FG_GUN_DEPTH	0.30001f	// RF_DEPTHHACK surfaces are drawn with glDepthRange( 0, 0.3 )
#define FG_BIG2D		0.10f		// one 2D quad covering this much of the screen after the scene = an overlay is up (the in-game menu panel is 0.21, HUD pieces are under 0.03)
#define FG_TS			16384

#define FGK_REAL	0
#define FGK_GEN		1

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE	0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT		0x00000001
#endif

typedef void *( APIENTRY *fgFenceSync_t )( GLenum condition, GLbitfield flags );
typedef GLenum ( APIENTRY *fgClientWaitSync_t )( void *sync, GLbitfield flags, unsigned long long timeout );
typedef void ( APIENTRY *fgDeleteSync_t )( void *sync );

typedef struct {
	image_t		*S, *F, *D;			// scene before the HUD, final picture, scene depth
	image_t		*M;					// motion: world displacement since the frame before, a = a surface that moves by itself
	image_t		*C;					// the movers drawn where they are HALFWAY to this frame: where each pixel of them is in this frame
	FBO_t		*fboS, *fboF, *fboM, *fboC;
} fgSet_t;

typedef struct {
	int			set;
	int			seq;
	qboolean	scene;				// S, D and the matrices are valid
	qboolean	vec;				// M and C are valid: something moved by itself in this frame
	qboolean	gun;				// C also holds the first-person weapon and hands where they are halfway (a < 0)
	qboolean	flash;				// a muzzle flash was drawn in this frame (a blended depth-hack effect)
	int			midPrev;			// C was drawn for the camera halfway between the frame with this number and this one
	qboolean	bypass;				// a big 2D overlay was drawn over the scene
	double		V[16], P[16];		// world -> eye, eye -> clip (column-major)
	float		fadeK, fadeB[3];	// full-screen translucent boxes drawn over the scene: final = scene * K + B
	unsigned int	layerTex;		// a full-screen cosmetic layer (the aiming vignette): its texture, drawn again over
	float		layerCol[4];		// the generated picture with this colour ...
	float		layerST[4];			// ... and these texture coordinates (s1 t1 s2 t2, top left to bottom right)
	qboolean	layerSeen;			// a cosmetic layer was drawn this frame (redrawn exactly or not)
	void		*sync;
	Uint64		tPub;
} fgFrame_t;

typedef struct {
	// ---- main thread only
	int				mode;			// what is running now (the cvar is what is wanted)
	qboolean		resTried, resOK;
	fgSet_t			sets[FG_SETS];
	int				w;				// the set this frame is written into
	fgFrame_t		build;
	qboolean		latched;
	double			latV[16], latP[16];
	float			orthoW, orthoH, vpShare, max2D;
	char			max2DName[64];
	int				seq;
	int				lastPubSet;
	fgFrame_t		slot[2];		// mode 3
	qboolean		slotValid[2];
	fgFenceSync_t		FenceSync;
	fgClientWaitSync_t	ClientWaitSync;
	fgDeleteSync_t		DeleteSync;
	int				swapInterval;
	int				width, height;

	// ---- main thread, motion vectors (the renderer's own context)
	qboolean		vecOK;			// its programs and textures exist
	GLuint			velProg;
	GLint			uVelMidVP, uVelCurVP, uVelSize, uVelPass, uVelSunVP;
	GLint			uWideShadow, uWideInvVP, uWideSunVP, uWideSunDir, uWideL;
	qboolean		velShadow;		// this frame: movers are also drawn as the sun sees them
	float			sunVP[16], invCurVP[16], sunDir[3];
	qboolean		vecTried;
	qboolean		lastBypass;		// the last frame handed over had an overlay up
	image_t			*MC0;			// scratch, twice the scene's width: M as rasterised | C, with a depth buffer for C
	FBO_t			*fboMC0;
	float			midVP[16], curVP[16];
	double			lastV[16], lastP[16];	// the last frame handed over with a scene
	int				lastSeq;
	GLint			uVelMVP, uVelModel, uVelPrevModel, uVelRigid;
	GLuint			wideProg;
	FBO_t			*velOldFbo;
	qboolean		velBound;		// the scratch target is bound and cleared
	qboolean		velBatchMoves;	// something in the batch being collected moved
	int				velDraws, velSkipped, velSurfs;
	float			velModel[16], velPrevModel[16];
	qboolean		velNewEntity;	// the two matrices above are not in the program yet
	// the first-person weapon (r_hzmFrameGenGun): drawn into C only, positions in VIEW space
	double			latWP[16], lastWP[16];	// the projection the depth-hack surfaces are drawn with, this frame / the last
	float			gunMidP[16], gunCurP[16];
	qboolean		gunOK;			// this frame: the weapon may be drawn (switch on, its projection did not step)
	qboolean		velBatchGun;	// the batch being collected is the weapon's
	qboolean		velBatchHold;	// ... a part of it that is held at the newer frame's place (no vectors)
	qboolean		gunBad;			// a part of the weapon could not be drawn: the whole weapon stays unwarped this frame
	int				gunDraws, gunOrdinal;
	int				gunDump;		// fg_gundump: passes still to be printed
	GLint			uVelGunF;
	int				gunHeld;

	// ---- the frame-gen context (whoever has it current)
	void			*ctx;
	qboolean		ctxInit;
	GLuint			prog, vao;
	GLint			uMode, uTint, uRes, uGun, uCurFromOut, uOutFromCur, uPrvFromOut, uOutFromPrv, uFadeK, uFadeB, uHudThr;
	GLint			uOutVP, uCurInvW, uPrvInvW, uCurK, uPrvK, uUseC, uEdge, uLayer, uLayerCol, uLayerST, uGunVec, uGunFlash, uOccl, uFill;

	// ---- shared, under mx
	SDL_Thread		*thread;
	SDL_mutex		*mx;
	SDL_cond		*cv;
	int				quit;
	int				threadState;	// 0 starting, 1 running, -1 failed
	fgFrame_t		pending;
	qboolean		hasPending;
	int				heldPrev, heldCur;
} fg_t;

static fg_t fg;

// present-time statistics: one writer (the present thread, or the main thread when the feature is off), read by fg_stats
static struct {
	Uint64			t[FG_TS];
	byte			kind[FG_TS];
	volatile int	n;
	volatile int	nReal, nGen, nNoScene, nBypass, nJump, nDropped, nEarlySkip, nGLErr, nSyncTimeout;
	volatile double	syncWaitMs, genMs;
	volatile int	nSyncWait, nGenTimed, nFade;
	volatile double	genDelayMs, realDelayMs;	// from the main thread handing a frame over to its pictures being shown
	volatile int	nGenDelay, nRealDelay;
	volatile int	nVec, nVelPass, velDraws, velSkipped, velBrush, nLayer, nLayerExact;	// main thread: frames with vectors, passes run, batches drawn / left out
	volatile int	gunHeld, nFlash;
	volatile int	nGun, nGunBad, nGunProj, gunDraws, nFovStep;	// frames with weapon vectors / without (a part not drawn, its projection stepped), weapon batches, fov steps
	char			lastOverlay[96];
	// the last camera steps worth a look (present thread writes, fg_stats reads): units moved, cosine of the turn,
	// field-of-view factor, 1 = taken as a cut
	struct { float d, dot, fov; int seqA, seqB, cut; } step[8];
	volatile int	nStep;
} fgs;

cvar_t *r_hzmFrameGen;
cvar_t *r_hzmFrameGenDebug;
cvar_t *r_hzmFrameGenStats;
cvar_t *r_hzmFrameGenVectors;
cvar_t *r_hzmFrameGenShadows;
cvar_t *r_hzmFrameGenEdge;
cvar_t *r_hzmFrameGenLayers;
cvar_t *r_hzmFrameGenGun;
cvar_t *r_hzmFrameGenOccl;
cvar_t *r_hzmFrameGenFill;
cvar_t *r_hzmFrameGenGunMax;
cvar_t *r_hzmFrameGenFovStep;
int r_fgVelPass;		// the motion-vector pass is collecting surfaces: RB_EndSurface hands them to R_FG_VelocityDraw

static const char *fg_vs =
	"out vec2 vUV;\n"
	"void main() {\n"
	"	vec2 p = vec2( float( ( gl_VertexID << 1 ) & 2 ), float( gl_VertexID & 2 ) );\n"
	"	vUV = p;\n"
	"	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
	"}\n";

static const char *fg_fs =
	"uniform sampler2D uCurS, uCurD, uPrvS, uPrvD, uHudF, uHudS, uHudD, uCurM, uPrvM, uCurC;\n"
	"uniform float uUseC;\n"
	"uniform float uEdge;\n"
	"uniform float uGunVec;\n"		// the newer frame's C also holds the first-person weapon where it is halfway (a < 0)
	"uniform float uOccl;\n"
	"uniform float uFill;\n"
	"uniform float uGunFlash;\n"		// a muzzle flash is in one of the two frames
	"uniform sampler2D uLayerT;\n"
	"uniform float uLayer;\n"
	"uniform vec4 uLayerCol, uLayerST;\n"
	"uniform mat4 uCurFromOut, uOutFromCur, uPrvFromOut, uOutFromPrv, uOutVP;\n"
	"uniform vec4 uCurInvW, uPrvInvW;\n"		// the row of ( view-projection )^-1 that gives a source pixel's world w
	"uniform float uCurK, uPrvK;\n"			// the share of a frame's own displacement to take back (0 = no vectors)
	"uniform vec2 uRes;\n"
	"uniform int uMode;\n"		// 0 = copy the final picture, 1 = generate
	"uniform int uTint;\n"
	"uniform float uGun;\n"
	"uniform float uFadeK;\n"
	"uniform vec4 uFadeB;\n"
	"uniform float uHudThr;\n"
	"in vec2 vUV;\n"
	"out vec4 oColor;\n"
	"ivec2 texel( sampler2D t, vec2 uv ) {\n"
	"	ivec2 sz = textureSize( t, 0 );\n"
	"	return clamp( ivec2( uv * vec2( sz ) ), ivec2( 0 ), sz - ivec2( 1 ) );\n"
	"}\n"
	"float depthAt( sampler2D t, vec2 uv ) {\n"
	"	return texelFetch( t, texel( t, uv ), 0 ).r;\n"
	"}\n"
	// Catmull-Rom through 9 bilinear fetches: a warped pixel almost never lands on a texel centre
	"vec3 sharp( sampler2D t, vec2 uv ) {\n"
	"	vec2 sz = vec2( textureSize( t, 0 ) );\n"
	"	vec2 sp = uv * sz;\n"
	"	vec2 p1 = floor( sp - 0.5 ) + 0.5;\n"
	"	vec2 f = sp - p1;\n"
	"	vec2 w0 = f * ( -0.5 + f * ( 1.0 - 0.5 * f ) );\n"
	"	vec2 w1 = 1.0 + f * f * ( -2.5 + 1.5 * f );\n"
	"	vec2 w2 = f * ( 0.5 + f * ( 2.0 - 1.5 * f ) );\n"
	"	vec2 w3 = f * f * ( -0.5 + 0.5 * f );\n"
	"	vec2 w12 = w1 + w2;\n"
	"	vec2 t0 = ( p1 - 1.0 ) / sz;\n"
	"	vec2 t3 = ( p1 + 2.0 ) / sz;\n"
	"	vec2 t12 = ( p1 + w2 / w12 ) / sz;\n"
	"	vec3 c = textureLod( t, vec2( t0.x, t0.y ), 0.0 ).rgb * w0.x * w0.y\n"
	"		+ textureLod( t, vec2( t12.x, t0.y ), 0.0 ).rgb * w12.x * w0.y\n"
	"		+ textureLod( t, vec2( t3.x, t0.y ), 0.0 ).rgb * w3.x * w0.y\n"
	"		+ textureLod( t, vec2( t0.x, t12.y ), 0.0 ).rgb * w0.x * w12.y\n"
	"		+ textureLod( t, vec2( t12.x, t12.y ), 0.0 ).rgb * w12.x * w12.y\n"
	"		+ textureLod( t, vec2( t3.x, t12.y ), 0.0 ).rgb * w3.x * w12.y\n"
	"		+ textureLod( t, vec2( t0.x, t3.y ), 0.0 ).rgb * w0.x * w3.y\n"
	"		+ textureLod( t, vec2( t12.x, t3.y ), 0.0 ).rgb * w12.x * w3.y\n"
	"		+ textureLod( t, vec2( t3.x, t3.y ), 0.0 ).rgb * w3.x * w3.y;\n"
	"	return clamp( c, 0.0, 1.0 );\n"
	"}\n"
	"vec2 proj( mat4 m, vec2 uv, float d ) {\n"
	"	vec4 c = m * vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
	"	if ( c.w < 0.00001 ) return vec2( -10.0 );\n"
	"	return c.xy / c.w * 0.5 + 0.5;\n"
	"}\n"
	// where the surface at source position q (depth d) is in the OUTPUT picture: xy, and z = how far away it is there.
	// A surface that moves by itself is first taken k of its own displacement back (k < 0: forward).
	"vec3 land( sampler2D mot, mat4 outFromSrc, vec4 invW, float k, vec2 q, float d ) {\n"
	"	vec4 c = vec4( q * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
	"	vec4 o = outFromSrc * c;\n"
	"	if ( k != 0.0 ) {\n"
	"		vec4 m = texelFetch( mot, texel( mot, q ), 0 );\n"
	"		if ( m.a > 0.5 ) o -= ( k * dot( invW, c ) ) * ( uOutVP * vec4( m.rgb, 0.0 ) );\n"
	"	}\n"
	"	if ( o.w < 0.00001 ) return vec3( -10.0, -10.0, 0.0 );\n"
	// o is the output clip position of the WORLD point over its homogeneous w (= 1 / the source frame's clip w), so
	// o.w alone is the ratio of the two frames' distances - about 1 everywhere. The distance in the output picture
	// is o.w over that w. (Until 2026-10-12 the ratio itself was returned and compared with a mover's distance in
	// game units: every mover pixel over a still background counted as hidden behind it - thin, fast things, a
	// rifle held up, a trailing boot, were cut out of the generated pictures, and every mover lost its edges.)
	"	return vec3( o.xy / o.w * 0.5 + 0.5, uOccl != 0.0 ? o.w / max( abs( dot( invW, c ) ), 1e-20 ) : o.w );\n"
	"}\n"
	// xy = where in the source picture the surface seen at output pixel p is (searched from q), z = how far off that
	// answer is (pixels), w = how far away the surface is in the output picture
	"vec4 find( sampler2D dep, sampler2D mot, mat4 outFromSrc, vec4 invW, float k, vec2 p, vec2 q ) {\n"
	"	vec3 l = vec3( 0.0 );\n"
	"	for ( int i = 0; i < 5; i++ ) {\n"
	"		if ( any( lessThan( q, vec2( 0.0 ) ) ) || any( greaterThan( q, vec2( 1.0 ) ) ) ) return vec4( q, 10000.0, 0.0 );\n"
	"		float d = depthAt( dep, q );\n"
	"		if ( d < uGun ) return vec4( q, 10000.0, 0.0 );\n"
	"		l = land( mot, outFromSrc, invW, k, q, d );\n"
	"		if ( i < 4 ) q += p - l.xy;\n"
	"	}\n"
	"	return vec4( q, length( ( l.xy - p ) * uRes ), l.z );\n"
	"}\n"
	"bool moving( sampler2D mot, vec2 q ) {\n"
	"	return texelFetch( mot, texel( mot, q ), 0 ).a > 0.5;\n"
	"}\n"
	// does the frame show the first-person weapon at q, or within a texel of it (its outline)?
	"bool gunAt( sampler2D dep, vec2 q ) {\n"
	"	if ( any( lessThan( q, vec2( 0.0 ) ) ) || any( greaterThan( q, vec2( 1.0 ) ) ) ) return false;\n"
	"	ivec2 c = texel( dep, q );\n"
	"	ivec2 m = textureSize( dep, 0 ) - ivec2( 1 );\n"
	"	for ( int i = 0; i < 9; i++ ) {\n"
	"		if ( texelFetch( dep, clamp( c + ivec2( i % 3 - 1, i / 3 - 1 ), ivec2( 0 ), m ), 0 ).r < uGun ) return true;\n"
	"	}\n"
	"	return false;\n"
	"}\n"
	"void main() {\n"
	"	ivec2 ip = ivec2( gl_FragCoord.xy );\n"
	"	vec3 f = texelFetch( uHudF, ip, 0 ).rgb;\n"
	"	if ( uMode == 0 ) { oColor = vec4( f, 1.0 ); return; }\n"
	"	vec3 s = texelFetch( uHudS, ip, 0 ).rgb;\n"
	// a full-screen cosmetic layer of the mod (the aiming vignette) is neither HUD nor scene: it is drawn again,
	// exactly, over the generated picture - so it is not warped, and the picture under it is still generated
	"	vec4 lay = vec4( 0.0 );\n"
	"	if ( uLayer != 0.0 ) {\n"
	"		vec4 lt = texture( uLayerT, mix( uLayerST.xy, uLayerST.zw, vec2( vUV.x, 1.0 - vUV.y ) ) ) * uLayerCol;\n"
	"		lay = vec4( lt.rgb * lt.a, lt.a );\n"
	"	}\n"
	"	vec3 df = abs( f - ( clamp( s * uFadeK + uFadeB.rgb, 0.0, 1.0 ) * ( 1.0 - lay.a ) + lay.rgb ) );\n"
	"	bool hud = max( df.r, max( df.g, df.b ) ) > uHudThr;\n"
	"	bool gunHere = depthAt( uHudD, vUV ) < uGun;\n"
	"	bool gun = gunHere;\n"
	"	int gsrc = 0;\n"
	"	vec2 gq = vec2( 0.0 );\n"
	"	bool gvec = false;\n"
	"	vec2 grg = vec2( 0.0 );\n"
	"	if ( uGunVec != 0.0 && !hud ) {\n"
	// The first-person weapon and hands were drawn into C where they are HALFWAY between their two poses (in view
	// space: the camera's own turn is not in it). A pixel they cover at this moment takes the weapon's colour from
	// where that point of it is in the newer frame. Any other pixel shows the world, also where the newer frame
	// has the weapon (it is not there yet at this moment).
	"		ivec2 gc = texel( uCurC, vUV );\n"
	"		vec4 gm = texelFetch( uCurC, gc, 0 );\n"
	"		bool gcov = gm.a < 0.0;\n"
	"		if ( !gcov ) {\n"
	// C has one sample a pixel; the real frames' weapon reaches up to a pixel further out (their colour there is the
	// blend of the multisamples). A pixel beside the weapon in C is its outline: it goes where its neighbour goes
	// and takes the real frame's blended outline colour from there.
	"			ivec2 gmax = textureSize( uCurC, 0 ) - ivec2( 1 );\n"
	"			for ( int i = 0; i < 9 && gm.a >= 0.0; i++ ) {\n"
	"				if ( i == 4 ) continue;\n"
	"				vec4 gn = texelFetch( uCurC, clamp( gc + ivec2( i % 3 - 1, i / 3 - 1 ), ivec2( 0 ), gmax ), 0 );\n"
	"				if ( gn.a < 0.0 ) gm = gn;\n"
	"			}\n"
	"		}\n"
	"		gun = false;\n"
	"		if ( gm.a < 0.0 ) {\n"
	"			vec2 gd = gm.rg * uRes;\n"
	"			vec2 q = vUV + gm.rg;\n"
	"			bool qin = all( greaterThanEqual( q, vec2( 0.0 ) ) ) && all( lessThanEqual( q, vec2( 1.0 ) ) );\n"
	"			gvec = true;\n"
	"			grg = gm.rg;\n"
	"			if ( dot( gd, gd ) < 0.0009 ) {\n"
	// it stands still on the screen here: the real picture's pixel, exactly (its outline's blend included)
	"				gun = gunHere || !gcov;\n"
	"			} else if ( qin && gunAt( uCurD, q ) ) {\n"
	"				gsrc = 1; gq = q;\n"
	"			} else if ( !qin && gcov && gunAt( uPrvD, vUV - gm.rg ) ) {\n"
	// that point of the weapon has left the screen in the newer frame (the weapon being lowered): the older one has it
	"				gsrc = 2; gq = vUV - gm.rg;\n"
	"			} else if ( gcov && gunHere ) {\n"
	"				gun = true;\n"			// a cut-out part: as before
	"			}\n"
	"		}\n"
	"	}\n"
	"	vec3 c;\n"
	"	vec3 tint = vec3( 1.0 );\n"
	"	if ( hud || gun ) {\n"
	"		c = f;\n"
	"		tint = hud ? vec3( 1.0, 0.45, 1.0 ) : vec3( 0.45, 0.45, 1.0 );\n"
	"	} else if ( gsrc == 1 ) {\n"
	"		c = sharp( uCurS, gq );\n"
	"		tint = vec3( 0.7, 0.35, 1.0 );\n"
	"	} else if ( gsrc == 2 ) {\n"
	"		c = sharp( uPrvS, gq );\n"
	"		tint = vec3( 0.35, 0.2, 0.7 );\n"
	"	} else {\n"
	"		float d0 = depthAt( uCurD, vUV );\n"
	"		if ( d0 < uGun ) d0 = 1.0;\n"
	"		vec2 q0 = proj( uCurFromOut, vUV, d0 );\n"
	"		vec4 a = find( uCurD, uCurM, uOutFromCur, uCurInvW, uCurK, vUV, q0 );\n"
	// the weapon is in the way in the newer frame - or its outline is: that pixel's colour is part weapon, and the
	// weapon is somewhere else at this moment
	"		bool gfill = uGunVec != 0.0 && gunAt( uCurD, a.xy );\n"
	"		if ( gfill ) a.z = 10000.0;\n"
	"		bool dyn = false;\n"
	"		bool lost = false;\n"
	"		if ( uCurK != 0.0 && a.z <= 1.0 ) dyn = moving( uCurM, a.xy );\n"
	"		if ( uUseC != 0.0 ) {\n"
	// a mover covers this pixel at this moment (it was drawn halfway between its two poses): C says where that
	// point of it is in the newer frame. Take it from there if the newer frame shows it there (same distance) and
	// nothing that stands still is in front of it at this moment.
	"			vec4 cm = texelFetch( uCurC, texel( uCurC, vUV ), 0 );\n"
	"			if ( cm.a > 0.0 && !( a.z <= 1.0 && !dyn && a.w < cm.a * 0.998 ) ) {\n"
	"				vec2 q = vUV + cm.rg;\n"
	"				bool in01 = all( greaterThanEqual( q, vec2( 0.0 ) ) ) && all( lessThanEqual( q, vec2( 1.0 ) ) );\n"
	"				float nd = in01 ? 1.0 - depthAt( uCurD, q ) : -1.0;\n"
	"				bool vis = in01 && abs( nd - cm.b ) <= 0.012 * cm.b + 0.000002;\n"
	"				if ( in01 && !vis && uEdge != 0.0 ) {\n"
	// the mover's outline: the point lies within a pixel of its edge in the newer frame, and the depth texel
	// under it is the background's. One of the four texels around it is the mover's: it is there, take it
	// (the colour fetch blends the edge as the real frames do). Without this every generated picture lost a
	// one-pixel ring off each mover.
	"					vec2 px = 1.0 / vec2( textureSize( uCurD, 0 ) );\n"
	"					for ( int i = 0; i < 4 && !vis; i++ ) {\n"
	"						vec2 o = ( i < 2 ? vec2( float( i * 2 - 1 ), 0.0 ) : vec2( 0.0, float( i * 2 - 5 ) ) ) * px;\n"
	"						vis = abs( 1.0 - depthAt( uCurD, q + o ) - cm.b ) <= 0.012 * cm.b + 0.000002;\n"
	"					}\n"
	"				}\n"
	"				if ( vis ) {\n"
	"					a = vec4( q, 0.0, cm.a );\n"
	"					dyn = true;\n"
	"				} else {\n"
	// hidden or off screen in the newer frame: the older one has it as far the other way
	"					vec2 qp = vUV - cm.rg;\n"
	"					if ( uPrvK != 0.0 && all( greaterThanEqual( qp, vec2( 0.0 ) ) ) && all( lessThanEqual( qp, vec2( 1.0 ) ) ) && moving( uPrvM, qp ) ) {\n"
	"						a = vec4( qp, 0.0, cm.a );\n"
	"						lost = true;\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"		if ( lost ) {\n"
	"			c = sharp( uPrvS, a.xy );\n"
	"			tint = vec3( 0.4, 0.7, 1.0 );\n"
	"		} else\n"
	"		if ( a.z <= 1.0 ) {\n"
	"			c = sharp( uCurS, a.xy );\n"
	"			if ( dyn ) tint = vec3( 0.4, 1.0, 1.0 );\n"
	"		} else {\n"
	"			float d1 = depthAt( uPrvD, vUV );\n"
	"			if ( d1 < uGun ) d1 = 1.0;\n"
	"			vec4 b = find( uPrvD, uPrvM, uOutFromPrv, uPrvInvW, uPrvK, vUV, proj( uPrvFromOut, vUV, d1 ) );\n"
	"			if ( uGunVec != 0.0 && b.z <= 1.0 && gunAt( uPrvD, b.xy ) ) b.z = 10000.0;\n"
	"			bool done = false;\n"
	"			vec2 gw = vec2( -1.0 );\n"
	"			if ( gfill ) {\n"
	// The world behind the weapon's place in the newer frame. If the older frame does not show it either: the world
	// beside the weapon in the newer frame, stepping out of the weapon along its own motion (the nearest part of
	// it in C says which way), else the nearest world pixel - never the weapon's own colour.
	"				vec2 px = 1.0 / uRes;\n"
	"				if ( !gvec ) {\n"
	"					for ( int r = 1; r <= 8 && !gvec; r++ ) {\n"
	"						for ( int k = 0; k < 8 && !gvec; k++ ) {\n"
	"							float an = float( k ) * 0.7853982;\n"
	"							vec2 qq = vUV + vec2( cos( an ), sin( an ) ) * ( float( r ) * 3.0 ) * px;\n"
	"							vec4 gn = texelFetch( uCurC, texel( uCurC, qq ), 0 );\n"
	"							if ( gn.a < 0.0 ) { gvec = true; grg = gn.rg; }\n"
	"						}\n"
	"					}\n"
	"				}\n"
	"				if ( gvec ) {\n"
	"					vec2 qq = vUV + grg;\n"
	"					if ( all( greaterThanEqual( qq, vec2( 0.0 ) ) ) && all( lessThanEqual( qq, vec2( 1.0 ) ) ) && !gunAt( uCurD, qq ) ) gw = qq;\n"
	"				}\n"
	"				for ( int r = 1; r <= 8 && gw.x < 0.0; r++ ) {\n"
	"					for ( int k = 0; k < 8 && gw.x < 0.0; k++ ) {\n"
	"						float an = float( k ) * 0.7853982;\n"
	"						vec2 qq = vUV + vec2( cos( an ), sin( an ) ) * ( float( r ) * 3.0 ) * px;\n"
	"						if ( all( greaterThanEqual( qq, vec2( 0.0 ) ) ) && all( lessThanEqual( qq, vec2( 1.0 ) ) ) && !gunAt( uCurD, qq ) ) gw = qq;\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"			if ( b.z <= 1.0 ) {\n"
	"				c = sharp( uPrvS, b.xy );\n"
	"				tint = vec3( 0.45, 1.0, 0.45 );\n"
	"				done = true;\n"
	"				if ( gw.x >= 0.0 && uGunFlash != 0.0 ) {\n"
	// the older frame shows the world here. But a muzzle flash (and its smoke) is in one of the two frames and not
	// in the other: the older frame's colour would stand beside the weapon as a white or a dark rim - take the
	// newer frame's world beside the weapon instead
	"					c = texture( uCurS, gw ).rgb;\n"
	"					tint = vec3( 1.0, 0.5, 0.8 );\n"
	"				}\n"
	"			} else if ( gw.x >= 0.0 ) {\n"
	"				c = texture( uCurS, gw ).rgb;\n"
	"				tint = vec3( 1.0, 0.5, 0.8 );\n"
	"				done = true;\n"
	"			} else if ( uCurK != 0.0 && all( greaterThanEqual( q0, vec2( 0.0 ) ) ) && all( lessThanEqual( q0, vec2( 1.0 ) ) ) && moving( uCurM, q0 ) ) {\n"
	// uncovered by something that moved on, and hidden in the older frame as well: the background beside the mover
	// in the newer frame, stepping out of it along its own motion (ahead first, then behind)
	// First the nearest pixel around that is not a mover's body (ground in a mover's shadow carries a vector too, with
	// a = 0.75: it is still ground). Stepping along the motion alone went 30 pixels through a leg and came out in
	// the sun: light patches in the shadow under a trailing boot.
	"				if ( uFill != 0.0 ) {\n"
	"					vec2 px = 1.0 / uRes;\n"
	"					for ( int r = 1; r <= 12 && !done; r++ ) {\n"
	"						for ( int k = 0; k < 8 && !done; k++ ) {\n"
	"							float an = float( k ) * 0.7853982;\n"
	"							vec2 qq = q0 + vec2( cos( an ), sin( an ) ) * ( float( r ) * 3.0 ) * px;\n"
	"							if ( all( greaterThanEqual( qq, vec2( 0.0 ) ) ) && all( lessThanEqual( qq, vec2( 1.0 ) ) )\n"
	"								&& texelFetch( uCurM, texel( uCurM, qq ), 0 ).a < 0.9 && depthAt( uCurD, qq ) >= uGun ) {\n"
	"								c = texture( uCurS, qq ).rgb;\n"
	"								tint = vec3( 1.0, 0.6, 0.2 );\n"
	"								done = true;\n"
	"							}\n"
	"						}\n"
	"					}\n"
	"				}\n"
	"				vec3 l0 = land( uCurM, uOutFromCur, uCurInvW, uCurK, q0, depthAt( uCurD, q0 ) );\n"
	"				vec2 st = vUV - l0.xy;\n"
	"				for ( int n = 0; n < 6 && !done; n++ ) {\n"
	"					float fn = float( n / 2 + 1 ) * ( ( n & 1 ) == 0 ? 1.25 : -1.25 );\n"
	"					vec2 qq = q0 + st * fn;\n"
	"					if ( all( greaterThanEqual( qq, vec2( 0.0 ) ) ) && all( lessThanEqual( qq, vec2( 1.0 ) ) )\n"
	"						&& !moving( uCurM, qq ) && depthAt( uCurD, qq ) >= uGun ) {\n"
	"						c = texture( uCurS, qq ).rgb;\n"
	"						tint = vec3( 1.0, 0.6, 0.2 );\n"
	"						done = true;\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"			if ( !done ) {\n"
	"				if ( min( a.z, b.z ) < 64.0 ) {\n"
	"					c = a.z <= b.z ? texture( uCurS, a.xy ).rgb : texture( uPrvS, b.xy ).rgb;\n"
	"					tint = vec3( 1.0, 0.3, 0.3 );\n"
	"				} else {\n"
	"					c = s;\n"
	"					tint = vec3( 1.0, 1.0, 0.3 );\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	if ( !( hud || gun ) ) c = clamp( c * uFadeK + uFadeB.rgb, 0.0, 1.0 ) * ( 1.0 - lay.a ) + lay.rgb;\n"
	"	if ( uTint == 3 || uTint == 4 ) {\n"
	// r_hzmFrameGenDebug 6: C - the movers where they are halfway (red / green = how far to their place in the newer
	// frame, blue = a world mover, no blue = the first-person weapon). 7: the same, but only the pixels of C whose
	// place in the newer frame does not show that mover (white = the depth there is nearer, black = further)
	"		vec4 m = texelFetch( uCurC, texel( uCurC, vUV ), 0 );\n"
	"		if ( m.a == 0.0 ) c = uTint == 3 ? c * 0.25 : c;\n"
	"		else if ( uTint == 3 ) c = clamp( vec3( 0.5 + m.r * uRes.x * 0.03, 0.5 + m.g * uRes.y * 0.03, m.a > 0.0 ? 1.0 : 0.0 ), 0.0, 1.0 );\n"
	"		else if ( m.a > 0.0 ) {\n"
	"			float nd = 1.0 - depthAt( uCurD, vUV + m.rg );\n"
	"			c = abs( nd - m.b ) <= 0.012 * m.b + 0.000002 ? c : ( nd > m.b ? vec3( 1.0 ) : vec3( 0.0, 0.0, 0.0 ) );\n"
	"			if ( abs( nd - m.b ) > 0.012 * m.b + 0.000002 ) c = mix( c, vec3( 1.0, 0.0, 0.0 ), 0.5 );\n"
	"		}\n"
	"	}\n"
	"	if ( uTint == 2 ) {\n"
	// r_hzmFrameGenDebug 5: the newer frame's motion vectors (displacement in world units, grey = none, dark = not a mover)
	"		vec4 m = texelFetch( uCurM, texel( uCurM, vUV ), 0 );\n"
	"		c = m.a > 0.5 ? clamp( vec3( 0.5 ) + m.rgb * 0.1, 0.0, 1.0 ) : c * 0.25;\n"
	"	}\n"
	"	oColor = vec4( uTint == 1 ? c * tint : c, 1.0 );\n"
	"}\n";

// ---- the motion-vector pass (main context). Every surface of an entity that has a number is drawn once more, with
// where each vertex was a frame ago: the world-space displacement goes into M wherever the surface is the visible one.
static const char *fg_vel_vs =
	"in vec3 attr_Position;\n"
	"in vec2 attr_TexCoord0;\n"		// model-space displacement of the vertex since the frame before: xy ...
	"in vec2 attr_TexCoord1;\n"		// ... and z (skinned models; zero for rigid ones)
	"uniform mat4 uMVP, uModel, uPrevModel, uMidVP, uCurVP, uSunVP;\n"
	"uniform float uSkinned;\n"		// 0 = the two attributes above are not displacements (a rigid brush model)
	"uniform int uPass;\n"			// 0 = M: where it is in the frame, 1 = C: where it is halfway, 2 = L: seen from the sun
	"out vec3 vDelta;\n"
	"out vec4 vCur;\n"
	"void main() {\n"
	"	vec3 dl = vec3( attr_TexCoord0, attr_TexCoord1.x ) * uSkinned;\n"
	"	vec3 now = ( uModel * vec4( attr_Position, 1.0 ) ).xyz;\n"
	"	vec3 was = ( uPrevModel * vec4( attr_Position - dl, 1.0 ) ).xyz;\n"
	"	vDelta = now - was;\n"
	"	vCur = uCurVP * vec4( now, 1.0 );\n"
	"	if ( uPass == 0 ) gl_Position = uMVP * vec4( attr_Position, 1.0 );\n"
	"	else if ( uPass == 1 ) gl_Position = uMidVP * vec4( ( now + was ) * 0.5, 1.0 );\n"
	"	else gl_Position = uSunVP * vec4( now, 1.0 );\n"
	"}\n";

// Both passes go into one scratch target twice the width (M left, C right): switching framebuffers per batch cost
// more than everything else in the pass on a crowded map.
static const char *fg_vel_fs =
	"uniform sampler2D uDepth;\n"
	"uniform vec2 uSize;\n"
	"uniform int uPass;\n"
	"uniform float uGunF;\n"
	"in vec3 vDelta;\n"
	"in vec4 vCur;\n"
	"out vec4 oColor;\n"
	"void main() {\n"
	"	if ( uPass == 0 ) {\n"
	// only where this surface is what the scene shows: its depth is the scene's (with multisampling the scene depth
	// is the nearest sample of the pixel, so allow the slope of the surface across a pixel and a little distance)
	"		float sd = texelFetch( uDepth, ivec2( gl_FragCoord.xy ), 0 ).r;\n"
	"		float z = gl_FragCoord.z;\n"
	"		if ( abs( z - sd ) > fwidth( z ) + 0.002 * ( 1.0 - z ) + 0.0000005 ) discard;\n"
	"		oColor = vec4( vDelta, 1.0 );\n"
	"	} else if ( uPass == 2 ) {\n"
	// L: the movers as the sun sees them (nearest to the sun on top): the displacement, and a = 2 - depth towards
	// the ground. What lies behind one of them along the sun's rays is in its shadow.
	"		oColor = vec4( vDelta, 2.0 - gl_FragCoord.z );\n"
	"	} else {\n"
	// rg = how far from here the same point of the surface is in the newer frame (screen 0..1), b = 1 - its depth
	// there, a = how far away it is here
	"		if ( vCur.w < 0.00001 ) discard;\n"
	"		vec3 n = vCur.xyz / vCur.w * 0.5 + 0.5;\n"
	"		oColor = vec4( n.xy - ( gl_FragCoord.xy - vec2( uSize.x, 0.0 ) ) / uSize, 1.0 - n.z, 1.0 / gl_FragCoord.w );\n"
	// the first-person weapon: a negative distance marks it
	"		if ( uGunF != 0.0 ) oColor.a = -oColor.a;\n"
	"	}\n"
	"}\n";

static const char *fg_quad_vs =
	"in vec3 attr_Position;\n"
	"void main() {\n"
	"	gl_Position = vec4( attr_Position.xy, 0.0, 1.0 );\n"
	"}\n";

// A pixel on a mover's outline is partly its colour, and with multisampling carries its depth (the nearest sample),
// but the vector pass - one sample, the pixel centre - missed it: left alone it stays behind as a dotted outline at
// the newer frame's place. It takes the vector of a neighbour that is a mover at the same depth.
static const char *fg_wide_fs =
	"uniform sampler2D uM, uD;\n"
	"uniform float uShadow;\n"
	"uniform mat4 uInvVP, uSunVP;\n"
	"uniform vec3 uSunDir;\n"
	"uniform vec3 uL;\n"			// L's place in the scratch target: x, y, size
	"out vec4 oColor;\n"
	"void main() {\n"
	"	ivec2 c = ivec2( gl_FragCoord.xy );\n"
	"	vec4 m = texelFetch( uM, c, 0 );\n"
	"	if ( m.a > 0.5 ) { oColor = m; return; }\n"
	"	ivec2 sz = textureSize( uD, 0 ) - ivec2( 1 );\n"		// uM is the scratch target, twice as wide
	"	float d = texelFetch( uD, c, 0 ).r;\n"
	"	float bd = 0.004 * ( 1.0 - d ) + 0.000001;\n"
	"	vec4 best = vec4( 0.0 );\n"
	"	for ( int i = 0; i < 9; i++ ) {\n"
	"		if ( i == 4 ) continue;\n"
	"		ivec2 q = clamp( c + ivec2( i % 3 - 1, i / 3 - 1 ), ivec2( 0 ), sz );\n"
	"		vec4 n = texelFetch( uM, q, 0 );\n"
	"		if ( n.a > 0.5 ) {\n"
	"			float dd = abs( texelFetch( uD, q, 0 ).r - d );\n"
	"			if ( dd < bd ) { bd = dd; best = n; }\n"
	"		}\n"
	"	}\n"
	// A mover's shadow is part of the ground's colour, and the ground stands still: in the generated pictures the
	// shadow stayed at the newer frame's place while its owner was put halfway - it led him by half a real frame.
	// A pixel that has a mover between it and the sun is in that mover's shadow: it gets the mover's displacement
	// (slid along the sun's ray back onto level ground), so the generator moves the shadowed patch with him.
	"	if ( best.a < 0.5 && uShadow != 0.0 && d < 1.0 ) {\n"
	"		vec2 uv = ( vec2( c ) + 0.5 ) / vec2( sz + ivec2( 1 ) );\n"
	"		vec4 wp = uInvVP * vec4( uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0 );\n"
	"		vec3 l = ( uSunVP * vec4( wp.xyz / wp.w, 1.0 ) ).xyz * 0.5 + 0.5;\n"
	"		if ( all( greaterThan( l.xy, vec2( 0.0 ) ) ) && all( lessThan( l.xy, vec2( 1.0 ) ) ) ) {\n"
	"			vec4 sm = texelFetch( uM, ivec2( uL.xy + l.xy * uL.z ), 0 );\n"
	"			if ( sm.a > 0.5 && 2.0 - sm.a < l.z - 0.003 ) {\n"
	"				vec3 dv = sm.rgb;\n"
	"				if ( abs( uSunDir.z ) > 0.1 ) dv -= uSunDir * ( dv.z / uSunDir.z );\n"
	"				best = vec4( dv, 0.75 );\n"		// a = 0.75: ground in a mover's shadow, not a mover's body
	"			}\n"
	"		}\n"
	"	}\n"
	"	oColor = best;\n"
	"}\n";

/*
=====================================================================================================================
small matrix helpers (double, column-major like GL)
=====================================================================================================================
*/
static void FG_M4Mul( const double *a, const double *b, double *o )
{
	int c, r, k;
	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			double s = 0.0;
			for ( k = 0; k < 4; k++ ) {
				s += a[k * 4 + r] * b[c * 4 + k];
			}
			o[c * 4 + r] = s;
		}
	}
}

static qboolean FG_M4Inv( const double *m, double *o )
{
	double inv[16], det;
	int i;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0 ) {
		return qfalse;
	}
	det = 1.0 / det;
	for ( i = 0; i < 16; i++ ) {
		o[i] = inv[i] * det;
	}
	return qtrue;
}

// the camera origin of a world -> eye matrix
static void FG_ViewOrigin( const double *V, double *o )
{
	int i;
	for ( i = 0; i < 3; i++ ) {
		o[i] = -( V[i * 4 + 0] * V[12] + V[i * 4 + 1] * V[13] + V[i * 4 + 2] * V[14] );
	}
}

// the view a fraction t of the way from V0 to V1: the rotation along the shortest arc, the origin on the straight line
static void FG_MidView( const double *V0, const double *V1, double t, double *Vm )
{
	double R0[3][3], R1[3][3], Rr[3][3], Rh[3][3], Rm[3][3], o0[3], o1[3], om[3];
	double tr, c, th, ax[3], s, cc, ss, k;
	int r, cidx, i;

	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			R0[r][cidx] = V0[cidx * 4 + r];
			R1[r][cidx] = V1[cidx * 4 + r];
		}
	}
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Rr[r][cidx] = R1[r][0] * R0[cidx][0] + R1[r][1] * R0[cidx][1] + R1[r][2] * R0[cidx][2];
		}
	}
	tr = Rr[0][0] + Rr[1][1] + Rr[2][2];
	c = ( tr - 1.0 ) * 0.5;
	if ( c > 1.0 ) c = 1.0;
	if ( c < -1.0 ) c = -1.0;
	th = acos( c );
	s = sin( th );
	if ( th < 1e-9 || fabs( s ) < 1e-9 ) {
		memset( Rh, 0, sizeof( Rh ) );
		Rh[0][0] = Rh[1][1] = Rh[2][2] = 1.0;
	} else {
		ax[0] = ( Rr[2][1] - Rr[1][2] ) / ( 2.0 * s );
		ax[1] = ( Rr[0][2] - Rr[2][0] ) / ( 2.0 * s );
		ax[2] = ( Rr[1][0] - Rr[0][1] ) / ( 2.0 * s );
		k = sqrt( ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2] );
		if ( k > 0.0 ) {
			ax[0] /= k; ax[1] /= k; ax[2] /= k;
		}
		cc = cos( th * t );
		ss = sin( th * t );
		k = 1.0 - cc;
		Rh[0][0] = cc + ax[0] * ax[0] * k;
		Rh[0][1] = ax[0] * ax[1] * k - ax[2] * ss;
		Rh[0][2] = ax[0] * ax[2] * k + ax[1] * ss;
		Rh[1][0] = ax[1] * ax[0] * k + ax[2] * ss;
		Rh[1][1] = cc + ax[1] * ax[1] * k;
		Rh[1][2] = ax[1] * ax[2] * k - ax[0] * ss;
		Rh[2][0] = ax[2] * ax[0] * k - ax[1] * ss;
		Rh[2][1] = ax[2] * ax[1] * k + ax[0] * ss;
		Rh[2][2] = cc + ax[2] * ax[2] * k;
	}
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Rm[r][cidx] = Rh[r][0] * R0[0][cidx] + Rh[r][1] * R0[1][cidx] + Rh[r][2] * R0[2][cidx];
		}
	}
	FG_ViewOrigin( V0, o0 );
	FG_ViewOrigin( V1, o1 );
	for ( i = 0; i < 3; i++ ) {
		om[i] = o0[i] + ( o1[i] - o0[i] ) * t;
	}
	memset( Vm, 0, 16 * sizeof( double ) );
	for ( r = 0; r < 3; r++ ) {
		for ( cidx = 0; cidx < 3; cidx++ ) {
			Vm[cidx * 4 + r] = Rm[r][cidx];
		}
		Vm[12 + r] = -( Rm[r][0] * om[0] + Rm[r][1] * om[1] + Rm[r][2] * om[2] );
	}
	Vm[15] = 1.0;
}

// did the camera cut (respawn, teleport, cinematic cut)? Then there is nothing sensible in between.
static qboolean FG_CameraJumped( const fgFrame_t *a, const fgFrame_t *b )
{
	double oa[3], ob[3], d, dot;

	FG_ViewOrigin( a->V, oa );
	FG_ViewOrigin( b->V, ob );
	d = ( oa[0] - ob[0] ) * ( oa[0] - ob[0] ) + ( oa[1] - ob[1] ) * ( oa[1] - ob[1] ) + ( oa[2] - ob[2] ) * ( oa[2] - ob[2] );
	// the eye's z axis in world space is row 2 of the rotation
	dot = a->V[2] * b->V[2] + a->V[6] * b->V[6] + a->V[10] * b->V[10];
	{
		double r = ( a->P[0] > 0.0 && b->P[0] > 0.0 ) ? ( a->P[0] > b->P[0] ? a->P[0] / b->P[0] : b->P[0] / a->P[0] ) : 1.0;
		qboolean fovCut = ( r_hzmFrameGenFovStep->value > 1.0f && r > (double)r_hzmFrameGenFovStep->value ) ? qtrue : qfalse;
		qboolean cut = ( fovCut || d > 96.0 * 96.0 || dot < 0.90 ) ? qtrue : qfalse;

		if ( cut || d > 24.0 * 24.0 || dot < 0.985 || r > 1.08 ) {
			int k = fgs.nStep & 7;

			fgs.step[k].d = (float)sqrt( d );
			fgs.step[k].dot = (float)dot;
			fgs.step[k].fov = (float)r;
			fgs.step[k].seqA = a->seq;
			fgs.step[k].seqB = b->seq;
			fgs.step[k].cut = cut;
			fgs.nStep++;
		}
		// the field of view stepped (binoculars, a scope snapping in or out): a picture at the fov in between was
		// never part of the game. A smooth aim zoom changes it by 5-13 % a frame and is interpolated as before.
		if ( fovCut ) {
			fgs.nFovStep++;
		}
		return cut;
	}
}

/*
=====================================================================================================================
the frame-gen context: raw GL, no renderer state cache
=====================================================================================================================
*/
static GLuint FG_Compile( GLenum type, const char *header, const char *src )
{
	const char *parts[2];
	GLuint sh;
	GLint ok = 0;

	parts[0] = header;
	parts[1] = src;
	sh = qglCreateShader( type );
	qglShaderSource( sh, 2, parts, NULL );
	qglCompileShader( sh );
	qglGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if ( !ok ) {
		qglDeleteShader( sh );
		return 0;
	}
	return sh;
}

// with the frame-gen context current. No ri.* calls: this also runs on the present thread.
static qboolean FG_CtxInit( void )
{
	const char *header;
	GLuint vs, fs;
	GLint ok = 0;
	static const char *samplers[11] = { "uCurS", "uCurD", "uPrvS", "uPrvD", "uHudF", "uHudS", "uHudD", "uCurM", "uPrvM", "uCurC", "uLayerT" };
	int i;

	if ( fg.ctxInit ) {
		return qtrue;
	}
	header = ( glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 50 ) ? "#version 150\n" : "#version 130\n";
	vs = FG_Compile( GL_VERTEX_SHADER, header, fg_vs );
	fs = FG_Compile( GL_FRAGMENT_SHADER, header, fg_fs );
	if ( !vs || !fs ) {
		return qfalse;
	}
	fg.prog = qglCreateProgram();
	qglAttachShader( fg.prog, vs );
	qglAttachShader( fg.prog, fs );
	qglLinkProgram( fg.prog );
	qglGetProgramiv( fg.prog, GL_LINK_STATUS, &ok );
	qglDeleteShader( vs );
	qglDeleteShader( fs );
	if ( !ok ) {
		return qfalse;
	}
	qglGenVertexArrays( 1, &fg.vao );
	qglBindVertexArray( fg.vao );
	qglUseProgram( fg.prog );
	for ( i = 0; i < 11; i++ ) {
		qglUniform1i( qglGetUniformLocation( fg.prog, samplers[i] ), i );
	}
	fg.uMode = qglGetUniformLocation( fg.prog, "uMode" );
	fg.uTint = qglGetUniformLocation( fg.prog, "uTint" );
	fg.uRes = qglGetUniformLocation( fg.prog, "uRes" );
	fg.uGun = qglGetUniformLocation( fg.prog, "uGun" );
	fg.uFadeK = qglGetUniformLocation( fg.prog, "uFadeK" );
	fg.uFadeB = qglGetUniformLocation( fg.prog, "uFadeB" );
	fg.uHudThr = qglGetUniformLocation( fg.prog, "uHudThr" );
	fg.uCurFromOut = qglGetUniformLocation( fg.prog, "uCurFromOut" );
	fg.uOutFromCur = qglGetUniformLocation( fg.prog, "uOutFromCur" );
	fg.uPrvFromOut = qglGetUniformLocation( fg.prog, "uPrvFromOut" );
	fg.uOutFromPrv = qglGetUniformLocation( fg.prog, "uOutFromPrv" );
	fg.uOutVP = qglGetUniformLocation( fg.prog, "uOutVP" );
	fg.uCurInvW = qglGetUniformLocation( fg.prog, "uCurInvW" );
	fg.uPrvInvW = qglGetUniformLocation( fg.prog, "uPrvInvW" );
	fg.uCurK = qglGetUniformLocation( fg.prog, "uCurK" );
	fg.uPrvK = qglGetUniformLocation( fg.prog, "uPrvK" );
	fg.uUseC = qglGetUniformLocation( fg.prog, "uUseC" );
	fg.uEdge = qglGetUniformLocation( fg.prog, "uEdge" );
	fg.uLayer = qglGetUniformLocation( fg.prog, "uLayer" );
	fg.uLayerCol = qglGetUniformLocation( fg.prog, "uLayerCol" );
	fg.uLayerST = qglGetUniformLocation( fg.prog, "uLayerST" );
	fg.uGunVec = qglGetUniformLocation( fg.prog, "uGunVec" );
	fg.uGunFlash = qglGetUniformLocation( fg.prog, "uGunFlash" );
	fg.uOccl = qglGetUniformLocation( fg.prog, "uOccl" );
	fg.uFill = qglGetUniformLocation( fg.prog, "uFill" );
	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_BLEND );
	qglDisable( GL_CULL_FACE );
	qglDisable( GL_SCISSOR_TEST );
	fg.ctxInit = qtrue;
	return qtrue;
}

static void FG_BindTex( int unit, const image_t *img )
{
	qglActiveTexture( GL_TEXTURE0 + unit );
	qglBindTexture( GL_TEXTURE_2D, img->texnum );
}

static void FG_SetMat( GLint loc, const double *m )
{
	float f[16];
	int i;
	for ( i = 0; i < 16; i++ ) {
		f[i] = (float)m[i];
	}
	qglUniformMatrix4fv( loc, 1, GL_FALSE, f );
}

static void FG_DrawBegin( void )
{
	qglViewport( 0, 0, fg.width, fg.height );
	qglUseProgram( fg.prog );
	qglBindVertexArray( fg.vao );
}

// the real picture, as it is
static void FG_DrawCopy( const fgFrame_t *fr )
{
	FG_DrawBegin();
	FG_BindTex( 4, fg.sets[fr->set].F );
	qglUniform1i( fg.uMode, 0 );
	qglDrawArrays( GL_TRIANGLES, 0, 3 );
}

// a picture for the camera ( outV, outP ), made from two real frames; HUD and weapon come from `hud' unwarped.
// vectors: the output is the moment halfway between prv and cur, so what moves by itself is taken half its own way
// back from cur (and half its last displacement forward from prv, where cur does not show the pixel).
static qboolean FG_DrawGenerated( const fgFrame_t *prv, const fgFrame_t *cur, const fgFrame_t *hud,
	const double *outV, const double *outP, int tint, qboolean vectors )
{
	double vpO[16], vpC[16], vpP[16], iO[16], iC[16], iP[16], m[16];
	float curK = 0.0f, prvK = 0.0f, useC = 0.0f, gunVec = 0.0f;

	if ( vectors && r_hzmFrameGenVectors->integer ) {
		// a dropped frame in between: cur's displacement is one frame's, the gap is n frames
		int n = cur->seq - prv->seq;
		if ( n >= 1 && n <= 3 ) {
			if ( cur->vec ) curK = 0.5f * n;
			if ( prv->vec ) prvK = -0.5f * n;
			// the movers were drawn for the camera halfway between cur and the frame before it: that is this
			// picture's camera unless a frame was dropped in between
			if ( cur->vec && cur->midPrev == prv->seq ) useC = 1.0f;
			if ( useC != 0.0f && cur->gun && hud == cur ) gunVec = 1.0f;
		}
	}

	FG_M4Mul( outP, outV, vpO );
	FG_M4Mul( cur->P, cur->V, vpC );
	FG_M4Mul( prv->P, prv->V, vpP );
	if ( !FG_M4Inv( vpO, iO ) || !FG_M4Inv( vpC, iC ) || !FG_M4Inv( vpP, iP ) ) {
		return qfalse;
	}
	FG_DrawBegin();
	FG_BindTex( 0, fg.sets[cur->set].S );
	FG_BindTex( 1, fg.sets[cur->set].D );
	FG_BindTex( 2, fg.sets[prv->set].S );
	FG_BindTex( 3, fg.sets[prv->set].D );
	FG_BindTex( 4, fg.sets[hud->set].F );
	FG_BindTex( 5, fg.sets[hud->set].S );
	FG_BindTex( 6, fg.sets[hud->set].D );
	if ( curK != 0.0f ) {
		FG_BindTex( 7, fg.sets[cur->set].M );
	}
	if ( useC != 0.0f ) {
		FG_BindTex( 9, fg.sets[cur->set].C );
	}
	qglUniform1f( fg.uUseC, useC );
	qglUniform1f( fg.uGunVec, gunVec );
	qglUniform1f( fg.uOccl, r_hzmFrameGenOccl->integer ? 1.0f : 0.0f );
	qglUniform1f( fg.uFill, r_hzmFrameGenFill->integer ? 1.0f : 0.0f );
	qglUniform1f( fg.uGunFlash, ( cur->flash || prv->flash ) ? 1.0f : 0.0f );
	qglUniform1f( fg.uEdge, r_hzmFrameGenEdge->integer ? 1.0f : 0.0f );
	if ( prvK != 0.0f ) {
		FG_BindTex( 8, fg.sets[prv->set].M );
	}
	qglUniform1f( fg.uCurK, curK );
	qglUniform1f( fg.uPrvK, prvK );
	qglUniform4f( fg.uCurInvW, (float)iC[3], (float)iC[7], (float)iC[11], (float)iC[15] );
	qglUniform4f( fg.uPrvInvW, (float)iP[3], (float)iP[7], (float)iP[11], (float)iP[15] );
	FG_SetMat( fg.uOutVP, vpO );
	qglUniform1i( fg.uMode, 1 );
	qglUniform1i( fg.uTint, tint );
	qglUniform2f( fg.uRes, (float)fg.width, (float)fg.height );
	qglUniform1f( fg.uGun, FG_GUN_DEPTH );
	qglUniform1f( fg.uFadeK, hud->fadeK );
	qglUniform4f( fg.uFadeB, hud->fadeB[0], hud->fadeB[1], hud->fadeB[2], 0.0f );
	// with a fade in play the scene copy and the final picture differ by two roundings instead of none; with a
	// layer drawn again here, by its filtering and one more rounding
	qglUniform1f( fg.uHudThr, hud->layerTex ? 0.014f
		: ( ( hud->fadeK != 1.0f || hud->fadeB[0] + hud->fadeB[1] + hud->fadeB[2] != 0.0f ) ? 0.009f : 0.0025f ) );
	qglUniform1f( fg.uLayer, hud->layerTex ? 1.0f : 0.0f );
	if ( hud->layerTex ) {
		qglActiveTexture( GL_TEXTURE0 + 10 );
		qglBindTexture( GL_TEXTURE_2D, hud->layerTex );
		qglUniform4f( fg.uLayerCol, hud->layerCol[0], hud->layerCol[1], hud->layerCol[2], hud->layerCol[3] );
		qglUniform4f( fg.uLayerST, hud->layerST[0], hud->layerST[1], hud->layerST[2], hud->layerST[3] );
	}
	FG_M4Mul( vpC, iO, m ); FG_SetMat( fg.uCurFromOut, m );
	FG_M4Mul( vpO, iC, m ); FG_SetMat( fg.uOutFromCur, m );
	FG_M4Mul( vpP, iO, m ); FG_SetMat( fg.uPrvFromOut, m );
	FG_M4Mul( vpO, iP, m ); FG_SetMat( fg.uOutFromPrv, m );
	qglDrawArrays( GL_TRIANGLES, 0, 3 );
	return qtrue;
}

static void FG_MidCamera( const fgFrame_t *a, const fgFrame_t *b, double *V, double *P )
{
	int i;
	FG_MidView( a->V, b->V, 0.5, V );
	for ( i = 0; i < 16; i++ ) {
		P[i] = ( a->P[i] + b->P[i] ) * 0.5;
	}
}

// fg_record <n>: the present thread keeps the next n pictures exactly as it shows them (real and generated), the main
// thread writes them out as screenshots/fgrec_<index>_<R|G>_<time in 0.1 ms>.tga. For looking at generated frames
// from live play; reading the back buffer costs the present thread a millisecond or two per picture while it runs.
#define FG_REC_MAX	96
static struct {
	volatile int	want, count, done;
	byte			*pix[FG_REC_MAX];
	byte			kind[FG_REC_MAX];
	Uint64			t[FG_REC_MAX];
} fgRec;

static void FG_RecFlush( void );

static void FG_RecGrab( int kind )
{
	int i = fgRec.count;

	if ( fgRec.done || i >= fgRec.want ) {
		return;
	}
	fgRec.pix[i] = (byte *)malloc( (size_t)fg.width * fg.height * 4 );
	if ( !fgRec.pix[i] ) {
		fgRec.done = 1;
		return;
	}
	// RGBA: rows are 4-aligned whatever the pack alignment is
	qglReadPixels( 0, 0, fg.width, fg.height, GL_RGBA, GL_UNSIGNED_BYTE, fgRec.pix[i] );
	fgRec.kind[i] = (byte)kind;
	fgRec.t[i] = SDL_GetPerformanceCounter();
	fgRec.count = i + 1;
	if ( fgRec.count >= fgRec.want ) {
		fgRec.done = 1;
	}
}

static void FG_Record( int kind )
{
	int i = fgs.n & ( FG_TS - 1 );
	fgs.t[i] = SDL_GetPerformanceCounter();
	fgs.kind[i] = (byte)kind;
	fgs.n++;
	if ( kind == FGK_GEN ) {
		fgs.nGen++;
	} else {
		fgs.nReal++;
	}
}

/*
=====================================================================================================================
the present thread
=====================================================================================================================
*/
static double FG_Ms( Uint64 a, Uint64 b )
{
	return (double)( b - a ) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

// how long after the main thread handed the frame over was this picture of it shown (the delay the feature adds)
static void FG_NoteDelay( const fgFrame_t *fr, int kind )
{
	double d = FG_Ms( fr->tPub, SDL_GetPerformanceCounter() );

	if ( kind == FGK_GEN ) {
		fgs.genDelayMs += d;
		fgs.nGenDelay++;
	} else {
		fgs.realDelayMs += d;
		fgs.nRealDelay++;
	}
}

// sleep until `target'. Returns qfalse if a newer frame was published first (and early is set).
static qboolean FG_WaitUntil( Uint64 target, qboolean wakeOnPending )
{
	for ( ;; ) {
		Uint64 now = SDL_GetPerformanceCounter();
		double rem;

		if ( now >= target ) {
			return qtrue;
		}
		rem = FG_Ms( now, target );
		if ( rem > 2.0 ) {
			qboolean pend;
			SDL_LockMutex( fg.mx );
			pend = fg.hasPending || fg.quit;
			if ( !pend || !wakeOnPending ) {
				SDL_CondWaitTimeout( fg.cv, fg.mx, (Uint32)( rem - 1.5 ) );
				pend = fg.hasPending || fg.quit;
			}
			SDL_UnlockMutex( fg.mx );
			if ( pend && wakeOnPending ) {
				return qfalse;
			}
			if ( pend ) {
				// the condition is already raised: a timed wait would return at once, so yield instead of spinning hot
				SDL_Delay( 1 );
			}
		}
		// the last stretch is spun: a timed wait is only good to a millisecond
	}
}

static int FG_ThreadMain( void *unused )
{
	fgFrame_t	cur, next;
	qboolean	haveCur = qfalse;
	double		dEst = 0.0;
	int			state;
	int			errTick = 0;

	(void)unused;
	memset( &cur, 0, sizeof( cur ) );

	state = ( GLimp_FG_MakeCurrent( fg.ctx ) && FG_CtxInit() ) ? 1 : -1;
	if ( state == 1 ) {
		GLimp_FG_SetSwapInterval( fg.swapInterval );
	}
	SDL_LockMutex( fg.mx );
	fg.threadState = state;
	SDL_CondBroadcast( fg.cv );
	SDL_UnlockMutex( fg.mx );
	if ( state != 1 ) {
		GLimp_FG_ReleaseCurrent();
		return 0;
	}
	SDL_SetThreadPriority( SDL_THREAD_PRIORITY_HIGH );

	for ( ;; ) {
		qboolean gen;
		Uint64 t0;

		SDL_LockMutex( fg.mx );
		while ( !fg.hasPending && !fg.quit ) {
			SDL_CondWaitTimeout( fg.cv, fg.mx, 200 );
		}
		if ( fg.quit ) {
			SDL_UnlockMutex( fg.mx );
			break;
		}
		next = fg.pending;
		fg.hasPending = qfalse;
		fg.heldPrev = haveCur ? cur.set : -1;
		fg.heldCur = next.set;
		SDL_UnlockMutex( fg.mx );

		// the main thread only flushed: wait until the GPU has really finished this frame's textures
		if ( next.sync ) {
			GLenum r;
			t0 = SDL_GetPerformanceCounter();
			r = fg.ClientWaitSync( next.sync, 0, 200000000ULL );
			if ( r == 0x911B /* GL_TIMEOUT_EXPIRED */ || r == 0x911D /* GL_WAIT_FAILED */ ) {
				fgs.nSyncTimeout++;
			}
			fg.DeleteSync( next.sync );
			next.sync = NULL;
			fgs.syncWaitMs += FG_Ms( t0, SDL_GetPerformanceCounter() );
			fgs.nSyncWait++;
		}

		gen = qfalse;
		if ( fg.mode == 1 ) {
			if ( !next.scene ) {
				fgs.nNoScene++;
			} else if ( next.bypass ) {
				fgs.nBypass++;
			} else if ( haveCur && cur.scene && !cur.bypass ) {
				if ( FG_CameraJumped( &cur, &next ) ) {
					fgs.nJump++;
				} else {
					gen = qtrue;
				}
			}
		}

		if ( haveCur ) {
			double d = FG_Ms( cur.tPub, next.tPub );
			if ( d > 100.0 ) d = 100.0;
			dEst = dEst <= 0.0 ? d : dEst + ( d - dEst ) * 0.2;
		}

		if ( gen ) {
			double V[16], P[16];
			Uint64 tGen;
			qboolean showReal = qtrue;

			t0 = SDL_GetPerformanceCounter();
			FG_MidCamera( &cur, &next, V, P );
			if ( FG_DrawGenerated( &cur, &next, &next, V, P,
			r_hzmFrameGenDebug->integer >= 7 ? 4 : ( r_hzmFrameGenDebug->integer == 6 ? 3 : ( r_hzmFrameGenDebug->integer >= 5 ? 2 : ( r_hzmFrameGenDebug->integer >= 3 ? 1 : 0 ) ) ), qtrue ) ) {
				FG_RecGrab( FGK_GEN );
				GLimp_FG_Swap();
				tGen = SDL_GetPerformanceCounter();
				FG_Record( FGK_GEN );
				FG_NoteDelay( &next, FGK_GEN );
				fgs.genMs += FG_Ms( t0, tGen );
				fgs.nGenTimed++;
				// the real picture half a real frame after the generated one
				if ( !FG_WaitUntil( tGen + (Uint64)( dEst * 0.5 / 1000.0 * (double)SDL_GetPerformanceFrequency() ), qtrue ) ) {
					// the next real frame is already here. If this one's turn was still far off, leave it out:
					// the generated pictures alone are an even sequence, two pictures back to back are not.
					if ( dEst * 0.5 - FG_Ms( tGen, SDL_GetPerformanceCounter() ) > dEst * 0.25 ) {
						showReal = qfalse;
						fgs.nEarlySkip++;
					}
				}
			}
			if ( showReal && !fg.quit ) {
				FG_DrawCopy( &next );
				FG_RecGrab( FGK_REAL );
				GLimp_FG_Swap();
				FG_Record( FGK_REAL );
				FG_NoteDelay( &next, FGK_REAL );
			}
		} else {
			FG_DrawCopy( &next );
			FG_RecGrab( FGK_REAL );
			GLimp_FG_Swap();
			FG_Record( FGK_REAL );
			FG_NoteDelay( &next, FGK_REAL );
		}

		if ( ( ++errTick & 63 ) == 0 ) {
			while ( qglGetError() != GL_NO_ERROR ) {
				fgs.nGLErr++;
			}
		}

		cur = next;
		haveCur = qtrue;
	}

	qglFinish();
	GLimp_FG_ReleaseCurrent();
	return 0;
}

static void FG_StopThread( void )
{
	if ( fg.thread ) {
		SDL_LockMutex( fg.mx );
		fg.quit = 1;
		SDL_CondBroadcast( fg.cv );
		SDL_UnlockMutex( fg.mx );
		SDL_WaitThread( fg.thread, NULL );
		fg.thread = NULL;
	}
	fg.quit = 0;
	if ( fg.hasPending && fg.pending.sync && fg.DeleteSync ) {
		fg.DeleteSync( fg.pending.sync );
	}
	fg.hasPending = qfalse;
	fg.heldPrev = fg.heldCur = -1;
	fg.lastPubSet = -1;
}

static qboolean FG_StartThread( void )
{
	fg.quit = 0;
	fg.threadState = 0;
	fg.hasPending = qfalse;
	fg.heldPrev = fg.heldCur = -1;
	fg.lastPubSet = -1;
	fg.swapInterval = r_swapInterval ? r_swapInterval->integer : 0;
	fg.thread = SDL_CreateThread( FG_ThreadMain, "hzm_present", NULL );
	if ( !fg.thread ) {
		return qfalse;
	}
	SDL_LockMutex( fg.mx );
	while ( fg.threadState == 0 ) {
		SDL_CondWaitTimeout( fg.cv, fg.mx, 100 );
	}
	SDL_UnlockMutex( fg.mx );
	if ( fg.threadState != 1 ) {
		SDL_WaitThread( fg.thread, NULL );
		fg.thread = NULL;
		return qfalse;
	}
	return qtrue;
}

/*
=====================================================================================================================
motion vectors (main thread, the renderer's own context)

The camera-only picture leaves everything that moves by itself where the NEWER frame has it: for half of the pictures
shown a running soldier, a truck, a squad mate walking beside the player is half a real frame ahead of the world
around it, and it reads as shaking (user, 2026-10-12: "when characters vibrate it looks super buggy").

So each frame also gets M: for every pixel that shows a surface of a numbered entity, how far that point of the
surface moved in the WORLD since the frame before (its entity's placement then and now, and for a skinned model each
vertex's own position then and now - the skinned vertices of the last frame are kept here, per entity and surface).
World units, not screen units: the generator then places the point with the same camera as everything else.
And C: the same surfaces drawn where they are halfway (each vertex halfway between then and now, the camera halfway
between the two frames - the same one the present thread works out), each pixel carrying where it is in this frame.
An earlier attempt found the movers from M alone, by searching from a per-tile "fastest mover" guess: a rifle slung
on a running soldier's back and his trailing boot have their own speed, the guess missed them and they vanished
from the generated pictures. Drawing them where they will be cannot miss.

Not in M or C (they keep the newer frame's place, as before): the first-person weapon and the HUD (never warped at all),
blended surfaces and sprites (smoke, fire, rain, beams, muzzle flashes, decals - they have no depth of their own and
ride with whatever is behind them), entities without a number (client-side effect models), the map itself.
=====================================================================================================================
*/
#define FG_TELEPORT		96.0f		// an entity that moved further than this in one frame was placed, it did not travel
#define FG_PSK_SLOTS	8192
#define FG_PSK_PROBES	8
#define FG_PSK_MAX		( 64 * 1024 * 1024 )

typedef struct {
	int			seq;				// the frame cur is from
	int			num, hModel;
	unsigned int	pose;			// its bones (and morphs) in that frame
	qboolean	use;
	qboolean	havePrev;
	float		cur[16], prev[16];	// model -> world
} fgEnt_t;

typedef struct {
	const void	*sf;
	int			ent;
	int			seq;
	int			n, cap;
	float		*xyz;				// the surface's skinned vertices in that frame, model space
} fgSkin_t;

// One entity number can be drawn as several models in a frame (a soldier, and client-side models hung on him under
// his number): the record is per ( number, model ). Two of the same model under one number cannot be told apart -
// the second one seen in a frame gets no vectors and keeps the newer frame's place.
#define FG_ENT_SLOTS	4096
#define FG_ENT_PROBES	8
static fgEnt_t	fgEnts[FG_ENT_SLOTS];

static fgEnt_t *FG_EntSlot( int num, int hModel, int frame )
{
	unsigned int h = (unsigned int)num * 2654435761u + (unsigned int)hModel * 40503u;
	fgEnt_t *old = NULL;
	int i;

	for ( i = 0; i < FG_ENT_PROBES; i++ ) {
		fgEnt_t *e = &fgEnts[( ( h >> 8 ) + i ) & ( FG_ENT_SLOTS - 1 )];
		if ( e->seq && e->num == num && e->hModel == hModel ) {
			return e;
		}
		if ( !old || e->seq < old->seq ) {
			old = e;
		}
	}
	if ( old->seq >= frame - 1 && old->seq ) {
		return NULL;
	}
	memset( old, 0, sizeof( *old ) );
	old->num = num;
	old->hModel = hModel;
	return old;
}
static fgSkin_t	fgSkins[FG_PSK_SLOTS];
static int		fgSkinBytes;

unsigned int R_FG_PoseHash( trRefEntity_t *ent );		// tr_model.cpp

// model data is being freed (surface pointers die), or the feature shuts down
void R_FG_FlushPrev( void )
{
	int i;

	for ( i = 0; i < FG_PSK_SLOTS; i++ ) {
		free( fgSkins[i].xyz );
	}
	memset( fgSkins, 0, sizeof( fgSkins ) );
	memset( fgEnts, 0, sizeof( fgEnts ) );
	fgSkinBytes = 0;
}

static fgSkin_t *FG_SkinSlot( int ent, const void *sf, int frame )
{
	unsigned int h = (unsigned int)( ( (size_t)sf >> 4 ) * 2654435761u ) ^ ( (unsigned int)ent * 40503u );
	fgSkin_t *old = NULL;
	int i;

	for ( i = 0; i < FG_PSK_PROBES; i++ ) {
		fgSkin_t *e = &fgSkins[( h + i ) & ( FG_PSK_SLOTS - 1 )];
		if ( e->sf == sf && e->ent == ent ) {
			return e;
		}
		if ( !old || e->seq < old->seq ) {
			old = e;
		}
	}
	// not there: the least recently used of the probed slots, unless it was used this frame or the last
	if ( old->sf && old->seq >= frame - 1 ) {
		return NULL;
	}
	old->sf = sf;
	old->ent = ent;
	old->seq = 0;
	old->n = 0;
	return old;
}

// the surface function has just put n vertices at tess.xyz[base]: write each one's displacement since the last frame
// where the vertex shader reads it (texCoords = x y, lightCoords = z), and keep the positions for the next frame
static void FG_FillPrev( int key, const surfaceType_t *surface, int base, int n, int frame )
{
	fgSkin_t *e;
	int i, m = 0;

	if ( n <= 0 ) {
		return;
	}
	if ( fg.gunDump > 0 ) {
		fgSkin_t *d = ( *surface == SF_TIKI_SKEL ) ? FG_SkinSlot( key, surface, frame ) : NULL;
		ri.Printf( PRINT_ALL, "FGSKIN frame %d key %d surface type %d %p base %d verts %d: %s, kept verts %d from frame %d\n", frame, key, (int)*surface, (void *)surface, base, n,
			*surface != SF_TIKI_SKEL ? "not a skinned surface" : ( !d ? "NO SLOT" : ( d->seq == frame - 1 ? "has last frame" : ( d->seq == frame ? "SEEN TWICE this frame" : "no last frame" ) ) ),
			d ? d->n : 0, d ? d->seq : 0 );
	}
	if ( *surface == SF_TIKI_SKEL && ( e = FG_SkinSlot( key, surface, frame ) ) != NULL ) {
		if ( e->seq == frame - 1 && e->xyz ) {
			qboolean moved = qfalse;

			m = n < e->n ? n : e->n;
			for ( i = 0; i < m; i++ ) {
				const float *c = tess.xyz[base + i];
				const float *o = &e->xyz[i * 3];
				float dx = c[0] - o[0], dy = c[1] - o[1], dz = c[2] - o[2];

				tess.texCoords[base + i][0] = dx;
				tess.texCoords[base + i][1] = dy;
				tess.lightCoords[base + i][0] = dz;
				tess.lightCoords[base + i][1] = 0.0f;
				if ( dx != 0.0f || dy != 0.0f || dz != 0.0f ) {
					moved = qtrue;
				}
			}
			if ( moved ) {
				fg.velBatchMoves = qtrue;
			}
		}
		if ( e->seq != frame ) {
			if ( e->cap < n ) {
				if ( fgSkinBytes > FG_PSK_MAX ) {
					R_FG_FlushPrev();
					e = FG_SkinSlot( key, surface, frame );
				}
				if ( e ) {
					free( e->xyz );
					e->cap = n + 16;
					e->xyz = (float *)malloc( (size_t)e->cap * 3 * sizeof( float ) );
					fgSkinBytes += e->cap * 3 * (int)sizeof( float );
				}
			}
			if ( e && e->xyz ) {
				for ( i = 0; i < n; i++ ) {
					VectorCopy( tess.xyz[base + i], &e->xyz[i * 3] );
				}
				e->n = n;
				e->seq = frame;
			}
		}
	}
	for ( i = m; i < n; i++ ) {
		tess.texCoords[base + i][0] = tess.texCoords[base + i][1] = 0.0f;
		tess.lightCoords[base + i][0] = tess.lightCoords[base + i][1] = 0.0f;
	}
}

// RB_EndSurface while r_fgVelPass is set: what the surface functions collected goes into M and C
void R_FG_VelocityDraw( void )
{
	fgSet_t *s = &fg.sets[fg.w];
	int w = s->M->width, h = s->M->height;

	if ( !tess.numIndexes || !tess.numVertexes ) {
		return;
	}
	if ( fg.velBatchGun && ( !tess.useInternalVao || tess.useCacheVao ) ) {
		fg.gunBad = qtrue;
		return;
	}
	if ( !fg.velBatchGun && ( ( !tess.useInternalVao && !tess.useCacheVao ) || !fg.velBatchMoves ) ) {
		// a surface drawn from buffers of its own is not handled; a batch in which nothing moved is the same as
		// not being in M at all
		fg.velSkipped++;
		return;
	}
	if ( !fg.velBound ) {
		FBO_Bind( fg.fboMC0 );
		qglViewport( 0, 0, w * 2 + h, h );
		qglScissor( 0, 0, w * 2 + h, h );
		GL_State( GLS_DEPTHMASK_TRUE );
		qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		qglClearDepth( 1.0 );
		qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		GL_BindToTMU( tr.renderDepthImage, 0 );
		GL_UseProgram( fg.velProg );
		qglUniformMatrix4fv( fg.uVelMidVP, 1, GL_FALSE, fg.midVP );
		qglUniformMatrix4fv( fg.uVelCurVP, 1, GL_FALSE, fg.curVP );
		qglUniform2f( fg.uVelSize, (float)w, (float)h );
		if ( fg.velShadow ) {
			qglUniformMatrix4fv( fg.uVelSunVP, 1, GL_FALSE, fg.sunVP );
		}
		qglUniform1f( fg.uVelGunF, 0.0f );
		fg.velBound = qtrue;
	}
	if ( tess.useCacheVao ) {
		// brush models (doors, lifts): rigid, their vertices sit in the surface cache with their real texture
		// coordinates - the shader must not read those as a displacement
		VaoCache_Commit();
		qglUniform1f( fg.uVelRigid, 0.0f );
		fgs.velBrush++;
	} else {
		RB_UpdateTessVao( ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD );
		qglUniform1f( fg.uVelRigid, 1.0f );
	}
	GL_Cull( tess.shader->cullType );
	qglUniformMatrix4fv( fg.uVelMVP, 1, GL_FALSE, glState.modelviewProjection );
	if ( fg.velNewEntity ) {
		qglUniformMatrix4fv( fg.uVelModel, 1, GL_FALSE, fg.velModel );
		qglUniformMatrix4fv( fg.uVelPrevModel, 1, GL_FALSE, fg.velPrevModel );
		fg.velNewEntity = qfalse;
	}
	if ( fg.velBatchGun ) {
		if ( fg.gunDump > 0 ) {
			ri.Printf( PRINT_ALL, "FGGUN   batch shader %s sort %.0f verts %d indexes %d\n", tess.shader->name, tess.shader->sort, tess.numVertexes, tess.numIndexes );
		}
		// the first-person weapon: C only, with its own projection, in front of every world mover
		qglUniformMatrix4fv( fg.uVelMidVP, 1, GL_FALSE, fg.gunMidP );
		qglUniformMatrix4fv( fg.uVelCurVP, 1, GL_FALSE, fg.gunCurP );
		qglUniform1f( fg.uVelGunF, 1.0f );
		if ( !fg.velBatchHold ) {
			// A part of the weapon that moves further than r_hzmFrameGenGunMax in one frame, in view space (the kick
			// of a shot, a weapon switch, the swing of a reload), is not placed halfway: the whole batch keeps the
			// newer frame's place. Per batch, not per vertex - a rigid weapon must not be bent.
			float lim = r_hzmFrameGenGunMax->value, far2 = 0.0f;
			const float *mc = fg.velModel, *mp = fg.velPrevModel;
			int i;

			for ( i = 0; i < tess.numVertexes; i++ ) {
				const float *v = tess.xyz[i];
				float px = v[0] - tess.texCoords[i][0], py = v[1] - tess.texCoords[i][1], pz = v[2] - tess.lightCoords[i][0];
				float dx = ( mc[0] * v[0] + mc[4] * v[1] + mc[8] * v[2] + mc[12] ) - ( mp[0] * px + mp[4] * py + mp[8] * pz + mp[12] );
				float dy = ( mc[1] * v[0] + mc[5] * v[1] + mc[9] * v[2] + mc[13] ) - ( mp[1] * px + mp[5] * py + mp[9] * pz + mp[13] );
				float dz = ( mc[2] * v[0] + mc[6] * v[1] + mc[10] * v[2] + mc[14] ) - ( mp[2] * px + mp[6] * py + mp[10] * pz + mp[14] );
				float d2 = dx * dx + dy * dy + dz * dz;

				if ( d2 > far2 ) {
					far2 = d2;
				}
			}
			if ( far2 > lim * lim ) {
				fg.velBatchHold = qtrue;
				fg.gunHeld++;
			}
			if ( fg.gunDump > 0 ) {
				ri.Printf( PRINT_ALL, "FGGUN     fastest vertex %.2f units%s\n", sqrt( far2 ), fg.velBatchHold ? " - held" : "" );
			}
		}
		if ( fg.velBatchHold ) {
			qglUniform1f( fg.uVelRigid, 0.0f );
			qglUniformMatrix4fv( fg.uVelPrevModel, 1, GL_FALSE, fg.velModel );
			fg.velNewEntity = qtrue;		// the next batch of this entity gets its own matrices back
		}
		qglDepthRange( 0, 0.3 );
		qglViewport( w, 0, w, h );
		qglScissor( w, 0, w, h );
		GL_State( GLS_DEPTHMASK_TRUE );
		qglUniform1i( fg.uVelPass, 1 );
		R_DrawElements( tess.numIndexes, tess.firstIndex );
		qglDepthRange( 0, 1 );
		qglUniformMatrix4fv( fg.uVelMidVP, 1, GL_FALSE, fg.midVP );
		qglUniformMatrix4fv( fg.uVelCurVP, 1, GL_FALSE, fg.curVP );
		qglUniform1f( fg.uVelGunF, 0.0f );
		fg.gunDraws++;
		return;
	}
	if ( fg.gunDump > 0 && !tess.useCacheVao ) {
		const float *mc = fg.velModel, *mp = fg.velPrevModel;
		float lo = 1e9f, hi = 0.0f, skin = 0.0f;
		int i;

		for ( i = 0; i < tess.numVertexes; i++ ) {
			const float *v = tess.xyz[i];
			float sx = tess.texCoords[i][0], sy = tess.texCoords[i][1], sz = tess.lightCoords[i][0];
			float px = v[0] - sx, py = v[1] - sy, pz = v[2] - sz;
			float dx = ( mc[0] * v[0] + mc[4] * v[1] + mc[8] * v[2] + mc[12] ) - ( mp[0] * px + mp[4] * py + mp[8] * pz + mp[12] );
			float dy = ( mc[1] * v[0] + mc[5] * v[1] + mc[9] * v[2] + mc[13] ) - ( mp[1] * px + mp[5] * py + mp[9] * pz + mp[13] );
			float dz = ( mc[2] * v[0] + mc[6] * v[1] + mc[10] * v[2] + mc[14] ) - ( mp[2] * px + mp[6] * py + mp[10] * pz + mp[14] );
			float d = sqrt( dx * dx + dy * dy + dz * dz ), s = sqrt( sx * sx + sy * sy + sz * sz );

			if ( d < lo ) lo = d;
			if ( d > hi ) hi = d;
			if ( s > skin ) skin = s;
		}
		ri.Printf( PRINT_ALL, "FGBATCH shader %s cull %d sort %.0f verts %d indexes %d: vertices moved %.2f .. %.2f units in the world, skinned part up to %.2f\n",
			tess.shader->name, (int)tess.shader->cullType, tess.shader->sort, tess.numVertexes, tess.numIndexes, lo, hi, skin );
	}
	// M: where it is in the frame, wherever it is what the scene shows there
	qglViewport( 0, 0, w, h );
	qglScissor( 0, 0, w, h );
	GL_State( GLS_DEPTHTEST_DISABLE );
	qglUniform1i( fg.uVelPass, 0 );
	R_DrawElements( tess.numIndexes, tess.firstIndex );
	// C: where it is halfway, nearest surface on top
	qglViewport( w, 0, w, h );
	qglScissor( w, 0, w, h );
	GL_State( GLS_DEPTHMASK_TRUE );
	qglUniform1i( fg.uVelPass, 1 );
	R_DrawElements( tess.numIndexes, tess.firstIndex );
	if ( fg.velShadow ) {
		// L: as the sun sees it (both faces: the nearest surface to the sun is what matters)
		qglViewport( w * 2, 0, h, h );
		qglScissor( w * 2, 0, h, h );
		GL_Cull( CT_TWO_SIDED );
		qglUniform1i( fg.uVelPass, 2 );
		R_DrawElements( tess.numIndexes, tess.firstIndex );
	}
	fg.velDraws++;
}

static void FG_FullQuad( void )
{
	vec4_t quad[4];
	vec2_t tc[4];

	VectorSet4( quad[0], -1.0f, -1.0f, 0.0f, 1.0f );
	VectorSet4( quad[1], 1.0f, -1.0f, 0.0f, 1.0f );
	VectorSet4( quad[2], 1.0f, 1.0f, 0.0f, 1.0f );
	VectorSet4( quad[3], -1.0f, 1.0f, 0.0f, 1.0f );
	VectorSet2( tc[0], 0.0f, 0.0f );
	VectorSet2( tc[1], 1.0f, 0.0f );
	VectorSet2( tc[2], 1.0f, 1.0f );
	VectorSet2( tc[3], 0.0f, 1.0f );
	RB_InstantQuad2( quad, tc );
}

/*
RB_DrawSurfs, the main world view, after its colour pass (and the multisample depth resolve): tr.renderDepthImage
is the finished opaque scene. Everything it needs from the back end is still set up for this view.
*/
void R_FG_VelocityPass( void *drawSurfsV, int numDrawSurfs )
{
	drawSurf_t		*drawSurfs = (drawSurf_t *)drawSurfsV, *drawSurf;
	shader_t		*shader = NULL, *oldShader = NULL;
	int				entityNum, oldEntityNum = -1;
	int				fogNum, dlighted, pshadowed, i;
	int				frame = fg.seq + 1;		// the frame being built (R_FG_Present numbers it)
	qboolean		bStaticModel, use = qfalse, isGun = qfalse, gunHold = qfalse;
	int				skinKey = 0;
	trRefEntity_t	*ent = NULL;
	double			originalTime = backEnd.refdef.floatTime;
	fgSet_t			*s = &fg.sets[fg.w];

	if ( fg.mode != 1 || !fg.vecOK || !fg.latched || fg.build.scene || fg.build.vec || !r_hzmFrameGenVectors->integer ) {
		return;
	}
	if ( !tr.renderDepthImage || tr.renderDepthImage->width != s->M->width || tr.renderDepthImage->height != s->M->height ) {
		return;
	}
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	if ( fg.lastSeq != frame - 1 || fg.lastBypass ) {
		// no frame straight before this one: nothing to be halfway to. Or an overlay is up (scope, menu, the
		// aiming vignette): nothing is being generated, so nothing needs vectors.
		return;
	}
	{
		double Vm[16], Pm[16], t[16];

		FG_MidView( fg.lastV, fg.latV, 0.5, Vm );
		for ( i = 0; i < 16; i++ ) {
			Pm[i] = ( fg.lastP[i] + fg.latP[i] ) * 0.5;
		}
		FG_M4Mul( Pm, Vm, t );
		for ( i = 0; i < 16; i++ ) {
			fg.midVP[i] = (float)t[i];
		}
		FG_M4Mul( fg.latP, fg.latV, t );
		for ( i = 0; i < 16; i++ ) {
			fg.curVP[i] = (float)t[i];
		}
		fg.velShadow = qfalse;
		if ( r_hzmFrameGenShadows->integer && R_CharShadowsActive() && FG_M4Inv( t, Vm ) ) {
			// an orthographic box around the camera, looking down the sun's rays: 1024 units to each side
			vec3_t d, u, v, up = { 0.0f, 0.0f, 1.0f };
			const float *cam = backEnd.viewParms.or.origin;
			const float R = 1024.0f;

			VectorCopy( backEnd.refdef.sunDir, d );
			if ( VectorNormalize( d ) > 0.5f ) {
				if ( fabs( d[2] ) > 0.99f ) {
					VectorSet( up, 1.0f, 0.0f, 0.0f );
				}
				CrossProduct( up, d, u );
				VectorNormalize( u );
				CrossProduct( d, u, v );
				memset( fg.sunVP, 0, sizeof( fg.sunVP ) );
				for ( i = 0; i < 3; i++ ) {
					fg.sunVP[i * 4 + 0] = u[i] / R;
					fg.sunVP[i * 4 + 1] = v[i] / R;
					fg.sunVP[i * 4 + 2] = -d[i] / R;
				}
				fg.sunVP[12] = -DotProduct( u, cam ) / R;
				fg.sunVP[13] = -DotProduct( v, cam ) / R;
				fg.sunVP[14] = DotProduct( d, cam ) / R;
				fg.sunVP[15] = 1.0f;
				for ( i = 0; i < 16; i++ ) {
					fg.invCurVP[i] = (float)Vm[i];
				}
				VectorCopy( d, fg.sunDir );
				fg.velShadow = qtrue;
			}
		}
	}
	fg.gunOK = qfalse;
	if ( r_hzmFrameGenGun->integer ) {
		// the weapon's projection (its own while aiming): halfway, unless it stepped - then the weapon stays as it is
		double r0 = fg.lastWP[0] != 0.0 ? fg.latWP[0] / fg.lastWP[0] : 0.0;
		double r5 = fg.lastWP[5] != 0.0 ? fg.latWP[5] / fg.lastWP[5] : 0.0;

		if ( r0 > 0.98 && r0 < 1.02 && r5 > 0.98 && r5 < 1.02
			&& fabs( fg.latWP[8] - fg.lastWP[8] ) < 0.03 && fabs( fg.latWP[9] - fg.lastWP[9] ) < 0.03 ) {
			for ( i = 0; i < 16; i++ ) {
				fg.gunMidP[i] = (float)( ( fg.latWP[i] + fg.lastWP[i] ) * 0.5 );
				fg.gunCurP[i] = (float)fg.latWP[i];
			}
			fg.gunOK = qtrue;
		} else {
			fgs.nGunProj++;
		}
	}
	fg.gunBad = qfalse;
	fg.velBatchGun = qfalse;
	fg.gunDraws = fg.gunOrdinal = fg.gunHeld = 0;
	fg.velOldFbo = glState.currentFBO;
	fg.velBound = qfalse;
	fg.velDraws = fg.velSkipped = fg.velSurfs = 0;
	GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
	r_fgVelPass = 1;

	for ( i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++ ) {
		int base;

		if ( *drawSurf->surface == SF_SPRITE && ( ( (refSprite_t *)drawSurf->surface )->renderfx & RF_DEPTHHACK ) ) {
			fg.build.flash = qtrue;		// a muzzle flash sprite
		}
		if ( *drawSurf->surface == SF_SPRITE || *drawSurf->surface == SF_ENTITY || *drawSurf->surface == SF_FLARE
			|| *drawSurf->surface == SF_MARK_FRAG || *drawSurf->surface == SF_POLY || *drawSurf->surface == SF_SWIPE
			|| *drawSurf->surface == SF_BAD || *drawSurf->surface == SF_SKIP ) {
			continue;
		}
		R_DecomposeSort( drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed, &bStaticModel );
		if ( bStaticModel || entityNum == REFENTITYNUM_WORLD || !shader ) {
			continue;
		}
		if ( entityNum != oldEntityNum || shader != oldShader ) {
			RB_EndSurface();
			tess.numIndexes = tess.numVertexes = 0;
			oldShader = shader;
			if ( entityNum != oldEntityNum ) {
				fgEnt_t *rec;
				int num;

				oldEntityNum = entityNum;
				ent = &backEnd.refdef.entities[entityNum];
				num = ent->e.entityNumber;
				use = qfalse;
				isGun = qfalse;
				gunHold = qfalse;
				fg.velBatchGun = qfalse;
				if ( ent->e.renderfx & ( RF_DEPTHHACK | RF_FIRST_PERSON ) ) {
					// the first-person weapon and hands (drawn with glDepthRange 0..0.3). Its records are in VIEW
					// space - model -> eye then and now - so the camera's turn is not counted a second time.
					const float *nm, *om;
					float dx, dy, dz, xx, zz, lx, lz;

					if ( !fg.gunOK || !( ent->e.renderfx & RF_DEPTHHACK ) ) {
						continue;
					}
					if ( ent->e.renderfx & RF_CROSSHAIR ) {
						fg.gunBad = qtrue;		// drawn with the world's projection and no vectors: the weapon stays unwarped this frame
						continue;
					}
					if ( num < 0 || num >= MAX_GENTITIES || num == ENTITYNUM_NONE ) {
						// a client-side part (a shell casing flying off): nothing tells one from the next, so it has
						// no "before" - it is drawn where it is and keeps the newer frame's place, as it always did
						num = MAX_GENTITIES + 1 + fg.gunOrdinal++;
						gunHold = qtrue;
					}
					skinKey = num + 2 * MAX_GENTITIES + 64;
					backEnd.currentEntity = ent;
					backEnd.currentStaticModel = NULL;
					backEnd.refdef.floatTime = originalTime - (double)ent->e.shaderTime;
					R_RotateForEntity( ent, &backEnd.viewParms, &backEnd.or );
					GL_SetModelviewMatrix( backEnd.or.modelMatrix );
					rec = FG_EntSlot( skinKey, ent->e.hModel, frame );
					if ( !rec || ( rec->seq == frame && memcmp( rec->cur, backEnd.or.modelMatrix, sizeof( rec->cur ) ) ) ) {
						fg.gunBad = qtrue;
						continue;
					}
					if ( rec->seq != frame ) {
						nm = backEnd.or.modelMatrix;
						om = rec->cur;
						dx = nm[12] - om[12];
						dy = nm[13] - om[13];
						dz = nm[14] - om[14];
						xx = nm[0] * om[0] + nm[1] * om[1] + nm[2] * om[2];
						zz = nm[8] * om[8] + nm[9] * om[9] + nm[10] * om[10];
						lx = ( nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2] ) * ( om[0] * om[0] + om[1] * om[1] + om[2] * om[2] );
						lz = ( nm[8] * nm[8] + nm[9] * nm[9] + nm[10] * nm[10] ) * ( om[8] * om[8] + om[9] * om[9] + om[10] * om[10] );
						// not seen in the last frame, or it jumped (weapon switch): it has no "before" - it is
						// drawn where it is, with no displacement, i.e. it keeps the newer frame's place
						rec->havePrev = ( !gunHold && rec->seq == frame - 1 && dx * dx + dy * dy + dz * dz < 24.0f * 24.0f
							&& xx > 0.0f && zz > 0.0f && xx * xx > 0.81f * lx && zz * zz > 0.81f * lz ) ? qtrue : qfalse;
						memcpy( rec->prev, rec->havePrev ? rec->cur : nm, sizeof( rec->prev ) );
						memcpy( rec->cur, nm, sizeof( rec->cur ) );
						rec->seq = frame;
						rec->use = qtrue;
					}
					if ( fg.gunDump > 0 ) {
						model_t *mod = R_GetModelByHandle( ent->e.hModel );
						ri.Printf( PRINT_ALL, "FGGUN frame %d ent %d key %d fx 0x%x model %s prev %d moved ( %.2f %.2f %.2f ) eye ( %.1f %.1f %.1f )\n",
							frame, ent->e.entityNumber, skinKey, ent->e.renderfx, mod ? mod->name : "?", rec->havePrev,
							rec->cur[12] - rec->prev[12], rec->cur[13] - rec->prev[13], rec->cur[14] - rec->prev[14],
							rec->cur[12], rec->cur[13], rec->cur[14] );
					}
					use = qtrue;
					isGun = qtrue;
					memcpy( fg.velModel, rec->cur, sizeof( fg.velModel ) );
					memcpy( fg.velPrevModel, rec->prev, sizeof( fg.velPrevModel ) );
					fg.velNewEntity = qtrue;
					goto entityDone;
				}
				if ( num < 0 || num >= MAX_GENTITIES || num == ENTITYNUM_NONE ) {
					continue;
				}
				skinKey = num;
				backEnd.currentEntity = ent;
				backEnd.currentStaticModel = NULL;
				backEnd.refdef.floatTime = originalTime - (double)ent->e.shaderTime;
				R_RotateForEntity( ent, &backEnd.viewParms, &backEnd.or );
				GL_SetModelviewMatrix( backEnd.or.modelMatrix );
				rec = FG_EntSlot( num, ent->e.hModel, frame );
				if ( !rec ) {
					if ( fg.gunDump > 0 ) {
						ri.Printf( PRINT_ALL, "FGVEL frame %d ent %d: no record slot free\n", frame, num );
					}
					continue;
				}
				if ( rec->seq == frame && memcmp( rec->cur, backEnd.or.transformMatrix, sizeof( rec->cur ) ) ) {
					if ( fg.gunDump > 0 ) {
						model_t *mod = R_GetModelByHandle( ent->e.hModel );
						ri.Printf( PRINT_ALL, "FGVEL frame %d ent %d model %s: a second copy under this number - no vectors\n", frame, num, mod ? mod->name : "?" );
					}
					continue;		// a second one of the same model under this number, somewhere else
				}
				if ( rec->seq != frame ) {
					// first sight of it this frame: last frame's placement becomes "before"
					float dx = backEnd.or.transformMatrix[12] - rec->cur[12];
					float dy = backEnd.or.transformMatrix[13] - rec->cur[13];
					float dz = backEnd.or.transformMatrix[14] - rec->cur[14];

					// turned by more than ~25 degrees in one frame (the model's x and z axes): placed, not turned
					const float *nm = backEnd.or.transformMatrix, *om = rec->cur;
					float xx = nm[0] * om[0] + nm[1] * om[1] + nm[2] * om[2];
					float zz = nm[8] * om[8] + nm[9] * om[9] + nm[10] * om[10];
					float lx = ( nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2] ) * ( om[0] * om[0] + om[1] * om[1] + om[2] * om[2] );
					float lz = ( nm[8] * nm[8] + nm[9] * nm[9] + nm[10] * nm[10] ) * ( om[8] * om[8] + om[9] * om[9] + om[10] * om[10] );

					int fg_dbgSeen = ( rec->seq == frame - 1 );

					rec->havePrev = ( rec->seq == frame - 1
						&& dx * dx + dy * dy + dz * dz < FG_TELEPORT * FG_TELEPORT
						&& xx > 0.0f && zz > 0.0f && xx * xx > 0.81f * lx && zz * zz > 0.81f * lz ) ? qtrue : qfalse;
					if ( rec->havePrev ) {
						memcpy( rec->prev, rec->cur, sizeof( rec->prev ) );
					}
					memcpy( rec->cur, backEnd.or.transformMatrix, sizeof( rec->cur ) );
					if ( !rec->havePrev ) {
						memcpy( rec->prev, rec->cur, sizeof( rec->prev ) );
					}
					rec->seq = frame;
					rec->use = rec->havePrev;
					if ( rec->use ) {
						// nothing about it changed (a dead soldier, a parked truck, a weapon lying there): not a
						// mover this frame. Small on screen and hardly moving: under a pixel either way.
						unsigned int pose = ent->e.tiki ? R_FG_PoseHash( ent ) : 0;
						float ox = nm[12] - backEnd.viewParms.or.origin[0];
						float oy = nm[13] - backEnd.viewParms.or.origin[1];
						float oz = nm[14] - backEnd.viewParms.or.origin[2];
						float px = fabs( backEnd.viewParms.projectionMatrix[5] ) * 0.5f * (float)backEnd.viewParms.viewportHeight
							/ ( sqrt( ox * ox + oy * oy + oz * oz ) + 1.0f );

						if ( pose == rec->pose && !memcmp( rec->cur, rec->prev, sizeof( rec->cur ) ) ) {
							rec->use = qfalse;
						} else if ( ent->e.radius > 0.0f && ent->e.radius * px < 12.0f
							&& sqrt( dx * dx + dy * dy + dz * dz ) * px < 0.6f ) {
							rec->use = qfalse;
						}
						rec->pose = pose;
					} else {
						rec->pose = ent->e.tiki ? R_FG_PoseHash( ent ) : 0;
					}
					if ( fg.gunDump > 0 ) {
						model_t *mod = R_GetModelByHandle( ent->e.hModel );
						ri.Printf( PRINT_ALL, "FGVEL frame %d ent %d model %s fx 0x%x use %d prev %d (seen last frame %d) moved %.2f turned cos %.3f %.3f radius %.1f\n",
							frame, num, mod ? mod->name : "?", ent->e.renderfx, rec->use, rec->havePrev, fg_dbgSeen,
							sqrt( dx * dx + dy * dy + dz * dz ), lx > 0.0f ? xx / sqrt( lx ) : 0.0f, lz > 0.0f ? zz / sqrt( lz ) : 0.0f, ent->e.radius );
					}
				}
				use = rec->use;
				memcpy( fg.velModel, rec->cur, sizeof( fg.velModel ) );
				memcpy( fg.velPrevModel, rec->prev, sizeof( fg.velPrevModel ) );
				fg.velNewEntity = qtrue;
			}
entityDone:
			if ( use ) {
				// opaque and alpha-tested surfaces only: a blended one has no depth of its own in the scene
				if ( shader->sort < SS_OPAQUE || shader->sort > SS_SEE_THROUGH ) {
					if ( isGun && gunHold ) {
						fg.build.flash = qtrue;		// a blended client-side part of the weapon: the muzzle flash model
					}
					oldShader = NULL;
					continue;
				}
				RB_BeginSurface( shader, 0, 0 );
				fg.velBatchMoves = memcmp( fg.velModel, fg.velPrevModel, sizeof( fg.velModel ) ) ? qtrue : qfalse;
				fg.velBatchGun = isGun;
				fg.velBatchHold = isGun && gunHold;
			}
		}
		if ( !use || oldShader == NULL ) {
			continue;
		}
		base = tess.numVertexes;
		rb_surfaceTable[*drawSurf->surface]( drawSurf->surface );
		if ( tess.numVertexes < base ) {
			base = 0;		// the surface function had to flush first
		}
		if ( tess.useInternalVao && !tess.useCacheVao ) {
			FG_FillPrev( skinKey, drawSurf->surface, base, tess.numVertexes - base, frame );
		}
		fg.velSurfs++;
	}
	RB_EndSurface();
	tess.numIndexes = tess.numVertexes = 0;
	r_fgVelPass = 0;
	fg.velBatchGun = qfalse;
	if ( fg.gunDump > 0 ) {
		ri.Printf( PRINT_ALL, "FGGUN frame %d: %d weapon batches, bad %d\n", frame, fg.gunDraws, fg.gunBad );
		fg.gunDump--;
	}

	backEnd.refdef.floatTime = originalTime;
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.or = backEnd.viewParms.world;
	GL_SetModelviewMatrix( backEnd.or.modelMatrix );

	if ( fg.velBound ) {
		ivec4_t right;

		GL_State( GLS_DEPTHTEST_DISABLE );
		GL_Cull( CT_TWO_SIDED );
		// M: outline pixels take their mover's vector
		FBO_Bind( s->fboM );
		qglViewport( 0, 0, s->M->width, s->M->height );
		qglScissor( 0, 0, s->M->width, s->M->height );
		GL_UseProgram( fg.wideProg );
		GL_BindToTMU( fg.MC0, 0 );
		GL_BindToTMU( tr.renderDepthImage, 1 );
		qglUniform1f( fg.uWideShadow, fg.velShadow ? 1.0f : 0.0f );
		if ( fg.velShadow ) {
			qglUniformMatrix4fv( fg.uWideInvVP, 1, GL_FALSE, fg.invCurVP );
			qglUniformMatrix4fv( fg.uWideSunVP, 1, GL_FALSE, fg.sunVP );
			qglUniform3f( fg.uWideSunDir, fg.sunDir[0], fg.sunDir[1], fg.sunDir[2] );
			qglUniform3f( fg.uWideL, (float)( s->M->width * 2 ), 0.0f, (float)s->M->height );
		}
		FG_FullQuad();
		// C: as it is
		VectorSet4( right, s->M->width, 0, s->M->width, s->M->height );
		FBO_FastBlit( fg.fboMC0, right, s->fboC, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		fg.build.midPrev = frame - 1;
		fg.build.vec = qtrue;
		fgs.nVec++;
		if ( fg.gunDraws ) {
			if ( fg.gunBad ) {
				fgs.nGunBad++;
			} else {
				fg.build.gun = qtrue;
				fgs.nGun++;
			}
			fgs.gunDraws += fg.gunDraws;
			fgs.gunHeld += fg.gunHeld;
			if ( fg.build.flash ) {
				fgs.nFlash++;
			}
		}
	}
	fgs.velDraws += fg.velDraws;
	fgs.velSkipped += fg.velSkipped;
	fgs.nVelPass++;
	FBO_Bind( fg.velOldFbo );
	GL_UseProgram( 0 );
}

static GLuint FG_MainProgram( const char *vsSrc, const char *fsSrc, qboolean attribs )
{
	const char *header = ( glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 50 ) ? "#version 150\n" : "#version 130\n";
	GLuint vs, fs, prog;
	GLint ok = 0;

	vs = FG_Compile( GL_VERTEX_SHADER, header, vsSrc );
	fs = FG_Compile( GL_FRAGMENT_SHADER, header, fsSrc );
	if ( !vs || !fs ) {
		return 0;
	}
	prog = qglCreateProgram();
	qglAttachShader( prog, vs );
	qglAttachShader( prog, fs );
	qglBindAttribLocation( prog, ATTR_INDEX_POSITION, "attr_Position" );
	if ( attribs ) {
		qglBindAttribLocation( prog, ATTR_INDEX_TEXCOORD, "attr_TexCoord0" );
		qglBindAttribLocation( prog, ATTR_INDEX_LIGHTCOORD, "attr_TexCoord1" );
	}
	qglLinkProgram( prog );
	qglGetProgramiv( prog, GL_LINK_STATUS, &ok );
	qglDeleteShader( vs );
	qglDeleteShader( fs );
	if ( !ok ) {
		qglDeleteProgram( prog );
		return 0;
	}
	return prog;
}

// the motion-vector textures and programs. Failing here only costs the vectors: the camera-only picture still works.
static qboolean FG_CreateVectorResources( void )
{
	int mw = tr.renderDepthImage->width, mh = tr.renderDepthImage->height;
	int i;

	if ( !glRefConfig.textureFloat ) {
		return qfalse;
	}
	for ( i = 0; i < FG_SETS; i++ ) {
		fgSet_t *s = &fg.sets[i];
		s->M = R_CreateImage( va( "_hzmfgM%d", i ), NULL, mw, mh, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F_ARB );
		s->C = R_CreateImage( va( "_hzmfgC%d", i ), NULL, mw, mh, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F_ARB );
		if ( !s->M || !s->C ) {
			return qfalse;
		}
		s->fboM = FBO_Create( va( "_hzmfgM%d", i ), mw, mh );
		s->fboC = FBO_Create( va( "_hzmfgC%d", i ), mw, mh );
		if ( !s->fboM || !s->fboC ) {
			return qfalse;
		}
		FBO_AttachImage( s->fboM, s->M, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( s->fboC, s->C, GL_COLOR_ATTACHMENT0, 0 );
		if ( !R_CheckFBO( s->fboM ) || !R_CheckFBO( s->fboC ) ) {
			return qfalse;
		}
	}
	if ( mw * 2 + mh > glConfig.maxTextureSize ) {
		return qfalse;
	}
	fg.MC0 = R_CreateImage( "_hzmfgMC0", NULL, mw * 2 + mh, mh, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F_ARB );
	fg.fboMC0 = fg.MC0 ? FBO_Create( "_hzmfgMC0", mw * 2 + mh, mh ) : NULL;
	if ( !fg.fboMC0 ) {
		return qfalse;
	}
	FBO_CreateBuffer( fg.fboMC0, GL_DEPTH_COMPONENT24, 0, 0 );
	FBO_AttachImage( fg.fboMC0, fg.MC0, GL_COLOR_ATTACHMENT0, 0 );
	if ( !R_CheckFBO( fg.fboMC0 ) ) {
		return qfalse;
	}
	fg.wideProg = FG_MainProgram( fg_quad_vs, fg_wide_fs, qfalse );
	if ( !fg.wideProg ) {
		return qfalse;
	}
	GL_UseProgram( fg.wideProg );
	qglUniform1i( qglGetUniformLocation( fg.wideProg, "uM" ), 0 );
	qglUniform1i( qglGetUniformLocation( fg.wideProg, "uD" ), 1 );
	fg.uWideShadow = qglGetUniformLocation( fg.wideProg, "uShadow" );
	fg.uWideInvVP = qglGetUniformLocation( fg.wideProg, "uInvVP" );
	fg.uWideSunVP = qglGetUniformLocation( fg.wideProg, "uSunVP" );
	fg.uWideSunDir = qglGetUniformLocation( fg.wideProg, "uSunDir" );
	fg.uWideL = qglGetUniformLocation( fg.wideProg, "uL" );
	fg.velProg = FG_MainProgram( fg_vel_vs, fg_vel_fs, qtrue );
	if ( !fg.velProg ) {
		return qfalse;
	}
	GL_UseProgram( fg.velProg );
	qglUniform1i( qglGetUniformLocation( fg.velProg, "uDepth" ), 0 );
	fg.uVelMVP = qglGetUniformLocation( fg.velProg, "uMVP" );
	fg.uVelModel = qglGetUniformLocation( fg.velProg, "uModel" );
	fg.uVelPrevModel = qglGetUniformLocation( fg.velProg, "uPrevModel" );
	fg.uVelRigid = qglGetUniformLocation( fg.velProg, "uSkinned" );
	fg.uVelMidVP = qglGetUniformLocation( fg.velProg, "uMidVP" );
	fg.uVelCurVP = qglGetUniformLocation( fg.velProg, "uCurVP" );
	fg.uVelSize = qglGetUniformLocation( fg.velProg, "uSize" );
	fg.uVelPass = qglGetUniformLocation( fg.velProg, "uPass" );
	fg.uVelSunVP = qglGetUniformLocation( fg.velProg, "uSunVP" );
	fg.uVelGunF = qglGetUniformLocation( fg.velProg, "uGunF" );
	GL_UseProgram( 0 );
	return qtrue;
}

/*
=====================================================================================================================
main thread
=====================================================================================================================
*/
static const char *FG_Unsupported( void )
{
	if ( !glRefConfig.framebufferObject || !tr.renderFbo || !glRefConfig.framebufferBlit ) {
		return "no framebuffer objects";
	}
	if ( tr.msaaResolveFbo && tr.renderFbo == tr.sceneFbo ) {
		// r_ext_framebuffer_multisample without render scale: the HUD is drawn into multisample renderbuffers
		return "the legacy multisample path (r_ext_framebuffer_multisample)";
	}
	if ( glConfig.stereoEnabled ) {
		return "stereo rendering";
	}
	if ( !( glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 30 ) || !glRefConfig.vertexArrayObject ) {
		return "needs GLSL 1.30 and vertex array objects";
	}
	if ( !tr.renderDepthImage ) {
		return "no scene depth texture";
	}
	return NULL;
}

static qboolean FG_CreateResources( void )
{
	const char *why = FG_Unsupported();
	int i;

	if ( why ) {
		ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - %s\n", why );
		return qfalse;
	}
	fg.width = glConfig.vidWidth;
	fg.height = glConfig.vidHeight;
	for ( i = 0; i < FG_SETS; i++ ) {
		fgSet_t *s = &fg.sets[i];
		s->S = R_CreateImage( va( "_hzmfgS%d", i ), NULL, fg.width, fg.height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		s->F = R_CreateImage( va( "_hzmfgF%d", i ), NULL, fg.width, fg.height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		s->D = R_CreateImage( va( "*hzmfgD%d", i ), NULL, tr.renderDepthImage->width, tr.renderDepthImage->height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_DEPTH_COMPONENT24 );
		if ( !s->S || !s->F || !s->D ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - could not create its textures\n" );
			return qfalse;
		}
		s->fboS = FBO_Create( va( "_hzmfgS%d", i ), fg.width, fg.height );
		s->fboF = FBO_Create( va( "_hzmfgF%d", i ), fg.width, fg.height );
		if ( !s->fboS || !s->fboF ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - could not create its framebuffers\n" );
			return qfalse;
		}
		FBO_AttachImage( s->fboS, s->S, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( s->fboF, s->F, GL_COLOR_ATTACHMENT0, 0 );
		if ( !R_CheckFBO( s->fboS ) || !R_CheckFBO( s->fboF ) ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - its framebuffers are incomplete\n" );
			return qfalse;
		}
	}
	GL_BindFramebuffer( GL_FRAMEBUFFER, 0 );
	glState.currentFBO = NULL;

	fg.ctx = GLimp_FG_CreateContext();
	if ( !fg.ctx ) {
		ri.Printf( PRINT_ALL, "r_hzmFrameGen: not available - no second GL context\n" );
		return qfalse;
	}
	fg.ctxInit = qfalse;
	fg.FenceSync = (fgFenceSync_t)GLimp_FG_GetProc( "glFenceSync" );
	fg.ClientWaitSync = (fgClientWaitSync_t)GLimp_FG_GetProc( "glClientWaitSync" );
	fg.DeleteSync = (fgDeleteSync_t)GLimp_FG_GetProc( "glDeleteSync" );
	if ( !fg.FenceSync || !fg.ClientWaitSync || !fg.DeleteSync ) {
		fg.FenceSync = NULL;	// the main thread then finishes each frame before publishing it
	}
	ri.Printf( PRINT_ALL, "r_hzmFrameGen: ready (%dx%d, depth %dx%d, %s)\n", fg.width, fg.height,
		tr.renderDepthImage->width, tr.renderDepthImage->height, fg.FenceSync ? "fence sync" : "glFinish" );
	return qtrue;
}

static void FG_ResetBuild( void )
{
	memset( &fg.build, 0, sizeof( fg.build ) );
	fg.build.fadeK = 1.0f;
	fg.latched = qfalse;
	fg.max2D = 0.0f;
}

static void FG_SetMode( void )
{
	int want = r_hzmFrameGen->integer;

	if ( want < 0 || want > 3 ) {
		want = 0;
	}
	if ( fg.mode && fg.thread && fg.threadState != 1 ) {
		want = 0;
	}
	if ( want == fg.mode ) {
		return;
	}
	FG_StopThread();
	fg.mode = 0;
	if ( !want ) {
		return;
	}
	if ( !fg.resTried ) {
		fg.resTried = qtrue;
		fg.resOK = FG_CreateResources();
	}
	if ( !fg.resOK ) {
		ri.Cvar_Set( "r_hzmFrameGen", "0" );
		return;
	}
	FG_ResetBuild();
	fg.slotValid[0] = fg.slotValid[1] = qfalse;
	if ( want == 3 ) {
		fg.w = 2;
	} else {
		fg.w = 0;
		if ( !FG_StartThread() ) {
			ri.Printf( PRINT_ALL, "r_hzmFrameGen: the present thread did not start\n" );
			ri.Cvar_Set( "r_hzmFrameGen", "0" );
			return;
		}
	}
	fg.mode = want;
}

qboolean R_FG_Wanted( void )
{
	return ( fg.mode || ( r_hzmFrameGen && r_hzmFrameGen->integer ) ) ? qtrue : qfalse;
}

void R_FG_LatchView( const void *viewParms )
{
	const viewParms_t *vp = (const viewParms_t *)viewParms;
	int sw, sh, i;

	if ( !fg.mode || fg.build.scene ) {
		return;
	}
	// only a view that fills the picture: anything smaller would need its own rectangle in every pass
	sw = tr.sceneFbo ? tr.sceneFbo->width : glConfig.vidWidth;
	sh = tr.sceneFbo ? tr.sceneFbo->height : glConfig.vidHeight;
	if ( vp->viewportX != 0 || vp->viewportY != 0 || vp->viewportWidth != sw || vp->viewportHeight != sh ) {
		fg.latched = qfalse;
		return;
	}
	for ( i = 0; i < 16; i++ ) {
		fg.latV[i] = vp->world.modelMatrix[i];
		fg.latP[i] = vp->projectionMatrix[i];
		fg.latWP[i] = vp->weaponFovActive ? vp->weaponProjectionMatrix[i] : vp->projectionMatrix[i];
	}
	fg.latched = qtrue;
}

void R_FG_SceneDone( qboolean depthValid )
{
	fgSet_t *s;
	FBO_t *depthFbo, *old;

	if ( !fg.mode || !fg.latched || fg.build.scene || !depthValid ) {
		return;
	}
	s = &fg.sets[fg.w];
	old = glState.currentFBO;

	FBO_FastBlit( tr.renderFbo, NULL, s->fboS, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	// the single-sampled scene depth (tr.renderDepthImage): with MSAA it is the resolve target's, else the scene's own
	depthFbo = tr.msaaResolveFbo ? tr.msaaResolveFbo : ( tr.sceneFbo ? tr.sceneFbo : tr.renderFbo );
	FBO_Bind( depthFbo );
	qglCopyTextureSubImage2DEXT( s->D->texnum, GL_TEXTURE_2D, 0, 0, 0, 0, 0, s->D->width, s->D->height );
	FBO_Bind( old );

	memcpy( fg.build.V, fg.latV, sizeof( fg.build.V ) );
	memcpy( fg.build.P, fg.latP, sizeof( fg.build.P ) );
	fg.build.scene = qtrue;
	fg.latched = qfalse;
	fg.max2D = 0.0f;
}

void R_FG_Set2DWindow( float w, float h, int vpW, int vpH )
{
	fg.orthoW = fabs( w );
	fg.orthoH = fabs( h );
	// a HUD element sets a small 2D window of its own: its quad fills that window, not the screen
	fg.vpShare = ( glConfig.vidWidth > 0 && glConfig.vidHeight > 0 )
		? (float)abs( vpW ) * (float)abs( vpH ) / ( (float)glConfig.vidWidth * (float)glConfig.vidHeight ) : 1.0f;
	if ( fg.vpShare > 1.0f ) {
		fg.vpShare = 1.0f;
	}
}

/*
What pauses generation, and what does not. A 2D quad over a tenth of the screen after the scene used to mean "a
menu is up" whatever it was - and the mod's aiming vignette is such a quad, so nothing was generated while the
player aimed (found 2026-10-12). Now:
  * full-screen UI (menus, console, loading, briefing and score panels, the scope picture): pauses, as before -
    anything big that is not on the list below;
  * the mod's full-screen COSMETIC layers, by shader name (the list below, plus r_hzmFrameGenLayers for more names):
    never pause. One per frame that is a plain alpha-blended picture over the whole screen is drawn again, exactly,
    over the generated picture (not warped, and what is under it is still generated); any other one is left to the
    per-pixel HUD test (the pixels it changes come from the newer real frame);
  * full-screen translucent boxes (damage flash, fades): carried over as K and B, as before.
*/
static const char *fg_cosmeticLayers[] = {
	"textures/hud/coop_ads_vignette",		// cg_drawtools.cpp CG_DrawAdsVignette: black, radial alpha, while aiming
	NULL
};

qboolean R_FG_NoteLayer( const void *shaderV, float w, float h, float s1, float t1, float s2, float t2 )
{
	const shader_t *shader = (const shader_t *)shaderV;
	const shaderStage_t *st;
	qboolean listed = qfalse;
	float a;
	int i;

	if ( !fg.mode || !fg.build.scene || fg.orthoW <= 0.0f || fg.orthoH <= 0.0f || !shader ) {
		return qfalse;
	}
	for ( i = 0; fg_cosmeticLayers[i]; i++ ) {
		if ( !Q_stricmp( shader->name, fg_cosmeticLayers[i] ) ) {
			listed = qtrue;
		}
	}
	if ( !listed && r_hzmFrameGenLayers->string[0] && strstr( r_hzmFrameGenLayers->string, shader->name ) ) {
		listed = qtrue;
	}
	if ( !listed ) {
		return qfalse;
	}
	a = fabs( w * h ) / ( fg.orthoW * fg.orthoH ) * fg.vpShare;
	if ( a < FG_BIG2D ) {
		return qfalse;		// a small one is ordinary HUD
	}
	fg.build.layerSeen = qtrue;
	st = shader->stages[0];
	if ( a >= 0.98f && !fg.build.layerTex && st && st->active && st->bundle[0].image[0]
		&& ( !shader->stages[1] || !shader->stages[1]->active )
		&& ( st->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) ) {
		fg.build.layerTex = st->bundle[0].image[0]->texnum;
		for ( i = 0; i < 4; i++ ) {
			fg.build.layerCol[i] = backEnd.color2D[i] / 255.0f;
		}
		fg.build.layerST[0] = s1;
		fg.build.layerST[1] = t1;
		fg.build.layerST[2] = s2;
		fg.build.layerST[3] = t2;
	}
	return qtrue;
}

void R_FG_Note2D( float w, float h, const char *what )
{
	float a;

	if ( !fg.mode || !fg.build.scene || fg.orthoW <= 0.0f || fg.orthoH <= 0.0f ) {
		return;
	}
	if ( what && what[0] == '(' && what[1] == 'b' && backEnd.color2D[3] == 0 ) {
		return;		// a box drawn with alpha 0 changes nothing
	}
	a = fabs( w * h ) / ( fg.orthoW * fg.orthoH );
	if ( a > 1.0f ) {
		a = 1.0f;
	}
	a *= fg.vpShare;
	if ( what && what[0] == '(' && a >= 0.98f && ( what[1] == 'a' || backEnd.color2D[3] < 230 ) ) {
		// a translucent box over the whole picture (damage flash, fade, tint): final = scene * K + B. The generated
		// picture gets the same K and B; if the guess is off, its pixels simply fail the "untouched by 2D" test and
		// are shown as the real frame has them.
		float al = backEnd.color2D[3] / 255.0f;
		int i;

		if ( what[1] == 'a' ) {
			for ( i = 0; i < 3; i++ ) {
				fg.build.fadeB[i] += backEnd.color2D[i] / 255.0f;
			}
		} else {
			fg.build.fadeK *= 1.0f - al;
			for ( i = 0; i < 3; i++ ) {
				fg.build.fadeB[i] = fg.build.fadeB[i] * ( 1.0f - al ) + backEnd.color2D[i] / 255.0f * al;
			}
		}
		return;
	}
	if ( a > fg.max2D ) {
		fg.max2D = a;
		Com_sprintf( fg.max2DName, sizeof( fg.max2DName ), "%s alpha %d", what ? what : "", (int)backEnd.color2D[3] );
	}
}

// mode 3: one thread, the generated picture goes where the real one would
static int FG_DebugPresent( void )
{
	int dbg = r_hzmFrameGenDebug->integer;
	fgFrame_t cur = fg.build;
	double V[16], P[16];
	qboolean ok;

	float max2D = fg.max2D;

	FG_ResetBuild();
	if ( fg.w != 2 ) {
		// fg_capture: this frame was written straight into a slot
		fg.slot[fg.w] = cur;
		fg.slotValid[fg.w] = cur.scene;
		{
			double o[3];
			FG_ViewOrigin( cur.V, o );
			ri.Printf( PRINT_ALL, "FGCAP slot %d %s at %.1f %.1f %.1f (biggest 2D quad %.2f of the screen: %s)\n", fg.w,
				cur.scene ? "captured" : "has NO scene", o[0], o[1], o[2], max2D, max2D > 0.0f ? fg.max2DName : "-" );
		}
		fg.w = 2;
		return 0;
	}
	if ( dbg <= 0 || !fg.slotValid[0] || !fg.slotValid[1] || !cur.scene ) {
		return 0;
	}
	if ( dbg == 2 || dbg == 4 ) {
		FG_MidCamera( &fg.slot[0], &fg.slot[1], V, P );
	} else {
		memcpy( V, cur.V, sizeof( V ) );
		memcpy( P, cur.P, sizeof( P ) );
	}
	qglFinish();
	if ( !GLimp_FG_MakeCurrent( fg.ctx ) ) {
		GLimp_FG_MakeCurrent( NULL );
		return 0;
	}
	ok = FG_CtxInit() && FG_DrawGenerated( &fg.slot[0], &fg.slot[1], &cur, V, P, dbg >= 3, qfalse );
	qglFinish();
	GLimp_FG_MakeCurrent( NULL );
	return ok ? 2 : 0;
}

int R_FG_Present( void )
{
	fgSet_t *s;
	void *oldSync = NULL;
	int i;

	FG_RecFlush();
	FG_SetMode();
	if ( !fg.mode ) {
		return 0;
	}
	if ( fg.mode == 1 && !fg.vecTried && r_hzmFrameGenVectors->integer ) {
		// the vector textures exist only once the vectors are wanted (about 0.17 GB at 1920x1080)
		fg.vecTried = qtrue;
		fg.vecOK = FG_CreateVectorResources();
		GL_BindFramebuffer( GL_FRAMEBUFFER, 0 );
		glState.currentFBO = NULL;
		GL_UseProgram( 0 );
		ri.Printf( PRINT_ALL, fg.vecOK ? "r_hzmFrameGen: motion vectors ready\n"
			: "r_hzmFrameGen: no motion vectors on this system - things that move keep the newer frame's place\n" );
	}
	s = &fg.sets[fg.w];
	FBO_FastBlit( tr.renderFbo, NULL, s->fboF, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	fg.build.set = fg.w;
	fg.build.seq = ++fg.seq;
	if ( fg.build.scene ) {
		memcpy( fg.lastV, fg.build.V, sizeof( fg.lastV ) );
		memcpy( fg.lastP, fg.build.P, sizeof( fg.lastP ) );
		memcpy( fg.lastWP, fg.latWP, sizeof( fg.lastWP ) );
		fg.lastSeq = fg.build.seq;
	}
	fg.build.bypass = ( fg.max2D >= FG_BIG2D ) ? qtrue : qfalse;
	if ( fg.build.layerSeen ) {
		fgs.nLayer++;
		if ( fg.build.layerTex ) {
			fgs.nLayerExact++;
		}
	}
	fg.lastBypass = fg.build.bypass;
	if ( fg.build.fadeK != 1.0f || fg.build.fadeB[0] + fg.build.fadeB[1] + fg.build.fadeB[2] != 0.0f ) {
		fgs.nFade++;
	}
	if ( fg.build.bypass ) {
		Com_sprintf( fgs.lastOverlay, sizeof( fgs.lastOverlay ), "%.2f %s", fg.max2D, fg.max2DName );
	}

	if ( fg.mode == 3 ) {
		return FG_DebugPresent();
	}

	if ( fg.FenceSync ) {
		fg.build.sync = fg.FenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
		qglFlush();
	} else {
		fg.build.sync = NULL;
		qglFinish();
	}
	fg.build.tPub = SDL_GetPerformanceCounter();

	SDL_LockMutex( fg.mx );
	if ( fg.hasPending ) {
		// the present thread never got to the last one: it is dropped, its set is free again
		oldSync = fg.pending.sync;
		fgs.nDropped++;
	}
	fg.pending = fg.build;
	fg.hasPending = qtrue;
	fg.lastPubSet = fg.w;
	for ( i = 0; i < FG_SETS; i++ ) {
		if ( i != fg.heldPrev && i != fg.heldCur && i != fg.pending.set ) {
			fg.w = i;
			break;
		}
	}
	SDL_CondBroadcast( fg.cv );
	SDL_UnlockMutex( fg.mx );

	if ( oldSync ) {
		fg.DeleteSync( oldSync );
	}
	FG_ResetBuild();
	GLimp_FG_EndFrameNoSwap();
	return 1;
}

void R_FG_NotePresentOff( void )
{
	if ( r_hzmFrameGenStats && r_hzmFrameGenStats->integer ) {
		FG_Record( FGK_REAL );
	}
}

qboolean R_FG_ReadSourceBegin( void )
{
	if ( ( fg.mode != 1 && fg.mode != 2 ) || fg.lastPubSet < 0 ) {
		return qfalse;
	}
	FBO_Bind( fg.sets[fg.lastPubSet].fboF );
	return qtrue;
}

void R_FG_ReadSourceEnd( void )
{
	FBO_Bind( NULL );
}

static int FG_CmpDouble( const void *a, const void *b )
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : ( x > y ? 1 : 0 );
}

static void FG_Stats_f( void )
{
	static const double edges[] = { 2, 4, 6, 8, 10, 12, 14, 17, 20, 25, 33, 50 };
	static double iv[FG_TS];
	int hist[13];
	int n, cnt, i, j, first, even;
	double sum, mean, sd, med, total;
	char line[512];

	if ( ri.Cmd_Argc() > 1 && !Q_stricmp( ri.Cmd_Argv( 1 ), "reset" ) ) {
		memset( &fgs, 0, sizeof( fgs ) );
		ri.Printf( PRINT_ALL, "FGSTAT reset\n" );
		return;
	}
	n = fgs.n;
	cnt = n > FG_TS ? FG_TS : n;
	if ( cnt < 3 ) {
		ri.Printf( PRINT_ALL, "FGSTAT mode=%d presents=%d (nothing to report)\n", fg.mode, cnt );
		return;
	}
	first = n - cnt;
	sum = 0.0;
	memset( hist, 0, sizeof( hist ) );
	for ( i = 1; i < cnt; i++ ) {
		double d = FG_Ms( fgs.t[( first + i - 1 ) & ( FG_TS - 1 )], fgs.t[( first + i ) & ( FG_TS - 1 )] );
		iv[i - 1] = d;
		sum += d;
		for ( j = 0; j < 12 && d >= edges[j]; j++ ) {
		}
		hist[j]++;
	}
	cnt--;
	total = sum;
	mean = sum / cnt;
	sd = 0.0;
	for ( i = 0; i < cnt; i++ ) {
		sd += ( iv[i] - mean ) * ( iv[i] - mean );
	}
	sd = sqrt( sd / cnt );
	qsort( iv, cnt, sizeof( double ), FG_CmpDouble );
	med = iv[cnt / 2];
	even = 0;
	for ( i = 0; i < cnt; i++ ) {
		if ( iv[i] >= med * 0.75 && iv[i] <= med * 1.25 ) {
			even++;
		}
	}
	ri.Printf( PRINT_ALL, "FGSTAT mode=%d presents=%d seconds=%.2f shown_fps=%.1f real=%d gen=%d interval_ms mean=%.2f sd=%.2f min=%.2f p01=%.2f p50=%.2f p99=%.2f max=%.2f even25=%.1f%%\n",
		fg.mode, cnt + 1, total / 1000.0, cnt * 1000.0 / total, fgs.nReal, fgs.nGen,
		mean, sd, iv[0], iv[cnt / 100], med, iv[cnt - 1 - cnt / 100], iv[cnt - 1], even * 100.0 / cnt );
	ri.Printf( PRINT_ALL, "FGSTAT2 noscene=%d overlay=%d fade=%d camjump=%d dropped=%d earlyskip=%d glerr=%d synctimeout=%d syncwait_ms=%.3f gen_ms=%.3f lastoverlay=[%s]\n",
		fgs.nNoScene, fgs.nBypass, fgs.nFade, fgs.nJump, fgs.nDropped, fgs.nEarlySkip, fgs.nGLErr, fgs.nSyncTimeout,
		fgs.nSyncWait ? fgs.syncWaitMs / fgs.nSyncWait : 0.0, fgs.nGenTimed ? fgs.genMs / fgs.nGenTimed : 0.0, fgs.lastOverlay );
	ri.Printf( PRINT_ALL, "FGSTAT3 layerframes=%d layerexact=%d vectors=%d vecframes=%d velpasses=%d veldraws=%d velbrush=%d velskipped=%d prevskin_kb=%d gen_delay_ms=%.2f real_delay_ms=%.2f\n",
		fgs.nLayer, fgs.nLayerExact, ( fg.vecOK && r_hzmFrameGenVectors->integer ) ? 1 : 0, fgs.nVec, fgs.nVelPass, fgs.velDraws, fgs.velBrush, fgs.velSkipped, fgSkinBytes / 1024,
		fgs.nGenDelay ? fgs.genDelayMs / fgs.nGenDelay : 0.0, fgs.nRealDelay ? fgs.realDelayMs / fgs.nRealDelay : 0.0 );
	ri.Printf( PRINT_ALL, "FGSTAT4 gunframes=%d gunbad=%d gunproj=%d gundraws=%d gunheld=%d flashframes=%d fovstep=%d\n",
		fgs.nGun, fgs.nGunBad, fgs.nGunProj, fgs.gunDraws, fgs.gunHeld, fgs.nFlash, fgs.nFovStep );
	for ( j = 0; j < 8 && j < fgs.nStep; j++ ) {
		int k = ( fgs.nStep - 1 - j ) & 7;
		ri.Printf( PRINT_ALL, "FGSTEP frames %d -> %d: camera moved %.1f units, turned cos %.4f, fov factor %.3f: %s\n",
			fgs.step[k].seqA, fgs.step[k].seqB, fgs.step[k].d, fgs.step[k].dot, fgs.step[k].fov, fgs.step[k].cut ? "a cut, nothing generated" : "generated across" );
	}
	line[0] = 0;
	for ( j = 0; j < 13; j++ ) {
		if ( j < 12 ) {
			Q_strcat( line, sizeof( line ), va( " <%g:%.1f%%", edges[j], hist[j] * 100.0 / cnt ) );
		} else {
			Q_strcat( line, sizeof( line ), va( " >=50:%.1f%%", hist[j] * 100.0 / cnt ) );
		}
	}
	ri.Printf( PRINT_ALL, "FGHIST%s\n", line );
}

// main thread: write out what fg_record grabbed
static void FG_RecFlush( void )
{
	int i, n, k, px;
	byte *buf;

	if ( !fgRec.done ) {
		return;
	}
	n = fgRec.count;
	px = fg.width * fg.height;
	buf = (byte *)malloc( 18 + px * 3 );
	for ( i = 0; i < n; i++ ) {
		if ( buf && fgRec.pix[i] ) {
			const byte *src = fgRec.pix[i];
			byte *dst = buf + 18;

			memset( buf, 0, 18 );
			buf[2] = 2;		// uncompressed true colour, origin bottom-left like glReadPixels
			buf[12] = fg.width & 255;
			buf[13] = fg.width >> 8;
			buf[14] = fg.height & 255;
			buf[15] = fg.height >> 8;
			buf[16] = 24;
			for ( k = 0; k < px; k++ ) {
				dst[k * 3] = src[k * 4 + 2];
				dst[k * 3 + 1] = src[k * 4 + 1];
				dst[k * 3 + 2] = src[k * 4];
			}
			ri.FS_WriteFile( va( "screenshots/fgrec_%02d_%c_%05d.tga", i, fgRec.kind[i] == FGK_GEN ? 'G' : 'R',
				(int)( FG_Ms( fgRec.t[0], fgRec.t[i] ) * 10.0 ) ), buf, 18 + px * 3 );
		}
		free( fgRec.pix[i] );
	}
	free( buf );
	memset( &fgRec, 0, sizeof( fgRec ) );
	ri.Printf( PRINT_ALL, "FGREC wrote %d pictures\n", n );
}

static void FG_Rec_f( void )
{
	int n = ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : 32;

	if ( ( fg.mode != 1 && fg.mode != 2 ) || fgRec.want ) {
		ri.Printf( PRINT_ALL, "usage: fg_record <pictures>  (needs r_hzmFrameGen 1 or 2, one recording at a time)\n" );
		return;
	}
	if ( n > FG_REC_MAX ) {
		n = FG_REC_MAX;
	}
	while ( n > 1 && (double)n * fg.width * fg.height * 4.0 > 400.0 * 1024.0 * 1024.0 ) {
		n--;
	}
	fgRec.count = 0;
	fgRec.done = 0;
	fgRec.want = n > 0 ? n : 1;
}

static void FG_GunDump_f( void )
{
	fg.gunDump = ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : 1;
}

static void FG_Capture_f( void )
{
	int n = ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : -1;

	if ( fg.mode != 3 || n < 0 || n > 1 ) {
		ri.Printf( PRINT_ALL, "usage: fg_capture <0|1>  (needs r_hzmFrameGen 3)\n" );
		return;
	}
	// commands run between frames: the next frame goes straight into the slot
	fg.w = n;
}

void R_FG_Init( void )
{
	memset( &fg, 0, sizeof( fg ) );
	memset( &fgs, 0, sizeof( fgs ) );
	fg.heldPrev = fg.heldCur = fg.lastPubSet = -1;
	r_hzmFrameGen = ri.Cvar_Get( "r_hzmFrameGen", "0", 0 );
	r_hzmFrameGenDebug = ri.Cvar_Get( "r_hzmFrameGenDebug", "0", 0 );
	r_hzmFrameGenStats = ri.Cvar_Get( "r_hzmFrameGenStats", "0", 0 );
	// 1 = what moves by itself (soldiers, vehicles, doors) is placed halfway too, from per-pixel motion vectors;
	// 0 = the camera-only picture of the first version (they keep the newer frame's place)
	r_hzmFrameGenVectors = ri.Cvar_Get( "r_hzmFrameGenVectors", "1", 0 );
	// with the vectors: 1 = the ground a mover's sun shadow falls on moves with him (0 = the shadow keeps the newer
	// frame's place and leads its owner by half a real frame). Only while the sun casts character shadows.
	// Measured 2026-10-12: the lead goes from 4.6-5.1 px to 3.2 px, not to nothing - the strip the shadow has to
	// cover behind itself is not filled (that needs the movers drawn from the sun at their HALFWAY pose and the
	// generator asking that map; see docs/proposals/framegen_2026-10-11/2026-10-12_motion_vectors).
	// Default 0 since 2026-10-12 (second look, close range and indoors): the rule knows only "a mover is between this
	// pixel and the sun", not whether the sun reaches either of them - indoors the wall behind an opening door was
	// slid along with the door in every generated picture (m2l2a: 0.4-0.6 % of the screen), and a running
	// soldier's shadow was torn at its edges at close range (the search through M does not converge there). A
	// shadow that leads its owner by half a real frame is whole; this one is not.
	r_hzmFrameGenShadows = ri.Cvar_Get( "r_hzmFrameGenShadows", "0", 0 );
	// with the vectors: 1 = a mover's outline point is also accepted when one of the four depth texels around it in
	// the newer frame is the mover's. Measured 2026-10-12: no difference to a soldier's silhouette area (0.86-0.92
	// of the real one either way - mostly the softer edge of a resampled picture), but thin carried things (a
	// rifle held overhead) are cut off less often with it. 0 = without.
	r_hzmFrameGenEdge = ri.Cvar_Get( "r_hzmFrameGenEdge", "1", 0 );
	// more shader names of full-screen cosmetic layers that must not pause generation (see R_FG_NoteLayer)
	r_hzmFrameGenLayers = ri.Cvar_Get( "r_hzmFrameGenLayers", "", 0 );
	// with the vectors: 1 = the first-person weapon and hands are placed halfway too (their own animation - recoil,
	// reload, bob - is shown at the generated rate); 0 = they are taken unwarped from the newer real frame, as in
	// every build before (their animation plays at the real frame rate).
	r_hzmFrameGenGun = ri.Cvar_Get( "r_hzmFrameGenGun", "1", 0 );
	// with the vectors: 1 = a mover is hidden by something that stands still only when that is really nearer (the
	// distances are compared); 0 = the comparison as it was until 2026-10-12, which took every still background
	// for nearer (kept for comparison: movers lose thin, fast parts and their edges)
	r_hzmFrameGenOccl = ri.Cvar_Get( "r_hzmFrameGenOccl", "1", 0 );
	// with the vectors: what a mover uncovers and neither frame shows is filled from 1 = the nearest background
	// around it (shadowed ground included), 0 = the background along its motion only, as before
	r_hzmFrameGenFill = ri.Cvar_Get( "r_hzmFrameGenFill", "1", 0 );
	// a part of the weapon (one batch: the gun, its magazine, a hand) with a vertex that moves further than this in
	// one frame, in view space (game units), keeps the newer frame's place
	r_hzmFrameGenGunMax = ri.Cvar_Get( "r_hzmFrameGenGunMax", "2.5", 0 );
	// the field of view changing by more than this factor from one frame to the next is a cut: no picture in
	// between (0 = interpolate any step, as before)
	r_hzmFrameGenFovStep = ri.Cvar_Get( "r_hzmFrameGenFovStep", "1.25", 0 );
	fg.mx = SDL_CreateMutex();
	fg.cv = SDL_CreateCond();
	ri.Cmd_AddCommand( "fg_stats", FG_Stats_f );
	ri.Cmd_AddCommand( "fg_capture", FG_Capture_f );
	ri.Cmd_AddCommand( "fg_record", FG_Rec_f );
	ri.Cmd_AddCommand( "fg_gundump", FG_GunDump_f );
}

void R_FG_Shutdown( void )
{
	if ( !fg.mx ) {
		return;		// never initialised
	}
	FG_StopThread();
	R_FG_FlushPrev();
	if ( fg.velProg ) qglDeleteProgram( fg.velProg );

	if ( fg.wideProg ) qglDeleteProgram( fg.wideProg );
	if ( fg.ctx ) {
		// its program and vertex array die with it; the textures and framebuffers are the renderer's own
		GLimp_FG_DeleteContext( fg.ctx );
	}
	ri.Cmd_RemoveCommand( "fg_stats" );
	ri.Cmd_RemoveCommand( "fg_capture" );
	ri.Cmd_RemoveCommand( "fg_record" );
	{
		int i;
		for ( i = 0; i < FG_REC_MAX; i++ ) {
			free( fgRec.pix[i] );
		}
		memset( &fgRec, 0, sizeof( fgRec ) );
	}
	SDL_DestroyCond( fg.cv );
	SDL_DestroyMutex( fg.mx );
	memset( &fg, 0, sizeof( fg ) );
}
