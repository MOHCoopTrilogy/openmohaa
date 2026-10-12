/*
===========================================================================
HZM gl2 [user 2026-09-27, graphics menu plan section 4] VIDEO MEMORY ACCOUNTING.

docs/proposals/graphics_menu_2026-09-27/plan.md P2: the graphics menu is to show "this game: about N GB" against what
the card and Windows allow. The game's number has to be OUR OWN count - a count can be predicted for a setting that
has not been applied yet, a driver query cannot - and the plan's gate (P2.0) is that this count tracks the real
per-process GPU memory within 15 % on every map tested before any bar is drawn from it.

WHAT IS COUNTED, and how: nothing is hooked into the allocation paths. Everything the renderer creates on the card
is already described by a table it keeps anyway, so the count is a WALK over those tables, made only when somebody
asks (the `vramacct` console command today; the menu block later, at most twice a second and only while it is open).
With nobody asking it costs nothing at all - no per-allocation add, no per-frame work.
  tex  every image_t that is a loaded texture: its full mip chain level by level (not the +33 % rule of thumb -
       R_CreateImage2 allocates the chain down to 1x1 for a mipmapped image), x6 for a cube map
  rt   every image_t the renderer made for itself (names starting with '_' or '*': the scene, depth, bloom and
       post-effect targets, the shadow maps, the built-in images), x samples for the multisample scene images
  rb   every renderbuffer of every FBO (the legacy MSAA scene buffers x samples, the depth buffers, the shadow FBOs'
       colour buffers)
  vao  vertex + index buffer bytes of every VAO
NOT counted: what only the driver knows - the context, compiled shader programs, staging copies, alignment padding.
That is the "allowance" the P2.0 numbers settle.

A format this table does not know is counted at 4 bytes a pixel AND reported (nUnknown / the first unknown format in
the VRAMACCT line), never guessed silently.

Windows side, read only, for the measurement line: DXGI (loaded by name - no import, so the renderer still loads on
a system without it) gives the adapter's dedicated memory and, on Windows 10+, this process's budget and current
usage (IDXGIAdapter3::QueryVideoMemoryInfo, local segment). Whether that usage figure includes OpenGL allocations
is one of the questions P2.0 answers. NVIDIA's GL_NVX_gpu_memory_info is printed beside it when present.
gl2 only; gl1 is untouched.
===========================================================================
*/

#ifdef USE_INTERNAL_SDL_HEADERS
#    include "SDL.h"
#else
#    include <SDL.h>
#endif

#include "tr_local.h"

#ifdef _WIN32
#    define COBJMACROS
#    include <windows.h>
#    include <dxgi1_4.h>
#endif

typedef struct {
    unsigned long long tex, rt, rb, vao;
    int                nTex, nRt, nRb, nVao;
    int                nUnknown;
    unsigned int       firstUnknown;
} hzmVram_t;

// bytes of one mip level of one face/sample. *known = qfalse when the format is not in the table (counted at 4 Bpp).
static unsigned long long R_VramLevelBytes(unsigned int fmt, int w, int h, qboolean *known)
{
    unsigned long long blocks = (unsigned long long)((w + 3) / 4) * (unsigned long long)((h + 3) / 4);
    unsigned long long px     = (unsigned long long)w * (unsigned long long)h;

    *known = qtrue;
    switch (fmt) {
    // 8 bytes per 4x4 block
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RED_RGTC1:
    case GL_COMPRESSED_SIGNED_RED_RGTC1:
    case GL_RGB4_S3TC:
        return blocks * 8;
    // 16 bytes per 4x4 block
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
    case GL_COMPRESSED_RG_RGTC2:
    case GL_COMPRESSED_SIGNED_RG_RGTC2:
    case GL_COMPRESSED_RGBA_BPTC_UNORM_ARB:
    case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB:
    case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB:
    case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB:
        return blocks * 16;
    // 1 byte
    case GL_LUMINANCE:
    case GL_LUMINANCE8:
    case GL_ALPHA:
    case GL_ALPHA8:
    case GL_INTENSITY:
    case GL_INTENSITY8:
    case GL_RED:
    case GL_R8:
    case 1:
        return px;
    // 2 bytes
    case GL_LUMINANCE_ALPHA:
    case GL_LUMINANCE8_ALPHA8:
    case GL_RG:
    case GL_RG8:
    case GL_RGB5:
    case GL_RGB5_A1:
    case GL_RGBA4:
    case GL_R16F:
    case GL_DEPTH_COMPONENT16_ARB:
    case 2:
        return px * 2;
    // 4 bytes (a 3-component 8-bit format is stored padded to 4 by every desktop driver)
    case GL_RGB:
    case GL_RGB8:
    case GL_RGBA:
    case GL_RGBA8:
    case GL_SRGB_EXT:
    case GL_SRGB8_EXT:
    case GL_SRGB_ALPHA_EXT:
    case GL_SRGB8_ALPHA8_EXT:
    case GL_RGB10_A2:
    case GL_R11F_G11F_B10F:
    case GL_RG16F:
    case 0x822C: // GL_RG16
    case GL_R32F:
    case GL_DEPTH_COMPONENT:
    case GL_DEPTH_COMPONENT24_ARB:
    case GL_DEPTH_COMPONENT32_ARB:
    case GL_DEPTH_STENCIL:
    case GL_DEPTH24_STENCIL8:
    case 3:
    case 4:
        return px * 4;
    // 1 byte of stencil
    case GL_STENCIL_INDEX:
    case GL_STENCIL_INDEX8:
        return px;
    // 8 bytes (RGB16F is stored as RGBA16F)
    case GL_RGB16F_ARB:
    case GL_RGBA16F_ARB:
    case GL_RGBA16:
    case GL_RGB16:
    case GL_RG32F:
        return px * 8;
    // 16 bytes
    case GL_RGB32F_ARB:
    case GL_RGBA32F_ARB:
        return px * 16;
    default:
        *known = qfalse;
        return px * 4;
    }
}

static void R_VramNoteUnknown(hzmVram_t *v, unsigned int fmt)
{
    if (!v->nUnknown) {
        v->firstUnknown = fmt;
    }
    v->nUnknown++;
}

