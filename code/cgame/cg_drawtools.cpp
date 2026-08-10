/*
===========================================================================
Copyright (C) 2025 the OpenMoHAA team

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
// Some tools used to drawing 2d stuff

#include "cg_local.h"

extern "C" void CG_DrawCoopIcons(void);

// HZM coop - HUD FADE state (logic lives above CG_Draw2D; declared here because huddraw/
// magazines drawing earlier in the file multiplies the same alpha)
static int   s_hudTouchTime = -100000; // cg.time of the last HUD-relevant activity
static float s_hudFadeAlpha = 1.0f;

/*
================
CG_AdjustFrom640

Adjusted for resolution and screen aspect ratio
================
*/
void CG_AdjustFrom640(float *x, float *y, float *w, float *h)
{
#if 0
	// adjust for wide screens
	if ( cgs.glconfig.vidWidth * 480 > cgs.glconfig.vidHeight * 640 ) {
		*x += 0.5 * ( cgs.glconfig.vidWidth - ( cgs.glconfig.vidHeight * 640 / 480 ) );
	}
#endif
    // scale for screen sizes
    *x *= cgs.screenXScale;
    *y *= cgs.screenYScale;
    *w *= cgs.screenXScale;
    *h *= cgs.screenYScale;
}

/*
=============
CG_TileClearBox

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void CG_TileClearBox(int x, int y, int w, int h, qhandle_t hShader)
{
    float s1, t1, s2, t2;

    s1 = x / 64.0;
    t1 = y / 64.0;
    s2 = (x + w) / 64.0;
    t2 = (y + h) / 64.0;
    cgi.R_DrawStretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
}

/*
==============
CG_TileClear

Clear around a sized down screen
==============
*/
void CG_TileClear(void)
{
    int top, bottom, left, right;
    int w, h;

    w = cgs.glconfig.vidWidth;
    h = cgs.glconfig.vidHeight;

    if (cg.refdef.x == 0 && cg.refdef.y == 0 && cg.refdef.width == w && cg.refdef.height == h) {
        return; // full screen rendering
    }

    top    = cg.refdef.y;
    bottom = top + cg.refdef.height - 1;
    left   = cg.refdef.x;
    right  = left + cg.refdef.width - 1;

    // clear above view screen
    CG_TileClearBox(0, 0, w, top, cgs.media.backTileShader);

    // clear below view screen
    CG_TileClearBox(0, bottom, w, h - bottom, cgs.media.backTileShader);

    // clear left of view screen
    CG_TileClearBox(0, top, left, bottom - top + 1, cgs.media.backTileShader);

    // clear right of view screen
    CG_TileClearBox(right, top, w - right, bottom - top + 1, cgs.media.backTileShader);
}

/*
===============================================================================

LAGOMETER

===============================================================================
*/

#define LAG_SAMPLES 128

typedef struct {
    int frameSamples[LAG_SAMPLES];
    int frameCount;
    int snapshotFlags[LAG_SAMPLES];
    int snapshotSamples[LAG_SAMPLES];
    int snapshotCount;
} lagometer_t;

lagometer_t lagometer;

/*
==============
CG_AddLagometerFrameInfo

Adds the current interpolate / extrapolate bar for this frame
==============
*/
void CG_AddLagometerFrameInfo(void)
{
    int offset;

    offset                                                           = cg.time - cg.latestSnapshotTime;
    lagometer.frameSamples[lagometer.frameCount & (LAG_SAMPLES - 1)] = offset;
    lagometer.frameCount++;
}

/*
==============
CG_AddLagometerSnapshotInfo

Each time a snapshot is received, log its ping time and
the number of snapshots that were dropped before it.

Pass NULL for a dropped packet.
==============
*/
void CG_AddLagometerSnapshotInfo(snapshot_t *snap)
{
    // dropped packet
    if (!snap) {
        lagometer.snapshotSamples[lagometer.snapshotCount & (LAG_SAMPLES - 1)] = -1;
        lagometer.snapshotCount++;
        return;
    }

    // add this snapshot's info
    lagometer.snapshotSamples[lagometer.snapshotCount & (LAG_SAMPLES - 1)] = snap->ping;
    lagometer.snapshotFlags[lagometer.snapshotCount & (LAG_SAMPLES - 1)]   = snap->snapFlags;
    lagometer.snapshotCount++;
}

/*
==============
CG_DrawDisconnect
==============
*/
void CG_DrawDisconnect(void)
{
    float     x, y;
    float     w, h;
    int       cmdNum;
    qhandle_t handle;
    usercmd_t cmd;

    // draw the phone jack if we are completely past our buffers
    cmdNum = cgi.GetCurrentCmdNumber() - CMD_BACKUP + 1;
    cgi.GetUserCmd(cmdNum, &cmd);
    if (!cg.snap || cmd.serverTime <= cg.snap->ps.commandTime
        || cmd.serverTime > cg.time) { // special check for map_restart
        return;
    }

    // blink it
    if ((cg.time >> 9) & 1) {
        return;
    }

    handle = cgi.R_RegisterShader("gfx/2d/net.tga");
    w      = cgi.R_GetShaderWidth(handle) * cgs.uiHiResScale[0];
    h      = cgi.R_GetShaderHeight(handle) * cgs.uiHiResScale[1];
    x      = ((float)cgs.glconfig.vidWidth - w) * 0.5;
    y      = (float)cgs.glconfig.vidHeight - h;

    cgi.R_DrawStretchPic(x, y, w, h, 0, 0, 1, 1, handle);
}

#define MAX_LAGOMETER_PING  900
#define MAX_LAGOMETER_RANGE 300

/*
==============
CG_DrawLagometer
==============
*/
void CG_DrawLagometer(void)
{
    int   a, i;
    float v;
    float ax, ay, aw, ah, mid, range;
    int   color;
    float vscale;

    if (!cg_lagometer->integer) {
        CG_DrawDisconnect();
        return;
    }

    //
    // draw the graph
    //
    ax = 272.0;
    ay = 432.0;
    aw = 96.0;
    ah = 48.0;
    CG_AdjustFrom640(&ax, &ay, &aw, &ah);

    cgi.R_SetColor(NULL);
    cgi.R_DrawStretchPic(ax, ay, aw, ah, 0, 0, 1, 1, cgs.media.lagometerShader);

    color = -1;
    range = ah / 3;
    mid   = ay + range;

    vscale = range / MAX_LAGOMETER_RANGE;

    // draw the frame interpoalte / extrapolate graph
    for (a = 0; a < aw; a++) {
        i = (lagometer.frameCount - 1 - a) & (LAG_SAMPLES - 1);
        v = lagometer.frameSamples[i];
        v *= vscale;
        if (v > 0) {
            if (color != 1) {
                color = 1;
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_YELLOW)]);
            }
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, mid - v, 1, v);
        } else if (v < 0) {
            if (color != 2) {
                color = 2;
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_BLUE)]);
            }
            v = -v;
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, mid, 1, v);
        }
    }

    // draw the snapshot latency / drop graph
    range  = ah / 2;
    vscale = range / MAX_LAGOMETER_PING;

    for (a = 0; a < aw; a++) {
        i = (lagometer.snapshotCount - 1 - a) & (LAG_SAMPLES - 1);
        v = lagometer.snapshotSamples[i];
        if (v > 0) {
            if (lagometer.snapshotFlags[i] & SNAPFLAG_RATE_DELAYED) {
                if (color != 5) {
                    color = 5; // YELLOW for rate delay
                    cgi.R_SetColor(g_color_table[ColorIndex(COLOR_YELLOW)]);
                }
            } else {
                if (color != 3) {
                    color = 3;
                    cgi.R_SetColor(g_color_table[ColorIndex(COLOR_GREEN)]);
                }
            }
            v = v * vscale;
            if (v > range) {
                v = range;
            }
            cgi.R_DrawBox(ax + aw - a, ay + ah - v, 1, v);
        } else if (v < 0) {
            if (color != 4) {
                color = 4; // RED for dropped snapshots
                cgi.R_SetColor(g_color_table[ColorIndex(COLOR_RED)]);
            }
            cgi.R_DrawBox(ax + aw - a, ay + ah - range, 1, range);
        }
    }

    cgi.R_SetColor(NULL);

    CG_DrawDisconnect();
}

static void CG_DrawPauseIcon()
{
    qhandle_t handle;
    float     x, y, w, h;

    if (paused->integer) {
        handle = cgs.media.pausedShader;
    } else {
        if (cg.predicted_player_state.pm_flags & PMF_LEVELEXIT) {
            // blink it
            if ((cg.time >> 9) & 1) {
                return;
            }
            handle = cgs.media.levelExitShader;
        } else {
            return;
        }
    }
    w = cgi.R_GetShaderWidth(handle);
    h = cgi.R_GetShaderHeight(handle);
    if (cg.snap && cg.snap->ps.blend[3] > 0) {
        y = cgs.glconfig.vidHeight * 0.45f - h / 2.f;
    } else {
        y = cgs.glconfig.vidHeight * 0.75f - h / 2.f;
    }
    x = (cgs.glconfig.vidWidth - w) / 2.f;

    cgi.R_SetColor(colorWhite);
    cgi.R_DrawStretchPic(x, y, w * cgs.uiHiResScale[0], h * cgs.uiHiResScale[1], 0, 0, 1, 1, handle);
}

static void CG_DrawServerLag()
{
    float     x, y;
    float     w, h;
    qhandle_t handle;

    if (!cg_drawsvlag->integer) {
        return;
    }

    if (!developer->integer && !cgs.gametype) {
        return;
    }

    if (!cgs.serverLagTime) {
        return;
    }

    if (cg.time - cgs.serverLagTime > 3000) {
        return;
    }

    // blink it
    if ((cg.time >> 9) & 1) {
        return;
    }

    handle = cgi.R_RegisterShader("gfx/2d/slowserver");
    w      = (float)cgi.R_GetShaderWidth(handle) * cgs.uiHiResScale[0] / 4;
    h      = (float)cgi.R_GetShaderHeight(handle) * cgs.uiHiResScale[1] / 4;
    x      = ((float)cgs.glconfig.vidWidth - w) / 2;
    y      = (float)cgs.glconfig.vidHeight - h;
    cgi.R_DrawStretchPic(x, y, w, h, 0.0, 0.0, 1.0, 1.0, handle);
}

/*
==============
CG_DrawIcons
==============
*/
void CG_DrawIcons(void)
{
    if (!cg_hud->integer) {
        return;
    }

    CG_DrawPauseIcon();
    CG_DrawServerLag();
}

