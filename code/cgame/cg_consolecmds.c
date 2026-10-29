/*
===========================================================================
Copyright (C) 2008-2024 the OpenMoHAA team

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

//
// DESCRIPTION:
// text commands typed in at the local console, or executed by a key binding

#include "cg_local.h"
#include "../client/keycodes.h"
#include "../fgame/bg_voteoptions.h"

void CG_TargetCommand_f(void);

/*
=================
CG_SizeUp_f

Keybinding command
=================
*/
static void CG_SizeUp_f(void)
{
    cgi.Cvar_Set("viewsize", va("%i", (int)(cg_viewsize->integer + 10)));
}

/*
=================
CG_SizeDown_f

Keybinding command
=================
*/
static void CG_SizeDown_f(void)
{
    cgi.Cvar_Set("viewsize", va("%i", (int)(cg_viewsize->integer - 10)));
}

/*
=============
CG_Viewpos_f

Debugging command to print the current position
=============
*/
static void CG_Viewpos_f(void)
{
    cgi.Printf(
        "(%i %i %i) : %i\n",
        (int)cg.refdef.vieworg[0],
        (int)cg.refdef.vieworg[1],
        (int)cg.refdef.vieworg[2],
        (int)cg.refdefViewAngles[YAW]
    );
}

void CG_SetDesiredObjectiveAlpha(float fAlpha)
{
    cg.ObjectivesDesiredAlpha = fAlpha;
    cg.ObjectivesAlphaTime    = (float)(cg.time + 250);
    cg.ObjectivesBaseAlpha    = cg.ObjectivesCurrentAlpha;
}

void CG_ScoresDown_f(void)
{
    if (cgs.gametype == GT_SINGLE_PLAYER) {
        if (!cg.scoresRequestTime) {
            cg.scoresRequestTime = cg.time;
            CG_SetDesiredObjectiveAlpha(1.0f);
        }

        return;
    }

    if (cg.scoresRequestTime + 2000 >= cg.time) {
        // send another request
        cg.showScores = qtrue;
        CG_PrepScoreBoardInfo();
        cgi.UI_ShowScoreBoard(cg.scoresMenuName);
        return;
    }

    cg.scoresRequestTime = cg.time;
    cgi.SendClientCommand("score");

    if (!cg.showScores) {
        // don't display anything until first score returns
        cg.showScores = qtrue;
        CG_PrepScoreBoardInfo();
        cgi.UI_ShowScoreBoard(cg.scoresMenuName);
    }
}

void CG_ScoresUp_f(void)
{
    if (cgs.gametype == GT_SINGLE_PLAYER) {
        if (cg.scoresRequestTime) {
            cg.scoresRequestTime = 0;
            CG_SetDesiredObjectiveAlpha(0.0f);
        }

        return;
    }

    if (!cg.showScores) {
        return;
    }

    cg.showScores = qfalse;
    cgi.UI_HideScoreBoard();
}

baseshader_t *CG_GetShaderUnderCrosshair(qboolean bVerbose, trace_t *pRetTrace)
{
    vec3_t        vPos, vEnd;
    vec3_t        axis[3];
    baseshader_t *pShader;
    trace_t       trace;

    AnglesToAxis(cg.refdefViewAngles, axis);
    vPos[0] = cg.refdef.vieworg[0];
    vPos[1] = cg.refdef.vieworg[1];
    vPos[2] = cg.refdef.vieworg[2];
    VectorMA(vPos, 4096.0, axis[0], vEnd);

    CG_Trace(&trace, vPos, vec3_origin, vec3_origin, vEnd, 0, MASK_SHOT, 0, 0, "CG_GetShaderUnderCrosshair");

    if (trace.startsolid || trace.fraction == 1.0) {
        return NULL;
    }

    if (bVerbose) {
        cgi.Printf("Surface hit at (%i %i %i)\n", (int)trace.endpos[0], (int)trace.endpos[1], (int)trace.endpos[2]);
    }

    pShader = cgi.GetShader(trace.shaderNum);
    if (pRetTrace) {
        *pRetTrace = trace;
    }

    return pShader;
}

static void CG_PrintContentTypes(int iContentFlags)
{
    if (iContentFlags & CONTENTS_SOLID) {
        cgi.Printf(" solid");
    }
    if (iContentFlags & CONTENTS_LAVA) {
        cgi.Printf(" lava");
    }
    if (iContentFlags & CONTENTS_SLIME) {
        cgi.Printf(" slime");
    }
    if (iContentFlags & CONTENTS_WATER) {
        cgi.Printf(" water");
    }
    if (iContentFlags & CONTENTS_FOG) {
        cgi.Printf(" fog");
    }
    if (iContentFlags & CONTENTS_FENCE) {
        cgi.Printf(" fence");
    }
    if (iContentFlags & CONTENTS_AREAPORTAL) {
        cgi.Printf(" areaportal");
    }
    if (iContentFlags & CONTENTS_PLAYERCLIP) {
        cgi.Printf(" playerclip");
    }
    if (iContentFlags & CONTENTS_VEHICLECLIP) {
        cgi.Printf(" vehicleclip");
    }
    if (iContentFlags & CONTENTS_MONSTERCLIP) {
        cgi.Printf(" monsterclip");
    }
    if (iContentFlags & CONTENTS_WEAPONCLIP) {
        cgi.Printf(" weaponclip");
    }
    if (iContentFlags & CONTENTS_SHOOTONLY) {
        cgi.Printf(" shootableonly");
    }
    if (iContentFlags & CONTENTS_ORIGIN) {
        cgi.Printf(" origin");
    }
    if (iContentFlags & CONTENTS_TRANSLUCENT) {
        cgi.Printf(" trans");
    }
}

static void CG_PrintSurfaceProperties(int iSurfaceFlags)
{
    if (iSurfaceFlags & SURF_NODAMAGE) {
        cgi.Printf(" nodamage");
    }
    if (iSurfaceFlags & SURF_SLICK) {
        cgi.Printf(" slick");
    }
    if (iSurfaceFlags & SURF_SKY) {
        cgi.Printf(" sky");
    }
    if (iSurfaceFlags & SURF_LADDER) {
        cgi.Printf(" ladder");
    }
    if (iSurfaceFlags & SURF_NOIMPACT) {
        cgi.Printf(" noimpact");
    }
    if (iSurfaceFlags & SURF_NOMARKS) {
        cgi.Printf(" nomarks");
    }
    if (iSurfaceFlags & SURF_CASTSHADOW) {
        cgi.Printf(" castshadow");
    }
    if (iSurfaceFlags & SURF_NODRAW) {
        cgi.Printf(" nodraw");
    }
    if (iSurfaceFlags & SURF_NOLIGHTMAP) {
        cgi.Printf(" nolightmap");
    }
    if (iSurfaceFlags & SURF_ALPHASHADOW) {
        cgi.Printf(" alphashadow");
    }
    if (iSurfaceFlags & SURF_NOSTEPS) {
        cgi.Printf(" nofootsteps");
    }
    if (iSurfaceFlags & SURF_NONSOLID) {
        cgi.Printf(" nonsolid");
    }
    if (iSurfaceFlags & SURF_OVERBRIGHT) {
        cgi.Printf(" overbright");
    }
    if (iSurfaceFlags & SURF_BACKSIDE) {
        cgi.Printf(" backside");
    }
    if (iSurfaceFlags & SURF_NODLIGHT) {
        cgi.Printf(" nodlight");
    }
    if (iSurfaceFlags & SURF_HINT) {
        cgi.Printf(" hint");
    }
    if (iSurfaceFlags & SURF_PATCH) {
        cgi.Printf(" patch");
    }
}

static void CG_PrintSurfaceType(int iSurfType)
{
    switch (iSurfType & MASK_SURF_TYPE) {
    case SURF_FOLIAGE:
        cgi.Printf("foliage");
        break;
    case SURF_SNOW:
        cgi.Printf("snow");
        break;
    case SURF_CARPET:
        cgi.Printf("carpet");
        break;
    case SURF_SAND:
        cgi.Printf("sand");
        break;
    case SURF_PUDDLE:
        cgi.Printf("puddle");
        break;
    case SURF_GLASS:
        cgi.Printf("glass");
        break;
    case SURF_GRAVEL:
        cgi.Printf("gravel");
        break;
    case SURF_MUD:
        cgi.Printf("mud");
        break;
    case SURF_DIRT:
        cgi.Printf("dirt");
        break;
    case SURF_GRILL:
        cgi.Printf("metal grill");
        break;
    case SURF_GRASS:
        cgi.Printf("grass");
        break;
    case SURF_ROCK:
        cgi.Printf("rock");
        break;
    case SURF_PAPER:
        cgi.Printf("paper");
        break;
    case SURF_WOOD:
        cgi.Printf("wood");
        break;
    case SURF_METAL:
        cgi.Printf("metal");
        break;
    default:
        cgi.Printf("!!*none specified*!!");
        break;
    }
}