/*
R_HzmVramCount - the walk. Reads the renderer's own tables only; makes no GL call.
*/
static void R_HzmVramCount(hzmVram_t *v)
{
    int i, j;

    memset(v, 0, sizeof(*v));

    for (i = 0; i < tr.numImages; i++) {
        image_t           *img = tr.images[i];
        unsigned long long bytes = 0;
        qboolean           known, allKnown = qtrue;
        int                w, h;

        if (!img || !img->texnum) {
            continue;
        }
        w = img->uploadWidth > 0 ? img->uploadWidth : img->width;
        h = img->uploadHeight > 0 ? img->uploadHeight : img->height;
        if (w < 1 || h < 1) {
            continue;
        }
        if (img->flags & IMGFLAG_MULTISAMPLE) {
            int samples = tr.msaaSamples > 1 ? tr.msaaSamples : 1;

            bytes = R_VramLevelBytes((unsigned int)img->internalFormat, w, h, &known) * (unsigned long long)samples;
            allKnown = known;
        } else {
            for (;;) {
                bytes += R_VramLevelBytes((unsigned int)img->internalFormat, w, h, &known);
                if (!known) {
                    allKnown = qfalse;
                }
                if (!(img->flags & IMGFLAG_MIPMAP) || (w == 1 && h == 1)) {
                    break;
                }
                w = w > 1 ? w >> 1 : 1;
                h = h > 1 ? h >> 1 : 1;
            }
            if (img->flags & IMGFLAG_CUBEMAP) {
                bytes *= 6;
            }
        }
        if (!allKnown) {
            R_VramNoteUnknown(v, (unsigned int)img->internalFormat);
        }
        if (img->imgName[0] == '_' || img->imgName[0] == '*') {
            v->rt += bytes;
            v->nRt++;
        } else {
            v->tex += bytes;
            v->nTex++;
        }
    }

    for (i = 0; i < tr.numFBOs; i++) {
        FBO_t             *fbo = tr.fbos[i];
        unsigned long long px;
        qboolean           known;
        int                samples = 1;

        if (!fbo || fbo->width < 1 || fbo->height < 1) {
            continue;
        }
        px = 0;
        // the one FBO whose renderbuffers are multisampled: the legacy-path scene (tr_fbo.c FBO_Init writes the
        // count it used back into the latched cvar, so ->integer is the live count for this renderer lifetime)
        if (fbo == tr.sceneFbo && !tr.msaaNewPath && r_ext_framebuffer_multisample
            && r_ext_framebuffer_multisample->integer > 1) {
            samples = r_ext_framebuffer_multisample->integer;
        }
        for (j = 0; j < 16; j++) {
            if (fbo->colorBuffers[j]) {
                v->rb += R_VramLevelBytes((unsigned int)fbo->colorFormat, fbo->width, fbo->height, &known)
                       * (unsigned long long)samples;
                v->nRb++;
                if (!known) {
                    R_VramNoteUnknown(v, (unsigned int)fbo->colorFormat);
                }
            }
        }
        if (fbo->depthBuffer) {
            v->rb += R_VramLevelBytes((unsigned int)fbo->depthFormat, fbo->width, fbo->height, &known)
                   * (unsigned long long)samples;
            v->nRb++;
            if (!known) {
                R_VramNoteUnknown(v, (unsigned int)fbo->depthFormat);
            }
        }
        if (fbo->stencilBuffer) {
            v->rb += R_VramLevelBytes((unsigned int)fbo->stencilFormat, fbo->width, fbo->height, &known)
                   * (unsigned long long)samples;
            v->nRb++;
            if (!known) {
                R_VramNoteUnknown(v, (unsigned int)fbo->stencilFormat);
            }
        }
        if (fbo->packedDepthStencilBuffer) {
            v->rb += R_VramLevelBytes((unsigned int)fbo->packedDepthStencilFormat, fbo->width, fbo->height, &known)
                   * (unsigned long long)samples;
            v->nRb++;
            if (!known) {
                R_VramNoteUnknown(v, (unsigned int)fbo->packedDepthStencilFormat);
            }
        }
        (void)px;
    }

    for (i = 0; i < tr.numVaos; i++) {
        vao_t *vao = tr.vaos[i];

        if (!vao) {
            continue;
        }
        if (vao->vertexesVBO && vao->vertexesSize > 0) {
            v->vao += (unsigned long long)vao->vertexesSize;
        }
        if (vao->indexesIBO && vao->indexesSize > 0) {
            v->vao += (unsigned long long)vao->indexesSize;
        }
        v->nVao++;
    }
}

/*
The Windows figures. All read-only queries; -1 = not available. The adapter is the one with the most dedicated
memory (a laptop's integrated chip reports a few hundred MB; the card the game renders on is the big one).
*/
typedef struct {
    int cardMB;   // DXGI DedicatedVideoMemory of the chosen adapter
    int budgetMB; // IDXGIAdapter3 local-segment Budget for this process
    int usageMB;  // IDXGIAdapter3 local-segment CurrentUsage of this process
} hzmVramOs_t;

static void R_HzmVramOs(hzmVramOs_t *o)
{
    o->cardMB = o->budgetMB = o->usageMB = -1;
#ifdef _WIN32
    {
        // the two interface ids, spelled out so no dxguid.lib is needed
        static const GUID iidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
        static const GUID iidAdapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
        typedef HRESULT(WINAPI * createFactory1_t)(REFIID, void **);
        HMODULE          dxgi = LoadLibraryA("dxgi.dll");
        createFactory1_t create;
        IDXGIFactory1   *factory = NULL;
        IDXGIAdapter1   *adapter = NULL, *best = NULL;
        SIZE_T           bestMem = 0;
        UINT             n;

        if (!dxgi) {
            return;
        }
        create = (createFactory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1");
        if (!create || FAILED(create(&iidFactory1, (void **)&factory)) || !factory) {
            FreeLibrary(dxgi);
            return;
        }
        for (n = 0; IDXGIFactory1_EnumAdapters1(factory, n, &adapter) == S_OK; n++) {
            DXGI_ADAPTER_DESC1 desc;

            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && desc.DedicatedVideoMemory > bestMem) {
                if (best) {
                    IDXGIAdapter1_Release(best);
                }
                best    = adapter;
                bestMem = desc.DedicatedVideoMemory;
            } else {
                IDXGIAdapter1_Release(adapter);
            }
            adapter = NULL;
        }
        if (best) {
            IDXGIAdapter3 *a3 = NULL;

            o->cardMB = (int)(bestMem >> 20);
            if (SUCCEEDED(IDXGIAdapter1_QueryInterface(best, &iidAdapter3, (void **)&a3)) && a3) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info;

                memset(&info, 0, sizeof(info));
                if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                    o->budgetMB = (int)(info.Budget >> 20);
                    o->usageMB  = (int)(info.CurrentUsage >> 20);
                }
                IDXGIAdapter3_Release(a3);
            }
            IDXGIAdapter1_Release(best);
        }
        IDXGIFactory1_Release(factory);
        FreeLibrary(dxgi);
    }
#endif
}

/*
================
R_HzmVramAcct_f - console command `vramacct`: one machine-readable line for the P2.0 measurement
(docs/proposals/graphics_menu_2026-09-27/plan.md 4.1). The harness reads the per-process GPU counter from outside
and compares it with ours=.
================
*/
void R_HzmVramAcct_f(void)
{
    hzmVram_t   v;
    hzmVramOs_t o;
    int         nvxTotal = -1, nvxAvail = -1;
    double      mb = 1.0 / (1024.0 * 1024.0);

    R_HzmVramCount(&v);
    R_HzmVramOs(&o);
    if (glRefConfig.memInfo == MI_NVX) {
        int value = 0;

        qglGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &value);
        nvxTotal = value / 1024;
        value    = 0;
        qglGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &value);
        nvxAvail = value / 1024;
    }
    ri.Printf(
        PRINT_ALL,
        "^~^~^ VRAMACCT tex=%.1f rt=%.1f rb=%.1f vao=%.1f ours=%.1f n=%d/%d/%d/%d unknown=%d:0x%x msaa=%d newpath=%d "
        "vid=%dx%d dxgi_card=%d dxgi_budget=%d dxgi_usage=%d nvx_total=%d nvx_avail=%d\n",
        (double)v.tex * mb, (double)v.rt * mb, (double)v.rb * mb, (double)v.vao * mb,
        (double)(v.tex + v.rt + v.rb + v.vao) * mb, v.nTex, v.nRt, v.nRb, v.nVao, v.nUnknown, v.firstUnknown,
        tr.msaaNewPath ? tr.msaaSamples
                       : (r_ext_framebuffer_multisample ? r_ext_framebuffer_multisample->integer : 0),
        tr.msaaNewPath ? 1 : 0, glConfig.vidWidth, glConfig.vidHeight, o.cardMB, o.budgetMB, o.usageMB, nvxTotal,
        nvxAvail
    );
}

