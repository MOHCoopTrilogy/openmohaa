/*
===========================================================================
HZM coop [user 2026-10-19] LOADING-SCREEN MUSIC (cl_loadMusic). Branch loadhold-fix, taken from loadcine-player (cl_loadplayer.cpp)
WITHOUT the squad composite.

A Frontline track (sound/music/frontline_<mission>_*.mp3, shipped by the mod) starts with the load, plays through the clip and its held
last frame, and fades out as the map starts. The stream is fed from the load tick and plays on chan_mp3 (snd_openal_new.cpp
S_LoadMusic_*), which neither the menu theme nor a map's own score (both chan_trig_music) uses; S_LoadMusic_Start also stops a menu
theme / leftover score so nothing stacks.
===========================================================================
*/

#include "client.h"
#include "cl_ui.h"

#include <ctype.h>
#include <string.h>

qboolean S_LoadMusic_Start(const char *path, int fadeInMs, int logMs);
void     S_LoadMusic_Stop(int fadeOutMs);
void     S_LoadMusic_Tick(void);
qboolean S_LoadMusic_Active(void);

static cvar_t *cl_loadMusic;
static cvar_t *cl_loadMusicFadeMs;  // fade in at the start, fade out at the end
static cvar_t *cl_loadMusicDefault; // track for maps with no Frontline mission of their own (basename, no extension)
static cvar_t *cl_loadMusicLog;     // ms between "stream position" proof lines (0 = off)

void CL_LoadMusic_Init(void)
{
    cl_loadMusic        = Cvar_Get("cl_loadMusic", "1", CVAR_ARCHIVE);
    cl_loadMusicFadeMs  = Cvar_Get("cl_loadMusicFadeMs", "1500", 0);
    cl_loadMusicDefault = Cvar_Get("cl_loadMusicDefault", "frontline_m5l1_crossroads", 0);
    cl_loadMusicLog     = Cvar_Get("cl_loadMusicLog", "0", 0);
}

static qboolean LM_FileExists(const char *base, char *out, int size)
{
    Com_sprintf(out, size, "sound/music/%s.mp3", base);
    return FS_ReadFile(out, NULL) > 0 ? qtrue : qfalse;
}

// Frontline mission track for a map: sound/music/frontline_<mission>_*.mp3 for m1l1, m3l1a ...; any other map (e1l1, t2l2, ...)
// takes one of the Frontline tracks by a stable hash of its name, so a map always opens with the same piece.
static qboolean LM_Pick(const char *mapname, char *out, int size)
{
    char   prefix[48];
    char **list;
    int    n = 0, i, hits = 0;
    char   base[MAX_QPATH];

    out[0] = 0;
    if (mapname && strlen(mapname) >= 4 && isalpha((unsigned char)mapname[0]) && isdigit((unsigned char)mapname[1])
        && isalpha((unsigned char)mapname[2]) && isdigit((unsigned char)mapname[3])) {
        Com_sprintf(prefix, sizeof(prefix), "frontline_%c%c%c%c_", tolower(mapname[0]), mapname[1], tolower(mapname[2]), mapname[3]);
    } else {
        prefix[0] = 0;
    }

    list = FS_ListFilteredFiles("sound/music", "mp3", NULL, qfalse, &n, qfalse);
    if (list && n > 0) {
        if (prefix[0]) {
            for (i = 0; i < n; i++) {
                if (!Q_stricmpn(list[i], prefix, (int)strlen(prefix))) {
                    Com_sprintf(out, size, "sound/music/%s", list[i]);
                    break;
                }
            }
        }
        if (!out[0]) {
            // hash into the Frontline pieces
            unsigned h = 2166136261u;
            const char *m;
            int         pick;

            for (m = mapname ? mapname : ""; *m; m++) {
                h = (h ^ (unsigned)tolower((unsigned char)*m)) * 16777619u;
            }
            for (i = 0; i < n; i++) {
                if (!Q_stricmpn(list[i], "frontline_", 10)) {
                    hits++;
                }
            }
            if (hits > 0) {
                pick = (int)(h % (unsigned)hits);
                for (i = 0; i < n; i++) {
                    if (!Q_stricmpn(list[i], "frontline_", 10) && pick-- == 0) {
                        Com_sprintf(out, size, "sound/music/%s", list[i]);
                        break;
                    }
                }
            }
        }
    }
    if (list) {
        FS_FreeFileList(list);
    }
    if (!out[0] && cl_loadMusicDefault && LM_FileExists(cl_loadMusicDefault->string, base, sizeof(base))) {
        Q_strncpyz(out, base, size);
    }
    return out[0] ? qtrue : qfalse;
}

void CL_LoadMusic_Begin(const char *mapname)
{
    char track[MAX_QPATH];

    if (!cl_loadMusic || !cl_loadMusic->integer) {
        return;
    }
    if (!LM_Pick(mapname, track, sizeof(track))) {
        Com_Printf("^~^~^ LOADMUS skip map=%s why=no track\n", mapname ? mapname : "");
        return;
    }
    if (S_LoadMusic_Start(track, cl_loadMusicFadeMs ? cl_loadMusicFadeMs->integer : 1500, cl_loadMusicLog ? cl_loadMusicLog->integer : 0)) {
        Com_Printf("^~^~^ LOADMUS map=%s track=%s\n", mapname ? mapname : "", track);
    }
}

void CL_LoadMusic_Tick(void)
{
    S_LoadMusic_Tick();
}

// the load is over: fade out while the map's own music comes in (abort: stop now)
void CL_LoadMusic_End(qboolean abort)
{
    if (!S_LoadMusic_Active()) {
        return;
    }
    S_LoadMusic_Stop(abort ? 0 : (cl_loadMusicFadeMs ? cl_loadMusicFadeMs->integer : 1500));
}

