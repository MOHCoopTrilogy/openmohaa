/*
===========================================================================
HZM gl2 [user 2026-10-12: "on by default, assuming it is smart enough to detect the video card, vram and usage for
anyone"] WHICH ADAPTER IS THE GAME DRAWING ON - the pure part of renderergl2/tr_hzm_vram.c.

No Windows header, no GL header, no engine header: plain C over recorded descriptors, so the same code that runs in
the renderer is compiled into a small test program (docs/tools/vram_pick_selftest) and fed the adapter lists of
machines we do not own - an AMD desktop, an Intel UHD laptop with shared memory only, hybrid laptops, an AMD APU
beside an AMD card.

Why it matters: DXGI lists EVERY adapter in the PC. A hybrid laptop has an integrated chip (a few hundred MB
"dedicated", gigabytes shared) and a discrete card; reporting one while the game draws on the other shows a 4 GB
budget for an 8 GB card, or the reverse. OpenGL names its device only by two strings - unless the driver has
GL_EXT_memory_object_win32, which gives the device LUID, the same id DXGI uses.

Order (the first that decides wins):
  1. luid     the GL device LUID equals an adapter's LUID. Exact.
  2. name     the best match of GL_RENDERER against the adapter names, among hardware adapters of GL_VENDOR's vendor
              when that is known (two Radeons in one laptop: the model words decide)
  3. vendor   exactly one hardware adapter of that vendor
  4. only     exactly one hardware adapter at all
  2b. same-card  the name fits several adapters and they are the SAME device (vendor, device id and memory equal):
              virtual-display drivers (remote desktop, VR, streaming) each list the one card again under a LUID of
              their own. The first is taken and `dup` is set: its size is right, but the caller must not use what
              is kept per LUID (the Windows budget and the usage counters), since only one of them carries the game.
  5. largest  several candidates, nothing tells them apart: the one with the most dedicated memory, and the caller
              is told the pick is a guess (it then never shows another adapter's shared memory or budget)
  -1          no hardware adapter (only Microsoft's software renderer is listed): nothing is reported from DXGI
The software adapter (flag, or vendor 0x1414 device 0x8c "Microsoft Basic Render Driver") is never picked.
===========================================================================
*/
#ifndef TR_HZM_VRAM_PICK_H
#define TR_HZM_VRAM_PICK_H

#include <string.h>

#define HZM_VRAM_MAX_ADAPTERS 8

typedef struct {
    char         name[128];   // adapter description, ASCII
    unsigned int vendorId;    // PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x1414 Microsoft
    unsigned int deviceId;
    unsigned int dedicatedMB; // DedicatedVideoMemory
    unsigned int sharedMB;    // SharedSystemMemory
    unsigned int luidLow;
    int          luidHigh;
    int          software;    // DXGI_ADAPTER_FLAG_SOFTWARE
} hzmAdapter_t;

typedef struct {
    int         index; // into the list, -1 = none
    const char *how;   // "luid" "name" "same-card" "vendor" "only" "largest" "none"
    int         guess; // 1 = "largest": not established
    int         dup;   // 1 = "same-card": the right device, but maybe not the LUID the game draws on
} hzmPick_t;

static int HZM_VramIsSoftware(const hzmAdapter_t *a)
{
    return a->software || (a->vendorId == 0x1414 && a->deviceId == 0x8c);
}

static void HZM_VramLower(const char *in, char *out, int outSize)
{
    int n = 0;

    for (; *in && n < outSize - 1; in++) {
        char c = *in;

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[n++] = c;
    }
    out[n] = 0;
}

// PCI vendor from the GL strings, 0 = cannot tell
static unsigned int HZM_VramVendorFromGL(const char *glVendor, const char *glRenderer)
{
    char v[160], r[200];

    HZM_VramLower(glVendor ? glVendor : "", v, sizeof(v));
    HZM_VramLower(glRenderer ? glRenderer : "", r, sizeof(r));
    if (strstr(v, "nvidia") || strstr(r, "geforce") || strstr(r, "quadro") || strstr(r, "nvidia")) {
        return 0x10DE;
    }
    if (strstr(v, "ati ") || strstr(v, "amd") || strstr(v, "advanced micro") || strstr(r, "radeon") || strstr(r, "firepro")) {
        return 0x1002;
    }
    if (strstr(v, "intel") || strstr(r, "intel")) {
        return 0x8086;
    }
    return 0;
}

// words of an adapter name that carry no identity
static int HZM_VramNoiseWord(const char *w)
{
    static const char *const noise[] = {"nvidia", "amd", "ati", "intel", "graphics", "series", "the", "with", "tm", "r",
                                        "corporation", "inc", "opengl", "engine", "pcie", "sse2", "sse", "gpu", "family",
                                        "adapter", "technologies"};
    int i;

    for (i = 0; i < (int)(sizeof(noise) / sizeof(noise[0])); i++) {
        if (!strcmp(w, noise[i])) {
            return 1;
        }
    }
    return 0;
}