/*
===========================================================================
P2.1 - THE NUMBERS THE VIDEO PANEL SHOWS (plan section 4.2 / 4.3; user 2026-10-12: "on by default, assuming it is
smart enough to detect the video card, vram and usage for anyone. If it can't it needs to be able to").

Switch: r_hzmVramBar (archived, -1 = automatic = HZM_VRAMBAR_AUTO; 0 off; 1 on; 2 on + one VRAMPOLL line per
publish). Off / dormant: r_vramShow stays 0, the menu block draws nothing, R_HzmVramEndFrame returns on its first
test, no thread is started and no Windows library is loaded.

WHAT IS SHOWN, AND FROM WHERE. Each number has its own chain; the first source that answers wins, and a number no
source can give is LEFT OUT (the line says so or is empty) - never estimated.

  this game        our own count (the walk above). Always known. P2.0: within 1.3 % of the per-process counter.
  adapter          the one the game DRAWS on: DXGI adapters matched to the GL device (tr_hzm_vram_pick.h: LUID from
                   GL_EXT_memory_object_win32, else the renderer name, else the vendor, else the only one).
                   No DXGI: the name is GL_RENDERER.
  scale of the bar 1. a card with its own memory (512 MB or more dedicated): DXGI DedicatedVideoMemory
                   2. an integrated chip (little or no dedicated memory - it draws from system RAM): the budget
                      Windows gives this process, else dedicated + shared from DXGI. Never "0 GB".
                   3. no DXGI adapter: GL_NVX_gpu_memory_info total (NVIDIA), else the registry
                      (HardwareInformation.qwMemorySize of the display class entry whose name matches GL_RENDERER)
                   4. none: no bar. The lines still show.
  the limit        IDXGIAdapter3::QueryVideoMemoryInfo budget, local segment (Windows 10+, any vendor). Else no
                   marker, and the line names the card size or the shared memory instead.
  other programs   1. the adapter's dedicated (integrated: shared) usage from the Windows GPU counter of that adapter's
                      LUID, minus this process's usage (DXGI CurrentUsage, else our count)
                   2. NVIDIA: NVX total - available - our count      3. AMD: GL_ATI_meminfo free memory, the same way
                   4. none: the segment is not drawn and the line reads "OTHER n/a"
A guess of the adapter ("largest", two cards nothing tells apart) uses nothing of that adapter's that could belong
to the other one: no shared-memory scale, no budget marker.

NEVER ON THE RENDER THREAD. Opening DXGI and the counter library can take from a few ms to, on a machine with broken
counters, seconds. All of it runs on one worker thread of this DLL (opened at R_Init, polled on request); the render
thread only signals it and reads its last answer under a lock, and makes the two cheap GL queries itself. If the
worker never answers, the block shows the game's own number alone. RE_Shutdown stops the worker (3 s grace, then
terminated: the DLL is about to unload).

"Costs nothing while the menu is closed": nothing here runs on a timer. One transparent Label of the menu block uses
a texture of its own (VRAM_MARK_IMAGE); gl2 stamps image->frameUsed on every bind (tr_backend.c GL_BindToTMU), so
"the block is on screen" is one integer compare per frame in RE_EndFrame - and only then, at most twice a second, are
the tables walked and the worker asked. Closed menu: that compare, nothing else.

Test hooks (flags 0, never saved): r_vramDebugNo, a bit mask that takes sources away - 1 LUID, 2 DXGI adapter list,
4 DXGI budget, 8 Windows counters, 16 NVX, 32 ATI, 64 registry; r_vramDebugAdapter N, use DXGI adapter N whatever
the match says (N+1; 0 = off) - e.g. Microsoft's software adapter, which has no dedicated memory at all, to see the
integrated-chip path; r_vramDebugCardMB, pretend the scale and the limit are this small.
===========================================================================
*/
#include "tr_hzm_vram_pick.h"

#define HZM_VRAMBAR_AUTO 0
#define VRAM_MARK_IMAGE  "textures/hud/vram/mark" // as the UI names it: no extension (docs/tools/gen_vram_panel.py)
#define VRAM_POLL_MS     500
#define VRAM_UMA_MB      512 // less dedicated memory than this = an integrated chip drawing from system RAM

static cvar_t  *r_hzmVramBar;
static qboolean s_vramOn       = qfalse;
static image_t *s_vramMark     = NULL;
static int      s_vramLastPoll = -100000;
static int      s_vramSerialPublished = -1;
static int      s_vramLastPublish     = 0;

// what the worker knows; copied whole under the lock
typedef struct {
    int          ready;    // the open has finished (successfully or not)
    int          serial;   // +1 after the open and after every poll
    int          nAdapters;
    hzmAdapter_t adapters[HZM_VRAM_MAX_ADAPTERS];
    int          pick;     // index into adapters, -1
    char         how[12];
    int          guess;
    int          dup;      // one card listed several times: the pick's LUID may not be the game's
    int          budgetMB; // -1 = not available
    int          procMB;
    int          adapterUseMB;
    int          regMB;    // registry total for the GL renderer's name, -1
    int          openMs, pollMs;
} hzmVramOsState_t;

// what the render thread hands the worker before it starts
typedef struct {
    char         glVendor[128];
    char         glRenderer[160];
    int          haveLuid;
    unsigned int luidLow;
    int          luidHigh;
    int          debugNo;
    int          debugAdapter;
} hzmVramOsIn_t;

static hzmVramOsState_t s_os;
static hzmVramOsIn_t    s_osIn;

#ifdef _WIN32
#    include <pdh.h>

static CRITICAL_SECTION s_osLock;
static qboolean         s_osLockInit = qfalse;
static HANDLE           s_osThread   = NULL;
static HANDLE           s_osEvent    = NULL;
static volatile LONG    s_osQuit     = 0;

typedef PDH_STATUS(WINAPI *pdhOpen_t)(LPCSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *pdhAdd_t)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *pdhCollect_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *pdhValue_t)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
typedef PDH_STATUS(WINAPI *pdhClose_t)(PDH_HQUERY);

