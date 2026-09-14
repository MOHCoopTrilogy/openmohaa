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

    // [2026-08-21] DO NOT "FIX" THESE PILLARS WITHOUT READING THIS.
    //
    // They look like a hardcoded opaque black letterbox - (vidWidth - vidHeight)/2 per side, 420px
    // each at 1920x1080 - and an audit of this function reported exactly that. It is WRONG.
    // cgs.media.lagometerShader is "gfx/2d/blank", whose shader declares:
    //     blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
    //     alphaGen oneMinusVertex        <- INVERTED
    // and color[3] is still fAlpha here, left over from the overlay draw above. So the rendered
    // alpha is (1 - fAlpha): at full zoom the pillars are fully TRANSPARENT, and they are only
    // visible while the scope fades in and out. There is no 44% of black screen to reclaim.
    //
    // Which means the real reason a scope does not show a proper surround is NOT these quads - it is
    // that the scene is rendered ONCE, at the zoomed fov, so the periphery is magnified exactly as
    // much as the lens. Fixing that needs a second scene render at normal fov with the zoomed image
    // composited into a circular mask. Do that; do not touch these.
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

    // [user 2026-08-21] VARIANT-SAFE: a binocular or camera skin variant would otherwise fall
    // through to the default zoom type.
    {
        static char vb[64];
        if (CoopStripSkinSuffix(weaponstring, vb, sizeof(vb))) {
            weaponstring = vb;
        }
    }
    // [user 2026-08-21] "Im seeing the texture over my screen problem we once solved before."
    //
    // bDrawOverlay is initialised qtrue above and ONLY the final else-branch ever clears it, so the
    // Spy Camera / Binoculars / Bombing Run arms below drew their overlay whenever that item was the
    // ACTIVE ITEM - not while actually looking through it. Binoculars and the coop Bombing Run reward
    // take the FULL-SCREEN overlay (zoomType 3), so simply having one selected pasted a texture over
    // the whole view and left it there.
    //
    // The variant-safe strip added directly above widened the blast radius the same day: a binocular
    // SKIN VARIANT used to fail the whole-string compare and fall through to the else-branch, which is
    // INZOOM-gated and therefore behaved correctly by accident. Stripping the suffix made variants
    // match here and inherit the ungated path.
    //
    // This is a ZOOM overlay, so gate every type on actually being zoomed. The generic arm keeps its
    // extra <= 30 test (that distinguishes a rifle scope from a wide binocular fov); the dedicated
    // arms only need "is the zoom live at all".
    if (!cg.snap->ps.stats[STAT_INZOOM]) {
        bDrawOverlay = qfalse;
    } else if (!Q_stricmp(weaponstring, "Spy Camera")) {
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

// [user 2026-09-03] CINEMATIC CROSSHAIR SUPPRESSION - a NON-ARCHIVED, SELF-EXPIRING hold.
//
// WHAT IS DELIBERATELY NOT DONE. ui_crosshair (cg_main.c) and cg_crosshair are BOTH CVAR_ARCHIVE -
// the player's own saved preference, and CVAR_ARCHIVE is exactly what Cvar_WriteVariables persists.
// A scene that wrote one and then failed to restore it - death, disconnect, map change, a Script
// Error that skipped the restore statement - would latch his crosshair off in his config FOREVER.
// That is bug-2298's shape, so neither is written.
//
// WHAT IS DONE. The server pokes coop_cineHud with a HOLD IN SECONDS. Three properties, in the
// order they matter:
//   1. flags 0 - it can never reach a config, because Cvar_WriteVariables writes only ARCHIVE.
//   2. it EXPIRES. The value is consumed into a deadline and the cvar cleared, so if the server
//      stops re-arming for ANY reason - the script thread dies, the map changes, the player
//      disconnects mid-beat - the crosshair returns on its own inside the hold. The script's
//      explicit release is a LATENCY fix, not the thing that makes this safe.
//   3. the hold is CLAMPED here, so no value on the wire can buy a long hide.
// An EMPTY string is the idle/consumed state; any non-empty value is a pending message, and "0"
// is an explicit immediate release. cg_main.c's hzmClearFx list writes "0" on every CG_Init, so
// a fresh connect or map load always releases on its first drawn frame.
static int s_coopCineHudUntil = 0;

static qboolean CG_CoopCineHudActive(void)
{
    static cvar_t *pCine = NULL;

    if (!pCine) {
        pCine = cgi.Cvar_Get("coop_cineHud", "0", 0);
    }

    if (pCine->string[0]) {
        float hold = pCine->value;

        if (hold > 8.0f) {
            hold = 8.0f;    // a hostile or garbage value cannot buy a long hide
        }
        if (hold <= 0.0f) {
            s_coopCineHudUntil = 0;
        } else {
            s_coopCineHudUntil = cg.time + (int)(hold * 1000.0f);
        }
        cgi.Cvar_Set("coop_cineHud", "");   // consumed, as coop_dizzy / coop_lensSplash are
    }

    if (!s_coopCineHudUntil) {
        return qfalse;
    }

    // cg.time runs BACKWARDS across a map load, and this static outlives one. Treat any backward
    // jump larger than the clamp as an expiry, so a stale deadline can never outlast the level it
    // was set in even if the hzmClearFx write above were somehow missed.
    if (cg.time >= s_coopCineHudUntil || cg.time < s_coopCineHudUntil - 9000) {
        s_coopCineHudUntil = 0;
        return qfalse;
    }
    return qtrue;
}

// [2026-08-28] Hit-marker state. Declared HERE rather than beside CG_DrawHitMarker because
// CG_DrawCrosshair - which is defined above it - writes the aim point every frame.
static float s_coopAimX = -1.0f, s_coopAimY = -1.0f;
static int   s_coopAimSet = 0;
static int   s_coopHitTime = 0;
static qboolean s_coopHitKill = qfalse;

// HZM MP - HARDCORE modifier (user 2026-09-14). True only when the server published the serverinfo flag
// g_mpHardcore (parsed into cgs.mpHardcore by CG_ParseServerinfo) AND this is not a coop session. The
// flag is set only by the MP hardcore script on an MP server, so cgs.mpHardcore is 0 in every coop
// session and the fast path returns immediately there; the coop_isCoopSession backstop (same flag the
// compass bar reads, set 1 by coop's player.scr, reset to 0 on CG_Init/Shutdown) is belt-and-braces so a
// stray serverinfo value can never blank the coop HUD. Consumers: CG_DrawCrosshair, CG_DrawStaminaArc
// (both skip drawing) and CG_UpdateHudFade (forces the health/ammo chrome alpha to 0).
static qboolean CG_MpHardcoreActive(void)
{
    static cvar_t *pSess = NULL;
    if (!cgs.mpHardcore) {
        return qfalse;
    }
    if (!pSess) {
        pSess = cgi.Cvar_Get("coop_isCoopSession", "0", 0);
    }
    if (pSess && pSess->integer) {
        return qfalse; // coop session - never touch the coop HUD whatever the flag says
    }
    return qtrue;
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

    // HZM MP - Hardcore removes the crosshair (MP only; inert in coop - see CG_MpHardcoreActive).
    if (CG_MpHardcoreActive()) {
        return;
    }

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

        // [2026-08-28] Remember where the crosshair ACTUALLY ended up, after the 3P true-aim
        // re-projection and the free-aim offset above. The hit marker anchors to this instead of
        // screen centre - in third person and free-aim the two are different places, and a marker
        // pinned to the middle of the screen would float away from the shot it is confirming.
        s_coopAimX = x + width * 0.5f;
        s_coopAimY = y + height * 0.5f;
        s_coopAimSet = cg.time;

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
CG_DrawStaminaArc

HZM coop [user 2026-09-09, bug-2555] "Could we find a way to represent stamina using the white
outline around the top of the health bar in the HUD?"

The arc used to be a static URC Label (hud_health.urc "healthframe"); it is drawn here instead so it
can be partially revealed. Two passes of the SAME shader over the SAME texture: dim grey across the
whole arc, then white across the left `frac` of it, using R_DrawStretchPic's explicit s/t so the crop
is a real UV clip - the curved stroke is cropped, never squashed. At full stamina the result is
pixel-identical to the old Label.

The value shares STAT_MGHEAT; see bg_public.h. 0 means the server has not written it, which is NOT
the same as an empty pool - draw full and stay quiet rather than show a false empty gauge.

Placement is cvar-driven because the URC-rect to screen-pixel transform was never derived, and
rebuilding the client for each nudge is a bad loop. Defaults are the computed values.
=================
*/
static void CG_DrawStaminaArc(void)
{
    static qhandle_t hFrame = 0;
    static cvar_t   *pX = NULL, *pY = NULL, *pW = NULL, *pH = NULL, *pOn = NULL;
    static float     fShown = 1.0f;
    int              raw;
    float            frac, x, y, w, h, fA;
    vec4_t           col;

    if (!cg.snap) {
        return;
    }
    // HZM MP - Hardcore removes the stamina indicator (MP only; inert in coop - see CG_MpHardcoreActive).
    if (CG_MpHardcoreActive()) {
        return;
    }
    if (!pOn) {
        pOn = cgi.Cvar_Get("coop_staminaArc", "1", CVAR_ARCHIVE);
        pX  = cgi.Cvar_Get("coop_staminaArcX", "32", CVAR_ARCHIVE);
        pY  = cgi.Cvar_Get("coop_staminaArcY", "95", CVAR_ARCHIVE);
        pW  = cgi.Cvar_Get("coop_staminaArcW", "256", CVAR_ARCHIVE);
        pH  = cgi.Cvar_Get("coop_staminaArcH", "60", CVAR_ARCHIVE);
    }
    if (!pOn->integer) {
        return;
    }
    // the same gates the URC menu had for free, now explicit: lobby (drawhud 0), intermission,
    // and the coop cinematic HUD suppression
    if (cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) {
        return;
    }
    if (CG_CoopCineHudActive()) {
        return;
    }
    if (!hFrame) {
        hFrame = cgi.R_RegisterShader("hud_health_frame");
    }
    if (!hFrame) {
        return;
    }

    raw = cg.snap->ps.stats[STAT_MGHEAT];
    if ((cg.snap->ps.pm_flags & PMF_TURRET) || raw <= 0) {
        frac = 1.0f;   // mounted (the stat is MG heat) or no data yet - a full, ordinary frame
    } else {
        frac = (float)(raw - 1) / 100.0f;
    }
    if (frac < 0.0f) { frac = 0.0f; } else if (frac > 1.0f) { frac = 1.0f; }

    // the stat arrives at snapshot rate (~20 Hz) as an integer, so ease it or it visibly ticks
    fShown += (frac - fShown) * 0.25f;
    if (fShown < 0.0f) { fShown = 0.0f; } else if (fShown > 1.0f) { fShown = 1.0f; }

    x = pX->value * cgs.uiHiResScale[0];
    y = cg.refdef.height - pY->value * cgs.uiHiResScale[1];
    w = pW->value * cgs.uiHiResScale[0];
    h = pH->value * cgs.uiHiResScale[1];

    // [user 2026-09-09] "It also appears to get stuck on the screen after all other icons fade."
    // Not an alpha bug - ui_hudAlpha IS s_hudFadeAlpha (published at the end of CG_UpdateHudFade),
    // so this already fades on exactly the same clock as the URC panels. It is PERCEPTUAL: a thin
    // bright line on a dark background stays legible at an alpha where a dark HUD panel has already
    // gone. Squaring keeps 1.0 at 1.0 and collapses the tail - 0.3 becomes 0.09 - so the two vanish
    // together without desynchronising them.
    fA = s_hudFadeAlpha * s_hudFadeAlpha;

    // [bug-2593] Hard cutoff to match the URC HUD widgets, which draw NOTHING once their fade
    // multiplier is essentially out (uiwidget.cpp: m_hudFadeMul <= 0.02 returns early). A thin
    // bright stroke stays perceptible at an alpha where the dark panels have already gone, so
    // stop drawing the arc at the same point the rest of the bottom-left cluster does, and the
    // whole cluster vanishes together instead of the frame lingering through the fade tail.
    if (s_hudFadeAlpha <= 0.02f) {
        return;
    }

    // [user 2026-09-09] "the gauge itself for stamina you can barely see, i thought you were going
    // to make the white outline itself the stamina bar." It IS the outline - the failure was
    // CONTRAST. At 0.40 grey against a 1.00 white remainder, a thin stroke over a dark scene reads
    // as one continuous line. At 0.13 the arc visibly SHORTENS instead of subtly greying.
    col[0] = col[1] = col[2] = 0.13f;
    col[3] = fA;
    cgi.R_SetColor(col);
    cgi.R_DrawStretchPic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, hFrame);

    // the remaining portion: full white, and amber under 15% - the only moment the number actually
    // changes what the player does, because sprint is about to cut out
    if (fShown > 0.001f) {
        col[0] = 1.00f;
        col[1] = (fShown < 0.15f) ? 0.72f : 1.00f;
        col[2] = (fShown < 0.15f) ? 0.35f : 1.00f;
        col[3] = fA;
        cgi.R_SetColor(col);
        cgi.R_DrawStretchPic(x, y, w * fShown, h, 0.0f, 0.0f, fShown, 1.0f, hFrame);
    }
    cgi.R_SetColor(NULL);
}

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
    int    heat, raw, belt;
    char   beltStr[32];
    float  bw, bh, bx, by, fillw;
    vec4_t cBg   = {0.0f, 0.0f, 0.0f, 0.55f};
    vec4_t cFill = {0.85f, 0.10f, 0.08f, 0.90f};

    if (!cg.snap) {
        return;
    }
    if (!(cg.snap->ps.pm_flags & PMF_TURRET)) {
        return; // only while mounted on a turret
    }

    // [user 2026-08-17] STAT_MGHEAT is PACKED - see fgame/weapturret.cpp. There was no free stat
    // slot for the belt count, so one short carries both: belt in bits 0-8, quantised heat in 9-14.
    // beltField 0 = no belt (unlimited) -> draw no number. Ship game.dll and cgame.dll TOGETHER.
    raw  = cg.snap->ps.stats[STAT_MGHEAT];
    belt = (raw & 511) - 1;              // -1 == unlimited / not ammo-gated
    heat = ((raw >> 9) & 63) * 100 / 63;
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

    // [user 2026-08-17] "I can't tell if the mg42's have 500 rounds, it doesn't tell me down by the
    // mag count." The belt WAS being sent - the server probe confirms 500 applied to all three
    // church guns - but a bare unlabelled number floating over a small bar is not where anyone
    // looks for ammo. LABEL it and make it the size of an ammo readout. Amber under 100, red under
    // 25, so the number carries the same warning the bar does.
    if (belt >= 0) {
        vec4_t cTxt = {0.90f, 0.90f, 0.85f, 0.95f};
        float  tw;
        if (belt < 25) {
            cTxt[1] = 0.20f; cTxt[2] = 0.15f;
        } else if (belt < 100) {
            cTxt[1] = 0.70f; cTxt[2] = 0.20f;
        }
        Com_sprintf(beltStr, sizeof(beltStr), "BELT %i", belt);
        tw = (float)strlen(beltStr) * 10.0f * cgs.uiHiResScale[0];
        cgi.R_SetColor(cTxt);
        cgi.R_DrawString(cgs.media.objectiveFont,
                         beltStr,
                         bx + bw - tw,
                         by - 20.0f * cgs.uiHiResScale[1],
                         -1,
                         cgs.uiHiResScale);
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

    // [user 2026-09-09, bug-2555] SPRINTING OR RECOVERING. None of the wakes above fire for it -
    // not the fire/ADS buttons, not a health or ammo delta, not cover, not DBNO - so a player who
    // had been quiet for coop_hudFadeTime would sprint and watch a FADED stamina arc, and the whole
    // ~9.5 s refill would always complete behind the fade, because recovering is by definition
    // quiet. Any value that is neither 0 (no data) nor 101 (full) means the pool is in motion.
    // Gated on PMF_TURRET because the stat carries MG heat while mounted (see bg_public.h).
    if (!(cg.snap->ps.pm_flags & PMF_TURRET)) {
        static int lastStamRaw = -1;
        int        stamRaw     = cg.snap->ps.stats[STAT_MGHEAT];

        // [vet 2026-09-09] Wake on a CHANGE, not on "the value is not full". The original test
        // pinned the entire HUD awake for as long as the pool sat at anything other than exactly
        // full, so any resting value short of the top would have held every panel on screen for
        // ever. A change is what "the player is exerting himself" actually means.
        if (stamRaw > 0 && stamRaw != lastStamRaw) {
            s_hudTouchTime = cg.time;
        }
        lastStamRaw = stamRaw;
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

    // HZM MP - HARDCORE hides the persistent health/ammo chrome by forcing the PUBLISHED alpha to 0.
    // MP only (CG_MpHardcoreActive gates on cgs.mpHardcore + !coop session), so coop's fade is untouched.
    // Only the published value is overridden, not the internal s_hudFadeAlpha, so normal fading resumes
    // the instant the flag clears (the next non-hardcore frame republishes the real alpha because lastPub
    // was left at 0). NOTE: ui_hudAlpha drives BOTH the hud_health AND hud_ammo menus (cl_ui.cpp
    // UI_ApplyHudFadeAlpha), so this also hides the ammo panel - the health bar is a client URC menu, not
    // cgame-drawn, and ui_hudAlpha is the only cgame-side lever for it (it is shared with ammo). The
    // crosshair and stamina arc are hidden at their own draw sites above.
    if (CG_MpHardcoreActive()) {
        if (lastPub != 0.0f) {
            cgi.Cvar_Set("ui_hudAlpha", "0");
            lastPub = 0.0f;
        }
        return;
    }

    // publish for the client UI layer - on change only (no per-frame cvar churn at rest)
    if (s_hudFadeAlpha < lastPub - 0.002f || s_hudFadeAlpha > lastPub + 0.002f
        || (s_hudFadeAlpha != lastPub && (s_hudFadeAlpha == 0.0f || s_hudFadeAlpha == 1.0f))) {
        cgi.Cvar_Set("ui_hudAlpha", va("%.3f", s_hudFadeAlpha));
        lastPub = s_hudFadeAlpha;
    }
}

/*
===============================================================================
HZM coop - TOP COMPASS BAR (user design 2026-09-13, hzm-mohaa-coop-mod/_research/compass_bar_design.md)

A heading strip across the top centre that REPLACES the round hud_compass ring while it is live:
a fixed 150-degree arc, 5-degree ticks, a label every 15 degrees, 8 cardinal letters, a centre
heading readout, the current objective (with its distance in metres) and same-team teammates.

COOP-ONLY (v1). The bar is LIVE only when all three hold:
  - the player pref coop_compassBar is on (CVAR_ARCHIVE, seeded in coop_defaults.cfg);
  - a coop script set the session flag coop_isCoopSession (player.scr stufftexts it on setup and
    re-pushes it from ::manage; it is reset to 0 on every CG_Init and CG_Shutdown). No MP file may
    name it - docs/tools/check_mp_isolation.py clause 15;
  - the exe registered coop_compassBarExe, i.e. it knows how to move the DM box. A new cgame on an
    older exe therefore draws nothing rather than a bar under an unmoved kill feed.

LIVE vs VISIBLE. coop_compassBarLive (flags 0) carries the band height in real pixels, or 0. It is
written on change only and derives from the three conditions above plus the resolution - never
from the per-frame gates - so the ring cannot flicker back during a scope or a cutscene. cl_ui.cpp
hides the ring and moves the DM box down by exactly that many pixels; at 0 the layout is stock.
CG_CompassBarVisible is the per-frame half. Turret and vehicle seats read STAT_INZOOM 80 and set
PMF_CAMERA_VIEW, so both of those gates exempt PMF_TURRET or every vehicle would lose the bar.

FADE. The bar follows the HUD fade on the same clock (s_hudFadeAlpha), squared like the stamina arc
(bug-2555: a thin bright stroke outlives a dark panel at the same alpha), with a hard early-out, and
the markers go with it (floor 0, user decision 2026-09-13). The only new wake is an edge: the
current objective index changing (CG_CompassBarObjectiveChanged).

ROWS. The mockup drew markers over the tick labels (the objective star on a number, teammate
chevrons on S and W). Labels now own the top row outright; ticks hang from its bottom edge and the
markers sit in the row below, bottom-aligned to the band and capped to that row's height, so a
marker can cross a tick but never reach a label. Distance and name text live in the marker row too.
The band stays at 0.040H, clear of the XP popup (virtual y21) and the ready-gate prompt (y20).

There is no scissor, rotation or triangle primitive (cg_public.h): ticks, the band and every marker
are *white stretch-pics (markers one quad per pixel row), all in real pixels from vidWidth/Height.
coop_compassProbe 1 prints the deciding inputs every 0.5 s as "^~^~^ CBPROBE" lines.
===============================================================================
*/

#define CB_ARC           150.0f // degrees across the bar - fixed, so ADS (fov) never rescales it
#define CB_MARKER_FLOOR  0.0f   // marker alpha floor under the HUD fade (user decision: markers fade out too)
#define CB_SENTINEL_L    1730   // Player::UpdateStats writes exactly this triple when there is no objective location
#define CB_SENTINEL_R    1870
#define CB_SENTINEL_C    1800
#define CB_MATE_MAX_AGE  5000   // ms a radar-only teammate stays on the bar
#define CB_MATE_FADE_AGE 1500   // ms before a radar-only teammate starts fading by packet age
#define CB_NAME_DEG      4.0f   // a teammate's name shows only this close to the centre

static cvar_t *s_cbOn      = NULL; // coop_compassBar         pref, ARCHIVE
static cvar_t *s_cbScale   = NULL; // coop_compassBarScale    pref, ARCHIVE, clamped 0.75..1.0
static cvar_t *s_cbOpacity = NULL; // coop_compassBarOpacity  pref, ARCHIVE, clamped 0.3..1.0
static cvar_t *s_cbObj     = NULL; // coop_compassBarObj      pref, ARCHIVE (2 = all active is not in v1; drawn as 1)
static cvar_t *s_cbMates   = NULL; // coop_compassBarMates    pref, ARCHIVE
static cvar_t *s_cbLive    = NULL; // coop_compassBarLive     published band height px, flags 0
static cvar_t *s_cbSession = NULL; // coop_isCoopSession      set by coop script, flags 0
static cvar_t *s_cbExe     = NULL; // coop_compassBarExe      CVAR_ROM "1" from an exe that honours the band
static cvar_t *s_cbProbe   = NULL; // coop_compassProbe       dev probe, flags 0

enum { CB_DIAMOND, CB_CHEVRON, CB_CHEVRON_HOLLOW, CB_CARET_LEFT, CB_CARET_RIGHT };

typedef struct {
    float cx, half, ppd;          // centre x, half width, pixels per degree
    float top, band;              // top margin and bottom edge (the published band height)
    float labelH, labelCy;        // label row height and centre
    float tickTop;                // ticks hang from here; the marker row lies below it
    float numPx, cardPx, smallPx; // text heights: numbers + NE/SE/SW/NW, N/E/S/W, marker-row text
    float edgeW, clampPx;         // edge fade width, current-objective pin distance from centre
    int   objPx, matePx;          // marker sizes, capped to the marker row
    int   tickW, minorH, majorH, cardH;
} cbGeo_t;

typedef struct {
    char          txt[8];
    fontheader_t *font;
    float         x, px, alpha;
    qboolean      north;
} cbLabel_t;

typedef struct {
    int      clientNum;
    float    x, alpha, absDeg;
    qboolean hollow;
} cbMate_t;

static float CB_Abs(float v)
{
    return (v < 0.0f) ? -v : v;
}

static float CB_Round(float v)
{
    return (float)floor(v + 0.5f);
}

static float CG_CompassBarClamp(float v, float lo, float hi)
{
    if (!(v >= lo)) {
        return lo; // also catches NaN from a garbage cvar
    }
    return (v > hi) ? hi : v;
}

static void CG_CompassBarCvars(void)
{
    if (s_cbOn) {
        return;
    }
    s_cbOn      = cgi.Cvar_Get("coop_compassBar", "1", CVAR_ARCHIVE);
    s_cbScale   = cgi.Cvar_Get("coop_compassBarScale", "1.0", CVAR_ARCHIVE);
    s_cbOpacity = cgi.Cvar_Get("coop_compassBarOpacity", "0.9", CVAR_ARCHIVE);
    s_cbObj     = cgi.Cvar_Get("coop_compassBarObj", "1", CVAR_ARCHIVE);
    s_cbMates   = cgi.Cvar_Get("coop_compassBarMates", "1", CVAR_ARCHIVE);
    // flags 0 on the three below: server-drivable or derived, so none of them may ever reach a config.
    // Registered EAGERLY (from CG_Init) because a stuffed "set" of a cvar that does not exist yet lands
    // user-created - the coop_cineHud / coop_voxCut first-frame trap (bug-2318).
    s_cbLive    = cgi.Cvar_Get("coop_compassBarLive", "0", 0);
    s_cbSession = cgi.Cvar_Get("coop_isCoopSession", "0", 0);
    s_cbExe     = cgi.Cvar_Get("coop_compassBarExe", "", 0); // an old exe never registers it: "" reads 0
    s_cbProbe   = cgi.Cvar_Get("coop_compassProbe", "0", 0);
}

static void CG_CompassBarReset(void)
{
    CG_CompassBarCvars();
    cgi.Cvar_Set("coop_isCoopSession", "0");
    cgi.Cvar_Set("coop_compassBarLive", "0");
}

void CG_CompassBarInit(void)
{
    CG_CompassBarReset();
}

void CG_CompassBarShutdown(void)
{
    CG_CompassBarReset();
}

// CS_CURRENT_OBJECTIVE changed value (cg_main.c). Wakes the HUD fade so the new marker is seen - only
// while the bar is live, so MP and a coop player with the bar off keep today's fade exactly.
void CG_CompassBarObjectiveChanged(void)
{
    CG_CompassBarCvars();
    if (!s_cbLive->integer) {
        return;
    }
    s_hudTouchTime = cg.time;
    CG_HudFadeDebug("current objective changed (CS_CURRENT_OBJECTIVE)");
}

static int CG_CompassBarBandPx(void)
{
    float s = CG_CompassBarClamp(s_cbScale->value, 0.75f, 1.0f);

    return (int)(0.040f * (float)cgs.glconfig.vidHeight * s + 0.5f);
}

// Written on change only (the ui_hudAlpha pattern). Compared against the cvar itself rather than a private
// copy, so a stray console or server write to coop_compassBarLive is corrected on the next frame.
static void CG_CompassBarPublishLive(void)
{
    int live = 0;

    if (s_cbOn->integer && s_cbSession->integer && s_cbExe->integer && cgs.glconfig.vidHeight > 0) {
        live = CG_CompassBarBandPx();
    }
    if (live != s_cbLive->integer) {
        cgi.Cvar_Set("coop_compassBarLive", va("%i", live));
    }
}

static qboolean CG_CompassBarVisible(void)
{
    const playerState_t *ps;
    int                  pmf;

    // FIRST and unconditional: the accessor consumes coop_cineHud and ages its deadline
    if (CG_CoopCineHudActive()) {
        return qfalse;
    }
    if (!cg.snap || !s_cbLive->integer || !cg_hud->integer) {
        return qfalse;
    }
    ps  = &cg.snap->ps;
    pmf = ps->pm_flags;
    // lobby (drawhud 0), intermission, spectating (while following, ps is the other player's state)
    if (pmf & (PMF_NO_HUD | PMF_INTERMISSION | PMF_SPECTATING)) {
        return qfalse;
    }
    // the letterbox is painted BEFORE CG_Draw2D, so cgame 2D would otherwise sit on top of it
    if (ps->stats[STAT_LETTERBOX] > 0) {
        return qfalse;
    }
    // NOT gated on PMF_CAMERA_VIEW: it stays set through free-look rides such as the m1l1 truck bed (runtime
    // 2026-09-13: camview=1 while the player turned 6 -> 40 degrees), where the stock HUD and round ring stay up.
    // Hiding the bar there left the player with no compass at all. Real cutscenes hide the HUD through the
    // cinematic hold, PMF_NO_HUD and the letterbox checks above.
    // dead. While riding, STAT_HEALTH carries the VEHICLE's health, so a seat is never "dead" here
    if (!(ps->stats[STAT_HEALTH] > 0 || (pmf & PMF_TURRET))) {
        return qfalse;
    }
    // scoped rifle / binoculars use 15..30; every VehicleTurretGun user, tank drivers included, reads 80
    if (ps->stats[STAT_INZOOM] > 0 && ps->stats[STAT_INZOOM] <= 30 && !(pmf & PMF_TURRET)) {
        return qfalse;
    }
    return qtrue;
}

static void CG_CompassBarGeometry(cbGeo_t *g)
{
    float H = (float)cgs.glconfig.vidHeight;
    float W = (float)cgs.glconfig.vidWidth;
    float s = CG_CompassBarClamp(s_cbScale->value, 0.75f, 1.0f);
    float bw, avail;

    // width: 0.42W, capped at 0.75H so ultrawide does not get a 3000 px strip and the half-width stays
    // clear of hud_timelimit/hud_score at top-right
    bw         = CB_Round(((0.42f * W < 0.75f * H) ? 0.42f * W : 0.75f * H) * s);
    g->cx      = (float)floor(W * 0.5f);
    g->half    = bw * 0.5f;
    g->ppd     = bw / CB_ARC;
    g->band    = (float)CG_CompassBarBandPx(); // exactly the published value, so the DM box meets the band
    g->top     = CB_Round(0.004f * H * s);
    g->numPx   = 0.0160f * H * s;
    g->cardPx  = 0.0185f * H * s;
    g->smallPx = 0.0130f * H * s;
    g->labelH  = CB_Round(g->cardPx);
    g->labelCy = g->top + g->labelH * 0.5f;
    g->tickTop = g->top + g->labelH + 1.0f;
    g->edgeW   = 0.12f * g->half;
    g->clampPx = g->half - 0.012f * H * s;

    // the marker row: from the tick top to the band bottom, less one row for the dark rim, so the
    // rim's top row still lies below the label row
    avail     = g->band - 2.0f - g->tickTop;
    g->objPx  = (int)CB_Round(0.016f * H * s);
    g->matePx = (int)CB_Round(0.013f * H * s);
    if ((float)g->objPx > avail) {
        g->objPx = (int)avail;
    }
    if ((float)g->matePx > avail) {
        g->matePx = (int)avail;
    }

    g->tickW  = (int)CB_Round(0.0019f * H);
    g->minorH = (int)CB_Round(0.0045f * H * s);
    g->majorH = (int)CB_Round(0.0070f * H * s);
    g->cardH  = (int)CB_Round(0.0090f * H * s);
    if (g->tickW < 1) {
        g->tickW = 1;
    }
    if (g->minorH < 1) {
        g->minorH = 1;
    }
}

// smoothstep over the outer 12% of each half - there is no scissor, so the ends fade instead of clipping
static float CG_CompassBarEdge(const cbGeo_t *g, float x)
{
    float t = (g->half - CB_Abs(x - g->cx)) / g->edgeW;

    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    return t * t * (3.0f - 2.0f * t);
}

// One *white quad snapped to whole pixels. Adjacent quads that share a float edge round it identically,
// so the stepped band has no gaps or doubled columns.
static void CG_CompassBarQuad(qhandle_t hWhite, float x0, float y0, float x1, float y1)
{
    x0 = CB_Round(x0);
    x1 = CB_Round(x1);
    y0 = CB_Round(y0);
    y1 = CB_Round(y1);
    if (x1 <= x0) {
        x1 = x0 + 1.0f;
    }
    if (y1 <= y0) {
        return;
    }
    cgi.R_DrawStretchPic(x0, y0, x1 - x0, y1 - y0, 0.0f, 0.0f, 1.0f, 1.0f, hWhite);
}

// A marker as one quad per pixel row, cropped to the bar. grow widens every span and adds a row above
// and below, which is how the dark rim is drawn under the fill.
static void CG_CompassBarShape(qhandle_t hWhite, const cbGeo_t *g, int shape, float cx, float bottom, int size, float grow)
{
    float hw    = (float)size * 0.5f;
    float thick = (shape == CB_CHEVRON_HOLLOW) ? (float)size * 0.16f : (float)size * 0.30f;
    float lo    = g->cx - g->half;
    float hi    = g->cx + g->half;
    int   ig    = (int)grow;
    int   r;

    if (thick < ((shape == CB_CHEVRON_HOLLOW) ? 1.0f : 2.0f)) {
        thick = (shape == CB_CHEVRON_HOLLOW) ? 1.0f : 2.0f;
    }

    for (r = -ig; r < size + ig; r++) {
        float y  = bottom - (float)size + (float)r;
        float rc = (float)r + 0.5f; // row centre; the rim rows reuse the nearest real row
        float spans[4];
        int   n = 0, k;

        if (rc < 0.5f) {
            rc = 0.5f;
        } else if (rc > (float)size - 0.5f) {
            rc = (float)size - 0.5f;
        }

        switch (shape) {
        case CB_DIAMOND:
            {
                float w  = hw * (1.0f - CB_Abs(rc - hw) / hw);
                spans[0] = cx - w - grow;
                spans[1] = cx + w + grow;
                n        = 1;
            }
            break;
        case CB_CHEVRON:
        case CB_CHEVRON_HOLLOW:
            {
                // a "v": the arms start at the edges on the top row and meet at the bottom
                float o  = (hw - thick * 0.5f) * (1.0f - rc / (float)size);
                float th = thick * 0.5f + grow;
                if (o <= th) {
                    spans[0] = cx - o - th;
                    spans[1] = cx + o + th;
                    n        = 1;
                } else {
                    spans[0] = cx - o - th;
                    spans[1] = cx - o + th;
                    spans[2] = cx + o - th;
                    spans[3] = cx + o + th;
                    n        = 2;
                }
            }
            break;
        case CB_CARET_LEFT:
        case CB_CARET_RIGHT:
            {
                float w   = (float)size * 0.8f * (1.0f - CB_Abs(rc - hw) / hw);
                float tip = (shape == CB_CARET_LEFT) ? cx - (float)size * 0.4f : cx + (float)size * 0.4f;
                if (shape == CB_CARET_LEFT) {
                    spans[0] = tip - grow;
                    spans[1] = tip + w + grow;
                } else {
                    spans[0] = tip - w - grow;
                    spans[1] = tip + grow;
                }
                n = 1;
            }
            break;
        }

        for (k = 0; k < n; k++) {
            float x0 = spans[k * 2], x1 = spans[k * 2 + 1];
            if (x0 < lo) {
                x0 = lo;
            }
            if (x1 > hi) {
                x1 = hi;
            }
            if (x1 > x0) {
                CG_CompassBarQuad(hWhite, x0, y, x1, y + 1.0f);
            }
        }
    }
}

static float CG_CompassBarTextWidth(fontheader_t *font, const char *txt, float px)
{
    if (!font || !font->sgl[0] || font->sgl[0]->height <= 0.0f) {
        return 0.0f;
    }
    return (float)cgi.UI_FontStringWidth(font, txt, -1) * (px / font->sgl[0]->height);
}

// R_DrawString scales the glyphs AND the passed position by pvVirtualScreen (tr_font.cpp), so a uniform
// {k,k} with the position divided by k puts a string of real pixel height px exactly where asked -
// independent of the 640x480 virtual space and of cgs.uiHiResScale. align: -1 left, 0 centre, 1 right.
static void CG_CompassBarText(fontheader_t *font, const char *txt, float x, float cy, float px, int align, const vec4_t col)
{
    vec2_t k;
    vec4_t shadow;
    float  w, y;

    if (!font || !font->sgl[0] || font->sgl[0]->height <= 0.0f || col[3] <= 0.004f) {
        return;
    }
    k[0] = k[1] = px / font->sgl[0]->height;
    w           = (float)cgi.UI_FontStringWidth(font, txt, -1) * k[0];
    if (align == 0) {
        x -= w * 0.5f;
    } else if (align > 0) {
        x -= w;
    }
    x = CB_Round(x);
    y = CB_Round(cy - px * 0.5f);

    shadow[0] = shadow[1] = shadow[2] = 0.0f;
    shadow[3]                         = col[3] * 0.85f;
    cgi.R_SetColor(shadow);
    cgi.R_DrawString(font, txt, (x + 1.0f) / k[0], (y + 1.0f) / k[1], -1, k);
    cgi.R_SetColor(col);
    cgi.R_DrawString(font, txt, x / k[0], y / k[1], -1, k);
}

// STAT_OBJECTIVECENTER is VIEW-relative, in tenths: AngleSubtract(v_angle, toYaw(objective - centroid)) + 180
// (Player::UpdateStats). Undone against the SAME snapshot's viewangles, so it neither swims against the
// predicted view nor depends on the seat. Returns the objective's world yaw.
static qboolean CG_CompassBarObjectiveYaw(float *outYaw)
{
    const playerState_t *ps = &cg.snap->ps;
    int                  l  = ps->stats[STAT_OBJECTIVELEFT];
    int                  r  = ps->stats[STAT_OBJECTIVERIGHT];
    int                  c  = ps->stats[STAT_OBJECTIVECENTER];

    if (cg.ObjectivesCurrentIndex < 0 || cg.ObjectivesCurrentIndex >= MAX_OBJECTIVES) {
        return qfalse; // current_objectives 0 sends -1
    }
    if (l <= 0 || r <= 0 || c <= 0) {
        return qfalse; // not written by the server (clamped 1..3599 once it is)
    }
    // no location. A real location yields this exact triple only inside a <0.1 degree cone dead ahead
    // within ~227 units, so hiding on an exact match costs nothing
    if (l == CB_SENTINEL_L && r == CB_SENTINEL_R && c == CB_SENTINEL_C) {
        return qfalse;
    }
    *outYaw = AngleNormalize360(ps->viewangles[YAW] - ((float)c * 0.1f - 180.0f));
    return qtrue;
}

// LEFT/RIGHT sit (60 - atan(300 / dist)) degrees either side of CENTER, clamped to 7 when near, so the
// distance reads back. Resolution coarsens with range, which is why it is shown as "~N m".
static float CG_CompassBarDecodeDistance(qboolean *outHere)
{
    const playerState_t *ps  = &cg.snap->ps;
    int                  d   = (ps->stats[STAT_OBJECTIVERIGHT] - ps->stats[STAT_OBJECTIVELEFT] + 3600) % 3600;
    float                off = (float)d * 0.05f;
    float                a;

    *outHere = qfalse;
    if (off <= 7.05f) {
        *outHere = qtrue; // the spread is at its near clamp: "here", no number
        return 0.0f;
    }
    a = 60.0f - off;
    if (a < 0.05f) {
        a = 0.05f;
    }
    return 300.0f / (float)tan(DEG2RAD(a));
}

// Exact distance from the configstring loc, used only when it is provably the point the stat follows: the
// current index, not TOW/Liberation (per-team locations behind one shared configstring), and within 2
// degrees of the stat bearing. set_objective_pos maps leave loc stale, and the bearing test catches that.
static qboolean CG_CompassBarLocDistance(float objYaw, float *outDist, float *outDyaw)
{
    const cobjective_t *obj;
    vec3_t              delta;
    float               locYaw;

    if (cg.ObjectivesCurrentIndex < 0 || cg.ObjectivesCurrentIndex >= MAX_OBJECTIVES) {
        return qfalse;
    }
    obj = &cg.Objectives[cg.ObjectivesCurrentIndex];
    if (!obj->hasLoc || cgs.gametype >= GT_TOW) {
        return qfalse;
    }
    VectorSubtract(obj->loc, cg.snap->ps.origin, delta);
    locYaw   = (float)(atan2(delta[1], delta[0]) * (180.0 / M_PI));
    *outDyaw = AngleSubtract(locYaw, objYaw);
    *outDist = VectorLength(delta);
    return (*outDyaw >= -2.0f && *outDyaw <= 2.0f) ? qtrue : qfalse;
}

// 1 map unit = 1 inch
static void CG_CompassBarDistanceText(char *buf, int size, float units, qboolean exact)
{
    int m = (int)(units * 0.0254f + 0.5f);

    if (!exact && m >= 100) {
        m = ((m + 5) / 10) * 10;
    }
    Com_sprintf(buf, size, exact ? "%i m" : "~%i m", m);
}

static void CG_CompassBarAddMate(
    cbMate_t *list, int *count, const cbGeo_t *g, float camYaw, int clientNum, const float *me, float wx, float wy,
    float alphaMul, qboolean hollow
)
{
    float     yaw, deg, x;
    cbMate_t *m;

    if (*count >= MAX_CLIENTS || (wx == me[0] && wy == me[1])) {
        return;
    }
    yaw = (float)(atan2(wy - me[1], wx - me[0]) * (180.0 / M_PI));
    deg = AngleSubtract(yaw, camYaw); // engine yaw runs counter-clockwise: + is to the LEFT
    x   = g->cx - deg * g->ppd;
    if (CB_Abs(x - g->cx) > g->half) {
        return; // outside the arc: teammates are hidden, never pinned
    }
    m            = &list[(*count)++];
    m->clientNum = clientNum;
    m->x         = x;
    m->alpha     = alphaMul * CG_CompassBarEdge(g, x);
    m->absDeg    = CB_Abs(deg);
    m->hollow    = hollow;
}

// coop_compassProbe 1 - the values design section 7.2 step 2 needs, printed OUTSIDE every gate (TRAPS T14)
static void CG_CompassBarProbe(qboolean visible)
{
    static int           nextPrint = 0;
    const playerState_t *ps;
    char                 ages[320];
    float                camYaw, north, objYaw = 0.0f, decoded, locDist = -1.0f, locDyaw = 0.0f;
    qboolean             haveObj, here = qfalse, locOk = qfalse;
    int                  i, len = 0;

    if (!cg.snap) {
        return;
    }
    // cg.time runs backwards across a map load: a deadline more than one period ahead is stale
    if (cg.time < nextPrint && cg.time >= nextPrint - 1000) {
        return;
    }
    nextPrint = cg.time + 500;

    ps      = &cg.snap->ps;
    camYaw  = cg.refdefViewAngles[YAW];
    north   = AngleNormalize360((float)ps->stats[STAT_COMPASSNORTH] * (360.0f / 65536.0f));
    haveObj = CG_CompassBarObjectiveYaw(&objYaw);
    decoded = CG_CompassBarDecodeDistance(&here);
    if (haveObj) {
        locOk = CG_CompassBarLocDistance(objYaw, &locDist, &locDyaw);
    }

    cgi.Printf(
        "^~^~^ CBPROBE t=%d live=%d vis=%d pref=%d sess=%d exe=%d gt=%d fade=%.2f vid=%dx%d\n",
        cg.time,
        s_cbLive->integer,
        (int)visible,
        s_cbOn->integer,
        s_cbSession->integer,
        s_cbExe->integer,
        (int)cgs.gametype,
        s_hudFadeAlpha,
        cgs.glconfig.vidWidth,
        cgs.glconfig.vidHeight
    );
    cgi.Printf(
        "^~^~^ CBPROBE yaw cam=%.1f ps=%.1f camang=%.1f north=%.1f heading=%.1f\n",
        camYaw,
        ps->viewangles[YAW],
        cg.camera_angles[YAW],
        north,
        AngleNormalize360(north - camYaw)
    );
    cgi.Printf(
        "^~^~^ CBPROBE obj idx=%d L=%d R=%d C=%d have=%d objyaw=%.1f bearing=%.1f decoded=%.0fu here=%d loc=%.0fu dyaw=%.1f exact=%d\n",
        cg.ObjectivesCurrentIndex,
        ps->stats[STAT_OBJECTIVELEFT],
        ps->stats[STAT_OBJECTIVERIGHT],
        ps->stats[STAT_OBJECTIVECENTER],
        (int)haveObj,
        objYaw,
        haveObj ? AngleNormalize360(north - objYaw) : -1.0f,
        decoded,
        (int)here,
        locDist,
        locDyaw,
        (int)locOk
    );
    cgi.Printf(
        "^~^~^ CBPROBE state inzoom=%d health=%d vhealth=%d pmf=0x%x turret=%d camview=%d spec=%d nohud=%d letterbox=%d team=%d\n",
        ps->stats[STAT_INZOOM],
        ps->stats[STAT_HEALTH],
        ps->stats[STAT_VEHICLE_HEALTH],
        ps->pm_flags,
        (ps->pm_flags & PMF_TURRET) ? 1 : 0,
        (ps->pm_flags & PMF_CAMERA_VIEW) ? 1 : 0,
        (ps->pm_flags & PMF_SPECTATING) ? 1 : 0,
        (ps->pm_flags & PMF_NO_HUD) ? 1 : 0,
        ps->stats[STAT_LETTERBOX],
        (ps->clientNum >= 0 && ps->clientNum < MAX_CLIENTS) ? cg.clientinfo[ps->clientNum].team : -1
    );

    // radar capture: client:ageMs, "c" = clamped to the rim, "s" = also in this snapshot
    ages[0] = 0;
    for (i = 0; i < MAX_CLIENTS && len < (int)sizeof(ages) - 24; i++) {
        const compassMate_t *cm = &cg.compassMates[i];
        if (!cm->time) {
            continue;
        }
        len += (int)Com_sprintf(
            ages + len,
            sizeof(ages) - (size_t)len,
            " %d:%d%s%s",
            i,
            cg.time - cm->time,
            cm->clamped ? "c" : "",
            cg_entities[i].currentValid ? "s" : ""
        );
    }
    cgi.Printf(
        "^~^~^ CBPROBE radar range=%s mates%s\n",
        cgi.Cvar_Get("com_radar_range", "1024", 0)->string,
        len ? ages : " none"
    );
}

static void CG_DrawCompassBar(void)
{
    static qhandle_t         hWhite     = 0;
    static const char *const cardinal[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    static const vec3_t      cKhaki     = {0.93f, 0.90f, 0.80f};
    static const vec3_t      cRed       = {0.90f, 0.30f, 0.18f};
    static const vec3_t      cGold      = {1.00f, 0.82f, 0.25f};
    static const vec3_t      cMate      = {0.50f, 0.85f, 0.50f};
    cbGeo_t                  g;
    cbLabel_t                labels[16];
    cbMate_t                 mates[MAX_CLIENTS];
    qboolean                 inSnap[MAX_CLIENTS];
    vec4_t                   col;
    char                     buf[32], distTxt[32], nameTxt[32];
    int                      numLabels = 0, numMates = 0, labelStep, first, b, i, j, bestMate = -1, distAlign = -1;
    float                    H, s, camYaw, north, heading, opacity, fA2, tickA, markerA, plateHalf, gap;
    float                    objYaw = 0.0f, objX = 0.0f, objL = 0.0f, objR = 0.0f, distX = 0.0f;
    qboolean                 visible, objOn = qfalse, objClamped = qfalse;

    CG_CompassBarCvars();
    CG_CompassBarPublishLive();
    visible = CG_CompassBarVisible();
    if (s_cbProbe->integer) {
        CG_CompassBarProbe(visible);
    }
    if (!visible) {
        return;
    }

    // same clock as every other faded element, squared (bug-2555), hard early-out: nothing lingers
    opacity = CG_CompassBarClamp(s_cbOpacity->value, 0.3f, 1.0f);
    fA2     = s_hudFadeAlpha * s_hudFadeAlpha;
    tickA   = opacity * fA2;
    markerA = opacity * ((fA2 > CB_MARKER_FLOOR) ? fA2 : CB_MARKER_FLOOR);
    if (tickA <= 0.02f && markerA <= 0.02f) {
        return;
    }

    if (!hWhite) {
        hWhite = cgi.R_RegisterShaderNoMip("*white");
    }
    CG_CompassBarGeometry(&g);
    if (g.half < 8.0f) {
        return;
    }
    H   = (float)cgs.glconfig.vidHeight;
    s   = CG_CompassBarClamp(s_cbScale->value, 0.75f, 1.0f);
    gap = CB_Round(0.003f * H * s) + 1.0f;

    // refdefViewAngles is the final rendered camera (third person, free-cam, every seat). No smoothing:
    // the stock ring's spring would make a numeric readout lag and overshoot.
    camYaw  = cg.refdefViewAngles[YAW];
    north   = AngleNormalize360((float)cg.snap->ps.stats[STAT_COMPASSNORTH] * (360.0f / 65536.0f)); // a short on the wire
    heading = AngleNormalize360(north - camYaw); // compass bearing, clockwise

    //
    // band: black, with a stepped alpha ramp over the edge fade at both ends
    //
    col[0] = col[1] = col[2] = 0.0f;
    col[3]                   = 0.35f * opacity * fA2;
    if (col[3] > 0.004f) {
        float x0    = g.cx - g.half;
        float x1    = g.cx + g.half;
        float stepW = g.edgeW / 8.0f;

        cgi.R_SetColor(col);
        CG_CompassBarQuad(hWhite, x0 + stepW * 8.0f, g.top, x1 - stepW * 8.0f, g.band);
        for (i = 0; i < 8; i++) {
            float t = ((float)i + 0.5f) / 8.0f;
            col[3]  = 0.35f * opacity * fA2 * t * t * (3.0f - 2.0f * t);
            cgi.R_SetColor(col);
            CG_CompassBarQuad(hWhite, x0 + stepW * (float)i, g.top, x0 + stepW * (float)(i + 1), g.band);
            CG_CompassBarQuad(hWhite, x1 - stepW * (float)(i + 1), g.top, x1 - stepW * (float)i, g.band);
        }
    }

    //
    // ticks (hanging from the label row) and the labels to draw later
    //
    labelStep = (15.0f * g.ppd >= 3.2f * g.numPx) ? 15 : 30; // 30 only where 15 would crowd (small 4:3 modes)
    plateHalf = 1.6f * g.numPx;
    first     = (int)floor((heading - CB_ARC * 0.5f) / 5.0f) * 5;
    for (b = first; (float)b <= heading + CB_ARC * 0.5f; b += 5) {
        int        bb = ((b % 360) + 360) % 360;
        float      dx = AngleSubtract((float)bb, heading) * g.ppd;
        float      x, ea, w;
        int        h;
        cbLabel_t *lab;

        if (CB_Abs(dx) > g.half) {
            continue;
        }
        x  = g.cx + dx;
        ea = CG_CompassBarEdge(&g, x);
        if (tickA * ea <= 0.004f) {
            continue;
        }

        VectorCopy((bb == 0) ? cRed : cKhaki, col);
        col[3] = tickA * ea;
        cgi.R_SetColor(col);
        h = (bb % 45 == 0) ? g.cardH : ((bb % 15 == 0) ? g.majorH : g.minorH);
        CG_CompassBarQuad(hWhite, x - (float)g.tickW * 0.5f, g.tickTop, x + (float)g.tickW * 0.5f, g.tickTop + (float)h);

        if (numLabels >= (int)(sizeof(labels) / sizeof(labels[0]))) {
            continue;
        }
        lab = &labels[numLabels];
        if (bb % 45 == 0) {
            Q_strncpyz(lab->txt, cardinal[bb / 45], sizeof(lab->txt));
            lab->font = (bb % 90 == 0) ? cgs.media.attackerFont : cgs.media.hudDrawFont;
            lab->px   = (bb % 90 == 0) ? g.cardPx : g.numPx;
        } else if (bb % labelStep == 0) {
            Com_sprintf(lab->txt, sizeof(lab->txt), "%i", bb);
            lab->font = cgs.media.hudDrawFont;
            lab->px   = g.numPx;
        } else {
            continue;
        }
        w = CG_CompassBarTextWidth(lab->font, lab->txt, lab->px);
        if (x + w * 0.5f > g.cx - plateHalf - 3.0f && x - w * 0.5f < g.cx + plateHalf + 3.0f) {
            continue; // under the heading plate
        }
        lab->x     = x;
        lab->alpha = col[3];
        lab->north = (bb == 0) ? qtrue : qfalse;
        numLabels++;
    }

    // exact-centre accent, in the tick row
    VectorCopy(cRed, col);
    col[3] = tickA;
    cgi.R_SetColor(col);
    CG_CompassBarQuad(hWhite, g.cx - 1.0f, g.tickTop, g.cx + 1.0f, g.tickTop + CB_Round(0.012f * H * s));

    // heading plate, in the label row
    col[0] = col[1] = col[2] = 0.0f;
    col[3]                   = 0.55f * opacity * fA2;
    cgi.R_SetColor(col);
    CG_CompassBarQuad(hWhite, g.cx - plateHalf, g.top, g.cx + plateHalf, g.top + g.labelH);
    VectorCopy(cKhaki, col);
    col[3] = 0.45f * tickA;
    cgi.R_SetColor(col);
    CG_CompassBarQuad(hWhite, g.cx - plateHalf, g.top, g.cx + plateHalf, g.top + 1.0f);
    CG_CompassBarQuad(hWhite, g.cx - plateHalf, g.top + g.labelH - 1.0f, g.cx + plateHalf, g.top + g.labelH);
    CG_CompassBarQuad(hWhite, g.cx - plateHalf, g.top, g.cx - plateHalf + 1.0f, g.top + g.labelH);
    CG_CompassBarQuad(hWhite, g.cx + plateHalf - 1.0f, g.top, g.cx + plateHalf, g.top + g.labelH);

    //
    // current objective: pinned to the nearer end (as a caret) when it is outside the arc
    //
    distTxt[0] = 0;
    if (s_cbObj->integer && g.objPx >= 3 && markerA > 0.02f && CG_CompassBarObjectiveYaw(&objYaw)) {
        float    half = (float)g.objPx * 0.5f;
        float    dx   = -AngleSubtract(objYaw, camYaw) * g.ppd;
        float    dist, locDist = 0.0f, locDyaw = 0.0f;
        qboolean exact, here;

        objOn = qtrue;
        if (CB_Abs(dx) > g.clampPx) {
            objClamped = qtrue;
            dx         = (dx < 0.0f) ? -g.clampPx : g.clampPx;
        }
        objX = g.cx + dx;
        objL = objX - half;
        objR = objX + half;

        dist  = CG_CompassBarDecodeDistance(&here);
        exact = CG_CompassBarLocDistance(objYaw, &locDist, &locDyaw);
        if (exact) {
            dist = locDist;
        }
        if (exact || !here) {
            CG_CompassBarDistanceText(distTxt, sizeof(distTxt), dist, exact);
        }
        if (distTxt[0]) {
            float tw = CG_CompassBarTextWidth(cgs.media.hudDrawFont, distTxt, g.smallPx);
            // beside the marker in the marker row; on the inner side when pinned right or out of room
            if ((objClamped && dx > 0.0f) || (!objClamped && objR + gap + tw > g.cx + g.half)) {
                distAlign = 1;
                distX     = objL - gap;
                objL      = distX - tw;
            } else {
                distAlign = -1;
                distX     = objR + gap;
                objR      = distX + tw;
            }
        }
    }

    //
    // teammates, same team only: exact from the snapshot, else from the raw radar capture (cg_radar.cpp)
    //
    if (s_cbMates->integer && g.matePx >= 3 && markerA > 0.02f && cgs.gametype >= GT_TEAM
        && cg.snap->ps.clientNum >= 0 && cg.snap->ps.clientNum < MAX_CLIENTS) {
        int          local  = cg.snap->ps.clientNum;
        int          myTeam = cg.clientinfo[local].team;
        const float *me     = cg.snap->ps.origin;

        if (myTeam >= TEAM_ALLIES) {
            memset(inSnap, 0, sizeof(inSnap));
            for (i = 0; i < cg.snap->numEntities; i++) {
                const entityState_t *es = &cg.snap->entities[i];
                int                  n  = es->number;

                if (n < 0 || n >= MAX_CLIENTS || n == local || es->eType != ET_PLAYER) {
                    continue;
                }
                inSnap[n] = qtrue;
                if ((es->eFlags & EF_DEAD) || cg.clientinfo[n].team != myTeam) {
                    continue;
                }
                CG_CompassBarAddMate(
                    mates, &numMates, &g, camYaw, n, me, cg_entities[n].lerpOrigin[0], cg_entities[n].lerpOrigin[1],
                    1.0f, qfalse
                );
            }
            for (i = 0; i < MAX_CLIENTS; i++) {
                const compassMate_t *cm  = &cg.compassMates[i];
                int                  age = cg.time - cm->time;
                float                ageA;

                if (i == local || inSnap[i] || !cm->time || age < 0 || age >= CB_MATE_MAX_AGE) {
                    continue;
                }
                if (cg.clientinfo[i].team != myTeam || !cg.clientinfo[i].name[0]) {
                    continue;
                }
                ageA = (age <= CB_MATE_FADE_AGE)
                         ? 1.0f
                         : 1.0f - (float)(age - CB_MATE_FADE_AGE) / (float)(CB_MATE_MAX_AGE - CB_MATE_FADE_AGE);
                CG_CompassBarAddMate(mates, &numMates, &g, camYaw, i, me, cm->origin[0], cm->origin[1], 0.6f * ageA, cm->clamped);
            }
        }
    }

    // farthest from centre first, so the one nearest the centre is drawn on top
    for (i = 1; i < numMates; i++) {
        cbMate_t m = mates[i];
        for (j = i - 1; j >= 0 && mates[j].absDeg < m.absDeg; j--) {
            mates[j + 1] = mates[j];
        }
        mates[j + 1] = m;
    }
    for (i = 0; i < numMates; i++) {
        int shape = mates[i].hollow ? CB_CHEVRON_HOLLOW : CB_CHEVRON;
        col[0] = col[1] = col[2] = 0.0f;
        col[3]                   = markerA * mates[i].alpha * 0.85f;
        cgi.R_SetColor(col);
        CG_CompassBarShape(hWhite, &g, shape, mates[i].x, g.band - 1.0f, g.matePx, 1.0f);
        VectorCopy(cMate, col);
        col[3] = markerA * mates[i].alpha;
        cgi.R_SetColor(col);
        CG_CompassBarShape(hWhite, &g, shape, mates[i].x, g.band - 1.0f, g.matePx, 0.0f);
        if (mates[i].absDeg <= CB_NAME_DEG && (bestMate < 0 || mates[i].absDeg < mates[bestMate].absDeg)) {
            bestMate = i;
        }
    }

    // the current objective is drawn over every teammate
    if (objOn) {
        float a     = markerA * (objClamped ? 0.8f : 1.0f);
        int   shape = objClamped ? ((objX < g.cx) ? CB_CARET_LEFT : CB_CARET_RIGHT) : CB_DIAMOND;

        col[0] = col[1] = col[2] = 0.0f;
        col[3]                   = a * 0.85f;
        cgi.R_SetColor(col);
        CG_CompassBarShape(hWhite, &g, shape, objX, g.band - 1.0f, g.objPx, 1.0f);
        VectorCopy(cGold, col);
        col[3] = a;
        cgi.R_SetColor(col);
        CG_CompassBarShape(hWhite, &g, shape, objX, g.band - 1.0f, g.objPx, 0.0f);
    }

    //
    // text last (every R_DrawString flushes; at most ~28 calls a frame)
    //
    for (i = 0; i < numLabels; i++) {
        VectorCopy(labels[i].north ? cRed : cKhaki, col);
        col[3] = labels[i].alpha;
        CG_CompassBarText(labels[i].font, labels[i].txt, labels[i].x, g.labelCy, labels[i].px, 0, col);
    }

    Com_sprintf(buf, sizeof(buf), "%03i", ((int)(heading + 0.5f)) % 360);
    col[0] = col[1] = col[2] = 1.0f;
    col[3]                   = tickA;
    CG_CompassBarText(cgs.media.hudDrawFont, buf, g.cx, g.labelCy, g.numPx, 0, col);

    if (objOn && distTxt[0]) {
        VectorCopy(cGold, col);
        col[3] = markerA * (objClamped ? 0.8f : 1.0f);
        CG_CompassBarText(
            cgs.media.hudDrawFont, distTxt, distX, g.band - 1.0f - (float)g.objPx * 0.5f, g.smallPx, distAlign, col
        );
    }

    // one name, for the teammate nearest the centre, and only where it does not run into the objective
    if (bestMate >= 0) {
        const cbMate_t *m  = &mates[bestMate];
        float           tw, x0, x1, nx;
        int             align = -1;

        Q_strncpyz(nameTxt, cg.clientinfo[m->clientNum].name, sizeof(nameTxt));
        tw = CG_CompassBarTextWidth(cgs.media.hudDrawFont, nameTxt, g.smallPx);
        nx = m->x + (float)g.matePx * 0.5f + gap;
        if (nx + tw > g.cx + g.half) {
            align = 1;
            nx    = m->x - (float)g.matePx * 0.5f - gap;
        }
        x0 = (align < 0) ? nx : nx - tw;
        x1 = x0 + tw;
        if (!objOn || x1 < objL - 3.0f || x0 > objR + 3.0f) {
            VectorCopy(cMate, col);
            col[3] = markerA * m->alpha;
            CG_CompassBarText(
                cgs.media.hudDrawFont, nameTxt, nx, g.band - 1.0f - (float)g.matePx * 0.5f, g.smallPx, align, col
            );
        }
    }

    cgi.R_SetColor(NULL);
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

/*
=================================================================================================
HZM coop [user 2026-08-21] DIRECTIONAL DAMAGE INDICATOR.

The data was already on the wire and unused. STAT_DAMAGEDIR is written by Player::Pain
(player.cpp:3704) as Vector::toYaw() * 10 - a WORLD yaw in tenths of a degree - and published every
frame by Player::UpdateStats. Nothing client-side has ever read it. There is even a deliberate +-1
nudge in Player::Pain so that a repeat hit from an identical bearing still registers as a CHANGE,
which is direct evidence the field was designed for a consumer that was never written.

That nudge is exact, not approximate: toYaw() truncates to whole degrees before the x10, so the stat
is always a multiple of 10 and the equality test at player.cpp:3706 cannot be defeated by float
error. Repeat hits alternate B <-> B+1. So triggering on CHANGE is sound; triggering on "non-zero"
would not be, because damage_yaw is latched and never decayed.

FOUR THINGS THAT WOULD OTHERWISE MAKE THIS LIE, all found in review before shipping:

 1. VEHICLES PUBLISH A DIFFERENT UNIT. Vehicle::EventDamage (vehicle.cpp:6023) writes
    AngleSubtract(camera_yaw, dir_yaw) + 180.5 - whole DEGREES, already VIEW-RELATIVE, and skipping
    the nudge. Reading that as tenths collapses every bearing into a 36 degree smear and then
    subtracts the view yaw a second time. A passenger still goes through Player::Pain, so the two
    encodings can interleave on the same stat frame to frame. Suppressed entirely while the player
    is glued to a vehicle or a turret.

 2. FALL / DROWN / SCRIPTED DAMAGE HAS NO BEARING. player.cpp:7111 passes vec_zero as the direction,
    and Vector::toYaw() returns 0.0 for a zero vector, so every fall would draw a confident arrow at
    world yaw 180. A stat of exactly 0 is therefore treated as "no bearing" and draws nothing.

 3. THE SCREEN SENSE IS MIRRORED. MOHAA yaw is counter-clockwise-positive; screen angles are
    clockwise-positive. Worked example: player at yaw 0, attacker at +Y (yaw 90) -> damage travels
    -Y -> dirYaw 270 -> AngleSubtract(270+180, 0) = +90, which Player::Pain itself classifies as
    PAIN_LEFT. Feeding +90 straight to a screen angle puts the wedge on the RIGHT. Negated below.

 4. IT MUST NOT FIRE WHILE SPECTATING. CG_Draw2D has no gate of its own, and while following another
    player cg.snap->ps is THEIR playerstate - so their hits would drive an indicator measured against
    OUR view angles. The predicate is copied from cg_view.c, which carries the warning that
    'health <= 0' alone is not enough because Player::Spectator() leaves health at max.

Drawn from small quads on the "*white" handle rather than an art asset: there is no rotated-2D
primitive in the cgame import table (R_DrawStretchPic and R_DrawBox are all there is), so a rotating
wedge would otherwise need a pre-rotated sprite sheet.
=================================================================================================
*/
#define COOP_DMGIND_SLOTS 4

static void CG_DrawDamageIndicator(void)
{
    static cvar_t *pOn = NULL, *pTime = NULL, *pRadius = NULL;
    static int     s_lastDir  = -1;
    static int     s_slotDir[COOP_DMGIND_SLOTS];
    static int     s_slotTime[COOP_DMGIND_SLOTS];
    static int     s_slotNext = 0;
    static qboolean s_init = qfalse;
    static qhandle_t hWhite = 0;
    playerState_t *ps;
    int            i, dir, life;
    float          cx, cy, rad;

    if (!pOn)     { pOn     = cgi.Cvar_Get("coop_dmgIndicator", "1", CVAR_ARCHIVE); }
    if (!pTime)   { pTime   = cgi.Cvar_Get("coop_dmgIndicatorTime", "1200", CVAR_ARCHIVE); }
    if (!pRadius) { pRadius = cgi.Cvar_Get("coop_dmgIndicatorRadius", "0.17", CVAR_ARCHIVE); }

    if (!s_init) {
        for (i = 0; i < COOP_DMGIND_SLOTS; i++) { s_slotDir[i] = -1; s_slotTime[i] = 0; }
        s_init = qtrue;
    }
    if (!hWhite) { hWhite = cgi.R_RegisterShaderNoMip("*white"); }
    if (pOn->integer <= 0 || !cg.snap) {
        return;
    }
    ps = &cg.snap->ps;

    // ---- gate (fix 4, plus fix 1) ----------------------------------------------------------
    // Re-seed the change detector whenever the gate is shut, so re-entering does not fire on the
    // delta that accumulated while we were not looking.
    if (ps->stats[STAT_HEALTH] <= 0
        || (ps->pm_flags & (PMF_SPECTATING | PMF_INTERMISSION | PMF_CAMERA_VIEW | PMF_TURRET
                            | PMF_NO_MOVE | PMF_FROZEN))) {
        s_lastDir = ps->stats[STAT_DAMAGEDIR];
        for (i = 0; i < COOP_DMGIND_SLOTS; i++) { s_slotDir[i] = -1; }
        return;
    }

    // ---- edge detect ------------------------------------------------------------------------
    dir = ps->stats[STAT_DAMAGEDIR];
    if (dir != s_lastDir) {
        s_lastDir = dir;
        if (dir != 0) {                      // fix 2: 0 means "no bearing", not "due south"
            s_slotDir[s_slotNext]  = dir;
            s_slotTime[s_slotNext] = cg.time;
            s_slotNext = (s_slotNext + 1) % COOP_DMGIND_SLOTS;
        }
    }

    life = pTime->integer;
    if (life < 100) { life = 100; }

    cx  = cgs.glconfig.vidWidth * 0.5f;
    cy  = cgs.glconfig.vidHeight * 0.5f;
    rad = cgs.glconfig.vidHeight * pRadius->value;

    for (i = 0; i < COOP_DMGIND_SLOTS; i++) {
        float  age, a, rel, mid, step;
        vec4_t col;
        int    seg;

        if (s_slotDir[i] < 0) {
            continue;
        }
        age = (float)(cg.time - s_slotTime[i]) / (float)life;
        if (age >= 1.0f || age < 0.0f) {
            s_slotDir[i] = -1;
            continue;
        }
        a = 1.0f - age;
        a *= a;                              // ease out: hold, then fade

        // world bearing of the ATTACKER: the stat carries the direction the damage TRAVELLED, so
        // +180 turns it back toward the source (the same convention Player::Pain uses before it
        // classifies front/left/right/rear). Then subtract our own yaw, and NEGATE for screen
        // handedness (fix 3).
        rel = (float)s_slotDir[i] * 0.1f + 180.0f - cg.refdefViewAngles[YAW];
        while (rel > 180.0f)  { rel -= 360.0f; }
        while (rel < -180.0f) { rel += 360.0f; }
        mid = -rel * (float)(M_PI / 180.0);

        col[0] = 0.85f;
        col[1] = 0.08f;
        col[2] = 0.06f;
        col[3] = a * 0.85f;
        cgi.R_SetColor(col);

        // a ~44 degree arc built from 11 short quads - thicker in the middle so it reads as a wedge
        step = (float)(M_PI / 180.0) * 4.0f;
        for (seg = -5; seg <= 5; seg++) {
            float ang = mid + seg * step;
            float t   = 1.0f - (float)(seg < 0 ? -seg : seg) / 6.0f;   // 1 at centre
            float w   = 3.0f + 5.0f * t;
            float px  = cx + (float)sin(ang) * rad;
            float py  = cy - (float)cos(ang) * rad;
            cgi.R_DrawStretchPic(px - w * 0.5f, py - w * 0.5f, w, w, 0, 0, 1, 1,
                                 hWhite);
        }
    }
    cgi.R_SetColor(NULL);
}

// HZM coop [user 2026-08-27] BRACE PIP - the discoverability story. Without an indicator a real
// share of players never work out the system exists and just think the gun feels inconsistent.
//
// DRAWN PROCEDURALLY, NOT FROM A TEXTURE, and that is the whole "ultra high def" answer: a texture
// has one native size and is resampled at every other resolution, so it is soft at 1440p and mush on
// a 4K ultrawide. Filled rects have no native size at all - every edge lands on a real pixel boundary
// at any resolution, so the mark is exactly as sharp at 3440x1440 as at 640x480. Geometry scales off
// screen HEIGHT (never a fixed pixel count) so it holds the same visual weight on any monitor, and
// the thickness is rounded to whole pixels so no edge ever half-covers one and greys itself.
//
// Four short ticks bracketing the crosshair, opening outward as the brace engages: it reads as the
// weapon settling into a rest, and it cannot be mistaken for the crosshair itself.
// HZM coop [user 2026-08-28] HIT MARKERS.
//
// Retail ships NO hit-marker art - a sweep of all 17 retail paks found only a 16x16 crosshair. So this
// is procedural, like the brace pip beside it: four diagonal arms stepped out of small filled boxes,
// which needs no texture, no shader registration and no asset that could go missing from a pak.
//
// Neutral for a hit, RED for a kill - the convention players already read fluently. The kill also holds
// longer, so the two differ in duration as well as colour and stay distinguishable for a colour-blind
// player. Radius is deliberately INSIDE the brace pip's 0.020-0.028 band so the two never collide.
void CG_CoopHitMark(qboolean bKill)
{
    // a kill outranks a hit landing in the same instant - never let the plain tick mask a confirmed kill
    if (bKill || !s_coopHitKill || cg.time - s_coopHitTime > 60) {
        s_coopHitKill = bKill;
    }
    s_coopHitTime = cg.time;
}

static void CG_DrawHitMarker(void)
{
    static cvar_t *pOn = NULL;
    float          cx, cy, gap, len, th, a, frac;
    int            life, age, i, steps;
    vec4_t         col;

    if (!pOn) { pOn = cgi.Cvar_Get("coop_hitMarker", "1", CVAR_ARCHIVE); }
    if (!pOn->integer || !s_coopHitTime) { return; }

    life = s_coopHitKill ? 420 : 260;
    age  = cg.time - s_coopHitTime;
    if (age < 0 || age > life) { return; }

    frac = 1.0f - (float)age / (float)life;
    a    = frac * frac;   // hold bright, then leave quickly - a linear fade reads as a smear

    // anchor to the real aim point; fall back to centre only if the crosshair is not being drawn
    if (s_coopAimSet && cg.time - s_coopAimSet < 500) {
        cx = s_coopAimX; cy = s_coopAimY;
    } else {
        cx = cgs.glconfig.vidWidth * 0.5f; cy = cgs.glconfig.vidHeight * 0.5f;
    }

    th  = cgs.glconfig.vidHeight * 0.0028f;
    if (th < 1.0f) { th = 1.0f; }
    th  = (float)(int)(th + 0.5f);          // whole pixels - a half-covered edge reads grey, not thin
    gap = cgs.glconfig.vidHeight * 0.008f;
    len = cgs.glconfig.vidHeight * 0.012f;
    gap += len * (1.0f - frac) * 0.35f;     // the arms drift outward as it fades, so it reads as a pop

    if (s_coopHitKill) { col[0] = 1.0f;  col[1] = 0.22f; col[2] = 0.17f; }
    else               { col[0] = 1.0f;  col[1] = 1.0f;  col[2] = 1.0f;  }
    col[3] = a;
    cgi.R_SetColor(col);

    // R_DrawBox is axis-aligned, so the diagonals are stepped. At this size the steps are sub-pixel
    // dense and read as clean strokes; a rotated texture would need an asset this game does not have.
    steps = (int)(len / (th * 0.7f));
    if (steps < 3)  { steps = 3; }
    if (steps > 24) { steps = 24; }
    for (i = 0; i <= steps; i++) {
        float d  = gap + (len * (float)i / (float)steps);
        float dx = d * 0.7071f;
        cgi.R_DrawBox(cx - dx - th * 0.5f, cy - dx - th * 0.5f, th, th);
        cgi.R_DrawBox(cx + dx - th * 0.5f, cy - dx - th * 0.5f, th, th);
        cgi.R_DrawBox(cx - dx - th * 0.5f, cy + dx - th * 0.5f, th, th);
        cgi.R_DrawBox(cx + dx - th * 0.5f, cy + dx - th * 0.5f, th, th);
    }
    cgi.R_SetColor(NULL);
}


static void CG_DrawBracePip(void)
{
    extern float CG_CoopBrace(void);
    float  env = CG_CoopBrace();
    float  cx, cy, len, thick, gap, a;
    vec4_t col;

    qboolean avail = CG_CoopBraceAvail();

    if (env <= 0.004f && !avail) {
        return;
    }

    // [user 2026-08-27] THE MOUNT PROMPT - a warm bracket that pulses while a surface is on offer,
    // replaced by the cool solid mark once Use commits. This is the half that was missing: with
    // nothing on screen there was no way to know a mount was even available, which is most of why
    // the automatic version read as invisible - "its hard to really tell youre actually braced".
    if (avail && env <= 0.5f) {
        float pcx   = cgs.glconfig.vidWidth * 0.5f;
        float pcy   = cgs.glconfig.vidHeight * 0.5f;
        float plen  = cgs.glconfig.vidHeight * 0.020f;
        float pth   = cgs.glconfig.vidHeight * 0.0026f;
        float pgap  = cgs.glconfig.vidHeight * 0.034f;
        float pulse = 0.45f + 0.28f * (float)sin((double)cg.time * 0.006);
        vec4_t pc;
        if (pth < 1.0f) { pth = 1.0f; }
        pth = (float)(int)(pth + 0.5f);
        pc[0] = 1.0f; pc[1] = 0.90f; pc[2] = 0.55f; pc[3] = pulse;
        cgi.R_SetColor(pc);
        cgi.R_DrawBox(pcx - pgap,        pcy - pgap,        plen, pth);
        cgi.R_DrawBox(pcx - pgap,        pcy - pgap,        pth,  plen);
        cgi.R_DrawBox(pcx + pgap - plen, pcy - pgap,        plen, pth);
        cgi.R_DrawBox(pcx + pgap - pth,  pcy - pgap,        pth,  plen);
        cgi.R_DrawBox(pcx - pgap,        pcy + pgap - pth,  plen, pth);
        cgi.R_DrawBox(pcx - pgap,        pcy + pgap - plen, pth,  plen);
        cgi.R_DrawBox(pcx + pgap - plen, pcy + pgap - pth,  plen, pth);
        cgi.R_DrawBox(pcx + pgap - pth,  pcy + pgap - plen, pth,  plen);
        cgi.R_SetColor(NULL);
    }

    if (env <= 0.004f) {
        return;
    }

    cx    = cgs.glconfig.vidWidth  * 0.5f;
    cy    = cgs.glconfig.vidHeight * 0.5f;
    len   = cgs.glconfig.vidHeight * 0.020f;   // ~23px at 1440p, scales with the display
    thick = cgs.glconfig.vidHeight * 0.0030f;  // ~3px at 1440p
    if (thick < 1.0f) { thick = 1.0f; }
    thick = (float)(int)(thick + 0.5f);        // whole pixels only - a half-covered edge reads grey
    if (len < 4.0f) { len = 4.0f; }

    // the ticks slide outward as the brace takes hold, so the motion itself carries the state
    gap = cgs.glconfig.vidHeight * (0.020f + 0.008f * env);
    a   = 0.45f + 0.50f * env;

    // cool + solid, so MOUNTED can never be mistaken for the warm pulsing offer
    col[0] = 0.72f; col[1] = 0.94f; col[2] = 1.0f; col[3] = a;
    cgi.R_SetColor(col);
    // left / right verticals
    cgi.R_DrawBox(cx - gap - thick, cy - len * 0.5f, thick, len);
    cgi.R_DrawBox(cx + gap,         cy - len * 0.5f, thick, len);
    // top / bottom horizontals
    cgi.R_DrawBox(cx - len * 0.5f, cy - gap - thick, len, thick);
    cgi.R_DrawBox(cx - len * 0.5f, cy + gap,         len, thick);
    cgi.R_SetColor(NULL);
}

void CG_Draw2D(void)
{
    CG_UpdateHudFade();
    CG_UpdateCountdown();
    CG_DrawGl2PostFxFallback(); // HZM gl2 re-port: under the HUD, over the scene (gl2 only)
    CG_DrawZoomOverlay();
    CG_DrawAdsVignette();
    CG_DrawLagometer();
    // HZM coop [user 2026-09-13] top compass bar: over the ADS vignette, UNDER script huddraw, so full-screen
    // script dims (Service Record panel, e3l4 curtain) still cover it
    CG_DrawCompassBar();
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
    // HZM coop [user 2026-09-03] "ensure crosshair is off during the full intro and cinematic".
    // All three go together: the brace pip and the hit marker are drawn AT the crosshair's aim
    // point and read as part of it. The accessor must be called every frame regardless of the
    // outcome - it is what consumes the cvar and ages the deadline - so it stays the condition,
    // never something short-circuited away.
    if (!CG_CoopCineHudActive()) {
        CG_DrawCrosshair();
        CG_DrawBracePip();      // HZM coop - gun-brace indicator, drawn over the crosshair
        CG_DrawHitMarker();     // HZM coop - hit/kill confirmation at the true aim point
    }
    CG_DrawDamageIndicator();
    CG_DrawCoopIcons();
    CG_DrawMGHeat();
    CG_DrawStaminaArc();
    CG_DrawMagazines();
    CG_SeedAdsTuneFromBaked(); // seed live cvars from the baked table so tune mode doesn't snap the gun
    CG_DrawAdsTune();
}
