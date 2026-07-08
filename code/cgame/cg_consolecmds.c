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

static consoleCommand_t commands[] = {
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

void CG_PushMenuTeamSelect_f(void)
{
    if (cgs.gametype == GT_SINGLE_PLAYER) {
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