// the registry's idea of the card's memory: the display class entry whose DriverDesc matches the GL renderer name
static int R_HzmVramRegistryMB(const char *glRenderer)
{
    static const char *cls = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY               hCls;
    int                i, best = -1, bestScore = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &hCls) != ERROR_SUCCESS) {
        return -1;
    }
    for (i = 0; i < 16; i++) {
        char      sub[8], desc[160];
        HKEY      h;
        DWORD     type = 0, size = sizeof(desc);
        ULONGLONG q    = 0;
        int       score;

        Com_sprintf(sub, sizeof(sub), "%04d", i);
        if (RegOpenKeyExA(hCls, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) {
            continue; // not every number exists, and some are not readable: neither ends the search
        }
        desc[0] = 0;
        if (RegQueryValueExA(h, "DriverDesc", NULL, &type, (LPBYTE)desc, &size) == ERROR_SUCCESS && type == REG_SZ) {
            desc[sizeof(desc) - 1] = 0;
            score = HZM_VramNameScore(glRenderer, desc);
            size  = sizeof(q);
            if (score > bestScore
                && RegQueryValueExA(h, "HardwareInformation.qwMemorySize", NULL, &type, (LPBYTE)&q, &size) == ERROR_SUCCESS
                && type == REG_QWORD && q >= (64ULL << 20)) {
                bestScore = score;
                best      = (int)(q >> 20);
            }
        }
        RegCloseKey(h);
    }
    RegCloseKey(hCls);
    return best;
}

static DWORD WINAPI R_HzmVramWorker(LPVOID arg)
{
    static const GUID iidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    static const GUID iidAdapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
    typedef HRESULT(WINAPI * createFactory1_t)(REFIID, void **);
    hzmVramOsIn_t    in = s_osIn; // fixed for this worker's life
    hzmVramOsState_t st;
    HMODULE          dxgiDll = NULL, pdhDll = NULL;
    IDXGIFactory1   *factory = NULL;
    IDXGIAdapter1   *kept[HZM_VRAM_MAX_ADAPTERS];
    IDXGIAdapter3   *adapter3 = NULL;
    PDH_HQUERY       query    = NULL;
    PDH_HCOUNTER     counter  = NULL;
    pdhCollect_t     pdhCollect = NULL;
    pdhValue_t       pdhValue   = NULL;
    pdhClose_t       pdhClose   = NULL;
    qboolean         haveCounter = qfalse;
    DWORD            t0 = GetTickCount();
    int              i;

    (void)arg;
    memset(&st, 0, sizeof(st));
    memset(kept, 0, sizeof(kept));
    st.pick = -1;
    st.budgetMB = st.procMB = st.adapterUseMB = st.regMB = -1;
    Q_strncpyz(st.how, "none", sizeof(st.how));

    // ---- the adapter list ------------------------------------------------------------------------------
    if (!(in.debugNo & 2)) {
        dxgiDll = LoadLibraryA("dxgi.dll");
    }
    if (dxgiDll) {
        createFactory1_t create = (createFactory1_t)GetProcAddress(dxgiDll, "CreateDXGIFactory1");

        if (create && SUCCEEDED(create(&iidFactory1, (void **)&factory)) && factory) {
            IDXGIAdapter1 *a = NULL;
            UINT           n;

            for (n = 0; st.nAdapters < HZM_VRAM_MAX_ADAPTERS && IDXGIFactory1_EnumAdapters1(factory, n, &a) == S_OK; n++) {
                DXGI_ADAPTER_DESC1 d;

                if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) {
                    hzmAdapter_t *o = &st.adapters[st.nAdapters];

                    WideCharToMultiByte(CP_ACP, 0, d.Description, -1, o->name, sizeof(o->name) - 1, "?", NULL);
                    o->name[sizeof(o->name) - 1] = 0;
                    o->vendorId    = d.VendorId;
                    o->deviceId    = d.DeviceId;
                    o->dedicatedMB = (unsigned int)(d.DedicatedVideoMemory >> 20);
                    o->sharedMB    = (unsigned int)(d.SharedSystemMemory >> 20);
                    o->luidLow     = d.AdapterLuid.LowPart;
                    o->luidHigh    = d.AdapterLuid.HighPart;
                    o->software    = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0;
                    kept[st.nAdapters++] = a;
                } else {
                    IDXGIAdapter1_Release(a);
                }
                a = NULL;
            }
        }
    }
    if (st.nAdapters > 0) {
        hzmPick_t p = HZM_VramPickAdapter(
            st.adapters, st.nAdapters, in.haveLuid && !(in.debugNo & 1), in.luidLow, in.luidHigh, in.glVendor,
            in.glRenderer
        );

        if (in.debugAdapter > 0 && in.debugAdapter <= st.nAdapters) {
            p.index = in.debugAdapter - 1;
            p.how   = "forced";
            p.guess = 0;
            p.dup   = 0;
        }
        st.pick  = p.index;
        st.guess = p.guess;
        st.dup   = p.dup;
        Q_strncpyz(st.how, p.how, sizeof(st.how));
    }
    // ---- the limit: IDXGIAdapter3 of the pick (Windows 10+) -------------------------------------------------
    if (st.pick >= 0 && !(in.debugNo & 4)) {
        if (FAILED(IDXGIAdapter1_QueryInterface(kept[st.pick], &iidAdapter3, (void **)&adapter3))) {
            adapter3 = NULL;
        }
    }
    // ---- the adapter-wide counter, by name (no import: the renderer must load where pdh.dll is missing) -----
    if (st.pick >= 0 && !(in.debugNo & 8)) {
        pdhDll = LoadLibraryA("pdh.dll");
        if (pdhDll) {
            pdhOpen_t open = (pdhOpen_t)GetProcAddress(pdhDll, "PdhOpenQueryA");
            pdhAdd_t  add  = (pdhAdd_t)GetProcAddress(pdhDll, "PdhAddEnglishCounterA");
            char      path[176];
            // an integrated chip's memory is all "shared"; a card's is "dedicated"
            const char *what = (st.adapters[st.pick].dedicatedMB < VRAM_UMA_MB) ? "Shared Usage" : "Dedicated Usage";

            pdhCollect = (pdhCollect_t)GetProcAddress(pdhDll, "PdhCollectQueryData");
            pdhValue   = (pdhValue_t)GetProcAddress(pdhDll, "PdhGetFormattedCounterValue");
            pdhClose   = (pdhClose_t)GetProcAddress(pdhDll, "PdhCloseQuery");
            Com_sprintf(
                path, sizeof(path), "\\GPU Adapter Memory(luid_0x%08lX_0x%08lX_phys_0)\\%s",
                (unsigned long)st.adapters[st.pick].luidHigh, (unsigned long)st.adapters[st.pick].luidLow, what
            );
            if (open && add && pdhCollect && pdhValue && pdhClose && open(NULL, 0, &query) == ERROR_SUCCESS) {
                if (add(query, path, 0, &counter) == ERROR_SUCCESS && pdhCollect(query) == ERROR_SUCCESS) {
                    haveCounter = qtrue;
                } else {
                    pdhClose(query);
                    query = NULL;
                }
            }
        }
    }
    // ---- the registry, only when DXGI gave no adapter at all -----------------------------------------------------
    if (st.pick < 0 && !(in.debugNo & 64)) {
        st.regMB = R_HzmVramRegistryMB(in.glRenderer);
    }
    st.openMs = (int)(GetTickCount() - t0);

    for (;;) {
        DWORD t1 = GetTickCount();

        st.budgetMB = st.procMB = st.adapterUseMB = -1;
        if (adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;

            memset(&info, 0, sizeof(info));
            if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))
                && info.Budget > 0) {
                st.budgetMB = (int)(info.Budget >> 20);
                st.procMB   = (int)(info.CurrentUsage >> 20);
            }
        }
        if (haveCounter) {
            PDH_FMT_COUNTERVALUE v;

            if (pdhCollect(query) == ERROR_SUCCESS && pdhValue(counter, PDH_FMT_LARGE, NULL, &v) == ERROR_SUCCESS
                && v.CStatus == ERROR_SUCCESS && v.largeValue >= 0) {
                st.adapterUseMB = (int)(v.largeValue >> 20);
            }
        }
        st.pollMs = (int)(GetTickCount() - t1);
        st.ready  = 1;
        st.serial++;
        EnterCriticalSection(&s_osLock);
        s_os = st;
        LeaveCriticalSection(&s_osLock);

        WaitForSingleObject(s_osEvent, INFINITE);
        if (InterlockedCompareExchange(&s_osQuit, 0, 0)) {
            break;
        }
    }

    if (query && pdhClose) {
        pdhClose(query);
    }
    if (adapter3) {
        IDXGIAdapter3_Release(adapter3);
    }
    for (i = 0; i < HZM_VRAM_MAX_ADAPTERS; i++) {
        if (kept[i]) {
            IDXGIAdapter1_Release(kept[i]);
        }
    }
    if (factory) {
        IDXGIFactory1_Release(factory);
    }
    if (pdhDll) {
        FreeLibrary(pdhDll);
    }
    if (dxgiDll) {
        FreeLibrary(dxgiDll);
    }
    return 0;
}