void CG_DrawOverlayTopBottom(qhandle_t handleTop, qhandle_t handleBottom, float fAlpha)
{
    int    iHalfWidth;
    int    iWidthOffset;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfWidth   = cgs.glconfig.vidHeight >> 1;
    iWidthOffset = (cgs.glconfig.vidWidth - cgs.glconfig.vidHeight) >> 1;

    cgi.R_DrawStretchPic(iWidthOffset, 0.0, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handleTop);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, 0.0, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handleTop);
    cgi.R_DrawStretchPic(iWidthOffset, iHalfWidth, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handleBottom);
    cgi.R_DrawStretchPic(
        iWidthOffset + iHalfWidth, iHalfWidth, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handleBottom
    );

    color[0] = 0.0;
    color[1] = 0.0;
    color[2] = 0.0;
    cgi.R_SetColor(color);

    cgi.R_DrawStretchPic(0.0, 0.0, iWidthOffset, cgs.glconfig.vidHeight, 0.0, 0.0, 1.0, 1.0, cgs.media.lagometerShader);
    cgi.R_DrawStretchPic(
        cgs.glconfig.vidWidth - iWidthOffset,
        0.0,
        iWidthOffset,
        cgs.glconfig.vidHeight,
        0.0,
        0.0,
        1.0,
        1.0,
        cgs.media.lagometerShader
    );
}

void CG_DrawOverlayMiddle(qhandle_t handle, float fAlpha)
{
    int    iHalfWidth;
    int    iWidthOffset;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfWidth   = cgs.glconfig.vidHeight >> 1;
    iWidthOffset = (cgs.glconfig.vidWidth - cgs.glconfig.vidHeight) >> 1;

    cgi.R_DrawStretchPic(iWidthOffset, 0.0, iHalfWidth, iHalfWidth, 0.0, 0.0, 1.0, 1.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, 0.0, iHalfWidth, iHalfWidth, 1.0, 0.0, 0.0, 1.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset, iHalfWidth, iHalfWidth, iHalfWidth, 0.0, 1.0, 1.0, 0.0, handle);
    cgi.R_DrawStretchPic(iWidthOffset + iHalfWidth, iHalfWidth, iHalfWidth, iHalfWidth, 1.0, 1.0, 0.0, 0.0, handle);

    color[0] = 0.0;
    color[1] = 0.0;
    color[2] = 0.0;
    cgi.R_SetColor(color);

    cgi.R_DrawStretchPic(0.0, 0.0, iWidthOffset, cgs.glconfig.vidHeight, 0.0, 0.0, 1.0, 1.0, cgs.media.lagometerShader);
    cgi.R_DrawStretchPic(
        cgs.glconfig.vidWidth - iWidthOffset,
        0.0,
        iWidthOffset,
        cgs.glconfig.vidHeight,
        .0,
        0.0,
        1.0,
        1.0,
        cgs.media.lagometerShader
    );
}

void CG_DrawOverlayFullScreen(qhandle_t handle, float fAlpha)
{
    int    iHalfWidth, iHalfHeight;
    vec4_t color;

    color[0] = 1.0;
    color[1] = 1.0;
    color[2] = 1.0;
    color[3] = fAlpha;
    cgi.R_SetColor(color);

    iHalfHeight = cgs.glconfig.vidHeight >> 1;
    iHalfWidth  = cgs.glconfig.vidWidth >> 1;

    cgi.R_DrawStretchPic(0.0, 0.0, iHalfWidth, iHalfHeight, 0.0, 0.0, 1.0, 1.0, handle);
    cgi.R_DrawStretchPic(iHalfWidth, 0.0, iHalfWidth, iHalfHeight, 1.0, 0.0, 0.0, 1.0, handle);
    cgi.R_DrawStretchPic(0.0, iHalfHeight, iHalfWidth, iHalfHeight, 0.0, 1.0, 1.0, 0.0, handle);
    cgi.R_DrawStretchPic(iHalfWidth, iHalfHeight, iHalfWidth, iHalfHeight, 1.0, 1.0, 0.0, 0.0, handle);
}

void CG_DrawZoomOverlay()
{
    static int   zoomType;
    static float fAlpha;
    const char  *weaponstring;
    qboolean     bDrawOverlay;

    weaponstring = "";
    bDrawOverlay = qtrue;

    if (!cg.snap) {
        return;
    }

    if (cg.snap->ps.activeItems[1] >= 0) {
        weaponstring = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }

    if (!Q_stricmp(weaponstring, "Spy Camera")) {
        zoomType = 2;
    } else if (!Q_stricmp(weaponstring, "Binoculars") || !Q_stricmp(weaponstring, "Bombing Run")) {
        // HZM coop: the "Bombing Run" weapon is a binoculars reskin - use the
        // full-screen binocular overlay instead of the generic sniper scope.
        zoomType = 3;
    } else {
        if (cg.snap->ps.stats[STAT_INZOOM] && cg.snap->ps.stats[STAT_INZOOM] <= 30) {
            if (!Q_stricmp(weaponstring, "KAR98 - Sniper")) {
                zoomType = 1;
            } else {
                zoomType = 0;
            }
        } else {
            bDrawOverlay = qfalse;
        }
    }

    if (bDrawOverlay) {
        fAlpha += cg.frametime * 0.015;
        if (fAlpha > 1.0) {
            fAlpha = 1.0;
        }
    } else {
        fAlpha -= cg.frametime * 0.015;
        if (fAlpha < 0.0) {
            fAlpha = 0.0;
        }

        if (!fAlpha) {
            return;
        }
    }

    switch (zoomType) {
    case 1:
        CG_DrawOverlayTopBottom(cgs.media.kar98TopOverlayShader, cgs.media.kar98BottomOverlayShader, fAlpha);
        break;
    case 3:
        CG_DrawOverlayFullScreen(cgs.media.binocularsOverlayShader, fAlpha);
        break;
    default:
        CG_DrawOverlayMiddle(cgs.media.zoomOverlayShader, fAlpha);
        break;
    }
}

void CG_HudDrawShader(int iInfo)
{
    if (cgi.HudDrawElements[iInfo].shaderName[0]) {
        cgi.HudDrawElements[iInfo].hShader = cgi.R_RegisterShaderNoMip(cgi.HudDrawElements[iInfo].shaderName);
    } else {
        cgi.HudDrawElements[iInfo].hShader = 0;
    }
}

void CG_HudDrawFont(int iInfo)
{
    if (cgi.HudDrawElements[iInfo].fontName[0]) {
        cgi.HudDrawElements[iInfo].pFont = cgi.R_LoadFont(cgi.HudDrawElements[iInfo].fontName);
    } else {
        cgi.HudDrawElements[iInfo].pFont = nullptr;
    }
}

void CG_RefreshHudDrawElements()
{
    int i;

    for (i = 0; i < MAX_HUDDRAW_ELEMENTS; ++i) {
        CG_HudDrawShader(i);
        CG_HudDrawFont(i);
    }
}

void CG_HudDrawElements()
{
    int    i;
    float  fX, fY;
    float  fWidth, fHeight;
    vec2_t virtualScale;

    if (!cg_huddraw_force->integer && !cg_hud->integer) {
        return;
    }

    virtualScale[0] = cgs.glconfig.vidWidth / 640.0;
    virtualScale[1] = cgs.glconfig.vidHeight / 480.0;

    for (i = 0; i < MAX_HUDDRAW_ELEMENTS; i++) {
        if ((!cgi.HudDrawElements[i].hShader && !cgi.HudDrawElements[i].string[0])
            || !cgi.HudDrawElements[i].vColor[3]) {
            // skip invisible elements
            continue;
        }

        fX      = cgi.HudDrawElements[i].iX;
        fY      = cgi.HudDrawElements[i].iY;
        fWidth  = cgi.HudDrawElements[i].iWidth;
        fHeight = cgi.HudDrawElements[i].iHeight;

        if (!cgi.HudDrawElements[i].bVirtualScreen) {
            fWidth *= cgs.uiHiResScale[0];
            fHeight *= cgs.uiHiResScale[1];
            fX *= cgs.uiHiResScale[0];
            fY *= cgs.uiHiResScale[1];
        }

        if (cgi.HudDrawElements[i].iHorizontalAlign == HUD_ALIGN_X_CENTER) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fX += 320.0 - fWidth * 0.5;
            } else {
                fX += cgs.glconfig.vidWidth * 0.5 - fWidth * 0.5;
            }
        } else if (cgi.HudDrawElements[i].iHorizontalAlign == HUD_ALIGN_X_RIGHT) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fX += 640.0;
            } else {
                fX += cgs.glconfig.vidWidth;
            }
        }

        if (cgi.HudDrawElements[i].iVerticalAlign == HUD_ALIGN_Y_CENTER) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fY += 240.0 - fHeight * 0.5;
            } else {
                fY += cgs.glconfig.vidHeight * 0.5 - fHeight * 0.5;
            }
        } else if (cgi.HudDrawElements[i].iVerticalAlign == HUD_ALIGN_Y_BOTTOM) {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                fY += 480.0;
            } else {
                fY += cgs.glconfig.vidHeight;
            }
        }

        // HZM coop - HUD fade: scripted huddraw chrome (score, reward icons, counters) follows the
        // same activity-driven fade as the health/ammo panels (compass exempt - it is not huddraw).
        // Slots >= 100 are PERSISTENT lobby UI (the bottom-left roster + the top-left control help) and
        // must stay fully visible - they are exempt from the fade.
        {
            vec4_t vFadedCol;
            Vector4Copy(cgi.HudDrawElements[i].vColor, vFadedCol);
            if (i < 100) {
                vFadedCol[3] *= s_hudFadeAlpha;
            }
            cgi.R_SetColor(vFadedCol);
        }
        if (cgi.HudDrawElements[i].string[0]) {
            fontheader_t *pFont = cgi.HudDrawElements[i].pFont;
            if (!pFont) {
                pFont = cgs.media.hudDrawFont;
            }

            if (cgi.HudDrawElements[i].bVirtualScreen) {
                cgi.R_DrawString(pFont, cgi.LV_ConvertString(cgi.HudDrawElements[i].string), fX, fY, -1, virtualScale);
            } else {
                cgi.R_DrawString(
                    pFont,
                    cgi.LV_ConvertString(cgi.HudDrawElements[i].string),
                    fX / cgs.uiHiResScale[0],
                    fY / cgs.uiHiResScale[1],
                    -1,
                    cgs.uiHiResScale
                );
            }
        } else {
            if (cgi.HudDrawElements[i].bVirtualScreen) {
                CG_AdjustFrom640(&fX, &fY, &fWidth, &fHeight);
            }

            cgi.R_DrawStretchPic(fX, fY, fWidth, fHeight, 0.0, 0.0, 1.0, 1.0, cgi.HudDrawElements[i].hShader);
        }
    }
}