void CG_GetCHShader(void)
{
    trace_t       trace;
    baseshader_t *pShader;

    pShader = CG_GetShaderUnderCrosshair(qtrue, &trace);
    cgi.Printf("\n");
    if (pShader) {
        if (pShader->surfaceFlags & SURF_SKY) {
            cgi.Printf("Hit the sky\n");
        } else {
            cgi.Printf("Shader: %s\n", pShader->shader);
            cgi.Printf("Shader Contents:");
            CG_PrintContentTypes(pShader->contentFlags);
            cgi.Printf("\n");
            cgi.Printf("Shader Surface Properties:");
            CG_PrintSurfaceProperties(pShader->surfaceFlags);
            cgi.Printf("\n");
            cgi.Printf("Shader Surfacetype: ");
            CG_PrintSurfaceType(pShader->surfaceFlags);
            cgi.Printf("\n");
            cgi.Printf("Trace Contents:");
            CG_PrintContentTypes(trace.contents);
            cgi.Printf("\n");
            cgi.Printf("Trace Surface Properties:");
            CG_PrintSurfaceProperties(trace.surfaceFlags);
            cgi.Printf("\n");
            cgi.Printf("Trace Surfacetype: ");
            CG_PrintSurfaceType(trace.surfaceFlags);
            cgi.Printf("\n\n");
        }
    } else {
        cgi.Printf("No surface selected\n");
    }
}

void CG_EditCHShader(void)
{
    char          name[1024];
    baseshader_t *pShader;

    pShader = CG_GetShaderUnderCrosshair(qfalse, NULL);
    if (pShader) {
        Q_strncpyz(name, "editspecificshader ", sizeof(name));
        Q_strcat(name, sizeof(name), pShader->shader);
        Q_strcat(name, sizeof(name), "\n");
        cgi.AddCommand(name);
    } else {
        cgi.Printf("No surface selected\n");
    }
}

// HZM coop - ADS tuning workbench: dump the gun you're holding + its current tune values to the console
// (and the log). Bind a key to "adssave" - dial the gun in cg_adsTune mode, then press it to capture the
// stand (cg_adsPitch/yaw + r_weaponshift) and crouch (cg_adsCrouch*) values for that weapon.
void CG_AdsSave_f(void)
{
    const char *wpn = "";
    qboolean    ducked;

    ducked = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? qtrue : qfalse;
    if (cg.snap && cg.snap->ps.activeItems[1] >= 0) {
        wpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }

    cgi.Printf("=== ADS TUNE  [%s]  (currently %s) ===\n", wpn, ducked ? "CROUCHED" : "STANDING");
    cgi.Printf("  STAND : pitch %g  yaw %g  roll %g   shift x %g  y %g\n",
        cg_adsPitch->value, cg_adsYaw->value, cg_adsRoll ? cg_adsRoll->value : 0.f,
        cg_adsShiftX ? cg_adsShiftX->value : 0.f, cg_adsShiftY ? cg_adsShiftY->value : 0.f);
    cgi.Printf("  CROUCH: pitch %g  yaw %g  roll %g   shift x %g  y %g\n",
        cg_adsCrouchPitch->value, cg_adsCrouchYaw->value, cg_adsCrouchRoll->value,
        cg_adsCrouchShiftX ? cg_adsCrouchShiftX->value : 0.f,
        cg_adsCrouchShiftY ? cg_adsCrouchShiftY->value : 0.f);
}

// HZM coop - ADS workbench NUDGE: move the active param (cg_adsMode) in a direction, live. The active
// STANCE (stand vs crouch) is auto-detected, so each tunes its own set: standing -> cg_adsPitch/cg_adsYaw
// + cg_adsShiftX/Y ; crouched -> cg_adsCrouchPitch/Yaw + cg_adsCrouchShiftX/Y (+ cg_adsCrouchRoll in ROLL
// mode). SHIFT slides the WHOLE weapon view (hands + gun) on screen. dx: -1 left / +1 right ; dy: -1 down /
// +1 up. Only acts while cg_adsTune is on (so the keys are inert in normal play). modes: 0 pitch 1 yaw 2
// shift 3 roll.
static void CG_AdsNudge(float dx, float dy)
{
    static cvar_t *rotStep = NULL, *shStep = NULL;
    float       rot, sh;
    int         mode;
    qboolean    ducked;
    char        val[32];
    cvar_t     *c;

    if (!cg_adsTune || !cg_adsTune->integer) {
        return;
    }
    // Live-tunable nudge granularity. cg_adsShiftStep defaults FINE (0.005) for precise sight centring;
    // raise it for coarse passes, lower it for micro-adjust. cg_adsRotStep = degrees/press for pitch/yaw/roll.
    if (!rotStep) { rotStep = cgi.Cvar_Get("cg_adsRotStep",   "0.5",   CVAR_ARCHIVE); }
    if (!shStep)  { shStep  = cgi.Cvar_Get("cg_adsShiftStep", "0.005", CVAR_ARCHIVE); }
    rot = rotStep->value;   // degrees per press (pitch/yaw/roll)
    sh  = shStep->value;    // screen-shift units per press
    mode   = cg_adsMode ? cg_adsMode->integer : 0;
    ducked = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? qtrue : qfalse;

    if (mode == 0) {
        // PITCH (up/down) - up raises the muzzle / front sight
        c = ducked ? cg_adsCrouchPitch : cg_adsPitch;
        Com_sprintf(val, sizeof(val), "%g", c->value + dy * rot);
        cgi.Cvar_Set(ducked ? "cg_adsCrouchPitch" : "cg_adsPitch", val);
    } else if (mode == 1) {
        // YAW (left/right)
        c = ducked ? cg_adsCrouchYaw : cg_adsYaw;
        Com_sprintf(val, sizeof(val), "%g", c->value + dx * rot);
        cgi.Cvar_Set(ducked ? "cg_adsCrouchYaw" : "cg_adsYaw", val);
    } else if (mode == 2) {
        // SHIFT - slide the whole weapon view (hands + gun) on screen toward centre. standing tunes
        // cg_adsShiftX/Y ; crouched tunes cg_adsCrouchShiftX/Y (added on top of standing by cg_view).
        const char *cvX = ducked ? "cg_adsCrouchShiftX" : "cg_adsShiftX";
        const char *cvY = ducked ? "cg_adsCrouchShiftY" : "cg_adsShiftY";
        if (dy != 0.0f) {
            c = cgi.Cvar_Get(cvY, "0", 0);
            Com_sprintf(val, sizeof(val), "%g", c->value - dy * sh); // up = move gun up = shifty more negative
            cgi.Cvar_Set(cvY, val);
        }
        if (dx != 0.0f) {
            c = cgi.Cvar_Get(cvX, "0", 0);
            Com_sprintf(val, sizeof(val), "%g", c->value + dx * sh);
            cgi.Cvar_Set(cvX, val);
        }
    } else {
        // ROLL (left/right) - un-tilt about the barrel axis. standing = cg_adsRoll, crouch = cg_adsCrouchRoll.
        if (dx != 0.0f) {
            c = ducked ? cg_adsCrouchRoll : cg_adsRoll;
            Com_sprintf(val, sizeof(val), "%g", c->value + dx * rot);
            cgi.Cvar_Set(ducked ? "cg_adsCrouchRoll" : "cg_adsRoll", val);
        }
    }
}

void CG_AdsModePitch_f(void) { cgi.Cvar_Set("cg_adsMode", "0"); cgi.Cvar_Set("cg_adsTune", "1"); cgi.Printf("ADS tune: PITCH (up/down)\n"); }
void CG_AdsModeYaw_f(void)   { cgi.Cvar_Set("cg_adsMode", "1"); cgi.Cvar_Set("cg_adsTune", "1"); cgi.Printf("ADS tune: YAW (left/right)\n"); }
void CG_AdsModeShift_f(void) { cgi.Cvar_Set("cg_adsMode", "2"); cgi.Cvar_Set("cg_adsTune", "1"); cgi.Printf("ADS tune: SHIFT - slide hands+gun (up/down/left/right)\n"); }
void CG_AdsModeRoll_f(void)  { cgi.Cvar_Set("cg_adsMode", "3"); cgi.Cvar_Set("cg_adsTune", "1"); cgi.Printf("ADS tune: ROLL (left/right un-tilt, stand + crouch)\n"); }
void CG_AdsUp_f(void)    { CG_AdsNudge(0.0f,  1.0f); }
void CG_AdsDown_f(void)  { CG_AdsNudge(0.0f, -1.0f); }
void CG_AdsLeft_f(void)  { CG_AdsNudge(-1.0f, 0.0f); }
void CG_AdsRight_f(void) { CG_AdsNudge( 1.0f, 0.0f); }

