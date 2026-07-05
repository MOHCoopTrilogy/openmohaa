/*
HZM coop - gl1 GLSL post-process layer (implementation).
See tr_postprocess_gl1.h. Pipeline (Approach B): the engine renders the frame NORMALLY to the
backbuffer; RB_PostFxApply copies it into a texture, runs shader passes on the copy, and draws the
result back. No render-target redirect (menus/loading/alt-tab unaffected). All state goes through the
engine's cached helpers (GL_State / GL_Bind) so glState stays consistent (raw calls corrupted it).
Phase 1 effect: bloom (bright-pass -> separable Gaussian blur -> additive composite).
*/
#include "tr_local.h"
#include "tr_postprocess_gl1.h"

// gl1 normally uses qgl_linked.h (direct #define qglX glX) and never declared the FBO/shader
// proc pointers. Those pointers ARE loaded (sdl_glimp.c, gl1 path) and stored as globals there;
// declare the ones we use here via the typedefs from renderercommon/qgl.h (included above).
extern GenFramebuffersproc        *qglGenFramebuffers;
extern BindFramebufferproc        *qglBindFramebuffer;
extern DeleteFramebuffersproc     *qglDeleteFramebuffers;
extern FramebufferTexture2Dproc   *qglFramebufferTexture2D;
extern CheckFramebufferStatusproc *qglCheckFramebufferStatus;
extern CreateShaderproc           *qglCreateShader;
extern ShaderSourceproc           *qglShaderSource;
extern CompileShaderproc          *qglCompileShader;
extern GetShaderivproc            *qglGetShaderiv;
extern GetShaderInfoLogproc       *qglGetShaderInfoLog;
extern DeleteShaderproc           *qglDeleteShader;
extern CreateProgramproc          *qglCreateProgram;
extern AttachShaderproc           *qglAttachShader;
extern LinkProgramproc            *qglLinkProgram;
extern GetProgramivproc           *qglGetProgramiv;
extern GetProgramInfoLogproc      *qglGetProgramInfoLog;
extern DeleteProgramproc          *qglDeleteProgram;
extern UseProgramproc             *qglUseProgram;
extern GetUniformLocationproc     *qglGetUniformLocation;
extern Uniform1iproc              *qglUniform1i;
extern Uniform1fproc              *qglUniform1f;
extern Uniform2fproc              *qglUniform2f;

// FBO / shader enums may be absent from the fixed-function GL headers gl1 includes.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0      0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT       0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                  0x8058
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24      0x81A6
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE          0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0               0x84C0
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER        0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER          0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS         0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS            0x8B82
#endif

typedef struct {
	qboolean inited;
	int      width, height;
	GLuint   sceneFbo;
	GLuint   sceneColor;
	GLuint   sceneDepth;
	GLuint   passProgram;     // passthrough (scene)
	// bloom
	qboolean bloomOk;
	int      bloomW, bloomH;
	GLuint   bloomFbo[2];
	GLuint   bloomTex[2];
	GLuint   brightProgram;   // bright-pass (threshold)
	GLuint   blurProgram;     // separable Gaussian (u_dir)
	GLuint   addProgram;      // texture * u_intensity (drawn additively)
	// ssao (reuses the bloom scratch FBOs; samples sceneDepth)
	qboolean ssaoOk;
	GLuint   ssaoProgram;     // AO from depth -> (r=AO, g=depth)
	GLuint   aoProgram;       // composite: broadcast .r (AO) to rgb for the multiply
	GLuint   ssaoBlurProgram; // depth-aware (bilateral) blur (optional; falls back to blurProgram)
	// depth-of-field (reuses the bloom scratch FBOs for the blurred scene; samples sceneDepth)
	qboolean dofOk;
	GLuint   dofProgram;      // CoC-from-depth composite of the blurred scene over the sharp backbuffer
	// tonemap / color grade + FXAA (full-screen passes that re-copy the backbuffer into sceneColor)
	qboolean tonemapOk;
	GLuint   tonemapProgram;
	qboolean fxaaOk;
	GLuint   fxaaProgram;
	qboolean sharpenOk;
	GLuint   sharpenProgram; // unsharp-mask crispen (counteracts FXAA/mip softening)
	qboolean lowHealthOk;
	GLuint   lowHealthProgram;
	qboolean suppressOk;
	GLuint   suppressProgram;
	qboolean heatHazeOk;
	GLuint   heatHazeProgram;
	qboolean rainOk;
	GLuint   rainProgram;     // rain-on-lens refractive beads
	qboolean godRaysOk;
	GLuint   godRaysProgram;
} postFxGl1_t;

static postFxGl1_t s;

// ------------------------------------------------------------------ shaders
static const char *PASS_VS =
	"#version 120\n"
	"varying vec2 v_uv;\n"
	"void main(){ v_uv = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";

static const char *PASS_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main(){ gl_FragColor = texture2D(u_tex, v_uv); }\n";

// keep only what exceeds the threshold (soft knee via per-pixel scale)
static const char *BRIGHT_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_threshold;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec3 c = texture2D(u_tex, v_uv).rgb;\n"
	"  float b = max(c.r, max(c.g, c.b));\n"
	"  float f = max(b - u_threshold, 0.0) / max(b, 1e-4);\n"
	"  gl_FragColor = vec4(c * f, 1.0);\n"
	"}\n";

// 9-tap separable Gaussian; u_dir is the per-tap texel offset (e.g. (1/w,0) or (0,1/h))
static const char *BLUR_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_dir;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec3 o = texture2D(u_tex, v_uv).rgb * 0.227027;\n"
	"  o += texture2D(u_tex, v_uv + u_dir*1.0).rgb * 0.1945946;\n"
	"  o += texture2D(u_tex, v_uv - u_dir*1.0).rgb * 0.1945946;\n"
	"  o += texture2D(u_tex, v_uv + u_dir*2.0).rgb * 0.1216216;\n"
	"  o += texture2D(u_tex, v_uv - u_dir*2.0).rgb * 0.1216216;\n"
	"  o += texture2D(u_tex, v_uv + u_dir*3.0).rgb * 0.0540541;\n"
	"  o += texture2D(u_tex, v_uv - u_dir*3.0).rgb * 0.0540541;\n"
	"  o += texture2D(u_tex, v_uv + u_dir*4.0).rgb * 0.0162162;\n"
	"  o += texture2D(u_tex, v_uv - u_dir*4.0).rgb * 0.0162162;\n"
	"  gl_FragColor = vec4(o, 1.0);\n"
	"}\n";

static const char *ADD_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_intensity;\n"
	"varying vec2 v_uv;\n"
	"void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb * u_intensity, 1.0); }\n";

// SSAO: crude screen-space ambient occlusion from the depth buffer (no normals). Linearizes depth,
// samples a disc around each pixel, and darkens where neighbours are closer (occluders in front).
// Output is grayscale AO (1=lit, <1=occluded), later multiplied onto the scene.
static const char *SSAO_FS =
	"#version 120\n"
	"uniform sampler2D u_depth;\n"
	"uniform float u_zNear;\n"
	"uniform float u_zFar;\n"
	"uniform float u_radius;\n"
	"uniform float u_bias;\n"
	"uniform float u_intensity;\n"
	"varying vec2 v_uv;\n"
	"float lin(vec2 uv){ float d = texture2D(u_depth, uv).r; return (u_zNear*u_zFar)/(u_zFar - d*(u_zFar-u_zNear)); }\n"
	"float occl(float c, vec2 uv){\n"
	"  float s = lin(uv);\n"
	"  float diff = c - s;\n"
	"  float occ = step(u_bias, diff);\n"
	"  float fade = 1.0 - clamp((diff - u_bias) / (u_radius + 1.0), 0.0, 1.0);\n"
	"  return occ * fade;\n"
	"}\n"
	"void main(){\n"
	"  float c = lin(v_uv);\n"
	"  float r = clamp(u_radius / max(c, 1.0), 0.0, 0.06);\n"
	"  float o = 0.0;\n"
	"  o += occl(c, v_uv + vec2( r,      0.0   ));\n"
	"  o += occl(c, v_uv + vec2(-r,      0.0   ));\n"
	"  o += occl(c, v_uv + vec2( 0.0,    r     ));\n"
	"  o += occl(c, v_uv + vec2( 0.0,   -r     ));\n"
	"  o += occl(c, v_uv + vec2( r*0.7,  r*0.7 ));\n"
	"  o += occl(c, v_uv + vec2(-r*0.7,  r*0.7 ));\n"
	"  o += occl(c, v_uv + vec2( r*0.7, -r*0.7 ));\n"
	"  o += occl(c, v_uv + vec2(-r*0.7, -r*0.7 ));\n"
	"  float ao = clamp(1.0 - (o / 8.0) * u_intensity, 0.0, 1.0);\n"
	"  gl_FragColor = vec4(ao, c / u_zFar, 0.0, 1.0);\n" // r=AO, g=normalized linear depth (for the bilateral blur)
	"}\n";

