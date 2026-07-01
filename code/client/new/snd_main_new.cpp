/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "../snd_local.h"
#include "../client.h"

#if defined(NO_MODERN_DMA) && NO_MODERN_DMA

qboolean s_bSoundPaused = qfalse;

void S_Init2()
{
    S_Init();
    SND_setup();

    // HACK: S_RegisterSound returns 0 when unsuccessful, or it returns the the sfx handle
    // But the first sfx handle is also 0...
    S_RegisterSound("sound/null.wav", qfalse);

    Cmd_AddCommand("tmstart", S_TriggeredMusic_Start);
    Cmd_AddCommand("tmstartloop", S_TriggeredMusic_StartLoop);
    Cmd_AddCommand("tmstop", S_TriggeredMusic_Stop);
}

/*
=================
S_StartSound
=================
*/
void S_StartSound(const vec3_t origin, int entNum, int entChannel, sfxHandle_t sfxHandle, float volume, float minDist, float pitch, float maxDist, qboolean streamed)
{
    S_StartSound((float*)origin, entNum, entChannel, sfxHandle);
    // FIXME: partially implemented
}

/*
=================
S_AddLoopingSound
=================
*/
void S_AddLoopingSound(const vec3_t origin, const vec3_t velocity, sfxHandle_t sfxHandle, float volume, float minDist, float maxDist, float pitch, int flags)
{
    if (!sfxHandle) {
        return;
    }

    // FIXME: unimplemented

    if (VectorCompare(origin, vec3_origin)) {
        // Consider it to be a local sound, uses the player origin
        S_AddLoopingSound(cl.snap.ps.clientNum, cl.snap.ps.origin, velocity, sfxHandle);
        return;
    }

    S_AddLoopingSound(ENTITYNUM_WORLD, origin, velocity, sfxHandle);
}

/*
=================
S_StopAllSounds
=================
*/
void S_StopAllSounds2(qboolean stop_music)
{
    // Call the original function
    S_StopAllSounds();
    // FIXME: stop music
}


/*
=================
S_ClearLoopingSounds
=================
*/
void S_ClearLoopingSounds(void)
{
    S_ClearLoopingSounds(qtrue);
}

/*
=================
S_Respatialize
=================
*/
void S_Respatialize(int entityNum, const vec3_t origin,
    vec3_t axis[3])
{
    S_Respatialize(entityNum, origin, axis, 0);
}

/*
=================
S_StartLocalSoundChannel
=================
*/
void S_StartLocalSound(const char* sound_name, qboolean force_load)
{
    sfxHandle_t h;

    h = S_RegisterSound(sound_name, qfalse);

    if (h) {
        S_StartLocalSound(h, CHAN_LOCAL_SOUND);
    }
}

/*
=================
S_StartLocalSound
=================
*/
void S_StartLocalSoundChannel(const char* sound_name, qboolean force_load, int channel)
{
    // FIXME: unimplemented
}

sfxHandle_t	S_RegisterSound(const char* sample, qboolean compressed, qboolean streamed) {
    return S_RegisterSound(sample, compressed);
}

/*
=================
S_StopSound
=================
*/
void S_StopSound(int entnum, int channel)
{
    // FIXME: unimplemented
}

/*
=================
S_IsSoundPlaying
=================
*/
qboolean S_IsSoundPlaying(int channelNumber, const char* name)
{
    channel_t* v;
    if (channelNumber >= MAX_CHANNELS) {
        return qfalse;
    }

	v = &s_channels[channelNumber];

    return v->thesfx ? qtrue : qfalse;
}

/*
=================
MUSIC_Pause
=================
*/
void MUSIC_Pause()
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_Unpause
=================
*/
void MUSIC_Unpause()
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_LoadSoundtrackFile
=================
*/
qboolean MUSIC_LoadSoundtrackFile(const char* filename)
{
    // FIXME: unimplemented
    STUB();
    return qfalse;
}

/*
=================
MUSIC_SongValid
=================
*/
qboolean MUSIC_SongValid(const char* mood)
{
    // FIXME: unimplemented
    STUB();
    return qfalse;
}