void CG_InitializeObjectives()
{
    int i;

    cg.ObjectivesAlphaTime    = 0.0;
    cg.ObjectivesBaseAlpha    = 0.0;
    cg.ObjectivesDesiredAlpha = 0.0;
    cg.ObjectivesCurrentAlpha = 0.0;

    for (i = 0; i < MAX_OBJECTIVES; i++) {
        cg.Objectives[i].flags   = 0;
        cg.Objectives[i].text[0] = 0;
    }
}

void CG_DrawObjectives()
{
    float        vColor[4];
    float        fX, fY;
    int          iNumObjectives;
    float        fObjectivesTop;
    static float fWidth;
    float        fHeight;
    int          iNumLines[20];
    int          iTotalNumLines;
    int          i;
    int          iCurrentObjective;
    float        fTimeDelta;
    const char  *pszLocalizedText;
    const char  *pszLine;

    iTotalNumLines = 0;
    for (i = CS_OBJECTIVES; i < CS_OBJECTIVES + MAX_OBJECTIVES; ++i) {
        CG_ProcessConfigString(i, qfalse);
    }

    iCurrentObjective         = atoi(CG_ConfigString(CS_CURRENT_OBJECTIVE));
    fTimeDelta                = cg.ObjectivesAlphaTime - cg.time;
    cg.ObjectivesCurrentAlpha = cg.ObjectivesDesiredAlpha;
    if (fTimeDelta > 0) {
        cg.ObjectivesCurrentAlpha =
            (cg.ObjectivesBaseAlpha - cg.ObjectivesDesiredAlpha) * sin(fTimeDelta / (M_PI * 50.f + 2.f))
            + cg.ObjectivesDesiredAlpha;
    }

    if (cg.ObjectivesCurrentAlpha < 0.02) {
        return;
    }

    // Added in 2.0
    //  Get the minimum Y value, it should be below the compass
    fObjectivesTop = cgi.UI_GetObjectivesTop();
    iNumObjectives = 0;

    for (i = 0; i < MAX_OBJECTIVES; i++) {
        if ((cg.Objectives[i].flags == OBJ_FLAG_NONE) || (cg.Objectives[i].flags & OBJ_FLAG_HIDDEN)) {
            continue;
        }

        iNumObjectives++;
        iNumLines[i]     = 0;
        pszLocalizedText = cgi.LV_ConvertString(cg.Objectives[i].text);

        for (pszLine = strchr(pszLocalizedText, '\n'); pszLine; pszLine = strchr(pszLine + 1, '\n')) {
            iNumLines[i]++;
        }

        iTotalNumLines += iNumLines[i];
    }

    fX        = 25.0;
    fY        = fObjectivesTop + 5;
    fWidth    = (float)(iTotalNumLines * 12 + fObjectivesTop + iNumObjectives * 25 + 32) - fY;
    vColor[2] = 0.2f;
    vColor[1] = 0.2f;
    vColor[0] = 0.2f;
    vColor[3] = cg.ObjectivesCurrentAlpha * 0.75;
    cgi.R_SetColor(vColor);
    cgi.R_DrawStretchPic(
        fX,
        fY,
        450.0 * cgs.uiHiResScale[0],
        fWidth * cgs.uiHiResScale[1],
        0.0,
        0.0,
        1.0,
        1.0,
        cgs.media.objectivesBackShader
    );

    fX        = 30.0;
    fY        = fObjectivesTop + 10;
    vColor[0] = 1.0;
    vColor[1] = 1.0;
    vColor[2] = 1.0;
    vColor[3] = cg.ObjectivesCurrentAlpha;
    cgi.R_SetColor(vColor);
    cgi.R_DrawString(
        cgs.media.objectiveFont,
        cgi.LV_ConvertString("Mission Objectives:"),
        fX,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    fY = fY + 5.0;

    cgi.R_DrawString(
        cgs.media.objectiveFont,
        "_______________________________________________________",
        fX,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    fHeight = fObjectivesTop + 35 * cgs.uiHiResScale[1];

    for (i = 0; i < MAX_OBJECTIVES; ++i) {
        qhandle_t hBoxShader;

        if ((cg.Objectives[i].flags == OBJ_FLAG_NONE) || (cg.Objectives[i].flags & OBJ_FLAG_HIDDEN)) {
            continue;
        }

        if ((cg.Objectives[i].flags & OBJ_FLAG_CURRENT) != 0) {
            vColor[0]  = 1.0;
            vColor[1]  = 1.0;
            vColor[2]  = 1.0;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.uncheckedBoxShader;
        } else if ((cg.Objectives[i].flags & OBJ_FLAG_COMPLETED) != 0) {
            vColor[0]  = 0.75;
            vColor[1]  = 0.75;
            vColor[2]  = 0.75;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.checkedBoxShader;
        } else {
            vColor[0]  = 1.0;
            vColor[1]  = 1.0;
            vColor[2]  = 1.0;
            vColor[3]  = cg.ObjectivesCurrentAlpha;
            hBoxShader = cgs.media.uncheckedBoxShader;
        }
        if (i == iCurrentObjective && !(cg.Objectives[i].flags & OBJ_FLAG_COMPLETED)) {
            vColor[0] = 1.0;
            vColor[1] = 1.0;
            vColor[2] = 0.0;
            vColor[3] = cg.ObjectivesCurrentAlpha;
        }

        cgi.R_SetColor(vColor);
        fX = 55.0;
        fY = fHeight;
        cgi.R_DrawString(
            cgs.media.objectiveFont,
            cgi.LV_ConvertString(cg.Objectives[i].text),
            55.0,
            fY / cgs.uiHiResScale[1],
            -1,
            cgs.uiHiResScale
        );

        fX        = 30.0;
        fY        = fHeight;
        vColor[0] = 1.0;
        vColor[1] = 1.0;
        vColor[2] = 1.0;
        vColor[3] = cg.ObjectivesCurrentAlpha;
        cgi.R_SetColor(vColor);
        cgi.R_DrawStretchPic(
            fX * cgs.uiHiResScale[0],
            fY,
            16.0 * cgs.uiHiResScale[0],
            16.0 * cgs.uiHiResScale[1],
            0.0,
            0.0,
            1.0,
            1.0,
            hBoxShader
        );

        fHeight += iNumLines[i] * 12 + 25 * cgs.uiHiResScale[1];
    }
}

void CG_DrawPlayerTeam()
{
    qhandle_t handle;
    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cgs.gametype <= GT_FFA) {
        return;
    }

    handle = 0;
    if (cg.snap->ps.stats[STAT_TEAM] == 3) {
        handle = cgi.R_RegisterShader("textures/hud/allies");
    } else if (cg.snap->ps.stats[STAT_TEAM] == 4) {
        handle = cgi.R_RegisterShader("textures/hud/axis");
    }

    if (handle) {
        // HZM coop - HUD fade: the team logo is persistent chrome, follow the fade
        vec4_t vTeamCol = {1.0f, 1.0f, 1.0f, 1.0f};
        vTeamCol[3] = s_hudFadeAlpha;
        if (vTeamCol[3] <= 0.02f) {
            return;
        }
        cgi.R_SetColor(vTeamCol);
        cgi.R_DrawStretchPic(
            96.0 * cgs.uiHiResScale[0],
            cgs.glconfig.vidHeight - 46 * cgs.uiHiResScale[1],
            24.0 * cgs.uiHiResScale[0],
            24.0 * cgs.uiHiResScale[1],
            0.0,
            0.0,
            1.0,
            1.0,
            handle
        );
    }
}

void CG_DrawPlayerEntInfo()
{
    int         iClientNum;
    const char *pszClientInfo;
    const char *pszName;
    float       fX, fY;
    float       color[4];
    qhandle_t   handle;

    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cg.snap->ps.stats[STAT_INFOCLIENT] == -1) {
        return;
    }

    iClientNum    = cg.snap->ps.stats[STAT_INFOCLIENT];
    handle        = 0;
    pszClientInfo = CG_ConfigString(iClientNum + CS_PLAYERS);
    pszName       = Info_ValueForKey(pszClientInfo, "name");

    color[0] = 0.5;
    color[1] = 1.0;
    color[2] = 0.5;
    color[3] = 1.0;

    fX = 56.0;
    fY = (float)cgs.glconfig.vidHeight * 0.5;

    if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
        handle = cgi.R_RegisterShader("textures/hud/allies");
    } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
        handle = cgi.R_RegisterShader("textures/hud/axis");
    }

    if (handle) {
        cgi.R_SetColor(0);
        cgi.R_DrawStretchPic(
            fX, fY, 16.0 * cgs.uiHiResScale[0], 16.0 * cgs.uiHiResScale[1], 0.0, 0.0, 1.0, 1.0, handle
        );
    }

    cgi.R_SetColor(color);
    cgi.R_DrawString(
        cgs.media.hudDrawFont,
        (char *)pszName,
        fX / cgs.uiHiResScale[0] + 24.0,
        fY / cgs.uiHiResScale[1],
        -1,
        cgs.uiHiResScale
    );
    cgi.R_DrawString(
        cgs.media.hudDrawFont,
        va("%i", cg.snap->ps.stats[STAT_INFOCLIENT_HEALTH]),
        fX / cgs.uiHiResScale[0] + 24.0,
        fY / cgs.uiHiResScale[1] + 20.0,
        -1,
        cgs.uiHiResScale
    );
}

void CG_UpdateAttackerDisplay()
{
    int         iClientNum;
    const char *pszClientInfo;
    const char *pszName;
    float       fX, fY;
    float       color[4];

    if (!cg_hud->integer) {
        return;
    }

    if (!cg.snap || cg.snap->ps.stats[STAT_ATTACKERCLIENT] == -1) {
        return;
    }

    iClientNum    = cg.snap->ps.stats[STAT_ATTACKERCLIENT];
    pszClientInfo = CG_ConfigString(CS_PLAYERS + iClientNum);
    pszName       = Info_ValueForKey(pszClientInfo, "name");

    color[3] = 1.0;
    fY       = (float)(cgs.glconfig.vidHeight - 90);
    fX       = 56.0;

    if (cgs.gametype > GT_FFA) {
        qhandle_t handle;

        handle = 0;
        if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
            handle = cgi.R_RegisterShader("textures/hud/allies");
        } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
            handle = cgi.R_RegisterShader("textures/hud/axis");
        }

        if (handle) {
            cgi.R_SetColor(0);
            cgi.R_DrawStretchPic(
                56.0 * cgs.uiHiResScale[0],
                fY,
                24.0 * cgs.uiHiResScale[0],
                24.0 * cgs.uiHiResScale[1],
                0.0,
                0.0,
                1.0,
                1.0,
                handle
            );
        }

        if ((cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS)
            && cg.clientinfo[iClientNum].team == cg.snap->ps.stats[STAT_TEAM]) {
            color[0] = 0.5;
            color[1] = 1.0;
            color[2] = 0.5;
        } else {
            color[0] = 1.0;
            color[1] = 0.5;
            color[2] = 0.5;
        }

        fX = 56.0;
    } else {
        color[0] = 1.0;
        color[1] = 0.5;
        color[2] = 0.5;
    }

    cgi.R_SetColor(color);
    cgi.R_DrawString(
        cgs.media.attackerFont, pszName, fX / cgs.uiHiResScale[0] + 32.0, fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );
}