// composite AO: broadcast the red channel (AO) to rgb. Used to multiply AO onto the scene. Works for both
// the plain-blurred (ao already in r) and depth-aware-blurred (ao in r, depth in g) AO textures.
static const char *AO_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main(){ float a = texture2D(u_tex, v_uv).r; gl_FragColor = vec4(a, a, a, 1.0); }\n";

// depth-aware (bilateral) separable blur for the AO buffer. u_tex.r = AO, u_tex.g = normalized depth.
// Each tap is weighted by a spatial gaussian AND depth similarity to the centre, so AO does not bleed
// across silhouette edges (kills the halo the plain Gaussian produced). u_sharp = depth sensitivity.
static const char *SSAOBLUR_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_dir;\n"
	"uniform float u_sharp;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec2 c = texture2D(u_tex, v_uv).rg;\n"
	"  float cd = c.g;\n"
	"  float sumA = c.r;\n"
	"  float sumW = 1.0;\n"
	"  for (int i = 1; i <= 4; i++) {\n"
	"    float fi = float(i);\n"
	"    float gw = exp(-fi * fi * 0.18);\n"
	"    vec2 sp = texture2D(u_tex, v_uv + u_dir * fi).rg;\n"
	"    vec2 sn = texture2D(u_tex, v_uv - u_dir * fi).rg;\n"
	"    float wp = gw * exp(-abs(sp.g - cd) * u_sharp);\n"
	"    float wn = gw * exp(-abs(sn.g - cd) * u_sharp);\n"
	"    sumA += sp.r * wp + sn.r * wn;\n"
	"    sumW += wp + wn;\n"
	"  }\n"
	"  float ao = sumA / max(sumW, 1e-4);\n"
	"  gl_FragColor = vec4(ao, cd, 0.0, 1.0);\n"
	"}\n";

// Depth-of-field composite: draws the half-res blurred scene (u_tex) over the sharp backbuffer with
// per-pixel alpha = circle-of-confusion from depth. u_focus<=0 -> auto-focus on the center-screen depth.
// Blended SRC_ALPHA/ONE_MINUS_SRC_ALPHA so backbuffer = sharp*(1-coc) + blurred*coc.
static const char *DOF_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"    // blurred scene (half-res)
	"uniform sampler2D u_depth;\n"  // full-res scene depth
	"uniform float u_zNear;\n"
	"uniform float u_zFar;\n"
	"uniform float u_focus;\n"      // fixed focus distance (linear); <=0 = auto
	"uniform float u_range;\n"      // sharp-band falloff scale
	"uniform float u_intensity;\n"  // max blur mix 0..1
	"varying vec2 v_uv;\n"
	"float lin(vec2 uv){ float d = texture2D(u_depth, uv).r; return (u_zNear*u_zFar)/(u_zFar - d*(u_zFar-u_zNear)); }\n"
	"void main(){\n"
	"  float fd  = (u_focus > 0.0) ? u_focus : lin(vec2(0.5, 0.5));\n"
	"  float d   = lin(v_uv);\n"
	"  float coc = clamp(abs(d - fd) / max(u_range, 1.0), 0.0, 1.0) * u_intensity;\n"
	"  gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, coc);\n"
	"}\n";

// Tonemap + color grade: exposure -> ACES filmic (Narkowicz approx) -> contrast about mid-grey -> saturation.
static const char *TONEMAP_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_exposure;\n"
	"uniform float u_contrast;\n"
	"uniform float u_saturation;\n"
	"uniform float u_temp;\n"   // warm(+) / cool(-) white-balance tint for color-grade presets
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec3 c = texture2D(u_tex, v_uv).rgb * u_exposure;\n"
	"  c = clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14), 0.0, 1.0);\n" // ACES
	"  c = clamp((c - 0.5) * u_contrast + 0.5, 0.0, 1.0);\n"
	"  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"  c = clamp(mix(vec3(l), c, u_saturation), 0.0, 1.0);\n"
	"  c.r = clamp(c.r * (1.0 + u_temp), 0.0, 1.0);\n"   // tint: warm boosts red, cools blue
	"  c.b = clamp(c.b * (1.0 - u_temp), 0.0, 1.0);\n"
	"  gl_FragColor = vec4(c, 1.0);\n"
	"}\n";

// FXAA (Timothy Lottes' compact luma edge AA). u_rcpFrame = (1/width, 1/height).
static const char *FXAA_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_rcpFrame;\n"
	"varying vec2 v_uv;\n"
	"float luma(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
	"void main(){\n"
	"  vec2 px = u_rcpFrame;\n"
	"  vec3 rgbM = texture2D(u_tex, v_uv).rgb;\n"
	"  float lM  = luma(rgbM);\n"
	"  float lNW = luma(texture2D(u_tex, v_uv + vec2(-px.x,-px.y)).rgb);\n"
	"  float lNE = luma(texture2D(u_tex, v_uv + vec2( px.x,-px.y)).rgb);\n"
	"  float lSW = luma(texture2D(u_tex, v_uv + vec2(-px.x, px.y)).rgb);\n"
	"  float lSE = luma(texture2D(u_tex, v_uv + vec2( px.x, px.y)).rgb);\n"
	"  float lMin = min(lM, min(min(lNW,lNE), min(lSW,lSE)));\n"
	"  float lMax = max(lM, max(max(lNW,lNE), max(lSW,lSE)));\n"
	"  vec2 dir;\n"
	"  dir.x = -((lNW + lNE) - (lSW + lSE));\n"
	"  dir.y =  ((lNW + lSW) - (lNE + lSE));\n"
	"  float dirReduce = max((lNW+lNE+lSW+lSE) * 0.25 * 0.0625, 1.0/128.0);\n"
	"  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
	"  dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;\n"
	"  vec3 rgbA = 0.5 * (texture2D(u_tex, v_uv + dir*(1.0/3.0-0.5)).rgb + texture2D(u_tex, v_uv + dir*(2.0/3.0-0.5)).rgb);\n"
	"  vec3 rgbB = rgbA*0.5 + 0.25 * (texture2D(u_tex, v_uv + dir*(-0.5)).rgb + texture2D(u_tex, v_uv + dir*0.5).rgb);\n"
	"  float lB = luma(rgbB);\n"
	"  gl_FragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);\n"
	"}\n";

// Sharpen: cross-kernel unsharp mask. Adds back (center - blurred-neighbours) * amount. Cheap crispen that
// counteracts the softening from FXAA / mip filtering / upscaling. u_amount ~0.2-0.5 (higher = crunchier).
// HZM - Contrast-adaptive sharpening (CAS-style): per-pixel weight scales with local
// contrast headroom, so flat areas and already-hard edges stay untouched - no halos,
// unlike the plain unsharp mask this replaced. u_amount 0..1 = sharpness.
static const char *SHARPEN_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_rcp;\n"
	"uniform float u_amount;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec3 e = texture2D(u_tex, v_uv).rgb;\n"
	"  vec3 b = texture2D(u_tex, v_uv + vec2(0.0,-u_rcp.y)).rgb;\n"
	"  vec3 h = texture2D(u_tex, v_uv + vec2(0.0, u_rcp.y)).rgb;\n"
	"  vec3 d = texture2D(u_tex, v_uv + vec2(-u_rcp.x,0.0)).rgb;\n"
	"  vec3 f = texture2D(u_tex, v_uv + vec2( u_rcp.x,0.0)).rgb;\n"
	"  vec3 mn = min(min(min(d,e),min(f,b)),h);\n"
	"  vec3 mx = max(max(max(d,e),max(f,b)),h);\n"
	"  vec3 amp = clamp(min(mn, vec3(2.0)-mx) / (mx + vec3(1e-5)), 0.0, 1.0);\n"
	"  amp = sqrt(amp);\n"
	"  float peak = -1.0 / mix(8.0, 5.0, clamp(u_amount,0.0,1.0));\n"
	"  vec3 w = amp * peak;\n"
	"  vec3 sharp = ((b+d+f+h)*w + e) / (4.0*w + vec3(1.0));\n"
	"  gl_FragColor = vec4(clamp(sharp,0.0,1.0), 1.0);\n"
	"}\n";

