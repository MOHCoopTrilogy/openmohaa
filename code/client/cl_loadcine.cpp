/*
===========================================================================
HZM coop [user 2026-10-18] LOADING-SCREEN CINEMATIC (docs/proposals/loadcine_2026-10-18, ENGINE.md "Loading cinematic").

A smooth looping clip behind the tip strip and bar while a map loads. The clip is a packed flipbook of baseline
JPEG frames (loadclips/<name>.hzlc, written by docs/tools/gen_loadclip.py), drawn with re.DrawStretchRaw - the
same call the RoQ player uses, so it makes no image/shader registration and is safe to draw from inside a load.

Why a flipbook and not RoQ: ffmpeg's RoQ encoder spends ~20 KB per 512x256 frame (no real inter-frame coding) and the
RoQ decoder must decode EVERY delta frame up to the wanted time; a flipbook frame is independent, so after a blocking
stall the next draw decodes exactly one frame (the one for "now") - no catch-up, no drift - at 1024x512 for about the
same bytes. Playback is time-based (Sys_Milliseconds), looping, with a fade in and a fade out.

Draw cadence is the existing load tick (UI_TestUpdateScreen from UI_LoadResource), re-timed while a clip plays.
Loads are single threaded on the GL context, so no frame can be drawn while one blocking call runs; the clip simply
jumps to the right frame at the next tick. See ENGINE.md for the measured gaps.

File format, little endian:
    u32 'HZLC'  u32 version(1)  u32 width  u32 height (both powers of two)  u32 fps  u32 frames  u32 aspect*1000
    u32 offset[frames + 1]      (from the start of the file; frame i = bytes [offset[i], offset[i+1]) )
    frame data                  (baseline JPEG, YCbCr, width x height)
The picture is the 16:9 (or any) source squeezed to width x height; it is drawn "cover" at the stored aspect.

ui/loadclips.txt picks the clip, one line each ('//' and '#' comments):
    map <mapname>[*] <clip> [clip ...]     first matching rule wins; several clips = a random one per load
    map <mapname>[*] -                     no clip for this map (its static or pool art shows, as before)
No match, no file, cl_loadCinematic 0, a damaged clip or a renderer without DrawStretchRaw all leave the old art alone.
===========================================================================
*/

#include "client.h"
#include "cl_ui.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jpeglib.h>
}

extern "C" int  IN_HZM_GetSwapInterval(void);
extern "C" void IN_HZM_SetSwapInterval(int interval);

#define LC_MAGIC     0x434C5A48u // 'HZLC'
#define LC_VERSION   1u
#define LC_TOKLEN    64
#define LC_TOKS      16
#define LC_MAXDIM    2048
#define LC_MAXFRAMES 4096

static cvar_t *cl_loadCinematic;  // 1 = play a clip when the map has one
static cvar_t *cl_loadCineVsync;  // 1 = swap interval 0 while a clip plays (the load must not wait for the display)
static cvar_t *cl_loadCineFps;    // draw cadence ceiling during a load (the clip's own rate is in the file)
static cvar_t *cl_loadCineFadeMs; // fade in and out length

static struct {
    qboolean  active;
    byte     *file;
    int       fileLen;
    unsigned  w, h, fps, frames, aspect1000;
    unsigned *off;  // frames + 1 offsets (copied out of the file: it is not aligned)
    byte     *rgba; // w * h * 4
    int       cur;  // frame currently in rgba, -1 none
    char      name[LC_TOKLEN];
    char      map[MAX_QPATH];
    int       startMs; // Sys_Milliseconds at the first draw; 0 until then
    int       beginMs;
    float     fadeOut; // 0 = none, up to 1 = black (UI_EndLoad's fade)

    // measurements (printed by CL_LoadCine_End; always counted, free)
    int draws, decodes, decodeMsTotal, decodeMsMax, drawMsTotal, drawMsMax, lastDrawMs;
    int gapMax, gaps50, gaps100, gaps200;
    int shown[LC_MAXFRAMES / 32];
    int distinct;
} s_lc;