// ===== HZM coop - LOBBY CAMERA tuning pad (mirrors the ADS pad) =====
// Nudges the server-read coop_lobbyCam* / coop_lobbyLook* cvars live. On a listen server the game and
// cgame share one cvar table, so these client sets reach coop_mod/lobby.scr::applyLobbyCam (which re-reads
// them every 0.5s and re-positions the shared camera via movetopos/watch). Gated by coop_lobbyCamTune so
// the numpad keys are inert until a mode is picked. Modes (coop_lobbyCamMode):
//   0 MOVE  : up/down = camera height (coop_lobbyCamZ)   left/right = dolly in/out (coop_lobbyCamY)
//   1 FRAME : up/down = FOV zoom (coop_lobbyCamFov)      left/right = side slide (coop_lobbyCamX)
//   2 AIM   : up/down = aim height (coop_lobbyLookZ)     left/right = aim side (coop_lobbyLookX)
static void CG_LobbyCamAdjust(const char *name, const char *def, float delta)
{
    cvar_t *c = cgi.Cvar_Get(name, def, CVAR_ARCHIVE);
    char    val[32];
    Com_sprintf(val, sizeof(val), "%g", c->value + delta);
    cgi.Cvar_Set(name, val);
}

static void CG_LobbyCamNudge(float dx, float dy)
{
    int mode;
    if (!cgi.Cvar_Get("coop_lobbyCamTune", "0", CVAR_ARCHIVE)->integer) {
        return;
    }
    mode = cgi.Cvar_Get("coop_lobbyCamMode", "0", CVAR_ARCHIVE)->integer;
    if (mode == 0) {
        if (dy != 0.0f) { CG_LobbyCamAdjust("coop_lobbyCamZ", "-205", dy * 8.0f); }
        if (dx != 0.0f) { CG_LobbyCamAdjust("coop_lobbyCamY", "-100", dx * 8.0f); }
    } else if (mode == 1) {
        if (dy != 0.0f) { CG_LobbyCamAdjust("coop_lobbyCamFov", "70", dy * 2.0f); }
        if (dx != 0.0f) { CG_LobbyCamAdjust("coop_lobbyCamX", "-5347", dx * 8.0f); }
    } else {
        if (dy != 0.0f) { CG_LobbyCamAdjust("coop_lobbyLookZ", "-245", dy * 6.0f); }
        if (dx != 0.0f) { CG_LobbyCamAdjust("coop_lobbyLookX", "-5347", dx * 6.0f); }
    }
}

void CG_LcamModeMove_f(void)  { cgi.Cvar_Set("coop_lobbyCamMode", "0"); cgi.Cvar_Set("coop_lobbyCamTune", "1"); cgi.Printf("Lobby cam: MOVE  (8/2 = height, 4/6 = dolly in/out)\n"); }
void CG_LcamModeFrame_f(void) { cgi.Cvar_Set("coop_lobbyCamMode", "1"); cgi.Cvar_Set("coop_lobbyCamTune", "1"); cgi.Printf("Lobby cam: FRAME (8/2 = FOV zoom, 4/6 = side slide)\n"); }
void CG_LcamModeAim_f(void)   { cgi.Cvar_Set("coop_lobbyCamMode", "2"); cgi.Cvar_Set("coop_lobbyCamTune", "1"); cgi.Printf("Lobby cam: AIM   (8/2 = aim height, 4/6 = aim side)\n"); }
void CG_LcamUp_f(void)    { CG_LobbyCamNudge(0.0f,  1.0f); }
void CG_LcamDown_f(void)  { CG_LobbyCamNudge(0.0f, -1.0f); }
void CG_LcamLeft_f(void)  { CG_LobbyCamNudge(-1.0f, 0.0f); }
void CG_LcamRight_f(void) { CG_LobbyCamNudge( 1.0f, 0.0f); }
void CG_LcamSave_f(void)
{
    cgi.Printf("=== LOBBY CAM  (paste these to bake as defaults) ===\n");
    cgi.Printf("  cam  X %g  Y %g  Z %g   FOV %g\n",
        cgi.Cvar_Get("coop_lobbyCamX", "-5347", 0)->value,
        cgi.Cvar_Get("coop_lobbyCamY", "-100", 0)->value,
        cgi.Cvar_Get("coop_lobbyCamZ", "-205", 0)->value,
        cgi.Cvar_Get("coop_lobbyCamFov", "70", 0)->value);
    cgi.Printf("  look X %g  Y %g  Z %g\n",
        cgi.Cvar_Get("coop_lobbyLookX", "-5347", 0)->value,
        cgi.Cvar_Get("coop_lobbyLookY", "-427", 0)->value,
        cgi.Cvar_Get("coop_lobbyLookZ", "-245", 0)->value);
}

#if 0


/*
=============================================================================

  MODEL TESTING

The viewthing and gun positioning tools from Q2 have been integrated and
enhanced into a single model testing facility.

Model viewing can begin with either "testmodel <modelname>" or "testgun <modelname>".

The names must be the full pathname after the basedir, like 
"models/weapons/v_launch/tris.md3" or "players/male/tris.md3"

Testmodel will create a fake entity 100 units in front of the current view
position, directly facing the viewer.  It will remain immobile, so you can
move around it to view it from different angles.

Testgun will cause the model to follow the player around and supress the real
view weapon model.  The default frame 0 of most guns is completely off screen,
so you will probably have to cycle a couple frames to see it.

"nextframe", "prevframe", "nextskin", and "prevskin" commands will change the
frame or skin of the testmodel.  These are bound to F5, F6, F7, and F8 in
q3default.cfg.

Note that none of the model testing features update while the game is paused, so
it may be convenient to test with deathmatch set to 1 so that bringing down the
console doesn't pause the game.

=============================================================================
*/

/*
=================
CG_TestModel_f

Creates an entity in front of the current position, which
can then be moved around
=================
*/
void CG_TestModel_f (void) {
	vec3_t		angles;

	memset( &cg.testModelEntity, 0, sizeof(cg.testModelEntity) );
	if ( cgi.Argc() < 2 ) {
		return;
	}

	Q_strncpyz (cg.testModelName, cgi.Argv( 1 ), MAX_QPATH );
	cg.testModelEntity.hModel = cgi.R_RegisterModel( cg.testModelName );

	if ( cgi.Argc() == 3 ) {
		cg.testModelEntity.backlerp = atof( cgi.Argv( 2 ) );
		cg.testModelEntity.frame = 1;
		cg.testModelEntity.oldframe = 0;
	}
	if (! cg.testModelEntity.hModel ) {
		cgi.Printf( "Can't register model\n" );
		return;
	}

	VectorMA( cg.refdef.vieworg, 100, cg.refdef.viewaxis[0], cg.testModelEntity.origin );

	angles[PITCH] = 0;
	angles[YAW] = 180 + cg.refdefViewAngles[1];
	angles[ROLL] = 0;

	AnglesToAxis( angles, cg.testModelEntity.axis );
	cg.testGun = qfalse;
}

void CG_TestModelNextFrame_f (void) {
	cg.testModelEntity.frame++;
	cgi.Printf( "frame %i\n", cg.testModelEntity.frame );
}

void CG_TestModelPrevFrame_f (void) {
	cg.testModelEntity.frame--;
	cgi.Printf( "frame %i\n", cg.testModelEntity.frame );
}

void CG_TestModelNextSkin_f (void) {
	cg.testModelEntity.skinNum++;
	cgi.Printf( "skin %i\n", cg.testModelEntity.skinNum );
}

void CG_TestModelPrevSkin_f (void) {
	cg.testModelEntity.skinNum--;
	if ( cg.testModelEntity.skinNum < 0 ) {
		cg.testModelEntity.skinNum = 0;
	}
	cgi.Printf( "skin %i\n", cg.testModelEntity.skinNum );
}

