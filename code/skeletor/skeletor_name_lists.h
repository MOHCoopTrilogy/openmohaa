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

// skeletor_name_lists.h : Skeletor name lists

#pragma once

// [user 2026-08-14, bug-1803] 2560 -> 16384. Limit was 2048 before 2.30, then 2560.
//
// THIS TABLE IS A PROCESS-GLOBAL STATIC (skeletor_c::m_channelNames) AND IS NEVER RESET BETWEEN
// MAPS - only zeroed in its constructor at startup. So it is not a per-map budget, it is a
// per-SESSION one: every unique bone/tag name from every model loaded since launch stays in it.
// A player working through the campaign without restarting therefore fills it gradually and dies
// on whichever map happens to hold the last channel, which makes it look like that map is at
// fault when it is not. Observed after ~28 map loads in one session: e2l2 tipped it over while
// registering rigidSim23/24 bones, and every other map that session had loaded cleanly.
//
// SIZING RULE: the budget must cover an ENTIRE UNINTERRUPTED PLAYTHROUGH - all three campaigns
// plus the lobbies between them - because nothing frees an entry short of restarting the game.
// Sizing it to "what we happened to observe" just moves the crash to a later map.
//
// So the ceiling is set against a MEASURED WHOLE-GAME WORST CASE, not an extrapolation. Parsing
// the real on-disk tables of every asset in main + mainta + maintt (5,692 raw SKAN channel-name
// arrays and 1,550 SKMD bone lists, deriving BOTH " rot" and " pos" for every bone and ignoring
// IsBogusChannelName filtering, so the number is an upper bound) gives 4,589 unique channels for
// the ENTIRE GAME. A session that somehow loaded every model that ships would use 4,589 of
// 16,384 - 28%. That makes the overflow structurally impossible on any route, rather than
// merely unlikely. Re-measure with docs/tools/count_skel_channels.py after adding model packs.
//
// Note what that number also proves: 4,589 > 2,560, so the OLD ceiling was below what the game
// can demand outright. A long enough session was always going to hit it - this was never bad
// luck on one map, it was arithmetic waiting to happen.
//
// Not larger, and the reason is not memory: SortIntoTable() memmoves the tail of the array on
// every insertion, so total insertion cost is O(n^2). 16384 entries is ~4.5 GB of memmove spread
// across a whole session (fractions of a second, unnoticeable); the 32767 `short` ceiling would
// be ~18 GB. 16384 buys the headroom without making registration quadratically worse.
//
// Memory is trivial either way: ChannelName_t is 34 bytes, so 16384 entries is ~560 KB plus a
// 32 KB lookup. Both m_iNumChannels and channelNum are `short`, which caps at 32767.
#define MAX_SKELETOR_CHANNELS 16384

// Warn once at 75%. The failure mode this replaces was silent right up until it was fatal, and
// fatal on an innocent map. This gives a map-load's worth of warning in the log instead.
#define SKEL_CHANNELS_WARN_AT ((MAX_SKELETOR_CHANNELS * 3) / 4)
#define MAX_CHANNEL_NAME      32

typedef struct ChannelName_s {
    char  name[MAX_CHANNEL_NAME];
    short channelNum;
} ChannelName_t;

#ifdef __cplusplus

class ChannelNameTable
{
    short int     m_iNumChannels;
    ChannelName_t m_Channels[MAX_SKELETOR_CHANNELS];
    short int     m_lookup[MAX_SKELETOR_CHANNELS];

public:
    ChannelNameTable();

    int         RegisterChannel(const char *name);
    int         FindNameLookup(const char *name);
    void        PrintContents();
    const char *FindName(int index);
    int         NumChannels() const;

private:
    const char *FindNameFromLookup(int index);
    bool        FindIndexFromName(const char *name, int *indexPtr);
    void        SortIntoTable(int index);
    void        CopyChannel(ChannelName_t *dest, const ChannelName_t *source);
    void        SetChannelName(ChannelName_t *channel, const char *newName);
};

typedef enum channelType_e {
    CHANNEL_ROTATION,
    CHANNEL_POSITION,
    CHANNEL_NONE,
    CHANNEL_VALUE
} channelType_t;

channelType_t GetBoneChannelType(const char *name);

#else

typedef struct {
    short int     m_iNumChannels;
    ChannelName_t m_Channels[MAX_SKELETOR_CHANNELS];
    short int     m_lookup[MAX_SKELETOR_CHANNELS];
} ChannelNameTable;

#endif