static void R_HzmVramOsStart(void)
{
    if (s_osThread) {
        return;
    }
    if (!s_osLockInit) {
        InitializeCriticalSection(&s_osLock);
        s_osLockInit = qtrue;
    }
    memset(&s_os, 0, sizeof(s_os));
    s_os.pick = -1;
    s_os.budgetMB = s_os.procMB = s_os.adapterUseMB = s_os.regMB = -1;
    s_osQuit  = 0;
    s_osEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (s_osEvent) {
        s_osThread = CreateThread(NULL, 0, R_HzmVramWorker, NULL, 0, NULL);
    }
}

// ask for fresh numbers; returns at once
static void R_HzmVramOsRequest(void)
{
    if (s_osThread && s_osEvent) {
        SetEvent(s_osEvent);
    }
}

static void R_HzmVramOsSnapshot(hzmVramOsState_t *out)
{
    if (!s_osLockInit) {
        memset(out, 0, sizeof(*out));
        out->pick = -1;
        out->budgetMB = out->procMB = out->adapterUseMB = out->regMB = -1;
        return;
    }
    EnterCriticalSection(&s_osLock);
    *out = s_os;
    LeaveCriticalSection(&s_osLock);
}

static void R_HzmVramOsStop(void)
{
    if (s_osThread) {
        InterlockedExchange(&s_osQuit, 1);
        SetEvent(s_osEvent);
        if (WaitForSingleObject(s_osThread, 3000) != WAIT_OBJECT_0) {
            // a counter library that never returns: the DLL is about to unload, the thread cannot be left in it
            TerminateThread(s_osThread, 0);
        }
        CloseHandle(s_osThread);
        s_osThread = NULL;
    }
    if (s_osEvent) {
        CloseHandle(s_osEvent);
        s_osEvent = NULL;
    }
}

// the GL device's LUID, when the driver has GL_EXT_memory_object_win32 (render thread)
static void R_HzmVramGlLuid(hzmVramOsIn_t *in)
{
    typedef void(APIENTRY * getUBv_t)(GLenum, GLubyte *);
    getUBv_t get;

    in->haveLuid = 0;
    if (!SDL_GL_ExtensionSupported("GL_EXT_memory_object_win32")) {
        return;
    }
    get = (getUBv_t)SDL_GL_GetProcAddress("glGetUnsignedBytevEXT");
    if (get) {
        GLubyte luid[8];
        int     i;

        memset(luid, 0, sizeof(luid));
        get(0x9599 /* GL_DEVICE_LUID_EXT */, luid);
        for (i = 0; i < 16 && qglGetError() != GL_NO_ERROR; i++) {
            luid[0] = luid[1] = luid[2] = luid[3] = luid[4] = luid[5] = luid[6] = luid[7] = 0; // not answered
        }
        in->luidLow  = (unsigned int)luid[0] | ((unsigned int)luid[1] << 8) | ((unsigned int)luid[2] << 16) | ((unsigned int)luid[3] << 24);
        in->luidHigh = (int)((unsigned int)luid[4] | ((unsigned int)luid[5] << 8) | ((unsigned int)luid[6] << 16) | ((unsigned int)luid[7] << 24));
        in->haveLuid = (in->luidLow || in->luidHigh) ? 1 : 0;
    }
}
#else
static void R_HzmVramOsStart(void) {}
static void R_HzmVramOsRequest(void) {}
static void R_HzmVramOsStop(void) {}
static void R_HzmVramGlLuid(hzmVramOsIn_t *in)
{
    in->haveLuid = 0;
}
static void R_HzmVramOsSnapshot(hzmVramOsState_t *out)
{
    memset(out, 0, sizeof(*out));
    out->pick = -1;
    out->budgetMB = out->procMB = out->adapterUseMB = out->regMB = -1;
}
#endif

static void R_VramSet(const char *name, const char *value)
{
    cvar_t *cv = ri.Cvar_Get(name, "", CVAR_ROM);

    if (!cv || strcmp(cv->string, value)) {
        ri.Cvar_Set(name, value);
    }
}

static const char *R_VramLatched(cvar_t *cv)
{
    return (cv && cv->latchedString) ? cv->latchedString : (cv ? cv->string : "");
}