// Low-health screen effect: desaturate toward grey + a dark-red vignette that grows as u_hurt -> 1
// (u_hurt is the already-ramped/pulsed intensity computed on the CPU from the health fraction).
static const char *LOWHEALTH_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_hurt;\n"   // 0 = healthy, 1 = near death
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec3 c = texture2D(u_tex, v_uv).rgb;\n"
	"  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"  c = mix(c, vec3(l), u_hurt * 0.5);\n"                 // drain colour as you weaken
	// luminance-PROPORTIONAL red: a bright wall becomes bright-red, a dark hall becomes dark-red, so the
	// tint reads the SAME no matter how bright the room is. (The old fixed vec3(0.45,0,0) looked vivid in
	// dark halls but washed out in bright rooms -> the "it changes as I move around the world" complaint.)
	"  vec3 red = vec3(l) * vec3(1.0, 0.30, 0.26);\n"
	"  vec2 d = v_uv - vec2(0.5);\n"
	"  float vig = clamp(dot(d, d) * 2.2, 0.0, 1.0);\n"      // 0 centre -> 1 at edges (screen-space, scene-independent)
	"  float amt = u_hurt * (0.30 + 0.50 * vig);\n"          // whole-frame red wash + heavier at the edges
	"  c = mix(c, red, amt);\n"
	"  c *= (1.0 - vig * u_hurt * 0.22);\n"                  // gentle edge darken
	"  gl_FragColor = vec4(c, 1.0);\n"
	"}\n";

// Suppression screen effect: when enemy rounds crack past you / you take fire, the world briefly
// desaturates and a dark TUNNEL vignette closes in (the flinch/"keep your head down" instinct). Driven by
// u_suppress (0..1), a decaying intensity the cgame spikes on near-miss bullet zings + taking damage.
static const char *SUPPRESSION_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_suppress;\n"   // 0 = calm, 1 = pinned down
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec2 d = v_uv - vec2(0.5);\n"
	"  float vig = clamp(dot(d, d) * 2.4, 0.0, 1.0);\n"         // 0 centre -> 1 at the edges
	"  float s = clamp(u_suppress, 0.0, 1.0);\n"                // 0 calm -> 1 pinned down
	"  vec3 c = texture2D(u_tex, v_uv).rgb;\n"
	// peripheral BLUR that grows with how pinned-down you are -> tunnel-vision smear at the edges.
	"  float blurAmt = clamp(s * (0.35 + vig * 1.4), 0.0, 0.92);\n"
	"  if (blurAmt > 0.01) {\n"
	"    vec2 o = vec2(0.0035 + 0.004 * s);\n"
	"    vec3 b = texture2D(u_tex, v_uv + vec2( o.x,  o.y)).rgb\n"
	"           + texture2D(u_tex, v_uv + vec2(-o.x,  o.y)).rgb\n"
	"           + texture2D(u_tex, v_uv + vec2( o.x, -o.y)).rgb\n"
	"           + texture2D(u_tex, v_uv + vec2(-o.x, -o.y)).rgb;\n"
	"    c = mix(c, b * 0.25, blurAmt);\n"
	"  }\n"
	"  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"  c = mix(c, vec3(l), s * 0.80);\n"                        // desaturate hard, scaling with fire
	"  c *= (1.0 - vig * s * 0.72);\n"                          // close a darker TUNNEL as suppression rises
	"  gl_FragColor = vec4(c, 1.0);\n"
	"}\n";

// Heat haze: a subtle animated UV refraction (shimmer) over the scene, scaled by u_heat - the cgame spikes
// it near explosions and it eases back. Two crossing sine waves warp the sample coords; amplitude grows
// with u_heat. u_time (seconds) animates the ripple.
static const char *HEATHAZE_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_heat;\n"     // FULLSCREEN heat (explosions) 0..1
	"uniform float u_time;\n"     // seconds, animates the ripple
	"uniform float u_muzHeat;\n"  // LOCALIZED gun-muzzle heat 0..1
	"uniform vec2  u_muzCtr;\n"   // muzzle point on screen (UV)
	"uniform float u_muzRad;\n"   // localized falloff radius (UV)
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  float amp = u_heat * 0.030;\n"                             // fullscreen warp (explosions)
	"  vec2 uv = v_uv;\n"
	"  uv.x += sin(v_uv.y * 42.0 + u_time * 8.0) * amp;\n"
	"  uv.y += sin(v_uv.x * 34.0 + u_time * 6.2) * amp * 0.7;\n"
	"  uv.x += sin(v_uv.y * 17.0 - u_time * 4.5) * amp * 0.5;\n"  // a second, slower wave -> richer shimmer
	// LOCALIZED MUZZLE SHIMMER: a tight, fast heat ripple concentrated around the gun's muzzle point on
	// screen, so gunfire heat reads as rising OFF THE BARREL instead of warping the whole frame. The warp
	// fades to nothing past u_muzRad (squared falloff keeps it tight around the barrel).
	"  float md = distance(v_uv, u_muzCtr);\n"
	"  float mf = u_muzHeat * clamp(1.0 - md / max(u_muzRad, 0.001), 0.0, 1.0);\n"
	"  mf = mf * mf;\n"
	"  float mamp = mf * 0.045;\n"
	"  uv.x += sin(v_uv.y * 95.0 + u_time * 17.0) * mamp;\n"
	"  uv.y += sin(v_uv.x * 85.0 + u_time * 14.0) * mamp;\n"
	"  uv.x += sin(v_uv.y * 50.0 - u_time * 11.0) * mamp * 0.6;\n"
	"  gl_FragColor = vec4(texture2D(u_tex, uv).rgb, 1.0);\n"
	"}\n";

// Rain on the lens: procedural water beads over the scene when the player is outdoors in rain. Two grid
// layers of droplets (different scales = depth) drift downward; each bead REFRACTS the scene under it (the
// sample coord is pushed toward the bead centre, like a tiny lens) plus a faint wet glint. u_wet (0..1) is
// the cgame-published wetness (rain intensity AND under-open-sky, eased so beads build/dry) - everything
// scales by it, so u_wet 0 = the scene passes through untouched. u_aspect keeps the beads round.
static const char *RAINDROPS_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_wet;\n"     // 0 dry .. 1 streaming
	"uniform float u_time;\n"    // seconds
	"uniform float u_aspect;\n"  // width/height
	"varying vec2 v_uv;\n"
	"float h21(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
	// one grid layer of beads -> returns (refraction dir .xy, bead mask .z)
	"vec3 dropLayer(vec2 uv, float cells, float speed, float t){\n"
	"  vec2 suv = vec2(uv.x * u_aspect, uv.y);\n"           // square cells so beads aren't oval
	"  vec2 g = suv * cells;\n"
	"  g.y -= t * speed;\n"                                  // the whole layer drifts DOWN (gravity; v increases upward here)
	"  vec2 id = floor(g);\n"
	"  vec2 f  = fract(g) - 0.5;\n"
	"  float present = step(0.55, h21(id + 11.3));\n"        // ~45% of cells carry a bead
	"  vec2 c = vec2((h21(id+1.7)-0.5)*0.5, (h21(id+5.1)-0.5)*0.5);\n" // jitter within the cell
	"  float rad = 0.16 + 0.18 * h21(id + 3.9);\n"
	"  float d = length(f - c);\n"
	"  float drop = present * smoothstep(rad, rad*0.3, d);\n"
	"  return vec3((f - c) * drop, drop);\n"                 // dir toward centre * mask, + mask
	"}\n"
	// PHASE 2 - vertical TRICKLES: a drop head slides DOWN a column (increasing y, matching the beads) and
	// leaves a fading wet trail above it. Returns (refraction dir .xy, mask .z), same contract as dropLayer.
	"vec3 trickleLayer(vec2 uv, float cols, float t){\n"
	"  float colf = uv.x * u_aspect * cols;\n"
	"  float col  = floor(colf);\n"
	"  float seed = h21(vec2(col, 7.3));\n"
	"  float present = step(0.62, h21(vec2(col, 2.1)));\n"        // ~38% of columns carry a trickle
	"  float spd  = 0.10 + seed * 0.22;\n"
	"  float hy   = 1.0 - fract(t * spd + seed * 11.0);\n"        // head Y slides DOWN (v increases upward here)
	"  float cx   = col + 0.35 + 0.30 * h21(vec2(col, 5.0));\n"   // jittered column centre (colf units)
	"  float dx   = colf - cx;\n"
	"  float xmask = smoothstep(0.42, 0.0, abs(dx));\n"           // across the streak width
	"  float above = uv.y - hy;\n"                                // >0 = pixel is ABOVE the head (where the drop fell from = the trail)
	"  float trail = smoothstep(0.16 + seed * 0.20, 0.0, above) * step(0.0, above);\n"
	"  float head  = smoothstep(0.03, 0.0, abs(uv.y - hy));\n"    // bright bead at the head
	"  float amt   = present * xmask * max(trail * 0.45, head);\n"
	"  return vec3(-dx * 0.5 * amt, head * 0.6 * amt, amt);\n"    // refract toward centre + a tug at the head
	"}\n"
	"void main(){\n"
	"  float wet = clamp(u_wet, 0.0, 1.0);\n"
	"  vec3 a = dropLayer(v_uv, 8.0,  0.06, u_time);\n"
	"  vec3 b = dropLayer(v_uv, 14.0, 0.10, u_time + 27.0);\n"
	"  vec3 c = trickleLayer(v_uv, 7.0, u_time);\n"           // PHASE 2 runs
	"  vec2 dir = a.xy + b.xy * 0.8 + c.xy;\n"
	"  float mask = clamp(a.z + b.z + c.z, 0.0, 1.0);\n"
	"  vec2 off = dir * vec2(1.0/u_aspect, 1.0) * 0.05 * wet;\n" // refraction (undo aspect on x)
	"  vec3 col = texture2D(u_tex, v_uv + off).rgb;\n"
	"  col += mask * 0.07 * wet;\n"                          // faint wet glint on beads + trickles
	"  gl_FragColor = vec4(col, 1.0);\n"
	"}\n";

