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

// bonetable.cpp : Bone table

#include "q_shared.h"
#include "tiki.h"

void ChannelNameTable::CopyChannel(ChannelName_t *dest, const ChannelName_t *source)
{
    memcpy(dest, source, sizeof(ChannelName_t));
}

void ChannelNameTable::SetChannelName(ChannelName_t *channel, const char *newName)
{
    Q_strncpyz(channel->name, newName, sizeof(channel->name));
}

ChannelNameTable::ChannelNameTable()
{
    memset(this, 0, sizeof(ChannelNameTable));
    m_iNumChannels = 0;
}

void ChannelNameTable::PrintContents()
{
    str channelList;
    int i;

    for (i = 0; i < m_iNumChannels; i++) {
        if (!m_Channels[i].name[0]) {
            continue;
        }

        channelList += str("c") + m_Channels[i].channelNum + ":" + str(m_Channels[i].name) + "\n";
    }

    SKEL_Message("ChannelNameTable contents:\n%s", channelList.c_str());
}

bool ChannelNameTable::FindIndexFromName(const char *name, int *indexPtr)
{
    int sortValue;
    int lowerBound;
    int upperBound;
    int index;

    lowerBound = 0;
    upperBound = m_iNumChannels - 1;
    while (lowerBound <= upperBound) {
        index     = (lowerBound + upperBound) / 2;
        sortValue = stricmp(name, m_Channels[index].name);
        if (!sortValue) {
            if (indexPtr) {
                *indexPtr = index;
            }
            return true;
        }
        if (sortValue <= 0) {
            upperBound = index - 1;
        } else {
            lowerBound = index + 1;
        }
    }

    if (indexPtr) {
        *indexPtr = lowerBound;
    }
    return false;
}

int ChannelNameTable::FindNameLookup(const char *name)
{
    int index;

    if (FindIndexFromName(name, &index)) {
        return m_Channels[index].channelNum;
    } else {
        return -1;
    }
}

const char *ChannelNameTable::FindName(int index)
{
    return FindNameFromLookup(m_lookup[index]);
}

void ChannelNameTable::SortIntoTable(int index)
{
    int           i;
    ChannelName_t tempName;

    CopyChannel(&tempName, &m_Channels[m_iNumChannels]);

    for (i = m_iNumChannels - 1; i >= index; i--) {
        m_lookup[m_Channels[i].channelNum] = i + 1;
        CopyChannel(&m_Channels[i + 1], &m_Channels[i]);
    }

    m_lookup[tempName.channelNum] = index;
    CopyChannel(&m_Channels[index], &tempName);
}

static const char *bogusNameTable[] = {
    "Bip01 Spine pos",
    "Bip01 Spine1 pos",
    "Bip01 Spine2 pos",
    "Bip01 Neck pos",
    "Bip01 Head pos",
    "helmet bone pos",
    "helmet bone rot",
    "Bip01 R Clavicle pos",
    "Bip01 R UpperArm pos",
    "Bip01 R Forearm pos",
    "Bip01 R Hand pos",
    "Bip01 R Finger0 pos",
    "Bip01 R Finger01 pos",
    "Bip01 R Finger02 pos",
    "Bip01 R Finger1 pos",
    "Bip01 R Finger11 pos",
    "Bip01 R Finger12 pos",
    "Bip01 R Finger2 pos",
    "Bip01 R Finger21 pos",
    "Bip01 R Finger22 pos",
    "Bip01 R Finger3 pos",
    "Bip01 R Finger31 pos",
    "Bip01 R Finger32 pos",
    "Bip01 R Finger4 pos",
    "Bip01 R Finger41 pos",
    "Bip01 R Finger42 pos",
    "Bip01 L Clavicle pos",
    "Bip01 L UpperArm pos",
    "Bip01 L Forearm pos",
    "Bip01 L Hand pos",
    "Bip01 L Finger0 pos",
    "Bip01 L Finger01 pos",
    "Bip01 L Finger02 pos",
    "Bip01 L Finger1 pos",
    "Bip01 L Finger11 pos",
    "Bip01 L Finger12 pos",
    "Bip01 L Finger2 pos",
    "Bip01 L Finger21 pos",
    "Bip01 L Finger22 pos",
    "Bip01 L Finger3 pos",
    "Bip01 L Finger31 pos",
    "Bip01 L Finger32 pos",
    "Bip01 L Finger4 pos",
    "Bip01 L Finger41 pos",
    "Bip01 L Finger42 pos",
    "Bip01 L Toe0 pos",
    "Bip01 R Toe0 pos",
    "buckle bone pos",
    "buckle bone rot",
    "belt attachments bone pos",
    "belt attachments bone rot",
    "backpack bone pos",
    "backpack bone rot",
    "helper Lelbow pos",
    "helper Lelbow rot",
    "helper Lshoulder pos",
    "helper Lshoulder rot",
    "helper Lshoulder01 pos",
    "helper Lshoulder01 rot",
    "helper Rshoulder pos",
    "helper Rshoulder rot",
    "helper Lshoulder02 pos",
    "helper Lshoulder02 rot",
    "helper Relbow pos",
    "helper Relbow rot",
    "helper Lankle pos",
    "helper Lankle rot",
    "helper Lknee pos",
    "helper Lknee rot",
    "helper Rankle pos",
    "helper Rankle rot",
    "helper Rknee pos",
    "helper Rknee rot",
    "helper Lhip pos",
    "helper Lhip rot",
    "helper Lhip01 pos",
    "helper Lhip01 rot",
    "helper Rhip pos",
    "helper Rhip rot",
    "helper Rhip01 pos",
    "helper Rhip01 rot",
    "target_left_hand pos",
    "target_left_hand rot",
    "target_right_hand pos",
    "target_right_hand rot",
    "JAW open-closed pos",
    "JAW open-closed rot",
    "BROW_worry$",
    "EYES_down$",
    "EYES_Excited_$",
    "EYES_L_squint$",
    "EYES_left$",
    "EYES_right$",
    "EYES_smile$",
    "EYES_up_$",
    "JAW_open-open$",
    "MOUTH_L_smile_open$",
    "VISEME_Eat$",
    "right foot placeholder pos"
    "right foot placeholder rot"
    "left foot placeholder pos"
    "left foot placeholder rot"
};