void CG_AddTestModel (void) {
	// re-register the model, because the level may have changed
	cg.testModelEntity.hModel = cgi.R_RegisterModel( cg.testModelName );
	if (! cg.testModelEntity.hModel ) {
		cgi.Printf ("Can't register model\n");
		return;
	}
	cgi.R_AddRefEntityToScene( &cg.testModelEntity );
}

#endif

typedef struct {
    char *cmd;
    void (*function)(void);
} consoleCommand_t;

// HZM coop [user 2026-08-21] SHOULDER SWAP. Requested as part of the third-person cover aim
// scheme ("middle mouse switches shoulders") and never wired: cg_adsShoulderRight has existed as a
// cvar since the staged-ADS work with no command and no key bound to it, so there was no way to
// actually swap in play.
static void CG_AdsSwapShoulder_f(void)
{
    cvar_t *pRight = cgi.Cvar_Get("cg_adsShoulderRight", "1", CVAR_ARCHIVE);

    cgi.Cvar_Set("cg_adsShoulderRight", pRight->integer ? "0" : "1");
}

// HZM coop [2026-09-04, bug-2460] RELAY +coopnade / -coopnade TO THE SERVER.
//
// These two are registered ONLY in the game module (gamecmds.cpp:154-155), and Cmd_ExecuteString
// (qcommon/cmd.c) tries cgame (CL_GameCommand) BEFORE the local-server shortcut (SV_GameCommand)
// and before CL_ForwardCommandToServer. That ordering is the whole bug: a listen HOST has
// com_sv_running set, so the shortcut caught the command and the bind worked - while a remote
// client fell through to CL_ForwardCommandToServer, which drops any '-' command silently and
// answers any '+' command with "Unknown command" (cl_main.cpp:1056-1074). Neither form is ever
// forwarded. So the quick-grenade bind has been HOST-ONLY since it shipped.
//
// Forwarding explicitly from here fixes the joiner and makes the host take the same path, so the
// two can no longer differ by accident. G_ProcessClientCommand matches G_ConsoleCmds by literal
// name, so the leading '+'/'-' survives and lands on the existing entries - no new command names,
// no protocol change, no game.dll change.
//
// NOTE FOR ANYONE ADDING ANOTHER HELD ACTION: docs/TRAPS.md T22 used to claim unmatched commands
// forward to the server and cited +coopnade as its worked example. It does not, and they did not.
// A +/- pair needs a relay like this one to work for anybody but the host.
static void CG_CoopNadeDown_f(void)
{
    cgi.SendClientCommand("+coopnade");
}

static void CG_CoopNadeUp_f(void)
{
    cgi.SendClientCommand("-coopnade");
}

// HZM coop [user 2026-09-04] QUICK-DRAW SIDEARM input relay. THIS IS A COPY OF THE +coopnade RELAY
// ONE SCREEN ABOVE, not a second mechanism for the same job.
//
// A '+name'/'-name' pair registered ONLY in fgame/gamecmds.cpp is listen-host-only: Cmd_ExecuteString
// tries cgame, then SV_GameCommand (gated on com_sv_running, which is why the HOST works), then
// CL_ForwardCommandToServer - which returns silently for a leading '-' and answers a leading '+'
// with "Unknown command" (client/cl_main.cpp:1056-1074). That is bug-2460, it is written up as
// docs/TRAPS.md T22, and it was FIXED for +coopnade in this file on 2026-09-04. Registering here and
// forwarding with cgi.SendClientCommand (= CL_CG_AddReliableCommand, client/cl_cgame.cpp:671) is the
// recipe T22 now requires. G_ProcessClientCommand matches G_ConsoleCmds by literal name, so the
// leading '+'/'-' survives the relay and lands on the rows added in gamecmds.cpp - no new command
// names, no argument parsing, no protocol change.
//
// The relay also fixes WHO the press belongs to: G_ConsoleCommand hardcodes ent = &g_entities[0]
// (fgame/gamecmds.cpp:236), so a command that reached the server any other way would hand every
// joiner's keypress to the host. The reliable-command path carries the real client.
void CG_QDMotion_KeyDown(void); // HZM coop [quick-draw motion 2026-10-05] cg_qdrawmotion.c: the motion starts on the press
void CG_QDMotion_KeyUp(void);

static void CG_CoopSidearmDown_f(void)
{
    cgi.SendClientCommand("+coopsidearm");
    CG_QDMotion_KeyDown();
}

static void CG_CoopSidearmUp_f(void)
{
    cgi.SendClientCommand("-coopsidearm");
    CG_QDMotion_KeyUp();
}

// E2 [MP armories slice 2, F1] - "hzm_armory" console command.
// The MP branch and the coop fallback are separated by clause 14g.
static void CG_HzmArmory_f(void)
{
    // HZM-MP-BEGIN(mp_armory_cmd)
    cvar_t *pSession = cgi.Cvar_Get("coop_mp_session", "0", 0);
    if (pSession->integer == 1) {
        if (cg.snap && cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
            cgi.Cmd_Execute(EXEC_NOW, "pushmenu coop_mpx_armory\n");
        } else {
            cgi.Cmd_Execute(EXEC_NOW, "pushmenu coop_mpa_armory\n");
        }
        return;
    }
    // HZM-MP-END(mp_armory_cmd)
    cgi.Cmd_Execute(EXEC_NOW, "pushmenu coop_loadout\n");
}

static consoleCommand_t commands[] = {
    {"footprintfill",          &CG_FootprintFill_f         }, // HZM coop footprints: QA/perf helper
    {"shoulderswap",           &CG_AdsSwapShoulder_f       },
    {"useweaponclass",         &CG_UseWeaponClass_f        },
    {"weapnext",               &CG_NextWeapon_f            },
    {"weapprev",               &CG_PrevWeapon_f            },
    {"uselast",                &CG_UseLastWeapon_f         },
    {"holster",                &CG_HolsterWeapon_f         },
    {"weapdrop",               &CG_DropWeapon_f            },
    {"toggleitem",             &CG_ToggleItem_f            },
    {"+scores",                &CG_ScoresDown_f            },
    {"-scores",                &CG_ScoresUp_f              },
    {"viewpos",                &CG_Viewpos_f               },
    {"sizeup",                 &CG_SizeUp_f                },
    {"sizedown",               &CG_SizeDown_f              },
    {"cg_eventlist",           &CG_EventList_f             },
    {"cg_eventhelp",           &CG_EventHelp_f             },
    {"cg_dumpevents",          &CG_DumpEventHelp_f         },
    {"cg_pendingevents",       &CG_PendingEvents_f         },
    {"cg_classlist",           &CG_ClassList_f             },
    {"cg_classtree",           &CG_ClassTree_f             },
    {"cg_classevents",         &CG_ClassEvents_f           },
    {"cg_dumpclassevents",     &CG_DumpClassEvents_f       },
    {"cg_dumpallclasses",      &CG_DumpAllClasses_f        },
    {"testemitter",            &CG_TestEmitter_f           },
    {"triggertestemitter",     &CG_TriggerTestEmitter_f    },
    {"prevemittercommand",     &CG_PrevEmitterCommand_f    },
    {"nextemittercommand",     &CG_NextEmitterCommand_f    },
    {"newemittercommand",      &CG_NewEmitterCommand_f     },
    {"deleteemittercommand",   &CG_DeleteEmitterCommand_f  },
    {"dumpemitter",            &CG_DumpEmitter_f           },
    {"loademitter",            &CG_LoadEmitter_f           },
    {"resetvss",               &CG_ResetVSSSources         },
    {"getchshader",            &CG_GetCHShader             },
    {"editchshader",           &CG_EditCHShader            },
    {"messagemode",            &CG_MessageMode_f           },
    {"messagemode_all",        &CG_MessageMode_All_f       },
    {"messagemode_team",       &CG_MessageMode_Team_f      },
    {"messagemode_private",    &CG_MessageMode_Private_f   },
    {"say",                    &CG_MessageSingleAll_f      },
    {"sayteam",                &CG_MessageSingleTeam_f     },
    {"teamsay",                &CG_MessageSingleTeam_f     },
    {"sayprivate",             &CG_MessageSingleClient_f   },
    {"sayone",                 &CG_MessageSingleClient_f   },
    {"wisper",                 &CG_MessageSingleClient_f   },
    {"instamsg_main",          &CG_InstaMessageMain_f      },
    {"instamsg_group_a",       &CG_InstaMessageGroupA_f    },
    {"instamsg_group_b",       &CG_InstaMessageGroupB_f    },
    {"instamsg_group_c",       &CG_InstaMessageGroupC_f    },
    {"instamsg_group_d",       &CG_InstaMessageGroupD_f    },
    {"instamsg_group_e",       &CG_InstaMessageGroupE_f    },
    {"pushmenu_teamselect",    &CG_PushMenuTeamSelect_f    },
    {"pushmenu_weaponselect",  &CG_PushMenuWeaponSelect_f  },
    // Added in 2.0
    {"pushcallvote",           &CG_PushCallVote_f          },
    {"pushcallvotesublist",    &CG_PushCallVoteSubList_f   },
    {"pushcallvotesubtext",    &CG_PushCallVoteSubText_f   },
    {"pushcallvotesubinteger", &CG_PushCallVoteSubInteger_f},
    {"+coopnade",             &CG_CoopNadeDown_f          }, // HZM coop bug-2460 - see above
    {"+coopwheel",            &CG_WheelDown_f             }, // [v1.10.46] weapon / ability wheel
    {"-coopwheel",            &CG_WheelUp_f               },
    {"+coopwheelpad",         &CG_WheelPadDown_f          },
    {"-coopwheelpad",         &CG_WheelPadUp_f            },
    {"-coopnade",             &CG_CoopNadeUp_f            }, // HZM coop bug-2460
    {"pushcallvotesubfloat",   &CG_PushCallVoteSubFloat_f  },
    {"pushcallvotesubclient",  &CG_PushCallVoteSubClient_f },
    {"pushvote",               &CG_PushVote_f              },
    {"callentryvote",          &CG_CallEntryVote_f         },
    {"adssave",                &CG_AdsSave_f               },
    {"adsm_pitch",             &CG_AdsModePitch_f          },
    {"adsm_yaw",               &CG_AdsModeYaw_f            },
    {"adsm_shift",             &CG_AdsModeShift_f          },
    {"adsm_roll",              &CG_AdsModeRoll_f           },
    {"adsn_up",                &CG_AdsUp_f                 },
    {"adsn_down",              &CG_AdsDown_f               },
    {"adsn_left",              &CG_AdsLeft_f               },
    {"adsn_right",             &CG_AdsRight_f              },
    {"lcamm_move",             &CG_LcamModeMove_f          },
    {"lcamm_frame",            &CG_LcamModeFrame_f         },
    {"lcamm_aim",              &CG_LcamModeAim_f           },
    {"lcamn_up",               &CG_LcamUp_f                },
    {"lcamn_down",             &CG_LcamDown_f              },
    {"lcamn_left",             &CG_LcamLeft_f              },
    {"lcamn_right",            &CG_LcamRight_f             },
    {"lcamsave",               &CG_LcamSave_f              },
    {"+coopsidearm",           &CG_CoopSidearmDown_f       }, // HZM coop - relay, see T22/bug-2460
    {"-coopsidearm",           &CG_CoopSidearmUp_f         }, // HZM coop - both halves or host-only
    {"hzm_armory",             &CG_HzmArmory_f             }, // HZM MP armories E2
};

