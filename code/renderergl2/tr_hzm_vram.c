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
P2.1 - THE NUMBERS THE MENU BLOCK SHOWS (plan section 4.2 / 4.3).

Switch: r_hzmVramBar (flags 0). -1 = automatic = HZM_VRAMBAR_AUTO, which is 0 today: DORMANT. Dormant means the ROM
cvars below keep their empty values, the menu block stays hidden (it is gated on r_vramCardMB > 0) and
R_HzmVramEndFrame returns on its first test. 1 switches it on for this renderer lifetime.

"Costs nothing while the menu is closed": nothing here runs on a timer. The block's bar track is drawn with one
small texture of its own (VRAM_MARK_IMAGE). gl2 already stamps image->frameUsed on every bind (tr_backend.c
GL_BindToTMU), so "is the block on screen" is one integer compare per frame in RE_EndFrame - and only then, at most
twice a second, are the tables walked and Windows asked. Closed menu: the compare, nothing else. Switch off: not
even that.

Published (all CVAR_ROM, never archived - they describe this machine right now):
  r_vramCardMB     the card's dedicated memory (0 = unknown or dormant: the block hides)
  r_vramBudgetMB   what Windows allows this process (DXGI local-segment budget; 0 = unknown)
  r_vramGameMB     our own count      r_vramPendingMB  what Apply would add or free (latched settings)
  r_vramOtherMB    other programs on the same card (-1 = unknown: no segment is drawn)
  r_vramSource     where "other" came from: counters / nvx / none
  r_vramBarGame / r_vramBarPending / r_vramBarAll / r_vramBarBudget   0-100 of the card, for `statbar horizontal 0 100`
  r_vramHot        1 when the game (after Apply) reaches 90 % of the budget (of the card when there is no budget)
  r_vramTextGame / r_vramTextOther / r_vramTextLimit / r_vramTextWarn   the lines, ready to print (a Label shows a
                   cvar's raw string and cannot format one)

"Other programs" = the adapter's dedicated usage, from the Windows GPU performance counter the Task Manager reads
(GPU Adapter Memory, the instance of OUR adapter's LUID), minus this process's own usage from DXGI. P2.0 measured
that DXGI figure against the per-process counter on ten maps: within 7 MB every time, so it is used as "this
process" here and no second counter is opened. Without the counters: NVIDIA's total - available - ours. Else none.
===========================================================================
*/
#define HZM_VRAMBAR_AUTO 0
#define VRAM_MARK_IMAGE  "textures/hud/vram/track" // as the UI names it: no extension (docs/tools/gen_menu_graphics.py)
#define VRAM_POLL_MS     500

static cvar_t  *r_hzmVramBar;
static qboolean s_vramOn       = qfalse;
static image_t *s_vramMark     = NULL;
static int      s_vramLastPoll = -100000;

#ifdef _WIN32
#    include <pdh.h>
// per process (the adapter never changes), released in R_HzmVramShutdown: the DLL unloads on every vid_restart
static HMODULE        s_dxgiDll  = NULL;
static IDXGIAdapter3 *s_adapter3 = NULL;
static int            s_cardMB   = -1;
static LUID           s_luid;
static HMODULE        s_pdhDll = NULL;
static PDH_HQUERY     s_pdhQuery;
static PDH_HCOUNTER   s_pdhCounter;
static qboolean       s_pdhOk    = qfalse;
static qboolean       s_osTried  = qfalse;
typedef PDH_STATUS(WINAPI *pdhOpen_t)(LPCSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *pdhAdd_t)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *pdhCollect_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *pdhValue_t)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
typedef PDH_STATUS(WINAPI *pdhClose_t)(PDH_HQUERY);
static pdhCollect_t s_pdhCollect = NULL;
static pdhValue_t   s_pdhValue   = NULL;
static pdhClose_t   s_pdhClose   = NULL;

static void R_HzmVramOsOpen(void)
{
    static const GUID iidFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    static const GUID iidAdapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
    typedef HRESULT(WINAPI * createFactory1_t)(REFIID, void **);
    createFactory1_t create;
    IDXGIFactory1   *factory = NULL;
    IDXGIAdapter1   *adapter = NULL, *best = NULL;
    SIZE_T           bestMem = 0;
    UINT             n;

    if (s_osTried) {
        return;
    }
    s_osTried = qtrue;
    s_dxgiDll = LoadLibraryA("dxgi.dll");
    if (!s_dxgiDll) {
        return;
    }
    create = (createFactory1_t)GetProcAddress(s_dxgiDll, "CreateDXGIFactory1");
    if (!create || FAILED(create(&iidFactory1, (void **)&factory)) || !factory) {
        return;
    }
    // the adapter with the most dedicated memory that is not software: the card the game renders on (an
    // integrated chip beside it reports a few hundred MB)
    for (n = 0; IDXGIFactory1_EnumAdapters1(factory, n, &adapter) == S_OK; n++) {
        DXGI_ADAPTER_DESC1 desc;

        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && desc.DedicatedVideoMemory > bestMem) {
            if (best) {
                IDXGIAdapter1_Release(best);
            }
            best    = adapter;
            bestMem = desc.DedicatedVideoMemory;
            s_luid  = desc.AdapterLuid;
        } else {
            IDXGIAdapter1_Release(adapter);
        }
        adapter = NULL;
    }
    if (best) {
        s_cardMB = (int)(bestMem >> 20);
        if (FAILED(IDXGIAdapter1_QueryInterface(best, &iidAdapter3, (void **)&s_adapter3))) {
            s_adapter3 = NULL;
        }
        IDXGIAdapter1_Release(best);
    }
    IDXGIFactory1_Release(factory);

    // the adapter-wide counter, by name (no import: the renderer must load where pdh.dll is missing)
    s_pdhDll = LoadLibraryA("pdh.dll");
    if (s_pdhDll && best) {
        pdhOpen_t open = (pdhOpen_t)GetProcAddress(s_pdhDll, "PdhOpenQueryA");
        pdhAdd_t  add  = (pdhAdd_t)GetProcAddress(s_pdhDll, "PdhAddEnglishCounterA");
        char      path[160];

        s_pdhCollect = (pdhCollect_t)GetProcAddress(s_pdhDll, "PdhCollectQueryData");
        s_pdhValue   = (pdhValue_t)GetProcAddress(s_pdhDll, "PdhGetFormattedCounterValue");
        s_pdhClose   = (pdhClose_t)GetProcAddress(s_pdhDll, "PdhCloseQuery");
        Com_sprintf(
            path, sizeof(path), "\\GPU Adapter Memory(luid_0x%08lX_0x%08lX_phys_0)\\Dedicated Usage",
            (unsigned long)s_luid.HighPart, (unsigned long)s_luid.LowPart
        );
        if (open && add && s_pdhCollect && s_pdhValue && s_pdhClose && open(NULL, 0, &s_pdhQuery) == ERROR_SUCCESS) {
            if (add(s_pdhQuery, path, 0, &s_pdhCounter) == ERROR_SUCCESS && s_pdhCollect(s_pdhQuery) == ERROR_SUCCESS) {
                s_pdhOk = qtrue;
            } else {
                s_pdhClose(s_pdhQuery);
            }
        }
    }
}

