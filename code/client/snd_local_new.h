/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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

#pragma once

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#ifdef __cplusplus
extern "C" {
#endif

extern cvar_t *s_volume;
extern cvar_t *s_khz;
extern cvar_t *s_loadas8bit;
extern cvar_t *s_separation;
extern cvar_t *s_musicVolume;
extern cvar_t *s_ambientVolume;
extern cvar_t *s_sfxvolume; // HZM coop
extern cvar_t *s_sfxduck; // HZM coop - effect-channel cinematic duck (music exempt)

// HZM coop [user 2026-09-01, bug-2309] 32 -> 96. THE BEACH RAN OUT OF SOUND CHANNELS.
//
// The user's own session log carried ~6,000 lines of
//     OpenAL: Couldn't play ND sound '<x>' for entity N on channel auto
// That message is printed when S_OPENAL_PickChannel3D returns -1 (snd_openal_new.cpp:1969), and it
// returns -1 only when NOT ONE of the 3D channels can be taken - i.e. every one is either already
// the listener's own (those are explicitly skipped as steal candidates) or is carrying something
// of equal-or-higher priority.
//
// *** THE 'Not playing sound' LINES ARE A DIFFERENT MESSAGE AND WERE WRONGLY FILED HERE. ***
// [2026-09-02] This block used to lump '175 Not playing sound m1garandfire1a' in as a third symptom
// of channel starvation. It is not. 'Not playing sound' is printed by the !S_OPENAL_ShouldPlay
// early-return at snd_openal_new.cpp:1949-1951, several lines BEFORE any channel is picked.
// ShouldPlay is a PER-SFX concurrency cap: sfx_infos[].max_number_playing, parsed from a `maxnumber`
// line in global/sound<N>.txt and otherwise DEFAULT_SFX_NUMBER_PLAYING (10), counted over the
// channels already playing that same sfx_t. It has nothing to do with how many channels exist, so
// raising the counts below cannot move it.
//
// MEASURED on the live 2026-09-02 Omaha log against this build: "Couldn't play" = 0 - the 32 -> 96
// raise did its job and starvation is gone - while "Not playing sound" = 968, every one of them one
// of four coop distance-gunfire aliases hitting the cap of 10 concurrent copies of the SAME wav:
// snd_gun_tail_far1 (329) and far2 (318), emitted client-side per impact at cg_parsemsg.cpp:987,
// plus snd_gun_sub_heavy (211) and coop_gun_tail_rifle (64), emitted server-side per MG shot at
// fgame/weapon.cpp:1872. That is the cap doing its job on emitters that have no cooldown of their
// own - not a shortage of anything. The two messages are separate diagnoses:
//     "Couldn't play ... on channel"  -> not enough CHANNELS       -> raise the counts below
//     "Not playing sound '<x>'"       -> too many copies of ONE sfx -> rate-limit the EMITTER
//
// Thirty-two simultaneous positional sounds is nothing on this map: fifty actors, four MG42s, a
// naval barrage, surf, nine burning wrecks, welders, and every gun's tail and impact. So the pool
// thrashed, and the symptoms were blamed on three different subsystems before the log named it:
//   - the M1 ping 'plays extremely short'  -> its channel stolen part-way through
//   - a line of dialogue never heard        -> it never got a channel at all
//   - gunshots that simply do not fire      -> the 175 'Not playing sound' lines
// One cause, three bug reports.
//
// RAISED RATHER THAN RATIONED, consistent with the entity pool and the effects pool: the user's
// standing call is 'id rather the pool get increased than we lose content'. Cost is one OpenAL
// source per channel (S_OPENAL_InitChannel does qalGenSources(1,...) each), so this takes the total
// from 101 to 165 sources. OpenAL Soft's default ceiling is 256 mono sources, so 165 leaves real
// headroom - but note that source generation runs through alDieIfError(), so a device that cannot
// supply them fails audio init loudly rather than degrading. If that is ever reported, lower this
// number first; it is the only thing here that scales with what the sound device can provide.
#define MAX_SOUNDSYSTEM_CHANNELS_3D        96
#define MAX_SOUNDSYSTEM_CHANNELS_2D        32
#define MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM 32
#define MAX_SOUNDSYSTEM_POSITION_CHANNELS  (MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM)
#define MAX_SOUNDSYSTEM_SONGS              2
#define MAX_SOUNDSYSTEM_MISC_CHANNELS      3
#define MAX_SOUNDSYSTEM_CHANNELS                                                                             \
    (MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM + MAX_SOUNDSYSTEM_SONGS \
     + MAX_SOUNDSYSTEM_MISC_CHANNELS)
// HZM coop [user 2026-09-02, bug-2315] 64 -> 128, following the 3D channel raise. The table holds
// every simultaneously-active looping sound, so it has to keep pace with the channels that can play
// them or the extra channels are unreachable for loops. openal_loop_sound_t is ~80 bytes, so this is
// about 10 KB.
#define MAX_SOUNDSYSTEM_LOOP_SOUNDS 128

// HZM coop [user 2026-09-02, bug-2315] A BUILD BREAK, BECAUSE KNOWING THE RULE WAS NOT ENOUGH.
//
// Seven loops in snd_openal_new.cpp walked loop_sounds[] with (CHANNELS_3D + CHANNELS_2D) as their
// bound. That happened to equal 64 while both were 32, so the array size and the loop bound were the
// same number by pure coincidence. Raising CHANNELS_3D to 96 turned the bound into 128 and the game
// crashed at launch, writing 64 entries past the end of the array.
//
// The loops now use MAX_SOUNDSYSTEM_LOOP_SOUNDS directly, but a fix that depends on nobody
// reintroducing the old expression is not a fix. If a future raise makes the channel count exceed
// what the loop-sound table holds, the compiler stops - not the player's game.
#if (MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D) > MAX_SOUNDSYSTEM_LOOP_SOUNDS
#  error "loop_sounds[] is smaller than the 3D+2D channel count. Raise MAX_SOUNDSYSTEM_LOOP_SOUNDS, and check every loop that walks loop_sounds[]: they must be bounded by MAX_SOUNDSYSTEM_LOOP_SOUNDS, never by the channel counts. See bug-2315."
#endif

#define SOUNDSYSTEM_CHANNEL_MP3_ID \
    (MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM + MAX_SOUNDSYSTEM_SONGS)
#define SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID (SOUNDSYSTEM_CHANNEL_MP3_ID + 1)
#define SOUNDSYSTEM_CHANNEL_MOVIE_ID         (SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID + 1)

typedef struct {
    int   format;
    float rate;

    float width;
    int   channels;
    int   samples;

    int dataofs;
    int datasize;
    int dataalign;
} wavinfo_t;

typedef struct sfx_s {
    int iFlags;

    int   length;
    int   width;
    byte *data;
    char  name[64];

    int   registration_sequence;
    int   sfx_info_index;
    float time_length;

    wavinfo_t    info;
    unsigned int buffer;
} sfx_t;

typedef struct {
    unsigned short wFormatTag;
    unsigned short nChannels;
    unsigned int   nSamplesPerSec;
    unsigned int   nAvgBytesPerSec;
    unsigned short nBlockAlign;
    unsigned short wBitsPerSample;
    unsigned short cbSize;
} wavinfo_x_t;

typedef struct {
    char name[64];

    int loop_start;
    int loop_end;

    int   max_number_playing;
    float max_factor;
} sfx_info_t;

typedef struct {
    vec3_t   position;
    vec3_t   velocity;
    int      time;
    qboolean use_listener;
} s_entity_t;

typedef struct {
    char alias[32];
    char path[64];

    int   mood_num;
    int   flags;
    float volume;
    float fadetime;

    int current_pos;
    int current_state;
} song_t;

typedef struct {
    int  iFlags;
    char szName[64];
} sfxsavegame_t;

typedef struct {
    qboolean      bPlaying;
    int           iStatus;
    sfxsavegame_t sfx;

    int    iEntNum;
    int    iEntChannel;
    vec3_t vOrigin;
    float  fVolume;
    int    iBaseRate;
    float  fNewPitchMult;
    float  fMinDist;
    float  fMaxDist;

    int iStartTime;
    int iTime;
    int iNextCheckObstructionTime;
    int iEndTime;

    int iFlags;
    int iOffset;
    int iLoopCount;
} channelbasesavegame_t;

typedef struct {
    channelbasesavegame_t Channels[MAX_SOUNDSYSTEM_CHANNELS];
} soundsystemsavegame_t;

enum channel_flags_t {
    CHANNEL_FLAG_PLAY_DEFERRED  = 1,
    CHANNEL_FLAG_LOCAL_LISTENER = 16,
    CHANNEL_FLAG_NO_ENTITY      = 32,
    CHANNEL_FLAG_PAUSED         = 64,
    CHANNEL_FLAG_LOOPING        = 128,
    // Added in OPM
    CHANNEL_FLAG_MISSING_ENT    = 256,
};

enum sfx_flags_t {
    SFX_FLAG_DEFAULT_SOUND = 1,
    SFX_FLAG_MP3           = 2,
    SFX_FLAG_STREAMED      = 4,
    SFX_FLAG_NO_OFFSET     = 8,
    SFX_FLAG_NULL          = 16,
};

enum loopsound_flags_t {
    LOOPSOUND_FLAG_NO_PAN = 1
};

#define MAX_SFX         4096
#define MAX_SFX_INFOS   1000
#define MAX_LOOP_SOUNDS 64
#define DEFAULT_SFX_NUMBER_PLAYING 10 //5

extern qboolean   s_bLastInitSound;
extern qboolean   s_bSoundStarted;
extern qboolean   s_bSoundPaused;
extern qboolean   s_bTryUnpause;
extern int        s_iListenerNumber;
extern float      s_fAmbientVolume;
extern int        number_of_sfx_infos;
extern sfx_info_t sfx_infos[];
extern sfx_t      s_knownSfx[];
extern int        s_numSfx;
extern s_entity_t s_entity[];

// The current sound driver.
// Currently OPENAL
#define SOUND_DRIVER OPENAL

//
// snd_info.cpp
//
void load_sfx_info();

//
// snd_dma_new.cpp
//

sfx_t *S_FindName(const char *name, int sequenceNumber);
void   S_DefaultSound(sfx_t *sfx);

void S_LoadData(soundsystemsavegame_t *pSave);
void S_SaveData(soundsystemsavegame_t *pSave);
void S_ClearSoundBuffer();

//
// snd_mem.c
//
qboolean S_LoadSound(const char *fileName, sfx_t *sfx, int streamed, qboolean force_load);

#define S_StopAllSounds2 S_StopAllSounds

//
// Driver-specific functions
//
#define S_Call_SndDriver(driver, func)  S_##driver##_##func
#define S_Call_SndDriverX(driver, func) S_Call_SndDriver(driver, func)

#define S_Driver_Init                   S_Call_SndDriverX(SOUND_DRIVER, Init)
#define S_Driver_Shutdown               S_Call_SndDriverX(SOUND_DRIVER, Shutdown)
#define S_Driver_StartSound             S_Call_SndDriverX(SOUND_DRIVER, StartSound)
#define S_Driver_AddLoopingSound        S_Call_SndDriverX(SOUND_DRIVER, AddLoopingSound)
#define S_Driver_ClearLoopingSounds     S_Call_SndDriverX(SOUND_DRIVER, ClearLoopingSounds)
#define S_Driver_StopSound              S_Call_SndDriverX(SOUND_DRIVER, StopSound)
#define S_Driver_StopAllSounds          S_Call_SndDriverX(SOUND_DRIVER, StopAllSounds)
#define S_Driver_Respatialize           S_Call_SndDriverX(SOUND_DRIVER, Respatialize)
#define S_Driver_SetReverb              S_Call_SndDriverX(SOUND_DRIVER, SetReverb)
#define S_Driver_Update                 S_Call_SndDriverX(SOUND_DRIVER, Update)
#define S_Driver_GetMusicFilename       S_Call_SndDriverX(SOUND_DRIVER, GetMusicFilename)
#define S_Driver_GetMusicLoopCount      S_Call_SndDriverX(SOUND_DRIVER, GetMusicLoopCount)
#define S_Driver_GetMusicOffset         S_Call_SndDriverX(SOUND_DRIVER, GetMusicOffset)

void S_PrintInfo();
void S_DumpInfo();
qboolean S_NeedFullRestart();
void S_ReLoad(soundsystemsavegame_t* pSave);

extern cvar_t *s_show_sounds;

#ifdef __cplusplus
}
#endif