// split into lower-case alphanumeric words; "(R)" "(TM)" and everything from the first '/' on (NVIDIA appends
// "/PCIe/SSE2") are dropped
static int HZM_VramWords(const char *s, char words[][32], int maxWords)
{
    char low[200];
    int  n = 0, i = 0, k;

    HZM_VramLower(s ? s : "", low, sizeof(low));
    for (k = 0; low[k]; k++) {
        if (low[k] == '/') {
            low[k] = 0;
            break;
        }
    }
    while (low[i] && n < maxWords) {
        while (low[i] && !((low[i] >= 'a' && low[i] <= 'z') || (low[i] >= '0' && low[i] <= '9'))) {
            if (low[i] == '(') { // (r) (tm)
                while (low[i] && low[i] != ')') {
                    i++;
                }
            }
            if (low[i]) {
                i++;
            }
        }
        k = 0;
        while ((low[i] >= 'a' && low[i] <= 'z') || (low[i] >= '0' && low[i] <= '9')) {
            if (k < 31) {
                words[n][k++] = low[i];
            }
            i++;
        }
        words[n][k] = 0;
        if (k > 0 && !HZM_VramNoiseWord(words[n])) {
            n++;
        }
    }
    return n;
}

// how well a GL_RENDERER string names this adapter: shared identity words, a word with a digit (the model number)
// counting three times. 0 = nothing in common.
static int HZM_VramNameScore(const char *glRenderer, const char *adapterName)
{
    char a[16][32], b[16][32];
    int  na = HZM_VramWords(glRenderer, a, 16), nb = HZM_VramWords(adapterName, b, 16), i, j, k, score = 0, hits = 0;

    for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
            if (!strcmp(a[i], b[j])) {
                int digit = 0;

                for (k = 0; a[i][k]; k++) {
                    if (a[i][k] >= '0' && a[i][k] <= '9') {
                        digit = 1;
                    }
                }
                score += digit ? 3 : 1;
                hits++;
                break;
            }
        }
    }
    // the same words, all of them, on both sides: the same name. This is what tells an APU ("AMD Radeon(TM)
    // Graphics") from the card beside it ("AMD Radeon RX 6600M") when the game runs on the APU.
    if (na > 0 && na == nb && hits == na) {
        score += 100;
    }
    return score;
}

static hzmPick_t HZM_VramPickAdapter(
    const hzmAdapter_t *list, int n, int haveLuid, unsigned int luidLow, int luidHigh, const char *glVendor,
    const char *glRenderer
)
{
    hzmPick_t    pick;
    unsigned int vendor = HZM_VramVendorFromGL(glVendor, glRenderer);
    int          i, hw = 0, hwOnly = -1, ofVendor = 0, vendorOnly = -1, best = -1, bestScore = 0, tie = 0, largest = -1;

    pick.index = -1;
    pick.how   = "none";
    pick.guess = 0;
    pick.dup   = 0;
    if (!list || n <= 0) {
        return pick;
    }
    if (haveLuid) {
        for (i = 0; i < n; i++) {
            if (!HZM_VramIsSoftware(&list[i]) && list[i].luidLow == luidLow && list[i].luidHigh == luidHigh) {
                pick.index = i;
                pick.how   = "luid";
                return pick;
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (HZM_VramIsSoftware(&list[i])) {
            continue;
        }
        hw++;
        hwOnly = i;
        if (vendor && list[i].vendorId == vendor) {
            ofVendor++;
            vendorOnly = i;
        }
    }
    if (!hw) {
        return pick;
    }
    // the name, among the adapters of the GL vendor when there are any, else among all hardware adapters
    for (i = 0; i < n; i++) {
        int s;

        if (HZM_VramIsSoftware(&list[i]) || (ofVendor && list[i].vendorId != vendor)) {
            continue;
        }
        s = HZM_VramNameScore(glRenderer, list[i].name);
        if (s > bestScore) {
            bestScore = s;
            best      = i;
            tie       = 0;
        } else if (s == bestScore && s > 0) {
            tie = 1;
        }
        if (largest < 0 || list[i].dedicatedMB > list[largest].dedicatedMB) {
            largest = i;
        }
    }
    if (best >= 0 && bestScore > 0 && !tie) {
        pick.index = best;
        pick.how   = "name";
        return pick;
    }
    if (best >= 0 && bestScore > 0 && tie) {
        int same = 1;

        for (i = 0; i < n; i++) {
            if (HZM_VramIsSoftware(&list[i]) || (ofVendor && list[i].vendorId != vendor)
                || HZM_VramNameScore(glRenderer, list[i].name) != bestScore) {
                continue;
            }
            if (list[i].vendorId != list[best].vendorId || list[i].deviceId != list[best].deviceId
                || list[i].dedicatedMB != list[best].dedicatedMB) {
                same = 0;
            }
        }
        if (same) {
            pick.index = best;
            pick.how   = "same-card";
            pick.dup   = 1;
            return pick;
        }
    }
    if (ofVendor == 1) {
        pick.index = vendorOnly;
        pick.how   = "vendor";
        return pick;
    }
    if (hw == 1) {
        pick.index = hwOnly;
        pick.how   = "only";
        return pick;
    }
    pick.index = largest;
    pick.how   = "largest";
    pick.guess = 1;
    return pick;
}

#endif