// God rays / sun shafts: radial accumulation of the bright (sun/sky) buffer from the on-screen sun point.
// Marches N samples from each pixel toward u_sun, summing brightness with a per-step decay -> light shafts.
static const char *GODRAYS_FS =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"   // bright-pass of the scene (sun/sky), half-res
	"uniform vec2 u_sun;\n"        // sun screen position in [0,1] UV
	"uniform float u_decay;\n"     // per-step illumination falloff
	"uniform float u_weight;\n"    // per-sample weight
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  vec2 delta = (v_uv - u_sun) * (1.0 / 32.0);\n"
	"  vec2 uv = v_uv;\n"
	"  vec3 col = vec3(0.0);\n"
	"  float illum = 1.0;\n"
	"  for (int i = 0; i < 32; i++) {\n"
	"    uv -= delta;\n"
	"    col += texture2D(u_tex, uv).rgb * (illum * u_weight);\n"
	"    illum *= u_decay;\n"
	"  }\n"
	"  gl_FragColor = vec4(col, 1.0);\n"
	"}\n";

// ------------------------------------------------------------------ helpers
static GLuint R_PostFx_CompileShader( GLenum type, const char *src ) {
	GLuint sh = qglCreateShader( type );
	GLint  ok = 0;
	if ( !sh ) return 0;
	qglShaderSource( sh, 1, (const GLchar **)&src, NULL );
	qglCompileShader( sh );
	qglGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if ( !ok ) {
		char log[1024]; log[0] = 0;
		qglGetShaderInfoLog( sh, sizeof(log), NULL, log );
		ri.Printf( PRINT_WARNING, "postfx: shader compile failed: %s\n", log );
		qglDeleteShader( sh );
		return 0;
	}
	return sh;
}

static GLuint R_PostFx_CompileProgram( const char *vs, const char *fs ) {
	GLuint v = R_PostFx_CompileShader( GL_VERTEX_SHADER, vs );
	GLuint f = R_PostFx_CompileShader( GL_FRAGMENT_SHADER, fs );
	GLuint p;
	GLint  ok = 0;
	if ( !v || !f ) { if (v) qglDeleteShader(v); if (f) qglDeleteShader(f); return 0; }
	p = qglCreateProgram();
	if ( !p ) { qglDeleteShader(v); qglDeleteShader(f); return 0; }
	qglAttachShader( p, v );
	qglAttachShader( p, f );
	qglLinkProgram( p );
	qglGetProgramiv( p, GL_LINK_STATUS, &ok );
	qglDeleteShader( v );
	qglDeleteShader( f );
	if ( !ok ) {
		char log[1024]; log[0] = 0;
		qglGetProgramInfoLog( p, sizeof(log), NULL, log );
		ri.Printf( PRINT_WARNING, "postfx: program link failed: %s\n", log );
		qglDeleteProgram( p );
		return 0;
	}
	return p;
}

static qboolean R_PostFx_MakeColorFbo( GLuint *fbo, GLuint *tex, int w, int h ) {
	GLenum st;
	qglGenTextures( 1, tex );
	qglBindTexture( GL_TEXTURE_2D, *tex );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	qglGenFramebuffers( 1, fbo );
	qglBindFramebuffer( GL_FRAMEBUFFER, *fbo );
	qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0 );
	st = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	return (qboolean)( st == GL_FRAMEBUFFER_COMPLETE );
}

// NDC fullscreen quad. The VS passes gl_Vertex straight through (clip space), so this covers
// whatever framebuffer/viewport is currently bound. Caller sets program + uniforms + textures.
static void R_PostFx_FSQuad( void ) {
	qglBegin( GL_QUADS );
		qglTexCoord2f( 0.0f, 0.0f ); qglVertex2f( -1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 0.0f ); qglVertex2f(  1.0f, -1.0f );
		qglTexCoord2f( 1.0f, 1.0f ); qglVertex2f(  1.0f,  1.0f );
		qglTexCoord2f( 0.0f, 1.0f ); qglVertex2f( -1.0f,  1.0f );
	qglEnd();
}

static void R_PostFx_SetTex( GLuint program ) {
	GLint loc = qglGetUniformLocation( program, "u_tex" );
	if ( loc >= 0 ) qglUniform1i( loc, 0 );
}