/*
=================
MUSIC_Loaded
=================
*/
qboolean MUSIC_Loaded(void)
{
    // FIXME: unimplemented
    STUB();
    return qfalse;
}

/*
=================
Music_Update
=================
*/
void Music_Update(void)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_SongEnded
=================
*/
void MUSIC_SongEnded(void)
{
    // FIXME: unimplemented
    STUB();
}

// ---------------------------------------------------------------------------
// HZM: MOHAA .mus "mood" music subsystem - DMA-backend port.
//
// Vanilla MOHAA delivers BOTH its score and its atmospheric ambience through the
// .mus mood system: a map runs `soundtrack music/<map>.mus` (CS_MUSIC configstring
// -> MUSIC_NewSoundtrack) then `forcemusic <mood>` at scripted beats (mood travels
// in the player-state snapshot -> cg_snapshot.c calls MUSIC_UpdateMood per client).
// OpenMOHAA only implemented this in the OpenAL backend (snd_openal_new.cpp), which
// is EXCLUDED from this NO_MODERN_DMA build - so these were stubbed and every map
// played silent. Port the parser + mood selection here, backed by the same proven
// background-track streamer that tmstartloop uses (S_StartBackgroundTrack loops the
// stream; S_UpdateBackgroundTrack already pumps it each frame). Single-stream model:
// each mood = one looping track, switched on mood change. Networked snapshot/cfgstr
// means every coop client drives this independently -> all players hear it.
// ---------------------------------------------------------------------------
#define MOOD_MAX_SONGS 16

typedef struct {
    char alias[32];
    char path[64];
    int  mood_num;
} moodSong_t;

static moodSong_t s_moodSongs[MOOD_MAX_SONGS];
static int        s_moodNumSongs       = 0;
static char       s_moodSoundtrack[64] = "";
static int        s_moodPlaying        = -2; // mood_num currently streaming (-2 = none selected yet)

static int MUSIC_FindMoodSong(int mood_num)
{
    int i;

    if (mood_num <= mood_none) {
        return -1;
    }
    for (i = 0; i < s_moodNumSongs; i++) {
        if (s_moodSongs[i].mood_num == mood_num) {
            return i;
        }
    }
    return -1;
}

static void MUSIC_LoadMoodFile(const char* name)
{
    char*       data = NULL;
    char*       buffer;
    char        load_path[64];
    char        a0[64], a1[64];
    int         len;
    moodSong_t* psong;

    s_moodNumSongs = 0;
    load_path[0]   = 0;

    len = FS_ReadFile(name, (void**)&data);
    if (len <= 0 || !data) {
        Com_Printf("MUSIC: could not load soundtrack %s\n", name);
        return;
    }

    buffer = data;
    while (1) {
        // first token of a line (allow crossing newlines); '' = end of file
        Q_strncpyz(a0, COM_GetToken(&buffer, qtrue), sizeof(a0));
        if (!a0[0]) {
            break;
        }
        // second token (same line only); trailing // comments are skipped by the tokenizer
        Q_strncpyz(a1, COM_GetToken(&buffer, qfalse), sizeof(a1));

        if (!Q_stricmp(a0, "path")) {
            Q_strncpyz(load_path, a1, sizeof(load_path));
            len = (int)strlen(load_path);
            if (len > 0 && load_path[len - 1] != '/' && load_path[len - 1] != '\\') {
                Q_strcat(load_path, sizeof(load_path), "/");
            }
        } else if (a0[0] == '!') {
            // per-song directive (!<alias> volume/loop/fadetime/...). The single-stream
            // port loops every track, so just consume the rest of the line and move on.
            while (COM_GetToken(&buffer, qfalse)[0]) {
            }
        } else if (a1[0]) {
            // "<mood-alias> <file>"
            if (s_moodNumSongs >= MOOD_MAX_SONGS) {
                continue;
            }
            psong = &s_moodSongs[s_moodNumSongs];
            Q_strncpyz(psong->alias, a0, sizeof(psong->alias));
            Q_strncpyz(psong->path, load_path, sizeof(psong->path));
            Q_strcat(psong->path, sizeof(psong->path), a1);
            psong->mood_num = MusicMood_NameToNum(a0);
            s_moodNumSongs++;
        }
    }

    FS_FreeFile(data);
    Com_Printf("MUSIC: loaded %d songs from %s\n", s_moodNumSongs, name);
}