// MB; -1 = not available
static void R_HzmVramOsRead(int *cardMB, int *budgetMB, int *procMB, int *adapterMB)
{
    *cardMB = *budgetMB = *procMB = *adapterMB = -1;
    R_HzmVramOsOpen();
    *cardMB = s_cardMB;
    if (s_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info;

        memset(&info, 0, sizeof(info));
        if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            *budgetMB = (int)(info.Budget >> 20);
            *procMB   = (int)(info.CurrentUsage >> 20);
        }
    }
    if (s_pdhOk) {
        PDH_FMT_COUNTERVALUE v;

        if (s_pdhCollect(s_pdhQuery) == ERROR_SUCCESS
            && s_pdhValue(s_pdhCounter, PDH_FMT_LARGE, NULL, &v) == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS) {
            *adapterMB = (int)(v.largeValue >> 20);
        }
    }
}

static void R_HzmVramOsClose(void)
{
    if (s_pdhOk && s_pdhClose) {
        s_pdhClose(s_pdhQuery);
    }
    s_pdhOk = qfalse;
    if (s_adapter3) {
        IDXGIAdapter3_Release(s_adapter3);
        s_adapter3 = NULL;
    }
    if (s_pdhDll) {
        FreeLibrary(s_pdhDll);
        s_pdhDll = NULL;
    }
    if (s_dxgiDll) {
        FreeLibrary(s_dxgiDll);
        s_dxgiDll = NULL;
    }
    s_osTried = qfalse;
    s_cardMB  = -1;
}
#else
static void R_HzmVramOsRead(int *cardMB, int *budgetMB, int *procMB, int *adapterMB)
{
    *cardMB = *budgetMB = *procMB = *adapterMB = -1;
}
static void R_HzmVramOsClose(void) {}
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