/*
R_HzmVramPendingBytes - what Apply would add (+) or free (-), from the settings that are set but not in force yet.
The same arithmetic the creation code uses, on the latched values:
  anti-aliasing   the two multisample scene images: scene pixels x (colour bytes + 4 depth) x samples
  render scale    every scene-sized target, x (new / old) squared
  shadows         the four sun shadow maps (and their colour buffers), x (new / old size) squared, or all on / off
  textures        the picmip-eligible textures, x 4 per picmip step (a first step up cannot exceed the source art -
                  an upper bound)
Auto anti-aliasing (-1) cannot be predicted without redoing the driver query, so a pending change TO Auto shows
no difference.
*/
static double R_HzmVramPendingBytes(const hzmVram_t *now)
{
    extern cvar_t *r_shadowMapSize, *r_sunShadows, *r_picmip;
    double         delta = 0.0;
    cvar_t        *msaa  = ri.Cvar_Get("r_msaa", "-1", CVAR_ARCHIVE);
    int            i, curS, newS;
    qboolean       known;

    (void)now;
    // anti-aliasing (the new path only; the legacy path's renderbuffers have no menu row)
    if (tr.msaaNewPath || (msaa && msaa->integer > 1)) {
        curS = (tr.msaaNewPath && tr.msaaSamples > 1) ? tr.msaaSamples : 0;
        newS = msaa ? msaa->integer : -1;
        if (newS == 1) {
            newS = 0;
        }
        if (newS >= 0 && newS != curS && tr.sceneWidth > 0 && tr.sceneHeight > 0) {
            double px  = (double)tr.sceneWidth * (double)tr.sceneHeight;
            double bpp = (tr.renderImage
                          ? (double)R_VramLevelBytes((unsigned int)tr.renderImage->internalFormat, 1, 1, &known)
                          : 4.0)
                       + 4.0;

            delta += px * bpp * (double)(newS - curS);
            // switching it on or off also makes or drops the resolve targets (tr_image.c: *sceneLinear RGBA16F,
            // *msaaDepthMinMax RG32F) and one depth renderbuffer: 20 bytes a pixel. With this the predicted 0 -> 8x
            // step at 1920x1080 is 229 MB; P2.0 measured 232-236 MB.
            if ((newS > 1) != (curS > 1)) {
                delta += px * 20.0 * ((newS > 1) ? 1.0 : -1.0);
            }
        }
    }
    // render scale
    if (r_renderScale && r_renderScale->latchedString && r_renderScale->value > 0.0f) {
        float  nv = (float)atof(r_renderScale->latchedString);
        double k;

        if (nv < 0.5f) {
            nv = 0.5f;
        } else if (nv > 2.0f) {
            nv = 2.0f;
        }
        k = ((double)nv / (double)r_renderScale->value);
        k = k * k - 1.0;
        for (i = 0; i < tr.numImages; i++) {
            image_t *img = tr.images[i];
            int      d;

            if (!img || !img->texnum || (img->imgName[0] != '_' && img->imgName[0] != '*')) {
                continue;
            }
            for (d = 0; d < 3; d++) {
                if (img->uploadWidth == (tr.sceneWidth >> d) && img->uploadHeight == (tr.sceneHeight >> d)) {
                    int samples = (img->flags & IMGFLAG_MULTISAMPLE) ? (tr.msaaSamples > 1 ? tr.msaaSamples : 1) : 1;

                    delta += k * (double)R_VramLevelBytes((unsigned int)img->internalFormat, img->uploadWidth,
                                                         img->uploadHeight, &known) * (double)samples;
                    break;
                }
            }
        }
    }
    // sun shadows: 4 depth maps, size x size, and a colour buffer each when the FBOs carry one
    if (r_shadowMapSize && r_sunShadows) {
        int    curOn = r_sunShadows->integer ? 1 : 0, newOn = atoi(R_VramLatched(r_sunShadows)) ? 1 : 0;
        int    curSz = r_shadowMapSize->integer, newSz = atoi(R_VramLatched(r_shadowMapSize));
        double per   = 4.0; // GL_DEPTH_COMPONENT24

        if (tr.sunShadowFbo[0] && tr.sunShadowFbo[0]->colorBuffers[0]) {
            per += 4.0; // + the RGBA8 colour buffer
        }
        if (curOn != newOn || (curOn && newOn && curSz != newSz)) {
            double cur = curOn ? 4.0 * per * (double)curSz * (double)curSz : 0.0;
            double nw  = newOn ? 4.0 * per * (double)newSz * (double)newSz : 0.0;

            delta += nw - cur;
        }
    }
    // textures
    if (r_picmip && r_picmip->latchedString) {
        int step = r_picmip->integer - atoi(r_picmip->latchedString); // + = sharper
        if (step) {
            double k = 1.0, sum = 0.0;

            for (i = 0; i < (step > 0 ? step : -step); i++) {
                k *= (step > 0) ? 4.0 : 0.25;
            }
            for (i = 0; i < tr.numImages; i++) {
                image_t *img = tr.images[i];
                int      w, h;

                if (!img || !img->texnum || !(img->flags & IMGFLAG_PICMIP) || img->imgName[0] == '_'
                    || img->imgName[0] == '*') {
                    continue;
                }
                w = img->uploadWidth;
                h = img->uploadHeight;
                for (;;) {
                    sum += (double)R_VramLevelBytes((unsigned int)img->internalFormat, w, h, &known);
                    if (!(img->flags & IMGFLAG_MIPMAP) || (w == 1 && h == 1)) {
                        break;
                    }
                    w = w > 1 ? w >> 1 : 1;
                    h = h > 1 ? h >> 1 : 1;
                }
            }
            delta += sum * (k - 1.0);
        }
    }
    return delta;
}

// what the chain resolved to at the last publish - also what `vramacct` and the bug report read
typedef struct {
    char name[128];     // adapter name
    char how[12];       // how the adapter was matched
    char src[64];       // which sources answered, e.g. "dxgi+budget+counters"
    int  uma;           // an integrated chip (draws from system RAM)
    int  scaleMB;       // the bar's 100 %, 0 = unknown (no bar)
    int  limitMB;       // the Windows budget, 0 = unknown
    int  gameMB, pendMB, otherMB; // otherMB -1 = unknown
    int  openMs, pollMs;
} hzmVramView_t;

static hzmVramView_t s_view;