/*
=================
CG_ConsoleCommand

The string has been tokenized and can be retrieved with
Cmd_Argc() / Cmd_Argv()
=================
*/
qboolean CG_ConsoleCommand(void)
{
    const char *cmd;
    int         i;

    cmd = cgi.Argv(0);

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!Q_stricmp(cmd, commands[i].cmd)) {
            commands[i].function();
            return qtrue;
        }
    }

    return qfalse;
}

/*
=================
CG_InitConsoleCommands

Let the client system know about all of our commands
so it can perform tab completion
=================
*/
void CG_InitConsoleCommands(void)
{
    int i;

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        cgi.AddCommand(commands[i].cmd);
    }

    cgi.AddCommand("callvote");
    cgi.AddCommand("vote");
}

void CG_Mapinfo_f(void)
{
    cgi.Printf("---------------------\n");
    cgi.R_PrintBSPFileSizes();
    cgi.CM_PrintBSPFileSizes();
    cgi.Printf("---------------------\n");
}

/*
HZM [v1.10.41] user: "when you enter a lobby you have to hit esc first because the main menu cursor is always on the screen".
On spawn the game sends "wait 250;pushmenu_teamselect" (fgame player.cpp); coop joins the player to a team and pops the
menu with staggered popmenus that all run BEFORE that 250-frame wait ends, so the picker opened last and stayed - hidden by
coop's UI, leaving only the GUI cursor until ESC. In the first 20 s of a map, a player who is already on a team is not
sent to a picker. Later (a player opening the team menu on purpose) and spectators are untouched.
*/
int g_hzmCgInitMs = 0;

static qboolean CG_HzmSkipAutoPicker(const char *what)
{
    int team;

    if (!cg.snap || !cgi.Milliseconds) {
        return qfalse;
    }
    if (cgi.Milliseconds() - g_hzmCgInitMs > 20000) {
        return qfalse;
    }
    team = cg.snap->ps.stats[STAT_TEAM];
    if (team != TEAM_ALLIES && team != TEAM_AXIS) {
        return qfalse;
    }
    cgi.Printf("^~^~^ UIPICKER skipped the auto %s picker: already on a team at map entry\n", what);
    return qtrue;
}

/*
===================
[v1.10.46] WEAPON / ABILITY WHEEL
user: "a weapon wheel out of tab ... change between your weapons, and then switch to the abilities wheel for ammo, sandbag, medkit,
airstrike, and paradrop ... for both controller and mouse/keyboard" - "Do it how modern games do it" - "show amounts and icons in
the actual wheel slot" - "list the exact gun it is ... with their exact icon" - "a way to switch between multiple weapons of the
same class".
  +coopwheel     (Tab)      hold: the wheel; release on a slot (or click it) picks it. A tap does nothing.
  +coopwheelpad  (pad Y)    hold: the wheel; a quick tap runs "vstr coop_padY" (weapon swap, Service Record in the lobby).
While cg_wheelOpen is 1 the client (cl_input.cpp) turns mouse / right-stick motion into a pointer (cl_wheelX / cl_wheelY) instead
of turning the view; you can still walk.
  page (weapons <-> abilities): right mouse, Q / E, pad LB / RB
  next / previous gun in the hovered slot when you carry two of a class: mouse wheel, pad D-pad left / right
The look: segmented ring (textures/hud/coop_wheel_*, docs/tools/gen_wheel_art.py), the hovered wedge lit, in every wedge the exact
gun you carry there - its own silhouette and short name from the loot table (cg_coopfeed_loot.h) - with its ammo, or the ability's
icon and count; empty slots dimmed and unselectable; the hovered item's name and ammo in the centre. The inventory comes from the
server feed "hzmwinv" (player.cpp HzmSendWheelInv). The page opens where you left it, each slot on the gun you last chose.
===================
*/
typedef struct {
    const char *label, *cmd, *icon;
} wheelSlice_t;
static const wheelSlice_t s_wheelWeapons[] = {
    {"Pistol", "useweaponclass pistol", "textures/hud/loot/coop_loot_pistol"},
    {"Rifle", "useweaponclass rifle", "textures/hud/loot/coop_loot_gun"},
    {"SMG", "useweaponclass smg", "textures/hud/loot/w_thompson"},
    {"Machine gun", "useweaponclass mg", "textures/hud/loot/w_bar"},
    {"Heavy", "useweaponclass heavy", "textures/hud/loot/w_bazooka"},
    {"Grenade", "useweaponclass grenade", "textures/hud/coop_wheel_nade"},
};
static const int s_wheelClassBits[6] = {WEAPON_CLASS_PISTOL, WEAPON_CLASS_RIFLE, WEAPON_CLASS_SMG,
                                        WEAPON_CLASS_MG,     WEAPON_CLASS_HEAVY, WEAPON_CLASS_GRENADE};
static const wheelSlice_t s_wheelAbilities[] = {
    {"Ammo box", "append name ,abx", "textures/hud/loot/coop_act_ammocrate"},
    {"Sandbags", "append name ,9cv", "textures/hud/loot/coop_act_sandbag"},
    {"Medkit", "append name ,8mk", "textures/hud/loot/coop_act_heal"},
    {"Airstrike", "use models/items/binoculars.tik", "textures/hud/loot/coop_act_binoc"},
    {"Paradrop", "use models/weapons/coop_smoke_grenade.tik", "textures/hud/coop_paradrop_icon"},
};
// s_wheelOpen: the button is held; s_wheelShown: held long enough (WHEEL_HOLD_MS) that the wheel is up. A tap never shows it.
#define WHEEL_HOLD_MS 200
static int s_wheelOpen, s_wheelShown, s_wheelPage, s_wheelPad, s_wheelHover = -1;
static int s_wheelT0;