/*
=================
MUSIC_NewSoundtrack
  Called when the CS_MUSIC configstring changes (server `soundtrack <file.mus>`).
=================
*/
void MUSIC_NewSoundtrack(const char* name)
{
    if (!name) {
        return;
    }
    if (!Q_stricmp(name, s_moodSoundtrack)) {
        return; // unchanged
    }

    Q_strncpyz(s_moodSoundtrack, name, sizeof(s_moodSoundtrack));
    s_moodNumSongs = 0;
    s_moodPlaying  = -2;

    if (!*name || !Q_stricmp(name, "none")) {
        S_StopBackgroundTrack();
        return;
    }

    MUSIC_LoadMoodFile(name);
    // Playback is driven by MUSIC_UpdateMood (called every snapshot with the live mood).
}

/*
=================
MUSIC_UpdateMood
  Called every snapshot with the networked player-state mood. Switches the looping
  background track when the desired mood changes.
=================
*/
void MUSIC_UpdateMood(int current, int fallback)
{
    int idx;

    if (!s_moodNumSongs) {
        return; // no soundtrack loaded yet
    }

    if (current == mood_none) {
        if (s_moodPlaying != mood_none) {
            S_StopBackgroundTrack();
            s_moodPlaying = mood_none;
        }
        return;
    }

    idx = MUSIC_FindMoodSong(current);
    if (idx < 0) {
        idx = MUSIC_FindMoodSong(fallback);
    }
    if (idx < 0) {
        idx = MUSIC_FindMoodSong(mood_normal);
    }
    if (idx < 0) {
        return; // no song for this mood; leave the current track playing
    }

    if (s_moodSongs[idx].mood_num == s_moodPlaying) {
        return; // already streaming this mood
    }

    s_moodPlaying = s_moodSongs[idx].mood_num;
    Com_Printf("MUSIC: start %s (mood %d)\n", s_moodSongs[idx].path, s_moodSongs[idx].mood_num);
    S_StartBackgroundTrack(s_moodSongs[idx].path, s_moodSongs[idx].path); // intro==loop -> loops forever
}

/*
=================
MUSIC_UpdateVolume
=================
*/
void MUSIC_UpdateVolume(float volume, float fade_time)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_StopAllSongs
=================
*/
void MUSIC_StopAllSongs(void)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_FreeAllSongs
=================
*/
void MUSIC_FreeAllSongs(void)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_Playing
=================
*/
qboolean MUSIC_Playing(void)
{
    // FIXME: unimplemented
    STUB();
    return qfalse;
}

/*
=================
MUSIC_FindSong
=================
*/
int MUSIC_FindSong(const char* name)
{
    // FIXME: unimplemented
    STUB();
    return 0;
}

/*
=================
MUSIC_CurrentSongChannel
=================
*/
int MUSIC_CurrentSongChannel(void)
{
    // FIXME: unimplemented
    STUB();
    return 0;
}

/*
=================
MUSIC_StopChannel
=================
*/
void MUSIC_StopChannel(int channel_number)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_PlaySong
=================
*/
qboolean MUSIC_PlaySong(const char* alias)
{
    // FIXME: unimplemented
    STUB();
    return qfalse;
}

/*
=================
MUSIC_UpdateMusicVolumes
=================
*/
void MUSIC_UpdateMusicVolumes(void)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
MUSIC_CheckForStoppedSongs
=================
*/
void MUSIC_CheckForStoppedSongs(void)
{
    // FIXME: unimplemented
    STUB();
}

/*
==============
S_CurrentSoundtrack
==============
*/
const char* S_CurrentSoundtrack()
{
    return "";
}