// ------------------------------------------------------------------ lifecycle
void R_InitPostFxGL1( void ) {
	GLenum st;
	R_ShutdownPostFxGL1();	// free any prior resources (e.g. on vid_restart) before rebuilding
	if ( !glPostFxProcsLoaded ) {
		ri.Printf( PRINT_ALL, "postfx: disabled (procs unavailable)\n" );
		return;
	}
	if ( !qglGenFramebuffers || !qglBindFramebuffer || !qglFramebufferTexture2D || !qglCheckFramebufferStatus ||
	     !qglDeleteFramebuffers || !qglCreateShader || !qglShaderSource || !qglCompileShader || !qglGetShaderiv ||
	     !qglGetShaderInfoLog || !qglDeleteShader || !qglCreateProgram || !qglAttachShader || !qglLinkProgram ||
	     !qglGetProgramiv || !qglGetProgramInfoLog || !qglDeleteProgram || !qglUseProgram ||
	     !qglGetUniformLocation || !qglUniform1i || !qglUniform1f || !qglUniform2f ) {
		ri.Printf( PRINT_WARNING, "postfx: a required GL proc is NULL - disabling post-FX (gl1 unchanged)\n" );
		return;
	}
	ri.Printf( PRINT_ALL, "postfx: init begin %dx%d\n", glConfig.vidWidth, glConfig.vidHeight );
	s.width  = glConfig.vidWidth;
	s.height = glConfig.vidHeight;

	qglGenTextures( 1, &s.sceneColor );
	qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, s.width, s.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	qglGenTextures( 1, &s.sceneDepth );
	qglBindTexture( GL_TEXTURE_2D, s.sceneDepth );
	qglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, s.width, s.height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	qglGenFramebuffers( 1, &s.sceneFbo );
	qglBindFramebuffer( GL_FRAMEBUFFER, s.sceneFbo );
	qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.sceneColor, 0 );
	qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, s.sceneDepth, 0 );
	st = qglCheckFramebufferStatus( GL_FRAMEBUFFER );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	s.passProgram = R_PostFx_CompileProgram( PASS_VS, PASS_FS );
	s.inited = ( st == GL_FRAMEBUFFER_COMPLETE ) && ( s.passProgram != 0 );
	ri.Printf( PRINT_ALL, "postfx: scene FBO %dx%d status=0x%x %s; passthrough program %s\n",
		s.width, s.height, (unsigned)st,
		(st == GL_FRAMEBUFFER_COMPLETE) ? "COMPLETE" : "FAILED",
		s.passProgram ? "linked" : "FAILED" );

	// ---- bloom: half-res scratch FBOs (ping-pong) + the 3 effect programs
	s.bloomW = s.width  / 2; if ( s.bloomW < 1 ) s.bloomW = 1;
	s.bloomH = s.height / 2; if ( s.bloomH < 1 ) s.bloomH = 1;
	{
		qboolean f0 = R_PostFx_MakeColorFbo( &s.bloomFbo[0], &s.bloomTex[0], s.bloomW, s.bloomH );
		qboolean f1 = R_PostFx_MakeColorFbo( &s.bloomFbo[1], &s.bloomTex[1], s.bloomW, s.bloomH );
		s.brightProgram = R_PostFx_CompileProgram( PASS_VS, BRIGHT_FS );
		s.blurProgram   = R_PostFx_CompileProgram( PASS_VS, BLUR_FS );
		s.addProgram    = R_PostFx_CompileProgram( PASS_VS, ADD_FS );
		s.bloomOk = (qboolean)( s.inited && f0 && f1 && s.brightProgram && s.blurProgram && s.addProgram );
		ri.Printf( PRINT_ALL, "postfx: bloom %dx%d %s\n", s.bloomW, s.bloomH, s.bloomOk ? "ready" : "DISABLED" );
	}

	// ---- ssao: reuses the bloom scratch FBOs; samples the copied sceneDepth
	s.ssaoProgram     = R_PostFx_CompileProgram( PASS_VS, SSAO_FS );
	s.aoProgram       = R_PostFx_CompileProgram( PASS_VS, AO_FS );
	s.ssaoBlurProgram = R_PostFx_CompileProgram( PASS_VS, SSAOBLUR_FS );	// optional; plain blur is the fallback
	s.ssaoOk = (qboolean)( s.inited && s.ssaoProgram && s.aoProgram && s.blurProgram &&
	                       s.bloomFbo[0] && s.bloomFbo[1] && s.sceneDepth );
	ri.Printf( PRINT_ALL, "postfx: ssao %s (depth-aware blur %s)\n",
		s.ssaoOk ? "ready" : "DISABLED", s.ssaoBlurProgram ? "available" : "unavailable" );

	// ---- depth of field: blurred scene (bloom scratch + blurProgram) composited via depth CoC
	s.dofProgram = R_PostFx_CompileProgram( PASS_VS, DOF_FS );
	s.dofOk = (qboolean)( s.inited && s.dofProgram && s.passProgram && s.blurProgram &&
	                      s.bloomFbo[0] && s.bloomFbo[1] && s.sceneDepth );
	ri.Printf( PRINT_ALL, "postfx: dof %s\n", s.dofOk ? "ready" : "DISABLED" );

	// ---- tonemap/color-grade + FXAA: full-screen passes over the finished backbuffer (reuse sceneColor)
	s.tonemapProgram = R_PostFx_CompileProgram( PASS_VS, TONEMAP_FS );
	s.tonemapOk = (qboolean)( s.inited && s.tonemapProgram && s.sceneColor );
	s.fxaaProgram = R_PostFx_CompileProgram( PASS_VS, FXAA_FS );
	s.fxaaOk = (qboolean)( s.inited && s.fxaaProgram && s.sceneColor );
	s.sharpenProgram = R_PostFx_CompileProgram( PASS_VS, SHARPEN_FS );
	s.sharpenOk = (qboolean)( s.inited && s.sharpenProgram && s.sceneColor );
	s.lowHealthProgram = R_PostFx_CompileProgram( PASS_VS, LOWHEALTH_FS );
	s.lowHealthOk = (qboolean)( s.inited && s.lowHealthProgram && s.sceneColor );
	s.suppressProgram = R_PostFx_CompileProgram( PASS_VS, SUPPRESSION_FS );
	s.suppressOk = (qboolean)( s.inited && s.suppressProgram && s.sceneColor );
	s.heatHazeProgram = R_PostFx_CompileProgram( PASS_VS, HEATHAZE_FS );
	s.heatHazeOk = (qboolean)( s.inited && s.heatHazeProgram && s.sceneColor );
	s.rainProgram = R_PostFx_CompileProgram( PASS_VS, RAINDROPS_FS );
	s.rainOk = (qboolean)( s.inited && s.rainProgram && s.sceneColor );
	s.godRaysProgram = R_PostFx_CompileProgram( PASS_VS, GODRAYS_FS );
	s.godRaysOk = (qboolean)( s.inited && s.godRaysProgram && s.brightProgram && s.addProgram &&
	                          s.bloomFbo[0] && s.bloomFbo[1] && s.sceneColor );
	ri.Printf( PRINT_ALL, "postfx: tonemap %s, fxaa %s, lowhealth %s, godrays %s\n",
		s.tonemapOk ? "ready" : "DISABLED", s.fxaaOk ? "ready" : "DISABLED",
		s.lowHealthOk ? "ready" : "DISABLED", s.godRaysOk ? "ready" : "DISABLED" );
}

void R_ShutdownPostFxGL1( void ) {
	if ( s.passProgram )   qglDeleteProgram( s.passProgram );
	if ( s.brightProgram ) qglDeleteProgram( s.brightProgram );
	if ( s.blurProgram )   qglDeleteProgram( s.blurProgram );
	if ( s.addProgram )    qglDeleteProgram( s.addProgram );
	if ( s.ssaoProgram )     qglDeleteProgram( s.ssaoProgram );
	if ( s.aoProgram )       qglDeleteProgram( s.aoProgram );
	if ( s.ssaoBlurProgram ) qglDeleteProgram( s.ssaoBlurProgram );
	if ( s.dofProgram )      qglDeleteProgram( s.dofProgram );
	if ( s.tonemapProgram )  qglDeleteProgram( s.tonemapProgram );
	if ( s.fxaaProgram )     qglDeleteProgram( s.fxaaProgram );
	if ( s.sharpenProgram )  qglDeleteProgram( s.sharpenProgram );
	if ( s.lowHealthProgram ) qglDeleteProgram( s.lowHealthProgram );
	if ( s.suppressProgram )  qglDeleteProgram( s.suppressProgram );
	if ( s.heatHazeProgram )  qglDeleteProgram( s.heatHazeProgram );
	if ( s.rainProgram )      qglDeleteProgram( s.rainProgram );
	if ( s.godRaysProgram )   qglDeleteProgram( s.godRaysProgram );
	if ( s.sceneFbo )      qglDeleteFramebuffers( 1, &s.sceneFbo );
	if ( s.bloomFbo[0] )   qglDeleteFramebuffers( 1, &s.bloomFbo[0] );
	if ( s.bloomFbo[1] )   qglDeleteFramebuffers( 1, &s.bloomFbo[1] );
	if ( s.sceneColor )    qglDeleteTextures( 1, &s.sceneColor );
	if ( s.sceneDepth )    qglDeleteTextures( 1, &s.sceneDepth );
	if ( s.bloomTex[0] )   qglDeleteTextures( 1, &s.bloomTex[0] );
	if ( s.bloomTex[1] )   qglDeleteTextures( 1, &s.bloomTex[1] );
	memset( &s, 0, sizeof(s) );
}

qboolean R_PostFxActive( void ) {
	// Skip post-FX unless a game/server is actually running. This keeps the effects off the main menu's
	// 3D background (no server there -> sv_running 0) while leaving them on during coop play. The in-game
	// ESC menu keeps sv_running 1, so the live world behind it is still processed (it's the real scene).
	static cvar_t *svRunning = NULL;
	if ( !svRunning ) svRunning = ri.Cvar_Get( "sv_running", "0", 0 );
	if ( svRunning && !svRunning->integer ) return qfalse;
	return (qboolean)( glPostFxProcsLoaded && s.inited && r_postProcess && r_postProcess->integer );
}