// the carried guns, from the server feed: CS_WEAPONS index, ammo, wheel slot (0..5)
#define WHEEL_MAXGUNS 64
static int s_wheelGunN, s_wheelGunIdx[WHEEL_MAXGUNS], s_wheelGunAmmo[WHEEL_MAXGUNS], s_wheelGunSlot[WHEEL_MAXGUNS];
static int s_wheelVar[6]; // which of a slot's guns is showing (index among that slot's guns)

// server feed: "hzmwinv <n> then n x <idx> <ammo> <slot>" (CG_ServerCommand)
void CG_WheelInv_f(void)
{
    int k, n = atoi(cgi.Argv(1));
    if (n > WHEEL_MAXGUNS) {
        n = WHEEL_MAXGUNS;
    }
    for (k = 0; k < n; k++) {
        s_wheelGunIdx[k]  = atoi(cgi.Argv(2 + k * 3));
        s_wheelGunAmmo[k] = atoi(cgi.Argv(3 + k * 3));
        s_wheelGunSlot[k] = atoi(cgi.Argv(4 + k * 3));
    }
    s_wheelGunN = n;
    if (cgi.Cvar_Get("cg_wheelDebug", "0", 0)->integer) {
        cgi.Printf("^~^~^ WHEELINV %s\n", cgi.Args());
    }
}

static int CG_WheelCount(int page)
{
    return page ? (int)(sizeof(s_wheelAbilities) / sizeof(s_wheelAbilities[0])) : (int)(sizeof(s_wheelWeapons) / sizeof(s_wheelWeapons[0]));
}

static const wheelSlice_t *CG_WheelSlice(int page, int i)
{
    return page ? &s_wheelAbilities[i] : &s_wheelWeapons[i];
}

// how many guns sit in a slot, and the feed entry of the v-th one (-1 = none)
static int CG_WheelSlotGuns(int slot)
{
    int k, n = 0;
    for (k = 0; k < s_wheelGunN; k++) {
        if (s_wheelGunSlot[k] == slot) {
            n++;
        }
    }
    return n;
}

static int CG_WheelSlotGun(int slot, int v)
{
    int k;
    for (k = 0; k < s_wheelGunN; k++) {
        if (s_wheelGunSlot[k] == slot && !v--) {
            return k;
        }
    }
    return -1;
}

// the gun a slot shows: the chosen one, kept in range when the inventory changes
static int CG_WheelShownGun(int slot)
{
    int n = CG_WheelSlotGuns(slot);
    if (!n) {
        return -1;
    }
    if (s_wheelVar[slot] < 0 || s_wheelVar[slot] >= n) {
        s_wheelVar[slot] = 0;
    }
    return CG_WheelSlotGun(slot, s_wheelVar[slot]);
}

// does the player have something in this weapon slot? The stat is the truth; the feed names it.
static qboolean CG_WheelOwned(int i)
{
    return cg.snap && (cg.snap->ps.stats[STAT_WEAPONS] & s_wheelClassBits[i]) ? qtrue : qfalse;
}

// ability counts: the same client cvars the HUD counters use (ammobox.scr, cover.scr, medkit.scr, officer.scr rewards).
// "-" / empty = not published (lobby, briefing) - shown blank and left usable, the server has the final say.
static const char *s_wheelAbilCvar[5] = {"coop_hudAmmo", "coop_hudCov", "coop_hudMed", "coop_hudBinoc", "coop_hudSmoke"};

static int CG_WheelAbilCount(int i)
{
    const char *v = cgi.Cvar_Get(s_wheelAbilCvar[i], "", 0)->string;
    // airstrike / paradrop are officer rewards: "-" means you hold none, paradrop publishes "y" while armed (officer.scr)
    if (i >= 3) {
        if (!v || !*v || *v == '-') {
            return 0;
        }
        return (*v == 'y') ? 1 : atoi(v);
    }
    if (!v || !*v || *v == '-') {
        return -1;
    }
    return atoi(v);
}

static qboolean CG_WheelUsable(int page, int i)
{
    return page ? (CG_WheelAbilCount(i) != 0) : CG_WheelOwned(i);
}

// CS_WEAPONS name of the gun a slot shows ("" = unknown): the feed, else the gun in hand when it is that class
static const char *CG_WheelGunName(int i)
{
    int g = CG_WheelShownGun(i);
    if (g >= 0) {
        return CG_ConfigString(CS_WEAPONS + s_wheelGunIdx[g]);
    }
    if (cg.snap && (cg.snap->ps.stats[STAT_EQUIPPED_WEAPON] & s_wheelClassBits[i]) && cg.snap->ps.activeItems[1] >= 0) {
        return CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }
    return "";
}

// the exact gun's art, through the loot prompt's table (its own silhouette + short name + how much of the 2:1 canvas it fills)
typedef struct {
    char      name[64];
    qhandle_t sh;
    char      shortName[48];
    float     vw, vh;
} wheelArt_t;

static const wheelArt_t *CG_WheelGunArt(const char *name)
{
    static wheelArt_t cache[24];
    static int        used;
    const char       *tex = NULL, *sn = NULL;
    float             vw = 0.9f, vh = 0.4f;
    wheelArt_t       *a;
    int               i;

    if (!name || !*name) {
        return NULL;
    }
    for (i = 0; i < used; i++) {
        if (!strcmp(cache[i].name, name)) {
            return cache[i].sh ? &cache[i] : NULL;
        }
    }
    a = &cache[used < 24 ? used++ : 23];
    Q_strncpyz(a->name, name, sizeof(a->name));
    a->sh = 0;
    if (CG_LootArtLookup(name, &tex, &sn, &vw, &vh) && tex) {
        a->sh = cgi.R_RegisterShader(va("textures/hud/loot/%s", tex));
        Q_strncpyz(a->shortName, sn ? sn : name, sizeof(a->shortName));
        a->vw = vw;
        a->vh = vh;
    }
    return a->sh ? a : NULL;
}

// the name a slot shows: the gun's short name when the table knows it, else its item name
static const char *CG_WheelGunLabel(int i)
{
    const char       *nm = CG_WheelGunName(i);
    const wheelArt_t *a  = CG_WheelGunArt(nm);
    return a ? a->shortName : nm;
}

// the slot the pointer is on, -1 in the dead centre. Slot 0 is straight up, then clockwise.
static int CG_WheelPick(void)
{
    float x = cgi.Cvar_Get("cl_wheelX", "0", 0)->value, y = cgi.Cvar_Get("cl_wheelY", "0", 0)->value;
    int   n = CG_WheelCount(s_wheelPage);
    float a;
    if (x * x + y * y < 34.0f * 34.0f) {
        return -1;
    }
    a = (float)atan2(x, -y); // 0 = up, clockwise positive
    if (a < 0.0f) {
        a += 2.0f * (float)M_PI;
    }
    return ((int)floor(a / (2.0f * (float)M_PI / n) + 0.5f)) % n;
}

static void CG_WheelOpen(int pad)
{
    int s;
    if (s_wheelOpen) {
        return;
    }
    s_wheelOpen  = 1;
    s_wheelPad   = pad;
    s_wheelHover = -1;
    s_wheelT0    = cg.time;
    // each slot starts on the gun in your hands when that is one of its guns
    if (cg.snap && cg.snap->ps.activeItems[1] >= 0) {
        for (s = 0; s < 6; s++) {
            int v, n = CG_WheelSlotGuns(s);
            for (v = 0; v < n; v++) {
                if (s_wheelGunIdx[CG_WheelSlotGun(s, v)] == cg.snap->ps.activeItems[1]) {
                    s_wheelVar[s] = v;
                }
            }
        }
    }
    s_wheelShown = 0; // [v1.10.47] user: "when you tap ... the wheel still appears and then disappears" - it shows only once held
}

// held past the threshold: show it and take over the mouse / right stick (called every frame from CG_DrawWheel)
static void CG_WheelMaybeShow(void)
{
    if (s_wheelOpen && !s_wheelShown && cg.time - s_wheelT0 >= WHEEL_HOLD_MS) {
        s_wheelShown = 1;
        cgi.Cvar_Set("cg_wheelOpen", "1");
        cgi.S_StartLocalSound("sound/menu/scroll.wav", qfalse);
    }
}

