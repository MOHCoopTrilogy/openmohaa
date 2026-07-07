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

// DESCRIPTION:
// UI features

#include "cg_local.h"
#include "str.h"
#include "../client/keycodes.h"

void CG_MessageMode_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cgi.UI_ToggleDMMessageConsole(300);
}

void CG_MessageMode_All_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cgi.UI_ToggleDMMessageConsole(100);
}

void CG_MessageMode_Team_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cgi.UI_ToggleDMMessageConsole(200);
}

void CG_MessageMode_Private_f(void)
{
    int clientNum;

    if (!cgs.gametype) {
        return;
    }

    clientNum = atoi(cgi.Argv(1));
    if (clientNum < 1 || clientNum >= MAX_CLIENTS) {
        cgi.Printf(HUD_MESSAGE_CHAT_WHITE "Message Error: %s is a bad client number\n", cgi.Argv(1));
        return;
    }

    cgi.UI_ToggleDMMessageConsole(clientNum);
}

void CG_MessageSingleAll_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    if (cgi.Argc() > 1) {
        cgi.SendClientCommand(va("dmmessage 0 %s\n", cgi.Args()));
    } else {
        cgi.UI_ToggleDMMessageConsole(-100);
    }
}

void CG_MessageSingleTeam_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    if (cgi.Argc() > 1) {
        cgi.SendClientCommand(va("dmmessage -1 %s\n", cgi.Args()));
    } else {
        cgi.UI_ToggleDMMessageConsole(-200);
    }
}

void CG_MessageSingleClient_f(void)
{
    int clientNum;

    if (!cgs.gametype) {
        return;
    }

    clientNum = atoi(cgi.Argv(1));
    if (clientNum < 1 || clientNum > MAX_CLIENTS) {
        cgi.Printf(HUD_MESSAGE_CHAT_WHITE "Message Error: %s is a bad client number\n", cgi.Argv(1));
        return;
    }

    if (cgi.Argc() > 2) {
        int i;
        str sString;

        sString = "dmmessage ";
        sString += va("%i", clientNum);

        // copy the rest
        for (i = 2; i < cgi.Argc(); i++) {
            sString += va(" %s", cgi.Argv(i));
        }

        sString += "\n";
        cgi.SendClientCommand(sString.c_str());
    } else {
        cgi.UI_ToggleDMMessageConsole(-clientNum);
    }
}

void CG_InstaMessageMain_f(void)
{
    if (!voiceChat->integer) {
        return;
    }

    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = -1;
}

void CG_InstaMessageGroupA_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = 1;
}

void CG_InstaMessageGroupB_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = 2;
}

void CG_InstaMessageGroupC_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = 3;
}

void CG_InstaMessageGroupD_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = 4;
}

void CG_InstaMessageGroupE_f(void)
{
    if (!cgs.gametype) {
        return;
    }

    cg.iInstaMessageMenu = 5;
}

void CG_HudPrint_f(void)
{
    cgi.Printf("\x1%s", cgi.Argv(1));
}

qboolean CG_CheckCaptureKey(int key, qboolean down, unsigned int time)
{
    char minKey = '1', maxKey = '9';

    // HZM coop - STAGED 3P ADS wheel capture. While a third-person player HOLDS the ADS button, the
    // mouse wheel changes the ADS stage instead of running its weapnext/weapprev binds: wheel-UP =
    // into first-person irons (cg_adsStage 1), wheel-DOWN = back to the shoulder view (cg_adsStage 0).
    // Swallow BOTH the down and the up half of the wheel event so the bind never fires (the wheel's
    // binding itself is untouched - it works normally the moment ADS is released). This hook runs from
    // CL_KeyEvent only when no UI/console catcher is active, so menus/console still get the wheel.
    if (key == K_MWHEELUP || key == K_MWHEELDOWN) {
        // single decider shared with the camera stage logic (cg_view.c): also FALSE for scoped
        // rifles (they go straight to the native zoom - the wheel keeps switching weapons).
        if (CG_AdsShoulderWheelActive()) {
            if (down) {
                cgi.Cvar_Set("cg_adsStage", (key == K_MWHEELUP) ? "1" : "0");
            }
            return qtrue;
        }
        // HZM coop [239] - FREE CAM wheel: wheel-up snaps to FIRST person (ADS-ready - hold RMB
        // for the per-gun tuned irons, works for every aligned weapon), wheel-down returns to the
        // 3P free cam while we own that trip. Without this the un-captured wheel fired its
        // weapnext/weapprev bind - the "gun held in one hand" the user saw was the NEXT WEAPON'S
        // pullout animation, not a broken ADS.
        {
            static qboolean bFcWheelFp = qfalse;

            if (CG_FreecamCaptureActive()) {
                if (down && key == K_MWHEELUP) {
                    cgi.Cvar_Set("cg_3rd_person", "0");
                    bFcWheelFp = qtrue;
                }
                return qtrue; // swallow both directions while the orbit owns the mouse
            }
            if (bFcWheelFp && !cg.renderingThirdPerson) {
                if (key == K_MWHEELDOWN) {
                    if (down) {
                        cgi.Cvar_Set("cg_3rd_person", "1");
                        bFcWheelFp = qfalse;
                    }
                    return qtrue; // the return trip belongs to the view, not weapprev
                }
            } else {
                bFcWheelFp = qfalse; // view changed by other means - release the wheel
            }
        }
    }

    // HZM coop - MOUSE3 while shoulder-aiming = SWAP SHOULDERS. Toggles the archived
    // cg_adsShoulderRight cvar; cg_view.c eases the side sign so the camera sweeps across the back.
    // Same decider + swallow pattern as the wheel steal above: the MOUSE3 bind never fires while the
    // shoulder view is up, and works normally the moment ADS is released.
    if (key == K_MOUSE3) {
        if (CG_AdsShoulderWheelActive()) {
            if (down) {
                cvar_t *pRight = cgi.Cvar_Get("cg_adsShoulderRight", "1", CVAR_ARCHIVE);
                cgi.Cvar_Set("cg_adsShoulderRight", pRight->integer ? "0" : "1");
            }
            return qtrue;
        }
    }

    if (!cg.iInstaMessageMenu || !down) {
        return qfalse;
    }

    if (cg_protocol >= protocol_e::PROTOCOL_MOHTA_MIN) {
        maxKey = '8';
    }

    if (key < minKey || key > maxKey) {
        if (key == K_ESCAPE || key == '0') {
            cg.iInstaMessageMenu = 0;
            return qtrue;
        }
        return qfalse;
    }

    if (cg.iInstaMessageMenu == -1) {
        if (key > '6') {
            cg.iInstaMessageMenu = 0;
        } else {
            cg.iInstaMessageMenu = key - '0';
        }
    } else if (cg.iInstaMessageMenu > 0) {
        cgi.SendClientCommand(va("dmmessage 0 *%i%i\n", cg.iInstaMessageMenu, key - '0'));
        cg.iInstaMessageMenu = 0;
    }

    return qtrue;
}