void CG_UpdateCountdown()
{
    const char *message = "";

    if (!cg.snap) {
        return;
    }

    if (cg.matchStartTime != -1) {
        int iSecondsLeft, iMinutesLeft;

        iSecondsLeft = (cgs.matchEndTime - cg.time) / 1000;
        if (iSecondsLeft >= 0) {
            iMinutesLeft = iSecondsLeft / 60;
            message      = va("%s %2i:%02i", cgi.LV_ConvertString("Time Left:"), iMinutesLeft, iSecondsLeft % 60);
        } else if (!cgs.matchEndTime) {
            message = "";
        }
    } else {
        // The match has not started yet
        message = "Waiting For Players";
    }

    if (strcmp(ui_timemessage->string, message)) {
        cgi.Cvar_Set("ui_timemessage", message);
    }
}

static void CG_RemoveStopwatch()
{
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_stopwatch\n");
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse\n");
    cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse_wet\n");
}

void CG_DrawStopwatch()
{
    int iFraction;

    if (!cg_hud->integer) {
        CG_RemoveStopwatch();
        return;
    }

    if (!cgi.stopWatch->iStartTime) {
        CG_RemoveStopwatch();
        return;
    }

    if (cgi.stopWatch->iStartTime >= cgi.stopWatch->iEndTime) {
        CG_RemoveStopwatch();
        return;
    }

    if (cgi.stopWatch->iEndTime <= cg.time) {
        CG_RemoveStopwatch();
        return;
    }

    if (cg.ObjectivesCurrentAlpha >= 0.02) {
        CG_RemoveStopwatch();
        return;
    }

    if (cg.snap && cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        CG_RemoveStopwatch();
        return;
    }
    if (cgi.stopWatch->eType >= SWT_FUSE_WET) {
        iFraction = cgi.stopWatch->iEndTime - cgi.stopWatch->iStartTime;
    } else {
        iFraction = cgi.stopWatch->iEndTime - cg.time;
    }

    cgi.Cvar_Set("ui_stopwatch", va("%i", iFraction));

    switch (cgi.stopWatch->eType) {
    case SWT_NORMAL:
    default:
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_stopwatch\n");
        break;
    case SWT_FUSE:
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_fuse\n");
        break;
    case SWT_FUSE_WET:
        cgi.Cmd_Execute(EXEC_NOW, "ui_removehud hud_fuse\n");
        cgi.Cmd_Execute(EXEC_NOW, "ui_addhud hud_fuse_wet\n");
        break;
    }
}

void CG_DrawInstantMessageMenu()
{
    float     w, h;
    float     x, y;
    qhandle_t handle;

    if (!cg.iInstaMessageMenu) {
        return;
    }

    if (cg.iInstaMessageMenu > 0) {
        handle = cgi.R_RegisterShader(va("textures/hud/instamsg_group_%c", cg.iInstaMessageMenu + 96));
    } else {
        handle = cgi.R_RegisterShader("textures/hud/instamsg_main");
    }

    w = cgi.R_GetShaderWidth(handle);
    h = cgi.R_GetShaderHeight(handle);
    x = 8.0;
    y = ((float)cgs.glconfig.vidHeight - h) * 0.5;
    cgi.R_SetColor(0);
    cgi.R_DrawStretchPic(
        x * cgs.uiHiResScale[0], y, w * cgs.uiHiResScale[0], h * cgs.uiHiResScale[1], 0.0, 0.0, 1.0, 1.0, handle
    );
}