// ------------------------------------------------------------------ frame hook (Approach B: copy + post)
// The earlier scanlines/darkening/freeze were caused by raw qgl* calls bypassing the engine's glState
// cache. All state here goes through GL_State / GL_Bind, with glState.currenttextures kept consistent,
// exactly as the existing RB_DepthOfField does. Binding our own scratch FBOs is fine (FBO binding isn't
// part of glState); we always end back on framebuffer 0.
void RB_PostFxApply( void ) {
	GLint    loc;
	qboolean doSSAO, doBloom, doDoF, doTonemap, doFXAA, doLowHealth, doGodRays, doSuppress, doHeatHaze, doRain, doSharpen;
	float    hurt = 0.0f;
	float    suppress = 0.0f;
	float    heat = 0.0f;
	float    muzHeat = 0.0f, muzX = 0.5f, muzY = 0.6f, muzR = 0.28f;
	float    rainWet = 0.0f;
	float    sunU = 0.5f, sunV = 0.5f;
	if ( !R_PostFxActive() ) return;

	doSSAO    = (qboolean)( s.ssaoOk    && r_ppSSAO    && r_ppSSAO->integer );
	doBloom   = (qboolean)( s.bloomOk   && r_ppBloom   && r_ppBloom->integer );
	doDoF     = (qboolean)( s.dofOk     && r_ppDoF     && r_ppDoF->integer );
	doTonemap = (qboolean)( s.tonemapOk && ( ( r_ppTonemap && r_ppTonemap->integer ) ||
	                                         ( r_ppGrade && r_ppGrade->integer ) ) );
	doFXAA    = (qboolean)( s.fxaaOk    && r_ppFXAA    && r_ppFXAA->integer );
	doSharpen = (qboolean)( s.sharpenOk && r_ppSharpen && r_ppSharpen->integer );

	// low-health: ramp the cgame-published health fraction into a hurt intensity, with a slow heartbeat pulse
	doLowHealth = qfalse;
	if ( s.lowHealthOk && r_ppLowHealth && r_ppLowHealth->integer ) {
		float frac  = ( r_ppHealthFrac ? r_ppHealthFrac->value : 1.0f );
		float start = ( r_ppLowHealthStart ? r_ppLowHealthStart->value : 0.5f );
		float amt   = ( r_ppLowHealthAmount ? r_ppLowHealthAmount->value : 1.0f );
		if ( start < 0.05f ) start = 0.05f;
		if ( frac < start ) {
			// the lower the HP, the WORSE it gets: accelerate the ramp (quadratic blend) so a light wound is
			// subtle but bleeding out near death is intense. ramp = 0 at the threshold -> 1 at zero HP.
			float ramp = ( start - frac ) / start;
			if ( ramp < 0.0f ) ramp = 0.0f; else if ( ramp > 1.0f ) ramp = 1.0f;
			// CLEAR onset at the threshold (user wants it visibly "kicked off" at ~50% HP, not a near-invisible
			// quadratic creep): a 0.35 floor the instant you cross the line, ramping to full as you bleed out.
			hurt = ( 0.35f + 0.65f * ramp ) * amt;
			if ( hurt > 1.0f ) hurt = 1.0f;
			// heartbeat: a real pulse (now that the vignette only shows at GENUINE low health). The rate
			// quickens as you near death (panic) - ~3.2 rad/s when lightly hurt up to ~7 rad/s when critical -
			// and the depth is ~+/-20%. r_ppLowHealthBeat scales the pulse depth (0 = steady, no throb).
			{
				cvar_t *pBeat   = ri.Cvar_Get( "r_ppLowHealthBeat", "0.25", CVAR_ARCHIVE );
				float   depth   = pBeat ? pBeat->value : 0.25f;
				float   beatRate = 1.8f + hurt * 1.6f;
				if ( depth < 0.0f ) depth = 0.0f; else if ( depth > 1.0f ) depth = 1.0f;
				hurt *= ( 1.0f - depth ) + depth * (float)sin( backEnd.refdef.time * 0.001f * beatRate );
			}
			if ( hurt < 0.0f ) hurt = 0.0f;
			if ( hurt > 1.0f ) hurt = 1.0f;
			if ( hurt > 0.001f ) doLowHealth = qtrue;
		}
	}

	// suppression: the cgame publishes r_ppSuppress (a decaying 0..1 spiked by near-miss bullet zings +
	// taking fire); scale it by r_ppSuppressAmount and desaturate + tunnel-vignette the frame.
	doSuppress = qfalse;
	if ( s.suppressOk && r_ppSuppression && r_ppSuppression->integer ) {
		float sv  = ( r_ppSuppress ? r_ppSuppress->value : 0.0f );
		float amt = ( r_ppSuppressAmount ? r_ppSuppressAmount->value : 1.0f );
		suppress = sv * amt;
		if ( suppress < 0.0f ) suppress = 0.0f;
		if ( suppress > 1.0f ) suppress = 1.0f;
		if ( suppress > 0.001f ) doSuppress = qtrue;
	}

	// heat haze: the cgame publishes r_ppHeat (a decaying 0..1 spiked near explosions); scale by
	// r_ppHeatAmount and warp the frame with an animated shimmer.
	doHeatHaze = qfalse;
	if ( s.heatHazeOk && r_ppHeatHaze && r_ppHeatHaze->integer ) {
		float hv  = ( r_ppHeat ? r_ppHeat->value : 0.0f );
		float amt = ( r_ppHeatAmount ? r_ppHeatAmount->value : 1.0f );
		heat = hv * amt;
		if ( heat < 0.0f ) heat = 0.0f;
		if ( heat > 1.0f ) heat = 1.0f;
		if ( heat > 0.001f ) doHeatHaze = qtrue;

		// LOCALIZED muzzle heat: cgame publishes r_ppMuzzleHeat (decaying gun-fire heat) + the muzzle screen
		// point (r_ppMuzzleX/Y) + falloff radius. Drives the tight shimmer around the barrel (shader above),
		// so gunfire heat reads as rising off the gun instead of the whole screen. Scaled by the same master
		// r_ppHeatAmount dial. Tunable: r_ppMuzzleX/Y move the hot-spot, r_ppMuzzleRadius sizes it.
		{
			static cvar_t *pMuzH = NULL, *pMuzX = NULL, *pMuzY = NULL, *pMuzR = NULL;
			if ( !pMuzH ) pMuzH = ri.Cvar_Get( "r_ppMuzzleHeat",   "0",    CVAR_ARCHIVE );
			if ( !pMuzX ) pMuzX = ri.Cvar_Get( "r_ppMuzzleX",      "0.5",  CVAR_ARCHIVE );
			if ( !pMuzY ) pMuzY = ri.Cvar_Get( "r_ppMuzzleY",      "0.6",  CVAR_ARCHIVE );
			if ( !pMuzR ) pMuzR = ri.Cvar_Get( "r_ppMuzzleRadius", "0.28", CVAR_ARCHIVE );
			muzHeat = ( pMuzH ? pMuzH->value : 0.0f ) * amt;
			if ( muzHeat < 0.0f ) muzHeat = 0.0f;
			if ( muzHeat > 1.0f ) muzHeat = 1.0f;
			muzX = pMuzX ? pMuzX->value : 0.5f;
			muzY = pMuzY ? pMuzY->value : 0.6f;
			muzR = pMuzR ? pMuzR->value : 0.28f;
			if ( muzR < 0.02f ) muzR = 0.02f;
			if ( muzHeat > 0.001f ) doHeatHaze = qtrue;
		}
	}

	// rain on lens: cgame publishes r_ppRainWet (0..1 = rain intensity AND under open sky, eased so beads
	// build/dry). Scale by r_ppRainAmount; gate on the master r_ppRainDrops. Refractive water beads over the
	// scene (shader above). Drawn after heat haze, before the vignettes.
	doRain = qfalse;
	{
		static cvar_t *pRainOn = NULL, *pRainWet = NULL, *pRainAmt = NULL;
		if ( !pRainOn )  pRainOn  = ri.Cvar_Get( "r_ppRainDrops",  "1", CVAR_ARCHIVE );
		if ( !pRainWet ) pRainWet = ri.Cvar_Get( "r_ppRainWet",    "0", 0 );           // cgame-published, per-frame
		if ( !pRainAmt ) pRainAmt = ri.Cvar_Get( "r_ppRainAmount", "1", CVAR_ARCHIVE );
		if ( s.rainOk && pRainOn->integer ) {
			rainWet = ( pRainWet ? pRainWet->value : 0.0f ) * ( pRainAmt ? pRainAmt->value : 1.0f );
			if ( rainWet < 0.0f ) rainWet = 0.0f;
			if ( rainWet > 1.0f ) rainWet = 1.0f;
			if ( rainWet > 0.003f ) doRain = qtrue;
		}
	}

	// god rays: project the (directional) sun to screen using the persisted 3D view matrices. Only when the
	// map actually has a sun (tr.sunLight), the sun is IN FRONT (clip.w>0), and within a margin of the screen.
	doGodRays = qfalse;
	if ( s.godRaysOk && r_ppSunShafts && r_ppSunShafts->integer &&
	     ( tr.sunLight[0] + tr.sunLight[1] + tr.sunLight[2] ) > 0.05f ) {
		vec3_t src;
		vec4_t eye, clip;
		VectorScale( tr.sunDirection, 100000.0f, src );
		R_TransformModelToClip( src, backEnd.viewParms.world.modelMatrix, backEnd.viewParms.projectionMatrix, eye, clip );
		if ( clip[3] > 1.0f ) {	// sun is in front of the camera
			sunU = ( clip[0] / clip[3] + 1.0f ) * 0.5f;
			sunV = ( clip[1] / clip[3] + 1.0f ) * 0.5f;
			if ( sunU > -0.35f && sunU < 1.35f && sunV > -0.35f && sunV < 1.35f ) {
				doGodRays = qtrue;
			}
		}
	}

	// select unit 0 (shaders sample u_tex from unit 0) and copy the finished backbuffer into sceneColor
	qglActiveTextureARB( GL_TEXTURE0_ARB );
	glState.currenttmu = 0;
	qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
	glState.currenttextures[0] = s.sceneColor;	// keep the GL_Bind cache consistent
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

	if ( doSSAO || doDoF ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneDepth );
		glState.currenttextures[0] = s.sceneDepth;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );
	}

	// ---- base scene to the backbuffer (replace). SSAO multiplies onto it; bloom adds onto it.
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	GL_State( GLS_DEPTHTEST_DISABLE );
	qglUseProgram( s.passProgram );
	R_PostFx_SetTex( s.passProgram );
	qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
	R_PostFx_FSQuad();

	// ---- SSAO: AO from depth -> blur -> multiply onto the scene
	if ( doSSAO ) {
		float znear = ( r_znear && r_znear->value > 0.1f ) ? r_znear->value : 4.0f;
		float zfar  = ( backEnd.viewParms.zFar > 1.0f ) ? backEnd.viewParms.zFar : 2048.0f;
		float rad   = ( r_ppSSAORadius    ? r_ppSSAORadius->value    : 16.0f );
		float bias  = ( r_ppSSAOBias      ? r_ppSSAOBias->value      : 0.5f );
		float si    = ( r_ppSSAOIntensity ? r_ppSSAOIntensity->value : 1.0f );
		// depth-aware (bilateral) blur when enabled and compiled; otherwise the plain Gaussian
		qboolean depthAware = (qboolean)( s.ssaoBlurProgram && r_ppSSAODepthAware && r_ppSSAODepthAware->integer );
		GLuint   blurProg   = depthAware ? s.ssaoBlurProgram : s.blurProgram;

		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.ssaoProgram );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_depth" );     if ( loc >= 0 ) qglUniform1i( loc, 0 );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_zNear" );     if ( loc >= 0 ) qglUniform1f( loc, znear );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_zFar" );      if ( loc >= 0 ) qglUniform1f( loc, zfar );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_radius" );    if ( loc >= 0 ) qglUniform1f( loc, rad );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_bias" );      if ( loc >= 0 ) qglUniform1f( loc, bias );
		loc = qglGetUniformLocation( s.ssaoProgram, "u_intensity" ); if ( loc >= 0 ) qglUniform1f( loc, si );
		qglBindTexture( GL_TEXTURE_2D, s.sceneDepth );
		R_PostFx_FSQuad();

		// blur AO [0]->[1]->[0]. blurProg = bilateral (reads r=AO,g=depth) or plain Gaussian (blurs channels).
		qglUseProgram( blurProg );
		R_PostFx_SetTex( blurProg );
		if ( depthAware ) {
			loc = qglGetUniformLocation( blurProg, "u_sharp" ); if ( loc >= 0 ) qglUniform1f( loc, 80.0f );
		}
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[1] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		loc = qglGetUniformLocation( blurProg, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 1.0f / (float)s.bloomW, 0.0f );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		loc = qglGetUniformLocation( blurProg, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 0.0f, 1.0f / (float)s.bloomH );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[1] );
		R_PostFx_FSQuad();

		// multiply AO onto the backbuffer scene (result = scene * AO). aoProgram broadcasts .r (AO) to rgb.
		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );
		qglUseProgram( s.aoProgram );
		R_PostFx_SetTex( s.aoProgram );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();
	}

	// ---- Depth of field: blur the scene (half-res) then composite it over the sharp backbuffer,
	//      per-pixel alpha = depth circle-of-confusion. Blurred scene reuses the bloom scratch FBOs.
	if ( doDoF ) {
		float znear = ( r_znear && r_znear->value > 0.1f ) ? r_znear->value : 4.0f;
		float zfar  = ( backEnd.viewParms.zFar > 1.0f ) ? backEnd.viewParms.zFar : 2048.0f;
		float focus = ( r_ppDoFFocus     ? r_ppDoFFocus->value     : 0.0f );
		float range = ( r_ppDoFRange     ? r_ppDoFRange->value     : 1200.0f );
		float inten = ( r_ppDoFIntensity ? r_ppDoFIntensity->value : 0.5f );

		// 1) downsample sceneColor -> bloomFbo[0] (half-res)
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.passProgram );
		R_PostFx_SetTex( s.passProgram );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();

		// 2) separable Gaussian blur [0]->[1]->[0]
		qglUseProgram( s.blurProgram );
		R_PostFx_SetTex( s.blurProgram );
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[1] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		loc = qglGetUniformLocation( s.blurProgram, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 1.0f / (float)s.bloomW, 0.0f );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		loc = qglGetUniformLocation( s.blurProgram, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 0.0f, 1.0f / (float)s.bloomH );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[1] );
		R_PostFx_FSQuad();

		// 3) composite over the backbuffer: blurred scene on unit 0, scene depth on unit 1
		GL_SelectTexture( 1 );
		qglBindTexture( GL_TEXTURE_2D, s.sceneDepth );
		glState.currenttextures[1] = s.sceneDepth;
		GL_SelectTexture( 0 );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		glState.currenttextures[0] = s.bloomTex[0];

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
		qglUseProgram( s.dofProgram );
		loc = qglGetUniformLocation( s.dofProgram, "u_tex" );       if ( loc >= 0 ) qglUniform1i( loc, 0 );
		loc = qglGetUniformLocation( s.dofProgram, "u_depth" );     if ( loc >= 0 ) qglUniform1i( loc, 1 );
		loc = qglGetUniformLocation( s.dofProgram, "u_zNear" );     if ( loc >= 0 ) qglUniform1f( loc, znear );
		loc = qglGetUniformLocation( s.dofProgram, "u_zFar" );      if ( loc >= 0 ) qglUniform1f( loc, zfar );
		loc = qglGetUniformLocation( s.dofProgram, "u_focus" );     if ( loc >= 0 ) qglUniform1f( loc, focus );
		loc = qglGetUniformLocation( s.dofProgram, "u_range" );     if ( loc >= 0 ) qglUniform1f( loc, range );
		loc = qglGetUniformLocation( s.dofProgram, "u_intensity" ); if ( loc >= 0 ) qglUniform1f( loc, inten );
		R_PostFx_FSQuad();

		// restore: unbind depth from unit 1, return to unit 0 for the rest of the frame
		GL_SelectTexture( 1 );
		qglBindTexture( GL_TEXTURE_2D, 0 );
		glState.currenttextures[1] = 0;
		GL_SelectTexture( 0 );
	}

	// ---- Bloom: bright-pass original scene -> blur -> additive onto the scene
	if ( doBloom ) {
		float thr   = ( r_ppBloomThreshold ? r_ppBloomThreshold->value : 0.6f );
		float bi    = ( r_ppBloomIntensity ? r_ppBloomIntensity->value : 1.3f );

		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.brightProgram );
		R_PostFx_SetTex( s.brightProgram );
		loc = qglGetUniformLocation( s.brightProgram, "u_threshold" ); if ( loc >= 0 ) qglUniform1f( loc, thr );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();

		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[1] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		qglUseProgram( s.blurProgram );
		R_PostFx_SetTex( s.blurProgram );
		loc = qglGetUniformLocation( s.blurProgram, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 1.0f / (float)s.bloomW, 0.0f );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		loc = qglGetUniformLocation( s.blurProgram, "u_dir" ); if ( loc >= 0 ) qglUniform2f( loc, 0.0f, 1.0f / (float)s.bloomH );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[1] );
		R_PostFx_FSQuad();

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		qglUseProgram( s.addProgram );
		R_PostFx_SetTex( s.addProgram );
		loc = qglGetUniformLocation( s.addProgram, "u_intensity" ); if ( loc >= 0 ) qglUniform1f( loc, bi );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();
	}

	// ---- God rays: bright-pass the scene (sun/sky) -> radial accumulate toward the sun -> additive shafts
	if ( doGodRays ) {
		float thr   = ( r_ppSunShaftThreshold ? r_ppSunShaftThreshold->value : 0.6f );
		float decay = ( r_ppSunShaftDecay     ? r_ppSunShaftDecay->value     : 0.95f );
		float inten = ( r_ppSunShaftIntensity ? r_ppSunShaftIntensity->value : 0.5f );

		// 1) bright-pass sceneColor -> bloomFbo[0] (only the bright sun/sky pixels seed the shafts)
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[0] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.brightProgram );
		R_PostFx_SetTex( s.brightProgram );
		loc = qglGetUniformLocation( s.brightProgram, "u_threshold" ); if ( loc >= 0 ) qglUniform1f( loc, thr );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();

		// 2) radial accumulate from the sun: bloomTex[0] -> bloomFbo[1]
		qglBindFramebuffer( GL_FRAMEBUFFER, s.bloomFbo[1] );
		qglViewport( 0, 0, s.bloomW, s.bloomH );
		qglUseProgram( s.godRaysProgram );
		R_PostFx_SetTex( s.godRaysProgram );
		loc = qglGetUniformLocation( s.godRaysProgram, "u_sun" );    if ( loc >= 0 ) qglUniform2f( loc, sunU, sunV );
		loc = qglGetUniformLocation( s.godRaysProgram, "u_decay" );  if ( loc >= 0 ) qglUniform1f( loc, decay );
		loc = qglGetUniformLocation( s.godRaysProgram, "u_weight" ); if ( loc >= 0 ) qglUniform1f( loc, 0.35f );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[0] );
		R_PostFx_FSQuad();

		// 3) additive composite the shafts onto the backbuffer (scaled by intensity)
		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		qglUseProgram( s.addProgram );
		R_PostFx_SetTex( s.addProgram );
		loc = qglGetUniformLocation( s.addProgram, "u_intensity" ); if ( loc >= 0 ) qglUniform1f( loc, inten );
		qglBindTexture( GL_TEXTURE_2D, s.bloomTex[1] );
		R_PostFx_FSQuad();
	}

	// ---- Tonemap / color grade: copy the finished backbuffer into sceneColor, grade it, draw back (replace)
	if ( doTonemap ) {
		float expo = ( r_ppExposure   ? r_ppExposure->value   : 1.0f );
		float cont = ( r_ppContrast   ? r_ppContrast->value   : 1.0f );
		float sat  = ( r_ppSaturation ? r_ppSaturation->value : 1.0f );
		float temp = 0.0f;
		int   grade = ( r_ppGrade ? r_ppGrade->integer : 0 );

		// Color-grade presets override the manual sliders (war-film looks). 0 = use the manual cvars above.
		switch ( grade ) {
		case 1: expo = 1.0f;  cont = 1.05f; sat = 1.0f;  temp =  0.00f; break; // Neutral (filmic, no tint)
		case 2: expo = 1.05f; cont = 1.10f; sat = 1.05f; temp =  0.10f; break; // Warm "Normandy" (golden)
		case 3: expo = 0.95f; cont = 1.10f; sat = 0.85f; temp = -0.10f; break; // Cold "Ardennes" (blue, desat)
		case 4: expo = 1.0f;  cont = 1.35f; sat = 0.55f; temp = -0.02f; break; // Bleach bypass (harsh, desat)
		default: break;
		}

		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.tonemapProgram );
		R_PostFx_SetTex( s.tonemapProgram );
		loc = qglGetUniformLocation( s.tonemapProgram, "u_exposure" );   if ( loc >= 0 ) qglUniform1f( loc, expo );
		loc = qglGetUniformLocation( s.tonemapProgram, "u_contrast" );   if ( loc >= 0 ) qglUniform1f( loc, cont );
		loc = qglGetUniformLocation( s.tonemapProgram, "u_saturation" ); if ( loc >= 0 ) qglUniform1f( loc, sat );
		loc = qglGetUniformLocation( s.tonemapProgram, "u_temp" );       if ( loc >= 0 ) qglUniform1f( loc, temp );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- FXAA: copy the finished backbuffer into sceneColor, edge-AA it, draw back (replace). Runs last.
	if ( doFXAA ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.fxaaProgram );
		R_PostFx_SetTex( s.fxaaProgram );
		loc = qglGetUniformLocation( s.fxaaProgram, "u_rcpFrame" );
		if ( loc >= 0 ) qglUniform2f( loc, 1.0f / (float)s.width, 1.0f / (float)s.height );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- Sharpen: unsharp mask over the finished (AA'd) frame; counteracts FXAA/mip softening. Runs after FXAA.
	if ( doSharpen ) {
		float amt = ( r_ppSharpenAmount ? r_ppSharpenAmount->value : 0.35f );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.sharpenProgram );
		R_PostFx_SetTex( s.sharpenProgram );
		loc = qglGetUniformLocation( s.sharpenProgram, "u_rcp" );
		if ( loc >= 0 ) qglUniform2f( loc, 1.0f / (float)s.width, 1.0f / (float)s.height );
		loc = qglGetUniformLocation( s.sharpenProgram, "u_amount" );
		if ( loc >= 0 ) qglUniform1f( loc, amt );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- Heat haze: animated UV shimmer near explosions. Warps the (graded/AA'd) scene; the vignette
	//      overlays below draw AFTER so they aren't distorted.
	if ( doHeatHaze ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.heatHazeProgram );
		R_PostFx_SetTex( s.heatHazeProgram );
		loc = qglGetUniformLocation( s.heatHazeProgram, "u_heat" ); if ( loc >= 0 ) qglUniform1f( loc, heat );
		loc = qglGetUniformLocation( s.heatHazeProgram, "u_time" ); if ( loc >= 0 ) qglUniform1f( loc, backEnd.refdef.time * 0.001f );
		loc = qglGetUniformLocation( s.heatHazeProgram, "u_muzHeat" ); if ( loc >= 0 ) qglUniform1f( loc, muzHeat );
		loc = qglGetUniformLocation( s.heatHazeProgram, "u_muzCtr" );  if ( loc >= 0 ) qglUniform2f( loc, muzX, muzY );
		loc = qglGetUniformLocation( s.heatHazeProgram, "u_muzRad" );  if ( loc >= 0 ) qglUniform1f( loc, muzR );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- Rain on lens: refractive water beads over the scene while outdoors in rain. Warps like heat haze,
	//      so it runs before the vignettes (which overlay AFTER and stay undistorted).
	if ( doRain ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.rainProgram );
		R_PostFx_SetTex( s.rainProgram );
		loc = qglGetUniformLocation( s.rainProgram, "u_wet" );    if ( loc >= 0 ) qglUniform1f( loc, rainWet );
		loc = qglGetUniformLocation( s.rainProgram, "u_time" );   if ( loc >= 0 ) qglUniform1f( loc, backEnd.refdef.time * 0.001f );
		loc = qglGetUniformLocation( s.rainProgram, "u_aspect" ); if ( loc >= 0 ) qglUniform1f( loc, (float)s.width / (float)s.height );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- Low-health: desaturate + red vignette as the player nears death. Runs LAST so it overlays the
	//      final graded/AA'd image. Driven by hurt (from the cgame-published health fraction).
	if ( doLowHealth ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.lowHealthProgram );
		R_PostFx_SetTex( s.lowHealthProgram );
		loc = qglGetUniformLocation( s.lowHealthProgram, "u_hurt" ); if ( loc >= 0 ) qglUniform1f( loc, hurt );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	// ---- Suppression: desaturate + dark tunnel vignette while under fire. Runs last (overlays everything).
	if ( doSuppress ) {
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		glState.currenttextures[0] = s.sceneColor;
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, s.width, s.height );

		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
		GL_State( GLS_DEPTHTEST_DISABLE );
		qglUseProgram( s.suppressProgram );
		R_PostFx_SetTex( s.suppressProgram );
		loc = qglGetUniformLocation( s.suppressProgram, "u_suppress" ); if ( loc >= 0 ) qglUniform1f( loc, suppress );
		qglBindTexture( GL_TEXTURE_2D, s.sceneColor );
		R_PostFx_FSQuad();
	}

	qglUseProgram( 0 );
	GL_Bind( tr.whiteImage );	// reset the GL_Bind cache to a known texture (mirrors RB_DepthOfField)
}