bool IsBogusChannelName(const char *name)
{
    int i;

    for (i = 0; i < sizeof(bogusNameTable) / sizeof(bogusNameTable[0]); i++) {
        if (!Q_stricmp(name, bogusNameTable[i])) {
            return true;
        }
    }

    if (strstr(name, "Bip0") && !strstr(name, "Bip01") && !strstr(name, "Footsteps")) {
        return true;
    }

    return false;
}

channelType_t GetBoneChannelType(const char *name)
{
    size_t len;

    if (!name) {
        return CHANNEL_NONE;
    }

    if (IsBogusChannelName(name)) {
        return CHANNEL_NONE;
    }

    len = strlen(name);
    if (len < 4) {
        return CHANNEL_VALUE;
    }

    if (!strcmp(name + len - 4, " rot")) {
        return CHANNEL_ROTATION;
    }

    if (!strcmp(name + len - 4, " pos")) {
        return CHANNEL_POSITION;
    }

    if (len >= 6 && !strcmp(name + len - 6, " rotFK")) {
        return CHANNEL_NONE;
    }

    return CHANNEL_VALUE;
}

int ChannelNameTable::RegisterChannel(const char *name)
{
    int index;

    if (IsBogusChannelName(name)) {
        return -1;
    }

    if (FindIndexFromName(name, &index)) {
        return m_Channels[index].channelNum;
    }

    if (m_iNumChannels >= MAX_SKELETOR_CHANNELS) {
        // [user 2026-08-14, bug-1803] DUMP THE TABLE ONCE, not on every failed registration.
        // Once the table is full EVERY subsequent bone fails, and each failure re-printed all
        // 2560 names. One observed overflow produced 491,630 lines and a 42 MB qconsole.log in a
        // single map load - which buries the actual error, makes the log useless for anything
        // else, and costs real time to write while the game is already dying. The list is
        // identical every time, so the second copy carries no information the first did not.
        static qboolean dumped = qfalse;
        if (!dumped) {
            dumped = qtrue;
            Com_Printf("==========================================================================\n");
            Com_Printf("Skeletor channel name table, contents (in order of addition):\n");

            for (index = 0; index < MAX_SKELETOR_CHANNELS; index++) {
                Com_Printf("%s\n", m_Channels[m_lookup[index]].name);
            }

            Com_Printf("==========================================================================\n");
        }

        // [user 2026-08-14, bug-1803] FAIL SOFT. This was SKEL_Error, i.e. ERR_DROP: one bone over
        // the line killed the entire map load, on the loading screen, with no way forward.
        //
        // Returning -1 instead is safe and is NOT a new code path: -1 is already the normal,
        // expected return for any name IsBogusChannelName() rejects, and the sole consumer,
        // skelChannelList_c::AddChannel, tests for it explicitly (skeletor_utilities.cpp:188).
        // The one other caller, CreatePosRotBoneFileData, assigns it to boneData->parent, where
        // -1 is already the "worldbone" sentinel. So the worst case degrades from "the map is
        // unplayable" to "one bone on one model does not animate" - and given the ceiling is now
        // ~3.5x the measured whole-game requirement, reaching this at all should be impossible.
        SKEL_Warning(
            "Channel name table is full (%i channels); ignoring channel '%s'. This table is never "
            "freed between maps - restart the game to reclaim it.\n",
            MAX_SKELETOR_CHANNELS,
            name
        );
        return -1;
    } else {
        // [user 2026-08-14, bug-1803] The table is never reset between maps, so its fill level is
        // a whole-session running total. Print a milestone every 1024 so the log shows how fast a
        // real playthrough consumes it (correlate against the map-load banners), and warn once at
        // 75% so a session that is genuinely heading for the ceiling says so while it is still
        // recoverable by restarting - rather than dying on an innocent map with no prior notice.
        if (m_iNumChannels && !(m_iNumChannels % 1024)) {
            Com_Printf("Skeletor channels: %i / %i used this session.\n", m_iNumChannels, MAX_SKELETOR_CHANNELS);
        }

        if (m_iNumChannels == SKEL_CHANNELS_WARN_AT) {
            Com_Printf(
                "WARNING: skeletor channel table is %i/%i full. This table is never freed between "
                "maps; restart the game to reclaim it if map loads begin to fail.\n",
                m_iNumChannels,
                MAX_SKELETOR_CHANNELS
            );
        }

        SetChannelName(&m_Channels[m_iNumChannels], name);
        m_Channels[m_iNumChannels].channelNum = m_iNumChannels;
        SortIntoTable(index);
        return m_iNumChannels++;
    }
}

const char *ChannelNameTable::FindNameFromLookup(int index)
{
    if (index >= m_iNumChannels) {
        return NULL;
    }

    return m_Channels[index].name;
}