void CG_DrawSpectatorView_ver_15()
{
    const char *pszString;
    int         iKey1, iKey2;
    int         iKey1b, iKey2b;
    float       fX, fY;
    qboolean    bOnTeam;

    if (!(cg.predicted_player_state.pm_flags & PMF_SPECTATING)) {
        return;
    }

    bOnTeam = qfalse;
    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
        bOnTeam = 1;
    }

    if (!bOnTeam) {
        cgi.Key_GetKeysForCommand("+attackprimary", &iKey1, &iKey2);
        pszString = cgi.LV_ConvertString(va("Press Fire(%s) to join the battle!", cgi.Key_KeynumToBindString(iKey1)));
        fX        = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = cgs.glconfig.vidHeight - 64.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(NULL);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) {
        cgi.Key_GetKeysForCommand("+moveup", &iKey1, &iKey2);
        cgi.Key_GetKeysForCommand("+movedown", &iKey1b, &iKey2b);

        pszString = cgi.LV_ConvertString(
            va("Press Jump(%s) or Duck(%s) to follow a different player.",
               cgi.Key_KeynumToBindString(iKey1),
               cgi.Key_KeynumToBindString(iKey1b))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 40.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (!bOnTeam && (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);
        pszString =
            cgi.LV_ConvertString(va("Press Use(%s) to enter free spectate mode.", cgi.Key_KeynumToBindString(iKey1)));

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);

        pszString = cgi.LV_ConvertString(
            va("Press Use(%s) to enter player following spectate mode.", cgi.Key_KeynumToBindString(iKey1))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }

    if ((cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) != 0 && cg.snap
        && cg.snap->ps.stats[STAT_INFOCLIENT] != -1) {
        int       iClientNum;
        qhandle_t hShader;
        vec4_t    color;
        char      buf[128];

        iClientNum = cg.snap->ps.stats[STAT_INFOCLIENT];
        Com_sprintf(
            buf, sizeof(buf), "%s : %i", cg.clientinfo[iClientNum].name, cg.snap->ps.stats[STAT_INFOCLIENT_HEALTH]
        );

        hShader  = 0;
        color[0] = 0.5;
        color[1] = 1.0;
        color[2] = 0.5;
        color[3] = 1.0;

        fX = (float)(cgs.glconfig.vidWidth
                     - (cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) - 16) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 80.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(color);
        cgi.R_DrawString(
            cgs.media.attackerFont, buf, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );

        if (cg.clientinfo[iClientNum].team == TEAM_ALLIES) {
            hShader = cgi.R_RegisterShader("textures/hud/allies");
        } else if (cg.clientinfo[iClientNum].team == TEAM_AXIS) {
            hShader = cgi.R_RegisterShader("textures/hud/axis");
        }

        if (hShader) {
            cgi.R_SetColor(NULL);
            cgi.R_DrawStretchPic(
                fX - 20.0 * cgs.uiHiResScale[0],
                fY,
                16.0 * cgs.uiHiResScale[0],
                16.0 * cgs.uiHiResScale[1],
                0.0,
                0.0,
                1.0,
                1.0,
                hShader
            );
        }
    }
}

void CG_DrawSpectatorView_ver_6()
{
    const char *pszString;
    int         iKey1, iKey2;
    int         iKey1b, iKey2b;
    float       fX, fY;
    qboolean    bOnTeam;

    if (!(cg.predicted_player_state.pm_flags & PMF_SPECTATING)) {
        return;
    }

    bOnTeam = qfalse;
    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
        bOnTeam = 1;
    }

    // retrieve keys for +use
    cgi.Key_GetKeysForCommand("+use", &iKey1, &iKey2);

    if (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW) {
        pszString =
            cgi.LV_ConvertString(va("Press Use(%s) to follow a different player.", cgi.Key_KeynumToBindString(iKey1)));
    } else {
        pszString = cgi.LV_ConvertString(va("Press Use(%s) to follow a player.", cgi.Key_KeynumToBindString(iKey1)));
    }

    fX = (float)(cgs.glconfig.vidWidth
                 - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
       * 0.5;
    fY = (float)cgs.glconfig.vidHeight - 40.0 * cgs.uiHiResScale[1];
    cgi.R_SetColor(0);
    cgi.R_DrawString(
        cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    if (!bOnTeam && (cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
        cgi.Key_GetKeysForCommand("+moveup", &iKey1, &iKey2);
        cgi.Key_GetKeysForCommand("+movedown", &iKey1b, &iKey2b);
        pszString = cgi.LV_ConvertString(
            va("Press Jump(%s) or Duck(%s) to free spectate.",
               cgi.Key_KeynumToBindString(iKey1),
               cgi.Key_KeynumToBindString(iKey1b))
        );

        fX = (float)(cgs.glconfig.vidWidth
                     - cgi.UI_FontStringWidth(cgs.media.attackerFont, pszString, -1) * cgs.uiHiResScale[0])
           * 0.5;
        fY = (float)cgs.glconfig.vidHeight - 24.0 * cgs.uiHiResScale[1];
        cgi.R_SetColor(0);
        cgi.R_DrawString(
            cgs.media.attackerFont, pszString, fX / cgs.uiHiResScale[0], fY / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
    }
}

void CG_DrawSpectatorView()
{
    if (cg_protocol >= PROTOCOL_MOHTA_MIN) {
        CG_DrawSpectatorView_ver_15();
    } else {
        CG_DrawSpectatorView_ver_6();
    }
}

void CG_DrawCrosshair()
{
    centity_t *friendEnt;
    qhandle_t  shader;
    vec3_t     forward;
    vec3_t     end;
    vec3_t     mins, maxs;
    trace_t    trace;
    float      x, y;
    float      width, height;

    shader = (qhandle_t)0;

    if (!cg_hud->integer || !ui_crosshair->integer) {
        return;
    }

    if (!cg.snap) {
        return;
    }

    // HZM coop [237] - the free-cam capture-hide that lived here was REMOVED: the user wants the
    // crosshair IN free cam too. The 3P true-aim projection below draws it where the gun actually
    // points (not screen center), so it stays truthful while the camera orbits freely.

    // HZM coop - hide the crosshair while aiming down sights (ADS button held) so the iron sights are used.
    // Sniper scopes already hide it via STAT_INZOOM below; this covers the iron-sight ADS (not scoped).
    // STAGED 3P ADS: while the camera is still THIRD person (over-the-shoulder aim stage) the crosshair
    // is the aiming reference, so keep it - hide only once the view is actually first-person irons.
    {
        usercmd_t adsCmd;
        cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &adsCmd);
        if ((adsCmd.buttons & BUTTON_COOPADS) && cg.snap->ps.stats[STAT_HEALTH] > 0
            && !cg.snap->ps.stats[STAT_INZOOM] && !cg.renderingThirdPerson) {
            return;
        }
    }

    if ((cg.snap->ps.pm_flags & PMF_NO_HUD) || (cg.snap->ps.pm_flags & PMF_INTERMISSION)) {
        return;
    }

    if (!cg.snap->ps.stats[STAT_CROSSHAIR]
        && (!cg.snap->ps.stats[STAT_INZOOM] || cg.snap->ps.stats[STAT_INZOOM] > 30)) {
        return;
    }

    // Fixed in OPM: R_RegisterShaderNoMip
    //  Use R_RegisterShaderNoMip, as it's UI stuff

    if (cgs.gametype != GT_FFA) {
        AngleVectorsLeft(cg.refdefViewAngles, forward, NULL, NULL);

        VectorMA(cg.refdef.vieworg, 8192, forward, end);
        VectorClear(mins);
        VectorClear(maxs);

        CG_Trace(&trace, cg.refdef.vieworg, mins, maxs, end, 9999, MASK_SOLID, qfalse, qtrue, "CG_DrawCrosshair");

        // ENTITYNUM_WORLD check added in OPM
        if ((trace.entityNum != ENTITYNUM_NONE && trace.entityNum != ENTITYNUM_WORLD)
            && trace.entityNum != cg.snap->ps.clientNum) {
            int myFlags;

            friendEnt = &cg_entities[trace.entityNum];
            if (cgs.gametype != GT_SINGLE_PLAYER) {
                myFlags = cg_entities[cg.snap->ps.clientNum].currentState.eFlags & EF_ANY_TEAM;
            } else {
                // the player will always be considered as an allied
                // in single-player
                myFlags = EF_ALLIES;
            }

            if (((myFlags & EF_ALLIES) && (friendEnt->currentState.eFlags & EF_ALLIES))
                || ((myFlags & EF_AXIS) && (friendEnt->currentState.eFlags & EF_AXIS))) {
                // friend
                if (cg.snap->ps.stats[STAT_CROSSHAIR]) {
                    shader = cgi.R_RegisterShaderNoMip(cg_crosshair_friend->string);
                }
            } else {
                // enemy
                if (cg.snap->ps.stats[STAT_CROSSHAIR]) {
                    shader = cgi.R_RegisterShaderNoMip(cg_crosshair->string);
                }
            }
        } else {
            if (cg.snap->ps.stats[STAT_CROSSHAIR]) {
                shader = cgi.R_RegisterShaderNoMip(cg_crosshair->string);
            }
        }
    } else {
        // FFA
        if (cg.snap->ps.stats[STAT_CROSSHAIR]) {
            shader = cgi.R_RegisterShaderNoMip(cg_crosshair->string);
        }
    }

    if (shader) {
        qboolean bProjected = qfalse;

        width  = cgi.R_GetShaderWidth(shader);
        height = cgi.R_GetShaderHeight(shader);

        // [user 07-11] CROSSHAIR SIZE slider (Options -> Advanced): scale the crosshair art. 1 = native
        // size. Applied here (before centring + the 3P true-aim projection + the draw) so every downstream
        // placement uses the scaled dimensions and the crosshair stays centred on the true aim point.
        {
            static cvar_t *pChSize = NULL;
            if (!pChSize) { pChSize = cgi.Cvar_Get("cg_crosshairSize", "1", CVAR_ARCHIVE); }
            if (pChSize->value > 0.05f) {
                width  *= pChSize->value;
                height *= pChSize->value;
            }
        }

        x      = (cgs.glconfig.vidWidth - width) * 0.5f;
        y      = (cgs.glconfig.vidHeight - height) * 0.5f;

        // HZM coop - THIRD-PERSON TRUE AIM: in 3P the camera sits behind/beside the shoulder, so the
        // screen-centre crosshair marks the CAMERA ray, not the bullet ray (bullets leave the EYE along
        // ps->viewangles). Trace the actual bullet ray and re-project its impact point through the
        // offset camera, so the crosshair sits exactly where the shot will land at any range.
        {
            static cvar_t *p3p = NULL;
            if (!p3p) { p3p = cgi.Cvar_Get("cg_crosshair3p", "1", CVAR_ARCHIVE); }
            if (p3p->integer && cg.renderingThirdPerson) {
                vec3_t  vEye, vFwd, vAimEnd, vDir;
                trace_t trAim;
                float   f, r, u, tanx, tany;

                VectorCopy(cg.predicted_player_state.origin, vEye);
                vEye[2] += cg.predicted_player_state.viewheight;

                // HZM coop [user 2026-08-07] IN COVER, trace from the MUZZLE, not the eye.
                // Weapon::GetMuzzlePosition raises the blind-fire origin by coop_blindfireRaise over
                // low cover (the gun is held overhead), so rounds leave from higher than the eye and
                // the crosshair was pointing somewhere they never went - the player had to tilt up to
                // compensate. Reading the SAME cvar the server fires from means the two cannot drift:
                // change the muzzle raise and the crosshair follows it automatically.
                // Vertical only. Wall blind-fire also swings the DIRECTION by coop_blindfireYaw (50
                // degrees around the corner); matching that needs the cover side and normal, which
                // the client does not have, so it is deliberately left alone.
                {
                    static cvar_t *pCoverV = NULL;
                    static cvar_t *pBfRaise = NULL;
                    if (!pCoverV)  { pCoverV  = cgi.Cvar_Get("coop_coverView",       "0",  0); }
                    if (!pBfRaise) { pBfRaise = cgi.Cvar_Get("coop_blindfireRaise", "26", CVAR_ARCHIVE); }
                    if (pCoverV->integer) {
                        vEye[2] += pBfRaise->value;
                    }
                }
                AngleVectorsLeft(cg.predicted_player_state.viewangles, vFwd, NULL, NULL);
                VectorMA(vEye, 8192, vFwd, vAimEnd);
                CG_Trace(
                    &trAim, vEye, vec3_origin, vec3_origin, vAimEnd, cg.snap->ps.clientNum, MASK_SHOT,
                    qfalse, qtrue, "CG_DrawCrosshair3P"
                );

                VectorSubtract(trAim.endpos, cg.refdef.vieworg, vDir);
                f = DotProduct(vDir, cg.refdef.viewaxis[0]);
                if (f > 1.0f) { // impact in front of the camera; else keep the centre crosshair
                    r    = DotProduct(vDir, cg.refdef.viewaxis[1]); // viewaxis[1] = LEFT
                    u    = DotProduct(vDir, cg.refdef.viewaxis[2]);
                    tanx = tan(DEG2RAD(((cg.refdef.fov_x > 1.0f) ? cg.refdef.fov_x : 90.0f) * 0.5f));
                    tany = tan(DEG2RAD(((cg.refdef.fov_y > 1.0f) ? cg.refdef.fov_y : 73.0f) * 0.5f));
                    x    = (cgs.glconfig.vidWidth  * 0.5f) * (1.0f - (r / f) / tanx) - width * 0.5f;
                    y    = (cgs.glconfig.vidHeight * 0.5f) * (1.0f - (u / f) / tany) - height * 0.5f;
                    bProjected = qtrue;
                }
            }
        }

        // HZM coop - FREE-AIM: move the crosshair to the actual aim point (offset from screen centre by the
        // deadzone offset) so it marks where bullets go, not the camera centre. (First-person mechanism;
        // skipped when the 3P projection above already placed the crosshair.)
        if (!bProjected) {
            float faYaw, faPitch;
            if (CG_GetFreeAim(&faYaw, &faPitch)) {
                float fovx = (cg.refdef.fov_x > 1.0f) ? cg.refdef.fov_x : 90.0f;
                float fovy = (cg.refdef.fov_y > 1.0f) ? cg.refdef.fov_y : 73.0f;
                x -= (cgs.glconfig.vidWidth  * 0.5f) * (tan(DEG2RAD(faYaw))   / tan(DEG2RAD(fovx * 0.5f)));
                y += (cgs.glconfig.vidHeight * 0.5f) * (tan(DEG2RAD(faPitch)) / tan(DEG2RAD(fovy * 0.5f)));
            }
        }

        cgi.R_SetColor(NULL);
        cgi.R_DrawStretchPic(x, y, width * cgs.uiHiResScale[0], height * cgs.uiHiResScale[1], 0, 0, 1, 1, shader);
    }
}

void CG_DrawVote()
{
    const char *text;
    int         seconds;
    int         percentYes;
    int         percentNo;
    int         percentUndecided;
    float       x, y;
    vec4_t      col;

    if (!cgs.voteTime) {
        return;
    }

    if (cgs.voteRefreshed) {
        cgs.voteRefreshed = qfalse;
    }

    seconds = (30000 - (cg.time - cgs.voteTime)) / 1000 + 1;
    if (seconds < 0) {
        seconds = 0;
    }

    percentYes       = cgs.numVotesYes * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);
    percentNo        = cgs.numVotesNo * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);
    percentUndecided = cgs.numUndecidedVotes * 100 / (cgs.numUndecidedVotes + cgs.numVotesNo + cgs.numVotesYes);

    x = 8 * cgs.uiHiResScale[0];
    y = ((cgs.glconfig.vidHeight > 480) ? (cgs.glconfig.vidHeight * 0.725f) : (cgs.glconfig.vidHeight * 0.75f));

    cgi.R_SetColor(NULL);

    text = va("%s: %s", cgi.LV_ConvertString("Vote Running"), cgs.voteString);
    cgi.R_DrawString(
        cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    y += 12 * cgs.uiHiResScale[1];

    text =
        va("%s: %isec  %s: %i%%  %s: %i%%  %s: %i%%",
           cgi.LV_ConvertString("Time"),
           seconds,
           cgi.LV_ConvertString("Yes"),
           percentYes,
           cgi.LV_ConvertString("No"),
           percentNo,
           cgi.LV_ConvertString("Undecided"),
           percentUndecided);
    cgi.R_DrawString(
        cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
    );

    if (cg.snap && !cg.snap->ps.voted) {
        col[0] = 0.5;
        col[1] = 1.0;
        col[2] = 0.5;
        col[3] = 1.0;
        cgi.R_SetColor(col);

        y += 12 * cgs.uiHiResScale[1];

        text = cgi.LV_ConvertString("Vote now, it's your patriotic duty!");
        cgi.R_DrawString(
            cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );

        y += 12 * cgs.uiHiResScale[1];

        text = cgi.LV_ConvertString("To vote Yes, press F1. To vote No, press F2.");
        cgi.R_DrawString(
            cgs.media.attackerFont, text, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale
        );
        cgi.R_SetColor(NULL);
    }
}

/*
==============
CG_Draw2D
==============
*/
/*
=================
CG_DrawMGHeat  (HZM coop)

Small RED heat meter for the mounted MG42 turret. Driven by STAT_MGHEAT (0..100), which the turret
fills as you fire and drains as it cools (see fgame/weapturret.cpp). Only shown while on a turret
(PMF_TURRET). Deliberately small - smaller than the usual coop meters.
=================
*/
static void CG_DrawMGHeat(void)
{
    int    heat;
    float  bw, bh, bx, by, fillw;
    vec4_t cBg   = {0.0f, 0.0f, 0.0f, 0.55f};
    vec4_t cFill = {0.85f, 0.10f, 0.08f, 0.90f};

    if (!cg.snap) {
        return;
    }
    if (!(cg.snap->ps.pm_flags & PMF_TURRET)) {
        return; // only while mounted on a turret
    }

    heat = cg.snap->ps.stats[STAT_MGHEAT];
    if (heat < 0) {
        heat = 0;
    } else if (heat > 100) {
        heat = 100;
    }

    // small bar, BOTTOM-RIGHT corner (out of the way of the firing lane)
    bw = 70.0f * cgs.uiHiResScale[0];
    bh = 5.0f * cgs.uiHiResScale[1];
    bx = cg.refdef.width - bw - 18.0f * cgs.uiHiResScale[0];
    by = cg.refdef.height - bh - 18.0f * cgs.uiHiResScale[1];

    // dark frame
    cgi.R_SetColor(cBg);
    cgi.R_DrawBox(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f);

    // red fill scaled by heat
    fillw = bw * ((float)heat / 100.0f);
    if (fillw > 0.0f) {
        cgi.R_SetColor(cFill);
        cgi.R_DrawBox(bx, by, fillw, bh);
    }

    cgi.R_SetColor(NULL);
}

// HZM coop - ADS TUNE seeding: entering tune mode normally snaps the gun to the un-tuned cvar defaults
// (tune mode reads the live cg_ads* cvars instead of the baked per-gun table). Seed those cvars with the
// held gun's BAKED values the first time you hold it in tune mode - and on every gun switch - so the gun
// stays exactly at its real (baked) position and you only nudge the small offset to centre it, instead of
// re-tuning from zero. Only re-seeds on gun change, so your live nudges are never clobbered mid-tune.
static void CG_SeedAdsTuneFromBaked(void)
{
    static int          sLastSeededWpn = -2;
    const adsGunTune_t *t;
    const char         *wpn;
    int                 wi;

    if (!cg_adsTune || !cg_adsTune->integer) {
        sLastSeededWpn = -2; // tune off: re-seed next time it's enabled
        return;
    }
    if (!cg.snap || cg.snap->ps.activeItems[1] < 0) {
        return;
    }
    wi = cg.snap->ps.activeItems[1];
    if (wi == sLastSeededWpn) {
        return; // already seeded this gun this session - keep the user's nudges
    }
    wpn            = CG_ConfigString(CS_WEAPONS + wi);
    t              = CG_FindAdsTune(wpn);
    sLastSeededWpn = wi;
    if (!t) {
        return; // no baked entry for this gun: leave the cvars at their fallback values
    }
    cgi.Cvar_Set("cg_adsPitch", va("%g", t->sPitch));
    cgi.Cvar_Set("cg_adsYaw", va("%g", t->sYaw));
    cgi.Cvar_Set("cg_adsRoll", va("%g", t->sRoll));
    cgi.Cvar_Set("cg_adsShiftX", va("%g", t->sShiftX));
    cgi.Cvar_Set("cg_adsShiftY", va("%g", t->sShiftY));
    cgi.Cvar_Set("cg_adsCrouchPitch", va("%g", t->cPitch));
    cgi.Cvar_Set("cg_adsCrouchYaw", va("%g", t->cYaw));
    cgi.Cvar_Set("cg_adsCrouchRoll", va("%g", t->cRoll));
    cgi.Cvar_Set("cg_adsCrouchShiftX", va("%g", t->cShiftX));
    cgi.Cvar_Set("cg_adsCrouchShiftY", va("%g", t->cShiftY));
}

// HZM coop - ADS tuning overlay: a bright centre crosshair (the bullet aim-point you align the sights to)
// plus a live readout of the held weapon and its tune values. Shown only while cg_adsTune is on.
static void CG_DrawAdsTune(void)
{
    float       cx, cy;
    vec4_t      col;
    char        txt[256];
    const char *wpn = "";

    if (!cg_adsTune || !cg_adsTune->integer) {
        return;
    }

    cx = cgs.glconfig.vidWidth * 0.5f;
    cy = cgs.glconfig.vidHeight * 0.5f;

    // HZM coop - SYMMETRY GUIDE LINES: a dim full-screen cross through the exact centre while tuning, so the
    // iron-sight picture can be judged for left/right (vertical line) and up/down (horizontal line) symmetry.
    // Kept faint so they don't fight the sight. cg_adsGuides 0 = off.
    {
        static cvar_t *adsGuides = NULL;
        if (!adsGuides) {
            adsGuides = cgi.Cvar_Get("cg_adsGuides", "1", CVAR_ARCHIVE);
        }
        if (adsGuides->integer) {
            vec4_t gcol;
            gcol[0] = 0.1f; gcol[1] = 1.0f; gcol[2] = 0.1f; gcol[3] = 0.28f;
            cgi.R_SetColor(gcol);
            cgi.R_DrawBox(cx, 0.0f, 1.0f, (float)cgs.glconfig.vidHeight); // vertical centre line
            cgi.R_DrawBox(0.0f, cy, (float)cgs.glconfig.vidWidth, 1.0f);  // horizontal centre line
        }
    }

    col[0] = 0.1f; col[1] = 1.0f; col[2] = 0.1f; col[3] = 1.0f;
    cgi.R_SetColor(col);
    cgi.R_DrawBox(cx - 6.0f, cy, 13.0f, 1.0f);
    cgi.R_DrawBox(cx, cy - 6.0f, 1.0f, 13.0f);

    if (cg.snap && cg.snap->ps.activeItems[1] >= 0) {
        wpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }
    {
        const char *modeName = (cg_adsMode && cg_adsMode->integer == 1) ? "YAW"
                             : (cg_adsMode && cg_adsMode->integer == 2) ? "SHIFT"
                             : (cg_adsMode && cg_adsMode->integer == 3) ? "ROLL"
                                                                        : "PITCH";
        qboolean    ducked   = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? qtrue : qfalse;
        Com_sprintf(
            txt,
            sizeof(txt),
            "ADS TUNE [%s] %s   MODE: %s   stand P%.1f Y%.1f R%.1f Sx%.2f Sy%.2f   crouch P%.1f Y%.1f R%.1f Sx%.2f Sy%.2f",
            wpn,
            ducked ? "CROUCH" : "STAND",
            modeName,
            cg_adsPitch->value,
            cg_adsYaw->value,
            cg_adsRoll ? cg_adsRoll->value : 0.f,
            cg_adsShiftX ? cg_adsShiftX->value : 0.f,
            cg_adsShiftY ? cg_adsShiftY->value : 0.f,
            cg_adsCrouchPitch->value,
            cg_adsCrouchYaw->value,
            cg_adsCrouchRoll->value,
            cg_adsCrouchShiftX ? cg_adsCrouchShiftX->value : 0.f,
            cg_adsCrouchShiftY ? cg_adsCrouchShiftY->value : 0.f
        );
    }
    col[0] = 1.0f; col[1] = 1.0f; col[2] = 0.3f; col[3] = 1.0f;
    cgi.R_SetColor(col);
    cgi.R_DrawString(cgs.media.objectiveFont, txt, 30.0f, 40.0f / cgs.uiHiResScale[1], -1, cgs.uiHiResScale);

    cgi.R_SetColor(NULL);
}

// HZM coop - ADS focus VIGNETTE: softly darken the screen edges while aiming, for a "focus on the sights"
// depth-of-field feel (renderergl1 has no true DoF). Fades in/out over ~0.15s. Texture is black with a
// radial alpha (clear centre -> soft-dark edges); drawn full-screen, alpha-blended.
static void CG_DrawAdsVignette(void)
{
    static qhandle_t hVig    = 0;
    static float     fAlpha  = 0.0f;
    static float     fBreath = 0.0f; // HZM coop - eased breath-hold "focus-in" boost (0..1)
    float            step, bstep;
    qboolean         bBreath;

    if (!hVig) {
        hVig = cgi.R_RegisterShaderNoMip("textures/hud/coop_ads_vignette");
    }
    if (!hVig) {
        return;
    }

    // HZM coop - the focus vignette/DoF applies in first-person irons AND the 3P over-the-shoulder aim
    // stage (user request 2026-07-06: shoulder aim gets the same base-ADS effects). In 3P we key off the
    // shoulder camera envelope so the effect engages with the camera ease; breath-hold deepening below
    // stays FP-only (there is no breath system in 3P).
    step = (cg.frametime > 0) ? ((float)cg.frametime / 150.0f) : 1.0f; // ~0.15s in/out
    if (CG_AimingDownSights() && (!cg.renderingThirdPerson || CG_AdsShoulderFrac() > 0.5f)) {
        fAlpha += step;
        if (fAlpha > 1.0f) { fAlpha = 1.0f; }
    } else {
        fAlpha -= step;
        if (fAlpha < 0.0f) { fAlpha = 0.0f; }
    }

    // HZM coop - breath-hold DEEPENS the ADS focus: ease a second value up while ACTIVELY steadying breath
    // (the same state that applies cg_breathZoom), and back down when the hold ends/recharges.
    bstep   = (cg.frametime > 0) ? ((float)cg.frametime / 250.0f) : 1.0f; // ~0.25s, a hair slower than the base
    bBreath = (CG_AimingDownSights() && !cg.renderingThirdPerson && CG_IsBreathSteady()) ? qtrue : qfalse;
    if (bBreath) {
        fBreath += bstep;
        if (fBreath > 1.0f) { fBreath = 1.0f; }
    } else {
        fBreath -= bstep;
        if (fBreath < 0.0f) { fBreath = 0.0f; }
    }

    // HZM coop - drive the renderer's DEPTH OF FIELD from this same eased ADS fade. Set every frame (0 when
    // not aiming) so the gl1 DoF pass (renderergl1 RB_DepthOfField) blurs the edges while you aim and is fully
    // disabled otherwise. cg_dofStrength scales it (0 = off). This is separate from the dark vignette below.
    {
        cvar_t *pDof = cgi.Cvar_Get("cg_dofStrength", "0.6", CVAR_ARCHIVE);
        float   fDof = pDof ? pDof->value : 0.6f;
        // breath-hold softens the world edges a touch MORE (up to +40% blur) to reinforce the focus.
        cgi.Cvar_Set("r_dofBlur", va("%g", (fAlpha + fBreath * 0.4f) * fDof));
    }

    if (fAlpha <= 0.0f) {
        return;
    }

    // draw ONCE full-screen (CG_DrawOverlayFullScreen mirrors the texture into 4 quadrants - meant for a
    // quarter-scope image - which tiled our full vignette into a dark frame). One quad = one big vignette.
    {
        vec4_t col;
        col[0] = 1.0f;
        col[1] = 1.0f;
        col[2] = 1.0f;
        col[3] = fAlpha;
        cgi.R_SetColor(col);
        cgi.R_DrawStretchPic(
            0.0f, 0.0f, (float)cgs.glconfig.vidWidth, (float)cgs.glconfig.vidHeight, 0.0f, 0.0f, 1.0f, 1.0f, hVig
        );
        cgi.R_SetColor(NULL);
    }

    // HZM coop - EXTRA darkening pass while holding breath: stacks a second vignette on top of the base for a
    // stronger "tunnel-in on the sights" focus. cg_adsBreathVignette scales the added darkness (0 = disable).
    if (fBreath > 0.0f) {
        cvar_t *pBVig = cgi.Cvar_Get("cg_adsBreathVignette", "0.55", CVAR_ARCHIVE);
        float   a     = fBreath * (pBVig ? pBVig->value : 0.55f);
        if (a > 0.0f) {
            vec4_t col;
            if (a > 1.0f) { a = 1.0f; }
            col[0] = 1.0f;
            col[1] = 1.0f;
            col[2] = 1.0f;
            col[3] = a;
            cgi.R_SetColor(col);
            cgi.R_DrawStretchPic(
                0.0f, 0.0f, (float)cgs.glconfig.vidWidth, (float)cgs.glconfig.vidHeight, 0.0f, 0.0f, 1.0f, 1.0f, hVig
            );
            cgi.R_SetColor(NULL);
        }
    }
}

// HZM coop - clean MAGAZINE counter (replaces the stock stacked-bullet-icons + round counters, which are
// stripped out via empty ui/hud_ammo_*.urc overrides). Shows how many full magazines are left in reserve for
// the held weapon = reserve ammo / clip size. Bottom-right, where the old ammo HUD lived.
static void CG_DrawMagazines(void)
{
    int    reserve, clipsize, mags;
    char   txt[16];
    float  sx, sy;
    vec4_t col;

    if (!cg.snap || !cg_hud->integer) {
        return;
    }
    if (cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return;
    }
    if (cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) {
        return;
    }
    if (cg.snap->ps.activeItems[1] < 0) {
        return; // no weapon equipped
    }

    reserve  = cg.snap->ps.stats[STAT_AMMO];
    clipsize = cg.snap->ps.stats[STAT_MAXCLIPAMMO];
    if (reserve < 0) {
        reserve = 0;
    }
    mags = (clipsize > 0) ? (reserve / clipsize) : reserve;

    Com_sprintf(txt, sizeof(txt), "%i", mags);

    // R_DrawString multiplies the passed position by cgs.uiHiResScale, so to land at an actual pixel we pass
    // (actualPixel / scale). Computing from the real vidWidth/vidHeight keeps it pinned to the bottom-right
    // corner on ANY aspect - a fixed virtual-640 x would land at screen centre on ultrawide (3440x1440).
    sx = ((float)cgs.glconfig.vidWidth  - 170.0f) / cgs.uiHiResScale[0];
    sy = ((float)cgs.glconfig.vidHeight -  80.0f) / cgs.uiHiResScale[1];

    col[0] = 0.80f;
    col[1] = 0.72f;
    col[2] = 0.35f;
    col[3] = 1.0f * s_hudFadeAlpha; // muted gold, matching the old ammo text; follows the HUD fade
    if (col[3] <= 0.02f) {
        return;
    }
    cgi.R_SetColor(col);
    // small "MAGS" caption above the number (offsets are in virtual units, like sx/sy)
    cgi.R_DrawString(cgs.media.objectiveFont, "MAGS", sx, sy - 14.0f, -1, cgs.uiHiResScale);
    cgi.R_DrawString(cgs.media.objectiveFont, txt, sx + 2.0f, sy, -1, cgs.uiHiResScale);
    cgi.R_SetColor(NULL);
}

/*
===============================================================================
HZM coop - HUD FADE (user design 2026-07-06)

The persistent HUD chrome (health + ammo panels; the COMPASS is exempt by user
request) fades out after coop_hudFadeTime seconds of "calm" and fades back fast
on any HUD-relevant activity:
  - firing (either barrel) or holding ADS (deliberate engagement)
  - health change (damage taken / heals), ammo or clip change (shots, reloads,
    pickups), weapon switch or weapon pickup
  - objective updates (CS_OBJECTIVES hook in cg_main.c) and incoming suppression
    (CG_AddSuppression hook in cg_view.c) via CG_HudFadeTouch()
Safety: below 25% max health (or dead/DBNO) the HUD never fades.
cgame computes one alpha and publishes it in ui_hudAlpha for the client UI layer
(cl_ui.cpp applies it to the hud_health / hud_ammo menu containers). Transient
panels (objectives list, prompts, weapon bar) keep their own lifetimes.
coop_hudFade 0 disables (classic always-on HUD).
===============================================================================
*/

// coop_hudFadeDebug 1: print every fade-touch with its reason (rate-limited per reason) so a stuck
// always-visible HUD can be diagnosed from qconsole.log.
static void CG_HudFadeDebug(const char *reason)
{
    static cvar_t *pDbg = NULL;
    static int     lastPrint = -100000;
    if (!pDbg) { pDbg = cgi.Cvar_Get("coop_hudFadeDebug", "0", 0); }
    if (pDbg->integer && cg.time - lastPrint > 250) {
        cgi.Printf("^~^~^ HUDFADE touch: %s (t=%d)\n", reason, cg.time);
        lastPrint = cg.time;
    }
}

void CG_HudFadeTouch(void)
{
    s_hudTouchTime = cg.time;
    CG_HudFadeDebug("external (objective/suppression)");
}

static void CG_UpdateHudFade(void)
{
    static cvar_t *pOn = NULL, *pHold = NULL;
    static int     lastHealth = -99999, lastWeap = -99999, lastAmmoSig = -99999, lastOwned = -99999;
    static float   lastPub = -2.0f;
    usercmd_t      ucmd;
    int            h, maxh, weap, ammoSig, owned;
    float          target, step;

    if (!pOn)   { pOn   = cgi.Cvar_Get("coop_hudFade", "1", CVAR_ARCHIVE); }
    if (!pHold) { pHold = cgi.Cvar_Get("coop_hudFadeTime", "5", CVAR_ARCHIVE); }
    if (!cg.snap) {
        return;
    }

    // deliberate inputs: firing or aiming (ADS hold; NOT free-look - that never rests)
    memset(&ucmd, 0, sizeof(ucmd));
    cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &ucmd);
    if (ucmd.buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT | BUTTON_COOPADS)) {
        s_hudTouchTime = cg.time;
        CG_HudFadeDebug(va("buttons 0x%x", ucmd.buttons));
    }

    // HZM coop [user 07-10]: viewing the objectives (scoreboard held) keeps the HUD awake so the
    // player can read their health/ammo alongside the objectives.
    if (cg.showScores) {
        s_hudTouchTime = cg.time;
    }

    // HZM coop [user 07-10]: script-driven wake. The server bumps the coop_hudPoke cvar (via a
    // stuffed seta - allowed by the coop_* servercmd whitelist) at moments the cgame can't see on
    // its own, e.g. the INSTANT a medkit heal starts (before health actually ticks up). Any change
    // to the cvar's value is treated as activity.
    {
        static char sLastPoke[32] = "";
        cvar_t    *pPoke = cgi.Cvar_Get("coop_hudPoke", "0", 0);
        if (pPoke && Q_stricmp(pPoke->string, sLastPoke)) {
            if (sLastPoke[0]) {
                s_hudTouchTime = cg.time;
                CG_HudFadeDebug("script poke (coop_hudPoke)");
            }
            Q_strncpyz(sLastPoke, pPoke->string, sizeof(sLastPoke));
        }
    }

    // state deltas: health, ammo+clip, active weapon, owned-weapons mask (pickups)
    h       = cg.snap->ps.stats[STAT_HEALTH];
    weap    = cg.snap->ps.activeItems[1];
    ammoSig = cg.snap->ps.stats[STAT_AMMO] * 1024 + cg.snap->ps.stats[STAT_CLIPAMMO];
    owned   = cg.snap->ps.stats[STAT_WEAPONS];
    if (h != lastHealth || weap != lastWeap || ammoSig != lastAmmoSig || owned != lastOwned) {
        if (lastHealth != -99999) { // very first snapshot is baseline, not activity
            CG_HudFadeDebug(
                va("delta h %d->%d weap %d->%d ammo %d->%d owned %d->%d",
                   lastHealth, h, lastWeap, weap, lastAmmoSig, ammoSig, lastOwned, owned)
            );
        }
        s_hudTouchTime = cg.time;
        lastHealth = h;
        lastWeap = weap;
        lastAmmoSig = ammoSig;
        lastOwned = owned;
    }

    // hurt/downed safety: the red health warning must never hide
    maxh = cg.snap->ps.stats[STAT_MAXHEALTH];
    if (h <= 0 || (maxh > 0 && h * 4 <= maxh)) {
        s_hudTouchTime = cg.time;
        CG_HudFadeDebug(va("low health %d/%d", h, maxh));
    }

    // [user 07-11] IN COVER or DOWN (DBNO): keep the HUD up the WHOLE time. Posted in cover you're holding a
    // position and watching; downed you need to read your state while bleeding out - neither should let the
    // health/ammo panels fade. coop_dbnoView is the per-client DBNO signal (also forces the bleed-out view).
    {
        static cvar_t *pHudDbno = NULL;
        if (!pHudDbno) { pHudDbno = cgi.Cvar_Get("coop_dbnoView", "0", 0); }
        if ((cg.predicted_player_state.pm_flags & PMF_COOP_COVER) || (pHudDbno && pHudDbno->integer)) {
            s_hudTouchTime = cg.time;
        }
    }

    // [user 07-12] BOMB TIMER: while the engine stopwatch is counting down (a charge was planted / a fuse
    // is burning), keep the HUD up - planting UNFADES it instantly and it stays readable for the whole
    // countdown; once the timer expires (or is cleared via 'stopwatch 0') normal fading resumes.
    // Running-test mirrors CG_DrawStopwatch exactly (same clock: cg.time).
    if (cgi.stopWatch->iStartTime && cgi.stopWatch->iStartTime < cgi.stopWatch->iEndTime
        && cgi.stopWatch->iEndTime > cg.time) {
        s_hudTouchTime = cg.time;
        CG_HudFadeDebug("stopwatch running (bomb timer)");
    }

    // [user 07-12] OBJECTIVES MENU OPEN (the O toggle): unfade instantly and stay up for as long as the
    // menu is open - the player is READING; fading the health/ammo away next to it made no sense. The
    // client-side flag is set 1/0 by ui/coop_objectives/obj_add|rem.cfg (and reset on spawn by obj_reset.cfg).
    {
        static cvar_t *pObjOpen = NULL;
        if (!pObjOpen) { pObjOpen = cgi.Cvar_Get("coop_objOpen", "0", 0); }
        if (pObjOpen && pObjOpen->integer) {
            s_hudTouchTime = cg.time;
        }
    }

    if (pOn->integer) {
        float hold = pHold->value;
        if (hold < 1.0f) { hold = 1.0f; }
        target = (cg.time - s_hudTouchTime < (int)(hold * 1000.0f)) ? 1.0f : 0.0f;
    } else {
        target = 1.0f;
    }

    // fast in (~0.2s), gentle out (~0.75s)
    step = (cg.frametime > 0) ? (float)cg.frametime / ((target > s_hudFadeAlpha) ? 200.0f : 750.0f) : 1.0f;
    if (target > s_hudFadeAlpha) {
        s_hudFadeAlpha += step;
        if (s_hudFadeAlpha > 1.0f) { s_hudFadeAlpha = 1.0f; }
    } else {
        s_hudFadeAlpha -= step;
        if (s_hudFadeAlpha < 0.0f) { s_hudFadeAlpha = 0.0f; }
    }

    // publish for the client UI layer - on change only (no per-frame cvar churn at rest)
    if (s_hudFadeAlpha < lastPub - 0.002f || s_hudFadeAlpha > lastPub + 0.002f
        || (s_hudFadeAlpha != lastPub && (s_hudFadeAlpha == 0.0f || s_hudFadeAlpha == 1.0f))) {
        cgi.Cvar_Set("ui_hudAlpha", va("%.3f", s_hudFadeAlpha));
        lastPub = s_hudFadeAlpha;
    }
}