// pick = run the hovered slot; otherwise just close (a pad tap falls back to the old Y action)
static void CG_WheelClose(qboolean pick)
{
    int sel   = s_wheelShown ? CG_WheelPick() : -1;
    int quick = !s_wheelShown;
    s_wheelOpen  = 0;
    s_wheelShown = 0;
    cgi.Cvar_Set("cg_wheelOpen", "0");
    if (pick && sel >= 0 && CG_WheelUsable(s_wheelPage, sel)) {
        int g = (s_wheelPage == 0) ? CG_WheelShownGun(sel) : -1;
        if (g >= 0 && CG_WheelSlotGuns(sel) > 1) {
            // two of a class: equip that exact one by its item name ("use" finds it by name)
            cgi.Cmd_Execute(EXEC_APPEND, va("use \"%s\"\n", CG_ConfigString(CS_WEAPONS + s_wheelGunIdx[g])));
        } else {
            cgi.Cmd_Execute(EXEC_APPEND, va("%s\n", CG_WheelSlice(s_wheelPage, sel)->cmd));
        }
        cgi.S_StartLocalSound("sound/menu/apply.wav", qfalse);
    } else if (pick && sel < 0 && quick && s_wheelPad) {
        cgi.Cmd_Execute(EXEC_APPEND, "vstr coop_padY\n");
    }
}

void CG_WheelDown_f(void) { CG_WheelOpen(0); }
void CG_WheelUp_f(void) { if (s_wheelOpen) { CG_WheelClose(qtrue); } }
void CG_WheelPadDown_f(void) { CG_WheelOpen(1); }
void CG_WheelPadUp_f(void) { if (s_wheelOpen) { CG_WheelClose(qtrue); } }

// step the hovered weapon slot to its next / previous gun; qfalse when there is nothing to step
static qboolean CG_WheelCycle(int dir)
{
    int sel = CG_WheelPick(), n;
    if (s_wheelPage != 0 || sel < 0) {
        return qfalse;
    }
    n = CG_WheelSlotGuns(sel);
    if (n < 2) {
        return qfalse;
    }
    s_wheelVar[sel] = (s_wheelVar[sel] + dir + n) % n;
    cgi.S_StartLocalSound("sound/menu/scroll.wav", qfalse);
    return qtrue;
}

// called from CG_CheckCaptureKey: while the wheel is open its keys are swallowed so the binds never fire
qboolean CG_WheelCaptureKey(int key, qboolean down)
{
    if (!s_wheelOpen || !s_wheelShown) {
        return qfalse;
    }
    // mouse wheel / D-pad left-right: the next gun of the hovered slot
    if (key == K_MWHEELUP || key == K_MWHEELDOWN || key == K_PAD0_DPAD_LEFT || key == K_PAD0_DPAD_RIGHT) {
        if (down) {
            CG_WheelCycle((key == K_MWHEELUP || key == K_PAD0_DPAD_LEFT) ? -1 : 1);
        }
        return qtrue;
    }
    if (key == K_MOUSE2 || key == 'q' || key == 'e' || key == K_PAD0_LEFTSHOULDER || key == K_PAD0_RIGHTSHOULDER) {
        if (down) {
            s_wheelPage  = !s_wheelPage;
            s_wheelHover = -1;
            cgi.S_StartLocalSound("sound/menu/scroll.wav", qfalse);
        }
        return qtrue;
    }
    // click / A picks the hovered slot at once (fire never goes off while the wheel is up)
    if (key == K_MOUSE1 || key == K_PAD0_A) {
        if (down && CG_WheelPick() >= 0) {
            CG_WheelClose(qtrue);
        }
        return qtrue;
    }
    if (key == K_ESCAPE || key == K_PAD0_B) {
        if (down) {
            CG_WheelClose(qfalse);
        }
        return qtrue;
    }
    return qfalse;
}

// text helpers: the wheel is laid out in real pixels (one scale for both axes, so it stays round on any aspect)
static void CG_WheelText(fontheader_t *font, const char *t, float px, float py, float scale, const float *rgba)
{
    vec2_t sc;
    float  tw = (float)cgi.UI_FontStringWidth(font, t, -1) * scale;
    sc[0] = sc[1] = scale;
    cgi.R_SetColor(rgba);
    cgi.R_DrawString(font, t, (px - tw * 0.5f) / scale, py / scale, -1, sc);
}

// the same, shrunk to fit maxw
static void CG_WheelTextFit(fontheader_t *font, const char *t, float px, float py, float scale, float maxw, const float *rgba)
{
    float tw = (float)cgi.UI_FontStringWidth(font, t, -1) * scale;
    if (tw > maxw && tw > 0.0f) {
        scale *= maxw / tw;
    }
    CG_WheelText(font, t, px, py, scale, rgba);
}