// survives a clip: the rotation state and the swap interval we changed
static struct {
    unsigned rng;
    char     last[LC_TOKLEN];
    int      savedSwap; // -1 = not changed by us
} s_lcKeep = {0, "", -1};

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               jb;
} lcErr_t;

static void LC_ErrorExit(j_common_ptr cinfo)
{
    longjmp(((lcErr_t *)cinfo->err)->jb, 1);
}

static void LC_OutputMessage(j_common_ptr cinfo)
{
    (void)cinfo;
}

static unsigned LC_U32(const byte *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void LC_Free(void)
{
    if (s_lc.file) {
        free(s_lc.file);
    }
    if (s_lc.off) {
        free(s_lc.off);
    }
    if (s_lc.rgba) {
        free(s_lc.rgba);
    }
    memset(&s_lc, 0, sizeof(s_lc));
}

// Decodes frame n into s_lc.rgba. qfalse on any damage (the caller drops the clip).
static qboolean LC_Decode(int n)
{
    struct jpeg_decompress_struct cinfo;
    lcErr_t                       jerr;
    unsigned                      y, a = s_lc.off[n], b = s_lc.off[n + 1];

    if (b <= a || b > (unsigned)s_lc.fileLen) {
        return qfalse;
    }

    cinfo.err               = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = LC_ErrorExit;
    jerr.pub.output_message = LC_OutputMessage;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return qfalse;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, s_lc.file + a, (unsigned long)(b - a));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space     = JCS_RGB;
    cinfo.dct_method          = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.do_block_smoothing  = FALSE;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width != s_lc.w || cinfo.output_height != s_lc.h || cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return qfalse;
    }
    for (y = 0; y < s_lc.h; y++) {
        // the 3 byte/pixel scanline is parked in the tail of its own 4 byte/pixel row (w*3 fits in w*4 after w bytes)
        // and spread forward in place: the writer (4x+3) never passes the unread input (w+3x+3) while x < w
        byte    *dst = s_lc.rgba + (size_t)y * s_lc.w * 4;
        byte    *src = dst + s_lc.w;
        JSAMPROW rp  = (JSAMPROW)src;
        unsigned x;

        jpeg_read_scanlines(&cinfo, &rp, 1);
        for (x = 0; x < s_lc.w; x++) {
            byte r = src[x * 3], g = src[x * 3 + 1], bl = src[x * 3 + 2];

            dst[x * 4]     = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = bl;
            dst[x * 4 + 3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return qtrue;
}

static int LC_Tokens(const char *ls, const char *le, char tok[][LC_TOKLEN], int max)
{
    int n = 0;

    while (ls < le && n < max) {
        int k = 0;
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) {
            ls++;
        }
        if (ls >= le || (le - ls >= 2 && ls[0] == '/' && ls[1] == '/') || *ls == '#') {
            break;
        }
        while (ls < le && *ls != ' ' && *ls != '\t' && *ls != '\r') {
            if (k < LC_TOKLEN - 1) {
                tok[n][k++] = *ls;
            }
            ls++;
        }
        tok[n][k] = 0;
        n++;
    }
    return n;
}

static qboolean LC_MapMatches(const char *pattern, const char *mapname)
{
    size_t len = strlen(pattern);

    if (len && pattern[len - 1] == '*') {
        return !Q_stricmpn(pattern, mapname, (int)(len - 1)) ? qtrue : qfalse;
    }
    return !Q_stricmp(pattern, mapname) ? qtrue : qfalse;
}

void CL_LoadCine_Init(void)
{
    cl_loadCinematic  = Cvar_Get("cl_loadCinematic", "1", CVAR_ARCHIVE);
    cl_loadCineVsync  = Cvar_Get("cl_loadCineVsync", "1", 0);
    cl_loadCineFps    = Cvar_Get("cl_loadCineFps", "30", 0);
    cl_loadCineFadeMs = Cvar_Get("cl_loadCineFadeMs", "450", 0);
    Cvar_Get("ui_loadCineOn", "0", 0);
}

qboolean CL_LoadCine_Active(void)
{
    return s_lc.active;
}

// ms between draws the load tick must keep while a clip plays (start to start)
int CL_LoadCine_TickMs(void)
{
    int fps = cl_loadCineFps ? cl_loadCineFps->integer : 30;

    if (fps < 5) {
        fps = 5;
    }
    if (fps > 60) {
        fps = 60;
    }
    return 1000 / fps;
}

static void LC_RestoreSwap(void)
{
    if (s_lcKeep.savedSwap >= 0) {
        IN_HZM_SetSwapInterval(s_lcKeep.savedSwap);
        s_lcKeep.savedSwap = -1;
    }
}

// the load tick calls this before each draw: the swap interval must be 0 (a vsynced swap is a ~16 ms wait per tick)
void CL_LoadCine_PreDraw(void)
{
    int cur;

    if (!s_lc.active || !cl_loadCineVsync || !cl_loadCineVsync->integer) {
        return;
    }
    cur = IN_HZM_GetSwapInterval();
    if (cur > 0) {
        if (s_lcKeep.savedSwap < 0) {
            s_lcKeep.savedSwap = cur;
        }
        IN_HZM_SetSwapInterval(0);
    }
}

void CL_LoadCine_End(const char *how)
{
    if (s_lc.active) {
        float avg = s_lc.decodes ? (float)s_lc.decodeMsTotal / (float)s_lc.decodes : 0.0f;
        float dav = s_lc.draws ? (float)s_lc.drawMsTotal / (float)s_lc.draws : 0.0f;

        Com_Printf(
            "^~^~^ LOADCINE end=%s map=%s clip=%s ms=%d draws=%d distinct=%d/%u decode_avg=%.2f decode_max=%d "
            "draw_avg=%.2f draw_max=%d gapmax=%d gaps_over50=%d over100=%d over200=%d\n",
            how,
            s_lc.map,
            s_lc.name,
            Sys_Milliseconds() - s_lc.beginMs,
            s_lc.draws,
            s_lc.distinct,
            s_lc.frames,
            avg,
            s_lc.decodeMsMax,
            dav,
            s_lc.drawMsMax,
            s_lc.gapMax,
            s_lc.gaps50,
            s_lc.gaps100,
            s_lc.gaps200
        );
    }
    LC_RestoreSwap();
    LC_Free();
    if (cl_loadCinematic) {
        Cvar_Set("ui_loadCineOn", "0");
    }
}

static qboolean LC_Open(const char *clip, const char *mapname)
{
    char         path[MAX_QPATH];
    fileHandle_t f = 0;
    long         len;
    byte        *buf;
    unsigned     i, hdr;

    Com_sprintf(path, sizeof(path), "loadclips/%s.hzlc", clip);
    len = FS_FOpenFileRead(path, &f, qtrue, qtrue);
    if (len < 40 || !f) {
        if (f) {
            FS_FCloseFile(f);
        }
        return qfalse;
    }
    buf = (byte *)malloc((size_t)len);
    if (!buf) {
        FS_FCloseFile(f);
        return qfalse;
    }
    if ((long)FS_Read(buf, (size_t)len, f) != len) {
        free(buf);
        FS_FCloseFile(f);
        return qfalse;
    }
    FS_FCloseFile(f);

    memset(&s_lc, 0, sizeof(s_lc));
    s_lc.file    = buf;
    s_lc.fileLen = (int)len;
    if (LC_U32(buf) != LC_MAGIC || LC_U32(buf + 4) != LC_VERSION) {
        goto bad;
    }
    s_lc.w          = LC_U32(buf + 8);
    s_lc.h          = LC_U32(buf + 12);
    s_lc.fps        = LC_U32(buf + 16);
    s_lc.frames     = LC_U32(buf + 20);
    s_lc.aspect1000 = LC_U32(buf + 24);
    if (!s_lc.w || !s_lc.h || s_lc.w > LC_MAXDIM || s_lc.h > LC_MAXDIM || (s_lc.w & (s_lc.w - 1))
        || (s_lc.h & (s_lc.h - 1)) || !s_lc.fps || s_lc.fps > 120 || s_lc.frames < 2 || s_lc.frames > LC_MAXFRAMES
        || s_lc.aspect1000 < 500 || s_lc.aspect1000 > 4000) {
        goto bad;
    }
    hdr = 28 + 4 * (s_lc.frames + 1);
    if ((unsigned)len < hdr) {
        goto bad;
    }
    s_lc.off = (unsigned *)malloc(sizeof(unsigned) * (s_lc.frames + 1));
    if (!s_lc.off) {
        goto bad;
    }
    for (i = 0; i <= s_lc.frames; i++) {
        s_lc.off[i] = LC_U32(buf + 28 + 4 * i);
        if (s_lc.off[i] > (unsigned)len || (i && s_lc.off[i] < s_lc.off[i - 1]) || s_lc.off[i] < hdr) {
            goto bad;
        }
    }
    s_lc.rgba = (byte *)malloc((size_t)s_lc.w * s_lc.h * 4);
    if (!s_lc.rgba) {
        goto bad;
    }
    if (!LC_Decode(0)) {
        goto bad;
    }
    s_lc.cur = 0;
    Q_strncpyz(s_lc.name, clip, sizeof(s_lc.name));
    Q_strncpyz(s_lc.map, mapname, sizeof(s_lc.map));
    return qtrue;

bad:
    Com_Printf("^~^~^ LOADCINE bad clip %s\n", path);
    LC_Free();
    return qfalse;
}

// Called by UI_BeginLoad. Returns qtrue when a clip will play (ui_loadCineOn 1).
qboolean CL_LoadCine_Begin(const char *mapname)
{
    char       *buf = NULL;
    const char *p, *end;
    long        len;
    char        tok[LC_TOKS][LC_TOKLEN];
    char        clips[LC_TOKS][LC_TOKLEN];
    int         nClips = -1, n, pick, i;

    if (s_lc.active) {
        CL_LoadCine_End("restart");
    }
    Cvar_Set("ui_loadCineOn", "0");
    if (!cl_loadCinematic || !cl_loadCinematic->integer || !mapname || !*mapname || !cls.rendererReady
        || !re.DrawStretchRaw) {
        return qfalse;
    }

    len = FS_ReadFile("ui/loadclips.txt", (void **)&buf);
    if (len <= 0 || !buf) {
        return qfalse;
    }
    p   = buf;
    end = buf + len;
    while (p < end && nClips < 0) {
        const char *ls = p;
        const char *le;

        while (p < end && *p != '\n') {
            p++;
        }
        le = p;
        if (p < end) {
            p++;
        }
        n = LC_Tokens(ls, le, tok, LC_TOKS);
        if (n >= 3 && !Q_stricmp(tok[0], "map") && LC_MapMatches(tok[1], mapname)) {
            nClips = 0;
            for (i = 2; i < n; i++) {
                if (!strcmp(tok[i], "-")) {
                    nClips = 0;
                    break;
                }
                Q_strncpyz(clips[nClips++], tok[i], LC_TOKLEN);
            }
            if (!nClips) {
                nClips = -2; // an explicit "-": no clip for this map (and no later rule is read)
            }
        }
    }
    FS_FreeFile(buf);
    if (nClips <= 0) {
        return qfalse;
    }

    s_lcKeep.rng ^= (unsigned)Sys_Milliseconds() * 2654435761u;
    s_lcKeep.rng = s_lcKeep.rng * 1664525u + 1013904223u;
    pick         = (int)((s_lcKeep.rng >> 8) % (unsigned)nClips);
    if (nClips > 1 && !Q_stricmp(clips[pick], s_lcKeep.last)) {
        pick = (pick + 1) % nClips;
    }
    if (!LC_Open(clips[pick], mapname)) {
        return qfalse;
    }
    Q_strncpyz(s_lcKeep.last, clips[pick], sizeof(s_lcKeep.last));
    s_lc.active  = qtrue;
    s_lc.beginMs = Sys_Milliseconds();
    Cvar_Set("ui_loadCineOn", "1");
    Com_Printf(
        "^~^~^ LOADCINE begin map=%s clip=%s %ux%u %ufps %u frames %.1f MB\n",
        mapname,
        s_lc.name,
        s_lc.w,
        s_lc.h,
        s_lc.fps,
        s_lc.frames,
        (double)s_lc.fileLen / 1048576.0
    );
    return qtrue;
}

void CL_LoadCine_SetFadeOut(float a)
{
    s_lc.fadeOut = a < 0 ? 0 : (a > 1 ? 1 : a);
}

int CL_LoadCine_FadeMs(void)
{
    int ms = cl_loadCineFadeMs ? cl_loadCineFadeMs->integer : 450;

    return ms < 0 ? 0 : (ms > 3000 ? 3000 : ms);
}

// Draws the current frame to cover the sw x sh screen, then the fade. Called from the loading menu's hzm_loadcine widget.
void CL_LoadCine_Draw(float sw, float sh)
{
    int    now, t, frame, fadeMs, t1, dm;
    float  aspect, dw, dh, a;
    vec4_t c;

    if (!s_lc.active || sw < 1 || sh < 1) {
        return;
    }
    now = Sys_Milliseconds();
    if (!s_lc.startMs) {
        s_lc.startMs = now;
    }
    t     = now - s_lc.startMs;
    frame = (int)(((long long)t * (long long)s_lc.fps / 1000) % (long long)s_lc.frames);

    if (frame != s_lc.cur) {
        int t0 = Sys_Milliseconds(), dt;

        if (!LC_Decode(frame)) {
            Com_Printf("^~^~^ LOADCINE decode failed frame %d - clip dropped\n", frame);
            s_lc.active = qfalse; // the next load starts clean
            return;
        }
        dt       = Sys_Milliseconds() - t0;
        s_lc.cur = frame;
        s_lc.decodes++;
        s_lc.decodeMsTotal += dt;
        if (dt > s_lc.decodeMsMax) {
            s_lc.decodeMsMax = dt;
        }
    }
    if (!(s_lc.shown[frame >> 5] & (1 << (frame & 31)))) {
        s_lc.shown[frame >> 5] |= 1 << (frame & 31);
        s_lc.distinct++;
    }

    aspect = (float)s_lc.aspect1000 / 1000.0f;
    if (sw / sh > aspect) {
        dw = sw;
        dh = sw / aspect;
    } else {
        dh = sh;
        dw = sh * aspect;
    }

    t1 = Sys_Milliseconds();
    re.SetColor(NULL);
    re.DrawStretchRaw((int)((sw - dw) * 0.5f), (int)((sh - dh) * 0.5f), (int)dw, (int)dh, (int)s_lc.w, (int)s_lc.h, 0, s_lc.rgba);

    // fade in from the first drawn frame, fade out under UI_EndLoad's ramp
    fadeMs = CL_LoadCine_FadeMs();
    a      = 1.0f;
    if (fadeMs > 0 && t < fadeMs) {
        a = (float)t / (float)fadeMs;
    }
    a *= 1.0f - s_lc.fadeOut;
    if (a < 0.999f) {
        c[0] = c[1] = c[2] = 0.0f;
        c[3]               = 1.0f - a;
        re.SetColor(c);
        re.DrawBox(0, 0, sw, sh);
        re.SetColor(NULL);
    }
    dm = Sys_Milliseconds() - t1;
    s_lc.drawMsTotal += dm;
    if (dm > s_lc.drawMsMax) {
        s_lc.drawMsMax = dm;
    }

    s_lc.draws++;
    if (s_lc.lastDrawMs) {
        int gap = now - s_lc.lastDrawMs;

        if (gap > s_lc.gapMax) {
            s_lc.gapMax = gap;
        }
        if (gap > 50) {
            s_lc.gaps50++;
        }
        if (gap > 100) {
            s_lc.gaps100++;
        }
        if (gap > 200) {
            s_lc.gaps200++;
        }
    }
    s_lc.lastDrawMs = now;
}