// HZM gl2 re-port (bug-gl2-dbnofx / bug-gl2-suppressfx): renderergl2 has no HZM post-FX
// module, so the low-health desaturate/red-vignette and the suppression tunnel-vignette
// (gl1 post passes, renderergl1/tr_postprocess_gl1.c) never appear under gl2. Approximate
// both as plain 2D overlays drawn through the material pipeline: a "*white" colour wash +
// the existing radial-alpha vignette texture (textures/hud/coop_ads_vignette - black with
// a clear centre). Gated HARD to cl_renderer == opengl2 so it can never double with gl1's
// shader implementation. Consumes the exact same cvars gl1 consumes (r_ppLowHealth*,
// r_ppSuppression / r_ppSuppress*, r_ppHealthFrac) - cg_view.c publishes the dynamic ones
// every frame, so the on/off switches and tuning dials behave identically on both renderers.
static void CG_DrawGl2PostFxFallback(void)
{
    static cvar_t   *pRenderer = NULL;
    static qhandle_t hWhite    = 0;
    static qhandle_t hVig      = 0;
    float            vidW, vidH;
    vec4_t           col;

    if (!pRenderer) {
        pRenderer = cgi.Cvar_Get("cl_renderer", "opengl1", 0);
    }
    if (!pRenderer || Q_stricmp(pRenderer->string, "opengl2")) {
        return; // gl1 (or anything else): the renderer's own post-FX pass owns these effects
    }

    if (!hWhite) {
        hWhite = cgi.R_RegisterShaderNoMip("*white");
    }
    if (!hVig) {
        hVig = cgi.R_RegisterShaderNoMip("textures/hud/coop_ads_vignette");
    }

    vidW = (float)cgs.glconfig.vidWidth;
    vidH = (float)cgs.glconfig.vidHeight;

    // ---- low-health / DBNO: red wash + darkened edges (plan Fix 5) ----
    {
        static cvar_t *pOn = NULL, *pFrac = NULL, *pStart = NULL, *pAmt = NULL, *pBeat = NULL;
        if (!pOn) {
            pOn = cgi.Cvar_Get("r_ppLowHealth", "1", CVAR_ARCHIVE);
        }
        if (!pFrac) {
            pFrac = cgi.Cvar_Get("r_ppHealthFrac", "1", 0);
        }
        if (!pStart) {
            pStart = cgi.Cvar_Get("r_ppLowHealthStart", "0.5", CVAR_ARCHIVE);
        }
        if (!pAmt) {
            pAmt = cgi.Cvar_Get("r_ppLowHealthAmount", "1.0", CVAR_ARCHIVE);
        }
        if (!pBeat) {
            pBeat = cgi.Cvar_Get("r_ppLowHealthBeat", "0.25", CVAR_ARCHIVE);
        }

        if (pOn->integer) {
            // identical ramp to gl1 (tr_postprocess_gl1.c:755-785): clear onset at the
            // threshold, ramping to full as you bleed out, with the heartbeat throb.
            // cg_view.c forces r_ppHealthFrac to 0.02 while DBNO -> near-max here.
            float frac  = pFrac->value;
            float start = pStart->value;
            float hurt  = 0.0f;
            if (start < 0.05f) {
                start = 0.05f;
            }
            if (frac < start) {
                float ramp = (start - frac) / start;
                float depth, beatRate;
                if (ramp < 0.0f) {
                    ramp = 0.0f;
                } else if (ramp > 1.0f) {
                    ramp = 1.0f;
                }
                hurt = (0.35f + 0.65f * ramp) * pAmt->value;
                if (hurt > 1.0f) {
                    hurt = 1.0f;
                }
                depth = pBeat->value;
                if (depth < 0.0f) {
                    depth = 0.0f;
                } else if (depth > 1.0f) {
                    depth = 1.0f;
                }
                beatRate = 1.8f + hurt * 1.6f;
                hurt *= (1.0f - depth) + depth * (float)sin(cg.time * 0.001f * beatRate);
                if (hurt < 0.0f) {
                    hurt = 0.0f;
                }
                if (hurt > 1.0f) {
                    hurt = 1.0f;
                }
            }
            if (hurt > 0.001f) {
                // whole-frame red wash (alpha blending can't desaturate, so the wash
                // carries the "wounded" read the gl1 shader gets from its red mix)
                if (hWhite) {
                    col[0] = 0.45f;
                    col[1] = 0.02f;
                    col[2] = 0.02f;
                    col[3] = hurt * 0.30f;
                    cgi.R_SetColor(col);
                    cgi.R_DrawStretchPic(0, 0, vidW, vidH, 0, 0, 1, 1, hWhite);
                }
                // darkened edges: radial-alpha vignette (clear centre -> dark edges)
                // approximates the shader's edge-heavy red + edge-darken terms
                if (hVig) {
                    col[0] = 1.0f;
                    col[1] = 1.0f;
                    col[2] = 1.0f;
                    col[3] = hurt * 0.60f;
                    cgi.R_SetColor(col);
                    cgi.R_DrawStretchPic(0, 0, vidW, vidH, 0, 0, 1, 1, hVig);
                }
                cgi.R_SetColor(NULL);
            }
        }
    }

    // ---- suppression: tunnel-vignette (plan Fix 6) ----
    {
        static cvar_t *pOn = NULL, *pVal = NULL, *pAmt = NULL;
        if (!pOn) {
            pOn = cgi.Cvar_Get("r_ppSuppression", "1", CVAR_ARCHIVE);
        }
        if (!pVal) {
            pVal = cgi.Cvar_Get("r_ppSuppress", "0", 0);
        }
        if (!pAmt) {
            pAmt = cgi.Cvar_Get("r_ppSuppressAmount", "1.0", CVAR_ARCHIVE);
        }

        if (pOn->integer) {
            float s = pVal->value * pAmt->value;
            if (s < 0.0f) {
                s = 0.0f;
            } else if (s > 1.0f) {
                s = 1.0f;
            }
            if (s > 0.001f) {
                // tunnel closes in: matches gl1's edge term c *= (1 - vig * s * 0.72)
                if (hVig) {
                    col[0] = 1.0f;
                    col[1] = 1.0f;
                    col[2] = 1.0f;
                    col[3] = s * 0.72f;
                    cgi.R_SetColor(col);
                    cgi.R_DrawStretchPic(0, 0, vidW, vidH, 0, 0, 1, 1, hVig);
                }
                // faint neutral-grey wash stands in for the shader's desaturation term
                if (hWhite) {
                    col[0] = 0.5f;
                    col[1] = 0.5f;
                    col[2] = 0.5f;
                    col[3] = s * 0.18f;
                    cgi.R_SetColor(col);
                    cgi.R_DrawStretchPic(0, 0, vidW, vidH, 0, 0, 1, 1, hWhite);
                }
                cgi.R_SetColor(NULL);
            }
        }
    }
}

void CG_Draw2D(void)
{
    CG_UpdateHudFade();
    CG_UpdateCountdown();
    CG_DrawGl2PostFxFallback(); // HZM gl2 re-port: under the HUD, over the scene (gl2 only)
    CG_DrawZoomOverlay();
    CG_DrawAdsVignette();
    CG_DrawLagometer();
    CG_HudDrawElements();
    CG_DrawObjectives();
    CG_DrawIcons();
    CG_DrawStopwatch();
    CG_DrawSpectatorView();
    CG_DrawPlayerTeam();
    CG_DrawPlayerEntInfo();
    CG_UpdateAttackerDisplay();
    CG_DrawVote();
    CG_DrawInstantMessageMenu();
    CG_DrawCrosshair();
    CG_DrawCoopIcons();
    CG_DrawMGHeat();
    CG_DrawMagazines();
    CG_SeedAdsTuneFromBaked(); // seed live cvars from the baked table so tune mode doesn't snap the gun
    CG_DrawAdsTune();
}