static void R_HzmVramResolve(hzmVramView_t *v)
{
    hzmVram_t        count;
    hzmVramOsState_t os;
    cvar_t          *dbgNo   = ri.Cvar_Get("r_vramDebugNo", "0", 0);
    cvar_t          *dbgCard = ri.Cvar_Get("r_vramDebugCardMB", "0", 0);
    int              no      = dbgNo ? dbgNo->integer : 0;
    int              nvxTotalMB = -1, nvxAvailMB = -1, atiFreeMB = -1, procMB;
    const hzmAdapter_t *a = NULL;

    memset(v, 0, sizeof(*v));
    v->otherMB = -1;
    R_HzmVramCount(&count);
    R_HzmVramOsSnapshot(&os);
    v->gameMB = (int)((count.tex + count.rt + count.rb + count.vao) >> 20);
    v->pendMB = (int)(R_HzmVramPendingBytes(&count) / 1048576.0);
    if (v->pendMB > -16 && v->pendMB < 16) {
        v->pendMB = 0; // not worth a lamp
    }
    v->openMs = os.openMs;
    v->pollMs = os.pollMs;

    // the two vendor extensions: cheap GL queries, made here on the render thread
    if (glRefConfig.memInfo == MI_NVX && !(no & 16)) {
        int totalKb = 0, availKb = 0;

        qglGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &totalKb);
        qglGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &availKb);
        if (totalKb > 0) {
            nvxTotalMB = totalKb / 1024;
            nvxAvailMB = availKb / 1024;
        }
    } else if (glRefConfig.memInfo == MI_ATI && !(no & 32)) {
        int freeKb[4] = {0, 0, 0, 0};

        qglGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, freeKb);
        if (freeKb[0] > 0) {
            atiFreeMB = freeKb[0] / 1024;
        }
    }

    // ---- adapter, name ----------------------------------------------------------------------------------
    if (os.ready && os.pick >= 0 && os.pick < os.nAdapters) {
        a = &os.adapters[os.pick];
        Q_strncpyz(v->name, a->name, sizeof(v->name));
        Q_strncpyz(v->how, os.how, sizeof(v->how));
        Q_strcat(v->src, sizeof(v->src), "dxgi");
        v->uma = (a->dedicatedMB < VRAM_UMA_MB) ? 1 : 0;
    } else {
        char *slash;

        Q_strncpyz(v->name, glConfig.renderer_string, sizeof(v->name));
        slash = strchr(v->name, '/');
        if (slash) {
            *slash = 0; // NVIDIA appends "/PCIe/SSE2"
        }
        Q_strncpyz(v->how, os.ready ? "gl-name" : "pending", sizeof(v->how));
    }
    // ---- the limit -----------------------------------------------------------------------------------------
    if (a && os.budgetMB > 0 && !os.guess && !os.dup) {
        v->limitMB = os.budgetMB;
        Q_strcat(v->src, sizeof(v->src), "+budget");
    }
    // ---- the scale -------------------------------------------------------------------------------------------
    if (a && !v->uma) {
        v->scaleMB = (int)a->dedicatedMB;
    } else if (a && v->uma && !os.guess) {
        v->scaleMB = (v->limitMB > 0) ? v->limitMB : (int)(a->dedicatedMB + a->sharedMB);
    } else if (!a && nvxTotalMB > 0) {
        v->scaleMB = nvxTotalMB;
        Q_strcat(v->src, sizeof(v->src), v->src[0] ? "+nvx-total" : "nvx-total");
    } else if (!a && os.regMB > 0) {
        v->scaleMB = os.regMB;
        Q_strcat(v->src, sizeof(v->src), v->src[0] ? "+registry" : "registry");
    }
    // ---- other programs ----------------------------------------------------------------------------------------
    procMB = (os.procMB >= 0 && !os.dup) ? os.procMB : v->gameMB;
    if (a && os.adapterUseMB >= 0 && !os.guess && !os.dup) {
        v->otherMB = os.adapterUseMB - procMB;
        Q_strcat(v->src, sizeof(v->src), "+counters");
    } else if (nvxTotalMB > 0 && nvxAvailMB >= 0) {
        v->otherMB = nvxTotalMB - nvxAvailMB - v->gameMB;
        Q_strcat(v->src, sizeof(v->src), v->src[0] ? "+nvx" : "nvx");
    } else if (atiFreeMB >= 0 && v->scaleMB > 0 && !v->uma) {
        v->otherMB = v->scaleMB - atiFreeMB - v->gameMB;
        Q_strcat(v->src, sizeof(v->src), v->src[0] ? "+ati" : "ati");
    }
    if (v->otherMB < -1) {
        v->otherMB = 0; // the two readings are taken a moment apart
    }
    if (!v->src[0]) {
        Q_strncpyz(v->src, "count-only", sizeof(v->src));
    }
    // test hook: pretend the scale and the limit are this small
    if (dbgCard && dbgCard->integer > 0) {
        v->scaleMB = dbgCard->integer;
        v->limitMB = (int)(dbgCard->integer * 0.94f);
        if (v->otherMB > v->scaleMB / 6) {
            v->otherMB = v->scaleMB / 6;
        }
    }
}

static const char *R_VramShortGB(int mb)
{
    // one decimal below 10 GB, none above: the lines sit in fixed, narrow places on the panel
    double gb = (double)mb / 1024.0;

    return (gb < 9.95) ? va("%.1f GB", gb) : va("%.0f GB", gb);
}

static void R_HzmVramPublish(void)
{
    hzmVramView_t *v = &s_view;
    int            afterMB, total, hot = 0, publishUs = 0;
#ifdef _WIN32
    LARGE_INTEGER q0, q1, qf;

    QueryPerformanceCounter(&q0);
#endif

    R_HzmVramResolve(v);
    s_vramLastPublish = ri.Milliseconds();
    afterMB = v->gameMB + (v->pendMB > 0 ? v->pendMB : 0);
    total   = afterMB + (v->otherMB > 0 ? v->otherMB : 0);

    R_VramSet("r_vramShow", "1");
    R_VramSet("r_vramAdapter", v->name);
    R_VramSet("r_vramHow", v->how);
    R_VramSet("r_vramSource", v->src);
    R_VramSet("r_vramCardMB", va("%d", v->scaleMB));
    R_VramSet("r_vramBudgetMB", va("%d", v->limitMB));
    R_VramSet("r_vramGameMB", va("%d", v->gameMB));
    R_VramSet("r_vramPendingMB", va("%d", v->pendMB));
    R_VramSet("r_vramOtherMB", va("%d", v->otherMB));
    if (v->scaleMB > 0) {
#define VRAM_PCT(mb) va("%d", (int)Com_Clamp(0.0f, 100.0f, (float)(mb) * 100.0f / (float)v->scaleMB + 0.5f))
        // at least 1: the first lamp is lit for any game share at all
        R_VramSet("r_vramBarGame", va("%d", (int)Com_Clamp(1.0f, 100.0f, (float)v->gameMB * 100.0f / (float)v->scaleMB + 0.5f)));
        R_VramSet("r_vramBarPending", v->pendMB > 0 ? VRAM_PCT(afterMB) : "0");
        R_VramSet("r_vramBarAll", v->otherMB > 0 ? VRAM_PCT(total) : "0");
        R_VramSet("r_vramBarBudget", (v->limitMB > 0 && v->limitMB < v->scaleMB) ? VRAM_PCT(v->limitMB) : "0");
#undef VRAM_PCT
        hot = ((double)afterMB >= 0.9 * (double)(v->limitMB > 0 ? v->limitMB : v->scaleMB)) ? 1 : 0;
    } else {
        R_VramSet("r_vramBarGame", "0");
        R_VramSet("r_vramBarPending", "0");
        R_VramSet("r_vramBarAll", "0");
        R_VramSet("r_vramBarBudget", "0");
    }
    R_VramSet("r_vramHot", hot ? "1" : "0");
    // the three lines. What is not known is said in a word or left empty - never filled in.
    if (v->pendMB) {
        R_VramSet("r_vramShortGame", va("APPLY %s", R_VramShortGB(v->gameMB + v->pendMB)));
    } else {
        R_VramSet("r_vramShortGame", va("GAME %s", R_VramShortGB(v->gameMB)));
    }
    R_VramSet("r_vramShortOther", v->otherMB >= 0 ? va("OTHER %s", R_VramShortGB(v->otherMB)) : "OTHER n/a");
    if (v->limitMB > 0) {
        R_VramSet("r_vramShortLimit", va("LIMIT %s", R_VramShortGB(v->limitMB)));
    } else if (v->scaleMB > 0) {
        R_VramSet("r_vramShortLimit", va(v->uma ? "RAM %s" : "CARD %s", R_VramShortGB(v->scaleMB)));
    } else {
        R_VramSet("r_vramShortLimit", "");
    }
#ifdef _WIN32
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    publishUs = (int)((q1.QuadPart - q0.QuadPart) * 1000000 / (qf.QuadPart ? qf.QuadPart : 1));
#endif
    if (r_hzmVramBar && r_hzmVramBar->integer >= 2) {
        // 2 = on + one line per publish, so a test can count them (none while the menu is closed). publish_us = what
        // this publish cost the render thread; open_ms / poll_ms = what the worker spent, off it.
        ri.Printf(
            PRINT_ALL,
            "^~^~^ VRAMPOLL frame=%d game=%d pending=%d other=%d limit=%d scale=%d uma=%d src=%s how=%s open_ms=%d "
            "poll_ms=%d publish_us=%d adapter=%s\n",
            tr.frameCount, v->gameMB, v->pendMB, v->otherMB, v->limitMB, v->scaleMB, v->uma, v->src, v->how, v->openMs,
            v->pollMs, publishUs, v->name
        );
    }
}