static const char *R_VramGB(int mb)
{
    return va("%.1f GB", (double)mb / 1024.0);
}

static void R_HzmVramPublish(void)
{
    hzmVram_t v;
    int       cardMB, budgetMB, procMB, adapterMB, oursMB, pendMB, otherMB = -1, afterMB, limitMB, total;
    const char *source = "none";

    R_HzmVramCount(&v);
    R_HzmVramOsRead(&cardMB, &budgetMB, &procMB, &adapterMB);
    oursMB = (int)((v.tex + v.rt + v.rb + v.vao) >> 20);
    pendMB = (int)(R_HzmVramPendingBytes(&v) / 1048576.0);
    if (pendMB > -16 && pendMB < 16) {
        pendMB = 0; // not worth a hatched sliver
    }
    if (adapterMB >= 0 && procMB >= 0) {
        otherMB = adapterMB - procMB;
        source  = "counters";
    } else if (glRefConfig.memInfo == MI_NVX) {
        int totalKb = 0, availKb = 0;

        qglGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &totalKb);
        qglGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &availKb);
        if (totalKb > 0) {
            otherMB = totalKb / 1024 - availKb / 1024 - oursMB;
            source  = "nvx";
            if (cardMB <= 0) {
                cardMB = totalKb / 1024;
            }
        }
    }
    if (otherMB < 0 && strcmp(source, "none")) {
        otherMB = 0;
    }
    // test hook (flags 0, never saved): pretend the card and the Windows limit are this many MB, to see the
    // near-the-limit state on a card that is nowhere near it
    {
        cvar_t *dbg = ri.Cvar_Get("r_vramDebugCardMB", "0", 0);

        if (dbg && dbg->integer > 0) {
            cardMB   = dbg->integer;
            budgetMB = (int)(dbg->integer * 0.94f);
            if (otherMB > cardMB / 6) {
                otherMB = cardMB / 6; // keep the pretend card's picture coherent
            }
        }
    }
    if (cardMB <= 0) {
        // nothing to measure against: one line of text, no bar (the block's bar is gated on r_vramCardMB > 0)
        R_VramSet("r_vramCardMB", "0");
        R_VramSet("r_vramGameMB", va("%d", oursMB));
        R_VramSet("r_vramTextGame", va("This game: about %s", R_VramGB(oursMB)));
        return;
    }
    afterMB = oursMB + (pendMB > 0 ? pendMB : 0);
    limitMB = (budgetMB > 0) ? budgetMB : cardMB;
    total   = afterMB + (otherMB > 0 ? otherMB : 0);

    R_VramSet("r_vramCardMB", va("%d", cardMB));
    R_VramSet("r_vramBudgetMB", va("%d", budgetMB > 0 ? budgetMB : 0));
    R_VramSet("r_vramGameMB", va("%d", oursMB));
    R_VramSet("r_vramPendingMB", va("%d", pendMB));
    R_VramSet("r_vramOtherMB", va("%d", otherMB));
    R_VramSet("r_vramSource", source);
#define VRAM_PCT(mb) va("%d", (int)Com_Clamp(0.0f, 100.0f, (float)(mb) * 100.0f / (float)cardMB + 0.5f))
    R_VramSet("r_vramBarGame", VRAM_PCT(oursMB));
    R_VramSet("r_vramBarPending", pendMB > 0 ? VRAM_PCT(afterMB) : "0");
    R_VramSet("r_vramBarAll", otherMB > 0 ? VRAM_PCT(total) : "0");
    R_VramSet("r_vramBarBudget", budgetMB > 0 ? VRAM_PCT(budgetMB) : "0");