void CG_DrawWheel(void)
{
    static fontheader_t *font = NULL, *fontBig = NULL;
    static qhandle_t     ring[2], lit[2][6], icons[2][6];
    static const float   white[4] = {0.92f, 0.90f, 0.84f, 1.0f}, gold[4] = {1.0f, 0.86f, 0.52f, 1.0f},
                       grey[4] = {0.55f, 0.55f, 0.52f, 1.0f}, ink[4] = {0.10f, 0.08f, 0.04f, 1.0f};
    float  k  = cgs.uiHiResScale[1] > 0.0f ? cgs.uiHiResScale[1] : 1.0f;
    float  W  = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
    float  cx = W * 0.5f, cy = H * 0.5f;
    float  RO = 165.0f * k * 0.98f, RI = 165.0f * k * 0.44f, RM = (RO + RI) * 0.5f;
    vec4_t c;
    int    i, n, sel, pg;

    if (!s_wheelOpen) {
        return;
    }
    if (!s_wheelShown) {
        CG_WheelMaybeShow();
        return; // shown from the next frame, once the client has reset the pointer
    }
    if (!cgi.Cvar_Get("cg_wheelOpen", "0", 0)->integer) {
        s_wheelOpen  = 0; // the client closed it (a menu / the console opened)
        s_wheelShown = 0;
        return;
    }
    if (!font) {
        font    = cgi.R_LoadFont("verdana-12");
        fontBig = cgi.R_LoadFont("verdana-14");
        for (pg = 0; pg < 2; pg++) {
            n        = CG_WheelCount(pg);
            ring[pg] = cgi.R_RegisterShaderNoMip(va("textures/hud/coop_wheel_ring%d", n));
            for (i = 0; i < n; i++) {
                lit[pg][i]   = cgi.R_RegisterShaderNoMip(va("textures/hud/coop_wheel_hl%d_%d", n, i));
                icons[pg][i] = cgi.R_RegisterShader(CG_WheelSlice(pg, i)->icon);
            }
        }
    }
    if (!font || !fontBig) {
        return;
    }
    pg  = s_wheelPage;
    n   = CG_WheelCount(pg);
    sel = CG_WheelPick();
    if (sel != s_wheelHover) {
        if (sel >= 0 && CG_WheelUsable(pg, sel)) {
            cgi.S_StartLocalSound("sound/menu/scroll.wav", qfalse);
        }
        s_wheelHover = sel;
    }

    // dim the world behind it
    c[0] = c[1] = c[2] = 0.0f;
    c[3]               = 0.38f;
    cgi.R_SetColor(c);
    cgi.R_DrawBox(0, 0, W, H);

    // the ring, then the lit wedge (texture covers 2*165 units, wedges reach 0.98 of it)
    cgi.R_SetColor(NULL);
    cgi.R_DrawStretchPic(cx - 165.0f * k, cy - 165.0f * k, 330.0f * k, 330.0f * k, 0, 0, 1, 1, ring[pg]);
    if (sel >= 0 && CG_WheelUsable(pg, sel)) {
        cgi.R_DrawStretchPic(cx - 165.0f * k, cy - 165.0f * k, 330.0f * k, 330.0f * k, 0, 0, 1, 1, lit[pg][sel]);
    }

    // in every wedge: the icon, then its name and amount under it
    for (i = 0; i < n; i++) {
        float        a    = (float)i * 2.0f * (float)M_PI / n;
        float        px   = cx + (float)sin(a) * RM, py = cy - (float)cos(a) * RM;
        float        maxw = RM * 2.0f * (float)sin(M_PI / n) * 0.86f;
        qboolean     ok   = CG_WheelUsable(pg, i);
        qboolean     hot  = (i == sel && ok);
        qhandle_t    ic   = icons[pg][i];
        const char  *nm   = CG_WheelSlice(pg, i)->label;
        const float *col  = !ok ? grey : (hot ? ink : white);
        float        iw, ih;
        char         sub[32], cap[80];
        sub[0] = 0;
        if (pg == 0) {
            const wheelArt_t *art = ok ? CG_WheelGunArt(CG_WheelGunName(i)) : NULL;
            int               g   = ok ? CG_WheelShownGun(i) : -1;
            iw                    = 74.0f * k; // the silhouettes are 2:1
            ih                    = 37.0f * k;
            if (art) {
                // the exact gun, sized by how much of its canvas it fills so a pistol and a rifle read at the same weight
                float fw = 64.0f * k / (art->vw > 0.2f ? art->vw : 0.2f), fh = 2.0f * 28.0f * k / (art->vh > 0.2f ? art->vh : 0.2f);
                ic = art->sh;
                iw = fw < fh ? fw : fh;
                ih = iw * 0.5f;
            }
            if (ok && *CG_WheelGunLabel(i)) {
                nm = CG_WheelGunLabel(i);
            }
            if (g >= 0) {
                Com_sprintf(sub, sizeof(sub), i == 5 ? "x%d" : "%d", s_wheelGunAmmo[g]);
            }
            // two of a class: "< 1/2 >" over the icon
            if (ok && CG_WheelSlotGuns(i) > 1) {
                CG_WheelText(font, va("<  %d / %d  >", s_wheelVar[i] + 1, CG_WheelSlotGuns(i)), px, py - 30.0f * k, k * 0.8f, hot ? ink : gold);
            }
        } else {
            int cnt = CG_WheelAbilCount(i);
            iw = ih = 42.0f * k;
            if (cnt >= 0) {
                Com_sprintf(sub, sizeof(sub), "x%d", cnt);
            }
        }
        c[0] = c[1] = c[2] = 1.0f;
        if (hot) {
            c[0] = 0.16f; c[1] = 0.12f; c[2] = 0.06f; // dark ink on the gold wedge, cream elsewhere
        }
        c[3] = ok ? 1.0f : 0.22f;
        cgi.R_SetColor(c);
        cgi.R_DrawStretchPic(px - iw * 0.5f, py - ih * 0.5f - 6.0f * k, iw, ih, 0, 0, 1, 1, ic);
        if (sub[0]) {
            Com_sprintf(cap, sizeof(cap), "%s  %s", nm, sub);
        } else {
            Q_strncpyz(cap, nm, sizeof(cap));
        }
        CG_WheelTextFit(font, cap, px, py + 14.0f * k, k * 0.85f, maxw, col);
    }

    // centre: what you are pointing at
    {
        const char *big = "", *small = "";
        char        ammo[48];
        ammo[0] = 0;
        if (sel >= 0) {
            if (pg == 0) {
                int g = CG_WheelShownGun(sel);
                big   = CG_WheelOwned(sel) && *CG_WheelGunLabel(sel) ? CG_WheelGunLabel(sel) : CG_WheelSlice(0, sel)->label;
                if (!CG_WheelOwned(sel)) {
                    small = "Empty";
                } else if (g >= 0) {
                    Com_sprintf(ammo, sizeof(ammo), sel == 5 ? "x%d" : "%d rounds", s_wheelGunAmmo[g]);
                    if (CG_WheelSlotGuns(sel) > 1) {
                        Q_strcat(ammo, sizeof(ammo), s_wheelPad ? "   D-pad: next" : "   Scroll: next");
                    }
                    small = ammo;
                }
            } else {
                int cnt = CG_WheelAbilCount(sel);
                big     = CG_WheelSlice(1, sel)->label;
                if (cnt == 0) {
                    small = sel >= 3 ? "Earn it from an officer" : "None left";
                } else if (cnt > 0) {
                    Com_sprintf(ammo, sizeof(ammo), "%d left", cnt);
                    small = ammo;
                }
            }
        } else {
            big   = pg ? "ABILITIES" : "WEAPONS";
            small = "Point at a slot";
        }
        CG_WheelTextFit(fontBig, big, cx, cy - 14.0f * k, k * 1.05f, RI * 1.8f, gold);
        if (*small) {
            CG_WheelTextFit(font, small, cx, cy + 6.0f * k, k * 0.9f, RI * 1.8f, white);
        }
    }

    // page tabs above the ring, and how to switch / pick below it
    {
        float ty = cy - 165.0f * k - 26.0f * k;
        float w1 = 120.0f * k, h1 = 20.0f * k;
        for (i = 0; i < 2; i++) {
            float x0 = cx + (i ? 10.0f * k : -w1 - 10.0f * k);
            c[0] = c[1] = c[2] = 0.05f;
            c[3]               = 0.72f;
            if (i == pg) {
                c[0] = 0.80f; c[1] = 0.64f; c[2] = 0.30f; c[3] = 0.95f;
            }
            cgi.R_SetColor(c);
            cgi.R_DrawBox(x0, ty, w1, h1);
            CG_WheelText(font, i ? "ABILITIES" : "WEAPONS", x0 + w1 * 0.5f, ty + 3.0f * k, k * 0.95f, i == pg ? ink : white);
        }
        CG_WheelText(font, s_wheelPad ? "LB / RB  page      Release Y  select      B  cancel" : "Right-click / Q / E  page      Release / Click  select",
                     cx, cy + 165.0f * k + 8.0f * k, k * 0.85f, grey);
    }
    cgi.R_SetColor(NULL);
}

void CG_PushMenuTeamSelect_f(void)
{
    if (cgs.gametype == GT_SINGLE_PLAYER) {
        return;
    }
    if (CG_HzmSkipAutoPicker("team")) {
        return;
    }

    cgi.Cmd_Execute(EXEC_NOW, "ui_getplayermodel\n");
    switch (cgs.gametype) {
    case GT_FFA:
        cgi.Cmd_Execute(EXEC_NOW, "pushmenu SelectFFAModel\n");
        break;
    case GT_OBJECTIVE:
        cgi.Cmd_Execute(EXEC_NOW, "pushmenu ObjSelectTeam\n");
        break;
    default:
        cgi.Cmd_Execute(EXEC_NOW, "pushmenu SelectTeam\n");
        break;
    }
}

void CG_PushMenuWeaponSelect_f(void)
{
    if (cgs.gametype == GT_SINGLE_PLAYER) {
        return;
    }

    cgi.Cmd_Execute(EXEC_NOW, "pushmenu SelectPrimaryWeapon\n");
}

void CG_UseWeaponClass_f(void)
{
    const char *cmd;

    cmd = cgi.Argv(1);

    if (!Q_stricmp(cmd, "pistol")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_PISTOL;
    } else if (!Q_stricmp(cmd, "rifle")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_RIFLE;
    } else if (!Q_stricmp(cmd, "smg")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_SMG;
    } else if (!Q_stricmp(cmd, "mg")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_MG;
    } else if (!Q_stricmp(cmd, "grenade")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_GRENADE;
    } else if (!Q_stricmp(cmd, "heavy")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_HEAVY;
    } else if (!Q_stricmp(cmd, "item1") || !Q_stricmp(cmd, "item")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_ITEM1;
    } else if (!Q_stricmp(cmd, "item2")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_ITEM2;
    } else if (!Q_stricmp(cmd, "item3")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_ITEM3;
    } else if (!Q_stricmp(cmd, "item4")) {
        cg.iWeaponCommand = WEAPON_COMMAND_USE_ITEM4;
    }

    cg.iWeaponCommandSend = 0;
}

void CG_NextWeapon_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_USE_NEXT_WEAPON;
    cg.iWeaponCommandSend = 0;
}

void CG_PrevWeapon_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_USE_PREV_WEAPON;
    cg.iWeaponCommandSend = 0;
}

void CG_UseLastWeapon_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_USE_LAST_WEAPON;
    cg.iWeaponCommandSend = 0;
}

void CG_HolsterWeapon_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_HOLSTER;
    cg.iWeaponCommandSend = 0;
}

void CG_DropWeapon_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_DROP;
    cg.iWeaponCommandSend = 0;
}

void CG_ToggleItem_f(void)
{
    cg.iWeaponCommand     = WEAPON_COMMAND_USE_ITEM1;
    cg.iWeaponCommandSend = 0;
}

int CG_WeaponCommandButtonBits(void)
{
    int iShiftedWeaponCommand;

    if (!cg.iWeaponCommand) {
        return 0;
    }

    iShiftedWeaponCommand = cg.iWeaponCommand << 7;

    cg.iWeaponCommandSend++;
    if (cg.iWeaponCommandSend > 2) {
        cg.iWeaponCommand = 0;
    }

    return iShiftedWeaponCommand & GetWeaponCommandMask(cg_protocol >= PROTOCOL_MOHTA_MIN ? WEAPON_COMMAND_MAX_VER17 : WEAPON_COMMAND_MAX_VER6);
}