/*
R_HzmVramRegister - every R_Init (R_Register). The cvars exist from the first launch so the menu's gates and the
bug report always find them; dormant, they keep these values.
*/
void R_HzmVramRegister(void)
{
    r_hzmVramBar = ri.Cvar_Get("r_hzmVramBar", "-1", CVAR_ARCHIVE);
    // one literal call each: the menu generators' wire gates and the generated cvar docs find registrations by text
    ri.Cvar_Get("r_vramShow", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramAdapter", "", CVAR_ROM);
    ri.Cvar_Get("r_vramHow", "", CVAR_ROM);
    ri.Cvar_Get("r_vramSource", "", CVAR_ROM);
    ri.Cvar_Get("r_vramCardMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBudgetMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramGameMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramPendingMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramOtherMB", "-1", CVAR_ROM);
    ri.Cvar_Get("r_vramBarGame", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarPending", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarAll", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarBudget", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramHot", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramShortGame", "", CVAR_ROM);
    ri.Cvar_Get("r_vramShortOther", "", CVAR_ROM);
    ri.Cvar_Get("r_vramShortLimit", "", CVAR_ROM);
    ri.Cvar_Get("r_vramDebugNo", "0", 0);
    ri.Cvar_Get("r_vramDebugAdapter", "0", 0);
    ri.Cvar_Get("r_vramDebugCardMB", "0", 0);
}

/*
R_HzmVramInit - end of every R_Init (the image table is new). Decides on / dormant for this renderer lifetime,
starts the worker, and looks the mark texture up.
*/
void R_HzmVramInit(void)
{
    int want = r_hzmVramBar ? r_hzmVramBar->integer : -1;

    s_vramMark            = NULL;
    s_vramLastPoll        = -100000;
    s_vramSerialPublished = -1;
    s_vramOn              = (want < 0 ? HZM_VRAMBAR_AUTO : want) ? qtrue : qfalse;
    if (!s_vramOn) {
        R_VramSet("r_vramShow", "0"); // the block draws nothing
        return;
    }
    Q_strncpyz(s_osIn.glVendor, glConfig.vendor_string, sizeof(s_osIn.glVendor));
    Q_strncpyz(s_osIn.glRenderer, glConfig.renderer_string, sizeof(s_osIn.glRenderer));
    R_HzmVramGlLuid(&s_osIn);
    s_osIn.debugNo      = ri.Cvar_Get("r_vramDebugNo", "0", 0)->integer;
    s_osIn.debugAdapter = ri.Cvar_Get("r_vramDebugAdapter", "0", 0)->integer;
    R_HzmVramOsStart();
    s_vramMark = R_FindImageFile(VRAM_MARK_IMAGE, IMGTYPE_COLORALPHA, IMGFLAG_NONE);
    R_HzmVramPublish(); // the game's own number at once; the worker's answer follows (R_HzmVramEndFrame)
    s_vramLastPoll = ri.Milliseconds();
}

/*
R_HzmVramEndFrame - RE_EndFrame. Off: one test. On, menu closed: a compare for the worker's first answer and one
for the mark texture. See the header of this part.
*/
void R_HzmVramEndFrame(void)
{
    int now;

    if (!s_vramOn) {
        return;
    }
    // the worker answered since the last publish (its open, or a poll we asked for): show it. s_os.serial is read
    // without the lock on purpose - a torn read only delays this by a frame.
    if (s_os.serial != s_vramSerialPublished) {
        s_vramSerialPublished = s_os.serial;
        R_HzmVramPublish();
        return;
    }
    // frameUsed starts at 0 and so does tr.frameCount: the first frames of a renderer lifetime are not "drawn"
    if (!s_vramMark || tr.frameCount < 8 || s_vramMark->frameUsed < tr.frameCount - 2) {
        return;
    }
    now = ri.Milliseconds();
    if (now - s_vramLastPoll < VRAM_POLL_MS && now >= s_vramLastPoll) {
        return;
    }
    s_vramLastPoll = now;
    R_HzmVramOsRequest(); // fresh Windows numbers; returns at once, and the publish follows when they arrive (above)
    if (!s_os.ready || now - s_vramLastPublish > 3 * VRAM_POLL_MS) {
        // no worker (not Windows), or one that has stopped answering: the game's own number must not go stale
        R_HzmVramPublish();
    }
}

void R_HzmVramShutdown(void)
{
    R_HzmVramOsStop();
    s_vramMark = NULL;
    s_vramOn   = qfalse;
}

/*
vramsrc - console command: the whole chain as the worker and the render thread see it, one line per adapter. For
the per-path test table and for a tester who reports a wrong number.
*/
void R_HzmVramSrc_f(void)
{
    hzmVramOsState_t os;
    int              i;

    R_HzmVramOsSnapshot(&os);
    ri.Printf(PRINT_ALL, "^~^~^ VRAMSRC on=%d ready=%d adapters=%d pick=%d how=%s guess=%d luid=%d:%08x_%08x budget=%d "
              "proc=%d adapter_use=%d reg=%d open_ms=%d poll_ms=%d gl_vendor=\"%s\" gl_renderer=\"%s\"\n",
              s_vramOn ? 1 : 0, os.ready, os.nAdapters, os.pick, os.how[0] ? os.how : "-", os.guess, s_osIn.haveLuid,
              (unsigned int)s_osIn.luidHigh, s_osIn.luidLow, os.budgetMB, os.procMB, os.adapterUseMB, os.regMB,
              os.openMs, os.pollMs, glConfig.vendor_string, glConfig.renderer_string);
    for (i = 0; i < os.nAdapters; i++) {
        ri.Printf(PRINT_ALL, "^~^~^ VRAMADP %d%s \"%s\" vendor=%04x device=%04x dedicated=%u shared=%u software=%d luid=%08x_%08x\n",
                  i, i == os.pick ? "*" : "", os.adapters[i].name, os.adapters[i].vendorId, os.adapters[i].deviceId,
                  os.adapters[i].dedicatedMB, os.adapters[i].sharedMB, os.adapters[i].software,
                  (unsigned int)os.adapters[i].luidHigh, os.adapters[i].luidLow);
    }
    ri.Printf(PRINT_ALL, "^~^~^ VRAMVIEW name=\"%s\" how=%s src=%s uma=%d scale=%d limit=%d game=%d pending=%d other=%d\n",
              s_view.name, s_view.how, s_view.src, s_view.uma, s_view.scaleMB, s_view.limitMB, s_view.gameMB,
              s_view.pendMB, s_view.otherMB);
}