/*
=================
S_IsSoundRegistered
=================
*/
qboolean S_IsSoundRegistered(const char* name)
{
    // FIXME: unimplemented
    return qfalse;
}
/*
=================
S_GetSoundTime
=================
*/
float S_GetSoundTime(sfxHandle_t handle)
{
    // FIXME: unimplemented
    STUB();
    return 0.0;
}

/*
=================
S_SetGlobalAmbientVolumeLevel
=================
*/
void S_SetGlobalAmbientVolumeLevel(float volume)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
S_SetReverb
=================
*/
void S_SetReverb(int reverb_type, float reverb_level)
{
    // FIXME: unimplemented
    STUB();
}

/*
=================
S_EndRegistration
=================
*/
void S_EndRegistration(void)
{
    // FIXME: unimplemented
}

void S_UpdateEntity(int entityNum, const vec3_t origin, const vec3_t velocity, qboolean use_listener)
{
    // FIXME: unimplemented
}

void S_FadeSound(float fTime)
{
    // FIXME: unimplemented
}

/*
==============
S_TriggeredMusic_Start
==============
*/
void S_TriggeredMusic_Start()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstart <sound file>\n");
        return;
    }

    S_StartBackgroundTrack(Cmd_Argv(1), "");
}

/*
==============
S_TriggeredMusic_StartLoop
==============
*/
void S_TriggeredMusic_StartLoop()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstartloop <sound file>\n");
        return;
    }

    S_StartBackgroundTrack(Cmd_Argv(1), Cmd_Argv(1));
}

/*
==============
S_TriggeredMusic_Stop
==============
*/
void S_TriggeredMusic_Stop()
{
    S_StopBackgroundTrack();
}

/*
==============
S_TriggeredMusic_PlayIntroMusic
==============
*/
void S_TriggeredMusic_PlayIntroMusic() {
    S_StartBackgroundTrack("sound/music/mus_MainTheme.mp3", "");
}

/*
==============
S_TriggeredMusic_SetupHandle
==============
*/
void S_TriggeredMusic_SetupHandle(const char* pszName, int iLoopCount, int iOffset, qboolean autostart) {
    // FIXME: unimplemented
}

/*
==============
S_GetMusicFilename
==============
*/
const char* S_GetMusicFilename() {
    // FIXME: unimplemented
    return "";
}

/*
==============
S_GetMusicLoopCount
==============
*/
int S_GetMusicLoopCount() {
    // FIXME: unimplemented
    return 0;
}

/*
==============
S_GetMusicOffset
==============
*/
unsigned int S_GetMusicOffset() {
    // FIXME: unimplemented
    return 0;
}

/*
==============
callbackServer
==============
*/
void callbackServer(int entnum, int channel_number, const char* name) {
    if (com_sv_running->integer) {
        SV_SoundCallback(entnum, channel_number, name);
    }
}

/*
==============
S_ChannelFree_Callback
==============
*/
void S_ChannelFree_Callback(channel_t* v) {
    if (v->entnum & S_FLAG_DO_CALLBACK) {
        callbackServer(v->entnum & ~S_FLAG_DO_CALLBACK, v - s_channels, v->thesfx->soundName);
    }
}

/*
==============
S_LoadData
==============
*/
void S_LoadData(soundsystemsavegame_t* pSave) {
    // FIXME: unimplemented
}

/*
==============
S_SaveData
==============
*/
void S_SaveData(soundsystemsavegame_t* pSave) {
    // FIXME: unimplemented
}

/*
==============
S_ReLoad
==============
*/
void S_ReLoad(soundsystemsavegame_t* pSave) {
    // FIXME: unimplemented
}

/*
==============
S_StopMovieAudio
==============
*/
void S_StopMovieAudio() {
}

/*
==============
S_CurrentMoviePosition
==============
*/
int S_CurrentMoviePosition() {
    return 0;
}

/*
==============
S_SetupMovieAudio
==============
*/
void S_SetupMovieAudio(const char* pszMovieName) {
}

#endif