#undef VRAM_PCT
    R_VramSet("r_vramHot", ((double)afterMB >= 0.9 * (double)limitMB) ? "1" : "0");
    if (pendMB) {
        R_VramSet("r_vramTextGame", va("After Apply: about %s", R_VramGB(oursMB + pendMB)));
    } else {
        R_VramSet("r_vramTextGame", va("This game: about %s", R_VramGB(oursMB)));
    }
    R_VramSet("r_vramTextOther", otherMB >= 0 ? va("Other programs: %s", R_VramGB(otherMB)) : "");
    if (budgetMB > 0) {
        R_VramSet("r_vramTextLimit", va("Windows limit: about %s", R_VramGB(budgetMB)));
    } else {
        R_VramSet("r_vramTextLimit", va("Your card: %d GB", (cardMB + 512) / 1024));
    }
    // the two biggest levers this build has a menu row for
    if (r_hzmVramBar && r_hzmVramBar->integer >= 2) {
        // 2 = on + one line per publish, so a test can count them (none while the menu is closed)
        ri.Printf(
            PRINT_ALL, "^~^~^ VRAMPOLL frame=%d game=%d pending=%d other=%d budget=%d card=%d src=%s\n",
            tr.frameCount, oursMB, pendMB, otherMB, budgetMB, cardMB, source
        );
    }
    R_VramSet(
        "r_vramTextWarn",
        (tr.msaaNewPath && tr.msaaSamples > 1)
            ? "Close to the limit - lower Anti-Aliasing or Texture Quality."
            : "Close to the limit - lower Texture Quality or Quality."
    );
}

/*
R_HzmVramRegister - every R_Init (R_Register). The ROM cvars exist from the first launch so the menu's wire audit
and its enabledcvar gates always find them; dormant, they stay at these values.
*/
void R_HzmVramRegister(void)
{
    r_hzmVramBar = ri.Cvar_Get("r_hzmVramBar", "-1", 0);
    // one literal call each: the menu generator's wire gate and the generated cvar docs find registrations by text
    ri.Cvar_Get("r_vramCardMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBudgetMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramGameMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramPendingMB", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramOtherMB", "-1", CVAR_ROM);
    ri.Cvar_Get("r_vramSource", "", CVAR_ROM);
    ri.Cvar_Get("r_vramBarGame", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarPending", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarAll", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramBarBudget", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramHot", "0", CVAR_ROM);
    ri.Cvar_Get("r_vramTextGame", "", CVAR_ROM);
    ri.Cvar_Get("r_vramTextOther", "", CVAR_ROM);
    ri.Cvar_Get("r_vramTextLimit", "", CVAR_ROM);
    ri.Cvar_Get("r_vramTextWarn", "", CVAR_ROM);
}

/*
R_HzmVramInit - end of every R_Init (the image table is new). Decides on / dormant for this renderer lifetime and
looks the mark texture up; without the texture (an older pk3) the block cannot be on screen and nothing is published.
*/
void R_HzmVramInit(void)
{
    int want = r_hzmVramBar ? r_hzmVramBar->integer : -1;

    s_vramMark     = NULL;
    s_vramLastPoll = -100000;
    s_vramOn       = (want < 0 ? HZM_VRAMBAR_AUTO : want) ? qtrue : qfalse;
    if (!s_vramOn) {
        R_VramSet("r_vramCardMB", "0"); // the block hides
        return;
    }
    s_vramMark = R_FindImageFile(VRAM_MARK_IMAGE, IMGTYPE_COLORALPHA, IMGFLAG_NONE);
    R_HzmVramPublish(); // so the block is populated the first time it is opened
    s_vramLastPoll = ri.Milliseconds();
}

/*
R_HzmVramEndFrame - RE_EndFrame. Off: one test. On, menu closed: two more. See the header of this part.
*/
void R_HzmVramEndFrame(void)
{
    int now;

    if (!s_vramOn) {
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
    R_HzmVramPublish();
}

void R_HzmVramShutdown(void)
{
    R_HzmVramOsClose();
    s_vramMark = NULL;
    s_vramOn   = qfalse;
}
