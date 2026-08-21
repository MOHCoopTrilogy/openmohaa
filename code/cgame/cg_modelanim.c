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
// Functions for doing model animation and attachments

#include "cg_local.h"
#include "tiki.h"

static qboolean cg_forceModelAllowed = qfalse;

// HZM coop [225] - set while dispatching frame commands for the draw-skipped 1P turret viewmodel
// in 3P: CG_ProcessEntityCommands (cg_commands.cpp) then runs only the SOUND-family commands, so
// the hidden gun keeps firing audio but never spawns its muzzle flash at the player's eyes.
qboolean cg_bCoopMuteVisualCmds = qfalse;

/*
===============
CG_GetPlayerModelTiki
===============
*/
const char *CG_GetPlayerModelTiki(const char *modelName)
{
    return va("models/player/%s.tik", modelName);
}

/*
===============
CG_GetPlayerLocalModelTiki
===============
*/
const char *CG_GetPlayerLocalModelTiki(const char *modelName)
{
    return va("models/player/%s_fps.tik", modelName);
}

/*
===============
CG_PlayerTeamIcon
===============
*/
void CG_PlayerTeamIcon(refEntity_t *pModel, entityState_t *pPlayerState)
{
    qboolean bInArtillery, bInTeam, bSpecialIcon;

    if (cg_protocol < PROTOCOL_MOHTA_MIN) {
        if (pPlayerState->eFlags & EF_ALLIES) {
            cg.clientinfo[pPlayerState->number].team = TEAM_ALLIES;
        } else if (pPlayerState->eFlags & EF_AXIS) {
            cg.clientinfo[pPlayerState->number].team = TEAM_AXIS;
        } else {
            cg.clientinfo[pPlayerState->number].team = TEAM_NONE;
        }
    }

    if (pPlayerState->number == cg.snap->ps.clientNum) {
        return;
    }

    bInTeam      = qfalse;
    bSpecialIcon = qfalse;
    if (cgs.gametype > GT_FFA
        && (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES && (pPlayerState->eFlags & EF_ALLIES)
            || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS && (pPlayerState->eFlags & EF_AXIS)
            || cg.snap->ps.stats[STAT_TEAM] != TEAM_AXIS && cg.snap->ps.stats[STAT_TEAM] != TEAM_ALLIES
                   && (pPlayerState->eFlags & EF_ANY_TEAM) != 0)) {
        bInTeam = qtrue;
    }

    if (cgs.gametype <= GT_FFA) {
        return;
    }

    bInArtillery = qfalse;
    if (pPlayerState->eFlags & EF_PLAYER_ARTILLERY) {
        bInArtillery = qtrue;
    }

    if (bInTeam || (pPlayerState->eFlags & (EF_PLAYER_IN_MENU | EF_PLAYER_TALKING)) || bInArtillery) {
        int         i;
        int         iTag;
        float       fAlpha;
        float       fDist;
        vec3_t      vTmp;
        refEntity_t iconEnt;

        memset(&iconEnt, 0, sizeof(iconEnt));
        if ((pPlayerState->eFlags & EF_PLAYER_TALKING) != 0 && ((cg.time >> 8) & 1) != 0) {
            iconEnt.hModel = cgi.R_RegisterModel("textures/hud/talking_headicon.spr");
            bSpecialIcon   = qtrue;
        } else if ((pPlayerState->eFlags & EF_PLAYER_IN_MENU) != 0) {
            iconEnt.hModel = cgi.R_RegisterModel("textures/hud/inmenu_headicon.spr");
            bSpecialIcon   = qtrue;
        } else {
            if (!bInTeam) {
                return;
            }

            if (bInArtillery) {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/inmenu_artilleryicon.spr");
                bSpecialIcon   = qtrue;
            } else if ((pPlayerState->eFlags & 0x80) != 0) {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/allies_headicon.spr");
            } else {
                iconEnt.hModel = cgi.R_RegisterModel("textures/hud/axis_headicon.spr");
            }
        }

        memset(vTmp, 0, sizeof(vTmp));
        AnglesToAxis(vTmp, iconEnt.axis);

        iconEnt.scale              = 0.5f;
        iconEnt.renderfx           = 0;
        iconEnt.reType             = RT_SPRITE;
        iconEnt.shaderTime         = 0.0f;
        iconEnt.frameInfo[0].index = 0;
        iconEnt.shaderRGBA[0]      = -1;
        iconEnt.shaderRGBA[1]      = -1;
        iconEnt.shaderRGBA[2]      = -1;
        VectorCopy(pModel->origin, iconEnt.origin);

        iTag = cgi.Tag_NumForName(pModel->tiki, "eyes bone");
        if (iTag == -1) {
            iconEnt.origin[2] = iconEnt.origin[2] + 96.0f;
        } else {
            orientation_t oEyes = cgi.TIKI_Orientation(pModel, iTag);

            for (i = 0; i < 3; ++i) {
                VectorMA(iconEnt.origin, oEyes.origin[i], pModel->axis[i], iconEnt.origin);
            }

            iconEnt.origin[2] = iconEnt.origin[2] + 20.0f;
        }

        VectorSubtract(iconEnt.origin, cg.refdef.vieworg, vTmp);
        fDist = VectorLength(vTmp);

        if (fDist < 256.0f) {
            iconEnt.scale = fDist / 853.0f + 0.2f;
        } else if (fDist > 512.0f) {
            // Make sure to scale so the icon can be seen far away
            iconEnt.scale = (fDist - 512.0f) / 2560.0f + 0.5f;
        }

        if (iconEnt.scale > 1.0f) {
            iconEnt.scale = 1.0f;
        }

        if (fDist > 256.0) {
            fAlpha = 1.0f;
        } else if (fDist >= 72.0f) {
            fAlpha = (fDist - 72.0f) / 184.0f;
        } else {
            fAlpha = 0.0f;
        }

        if (cg.snap->ps.stats[STAT_TEAM] == TEAM_ALLIES || cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
            fAlpha = fAlpha * 0.65f;
        } else {
            fAlpha = fAlpha * 0.4f;
        }

        if (bSpecialIcon) {
            int value = (int)((fAlpha + 0.6f) * 255.0f);
            if (value > 255) {
                value = 255;
            }
            iconEnt.shaderRGBA[3] = value;
        } else {
            iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);
        }

        if (fAlpha > 0.0 || bSpecialIcon) {
            if (bSpecialIcon) {
                VectorMA(iconEnt.origin, -2.0f, cg.refdef.viewaxis[0], iconEnt.origin);
                iconEnt.scale += 0.05f;
            }

            cgi.R_AddRefSpriteToScene(&iconEnt);

            if (bSpecialIcon && bInTeam && fAlpha > 0.0f) {
                if (pPlayerState->eFlags & EF_ALLIES) {
                    iconEnt.hModel = cgi.R_RegisterModel("textures/hud/allies_headicon.spr");
                } else {
                    iconEnt.hModel = cgi.R_RegisterModel("textures/hud/axis_headicon.spr");
                }
                VectorMA(iconEnt.origin, 4.0f, cg.refdef.viewaxis[0], iconEnt.origin);
                iconEnt.scale         = iconEnt.scale - 0.1;
                iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);
                cgi.R_AddRefSpriteToScene(&iconEnt);
            }
        }
    }
}

/*
===============
CG_ActorOverheadIcon
Draws an authentic MP-style overhead faction marker above an AI actor's head, using
the SAME render path as CG_PlayerTeamIcon: a world-space RT_SPRITE billboard submitted
via cgi.R_AddRefSpriteToScene. The engine sizes/positions it, so it sits centered above
the head and stays a readable size at range (the previous hand-rolled 2D screen
projection drifted off to the side and mis-scaled). bEnemy selects the swastika sprite
(axis) vs the allied star sprite. Anchored to the head tag + 20 units, same as players.
===============
*/
/* iconType: 0 = allied star, 1 = axis swastika, 2 = officer eagle (Reichsadler) */
static void CG_ActorOverheadIcon(refEntity_t *pModel, int iconType)
{
    int         i, iTag;
    float       fAlpha, fDist;
    vec3_t      vTmp;
    refEntity_t iconEnt;
    const char *sprName;

    if (iconType == 2) {
        sprName = "textures/hud/coop_officer_icon.spr";
    } else if (iconType == 1) {
        sprName = "textures/hud/coop_axis_icon.spr";
    } else {
        sprName = "textures/hud/coop_ally_icon.spr";
    }

    memset(&iconEnt, 0, sizeof(iconEnt));
    iconEnt.hModel = cgi.R_RegisterModel(sprName);
    if (!iconEnt.hModel) {
        return;
    }

    memset(vTmp, 0, sizeof(vTmp));
    AnglesToAxis(vTmp, iconEnt.axis);

    iconEnt.scale              = 0.5f;
    iconEnt.renderfx           = 0;
    iconEnt.reType             = RT_SPRITE;
    iconEnt.shaderTime         = 0.0f;
    iconEnt.frameInfo[0].index = 0;
    iconEnt.shaderRGBA[0]      = -1;
    iconEnt.shaderRGBA[1]      = -1;
    iconEnt.shaderRGBA[2]      = -1;
    VectorCopy(pModel->origin, iconEnt.origin);

    iTag = cgi.Tag_NumForName(pModel->tiki, "eyes bone");
    if (iTag == -1) {
        iTag = cgi.Tag_NumForName(pModel->tiki, "Bip01 Head");
    }
    if (iTag == -1) {
        iconEnt.origin[2] = iconEnt.origin[2] + 96.0f;
    } else {
        orientation_t oHead = cgi.TIKI_Orientation(pModel, iTag);
        for (i = 0; i < 3; ++i) {
            VectorMA(iconEnt.origin, oHead.origin[i], pModel->axis[i], iconEnt.origin);
        }
        iconEnt.origin[2] = iconEnt.origin[2] + 20.0f;
    }

    VectorSubtract(iconEnt.origin, cg.refdef.vieworg, vTmp);
    fDist = VectorLength(vTmp);

    if (fDist < 256.0f) {
        iconEnt.scale = fDist / 853.0f + 0.2f;
    } else if (fDist > 512.0f) {
        iconEnt.scale = (fDist - 512.0f) / 2560.0f + 0.5f;
    }
    if (iconEnt.scale > 1.0f) {
        iconEnt.scale = 1.0f;
    }

    if (fDist > 256.0f) {
        fAlpha = 1.0f;
    } else if (fDist >= 72.0f) {
        fAlpha = (fDist - 72.0f) / 184.0f;
    } else {
        fAlpha = 0.0f;
    }
    iconEnt.shaderRGBA[3] = (int)(fAlpha * 255.0f);

    // HZM coop - sprite world size = image pixel dims * scale (tr_model sprite path), so the HD icon
    // upscale (textures/hud/coop_*_icon.tga 32 -> 128) quadrupled the on-screen size ("overhead icons
    // are now MASSIVE"). Normalize back to the 32px-authored world size the distance curve above expects.
    iconEnt.scale *= 32.0f / 128.0f;

    if (fAlpha > 0.0f) {
        cgi.R_AddRefSpriteToScene(&iconEnt);
    }
}

/* Retained as an empty stub so CG_Draw2D's call (cg_drawtools.cpp) still links. The
 * old deferred 2D-stretchpic icon buffer it used to flush is gone, replaced by the
 * RT_SPRITE world sprite in CG_ActorOverheadIcon above. */
void CG_DrawCoopIcons(void)
{
}

/*
===============
CG_InterpolateAnimParms

Interpolate between current and next entity
===============
*/
void CG_InterpolateAnimParms(entityState_t *state, entityState_t *sNext, refEntity_t *model)
{
    static cvar_t *vmEntity = NULL;
    int            i;
    float          t;
    float          animLength;
    float          t1, t2;

    if (!vmEntity) {
        vmEntity = cgi.Cvar_Get("viewmodelanim", "1", 0);
    }

    if (sNext && sNext->usageIndex == state->usageIndex) {
        t1 = cg.time - cg.snap->serverTime;
        t2 = cg.nextSnap->serverTime - cg.snap->serverTime;
        t  = t1 / t2;

        model->actionWeight = (sNext->actionWeight - state->actionWeight) * t + state->actionWeight;

        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if (sNext->frameInfo[i].weight) {
                model->frameInfo[i].index = sNext->frameInfo[i].index;
                if (sNext->frameInfo[i].index == state->frameInfo[i].index && state->frameInfo[i].weight) {
                    model->frameInfo[i].weight =
                        (sNext->frameInfo[i].weight - state->frameInfo[i].weight) * t + state->frameInfo[i].weight;

                    if (sNext->frameInfo[i].time >= state->frameInfo[i].time) {
                        model->frameInfo[i].time =
                            (sNext->frameInfo[i].time - state->frameInfo[i].time) * t + state->frameInfo[i].time;
                    } else {
                        animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                        if (!animLength) {
                            t1 = 0.0;
                        } else {
                            t1 = (animLength + sNext->frameInfo[i].time - state->frameInfo[i].time) * t
                               + state->frameInfo[i].time;
                        }

                        t2 = t1;
                        while (t2 > animLength) {
                            t2 -= animLength;

                            if (t2 == t1) {
                                t2 = 1.0;
                                break;
                            }

                            t1 = t2;
                        }

                        model->frameInfo[i].time = t2;
                    }
                } else {
                    animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                    if (!animLength) {
                        t1 = 0.0;
                    } else {
                        t1 = sNext->frameInfo[i].time - (cg.nextSnap->serverTime - cg.time) / 1000.0;
                    }

                    model->frameInfo[i].time   = Q_max(0, t1);
                    model->frameInfo[i].weight = sNext->frameInfo[i].weight;
                }
            } else if (sNext->frameInfo[i].index == state->frameInfo[i].index) {
                animLength = cgi.Anim_Time(model->tiki, sNext->frameInfo[i].index);
                if (!animLength) {
                    t1 = 0.0;
                } else {
                    t1 = (cg.time - cg.snap->serverTime) / 1000.0 + state->frameInfo[i].time;
                }

                model->frameInfo[i].index  = Q_clamp_int(state->frameInfo[i].index, 0, model->tiki->a->num_anims - 1);
                model->frameInfo[i].time   = Q_min(animLength, t1);
                model->frameInfo[i].weight = (1.0 - t) * state->frameInfo[i].weight;
            } else {
                model->frameInfo[i].index  = -1;
                model->frameInfo[i].weight = 0.0;
            }
        }
    } else {
        // no next state, don't blend anims

        model->actionWeight = state->actionWeight;
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if (state->frameInfo[i].weight) {
                model->frameInfo[i].index  = Q_clamp_int(state->frameInfo[i].index, 0, model->tiki->a->num_anims - 1);
                model->frameInfo[i].time   = state->frameInfo[i].time;
                model->frameInfo[i].weight = state->frameInfo[i].weight;
            } else {
                model->frameInfo[i].index  = -1;
                model->frameInfo[i].weight = 0.0;
            }
        }
    }

    if (vmEntity->integer == state->number) {
        static cvar_t *curanim;
        if (!curanim) {
            curanim = cgi.Cvar_Get("viewmodelanimslot", "1", 0);
        }

        cgi.Cvar_Set("viewmodelanimclienttime", va("%0.2f", model->frameInfo[curanim->integer].time));
    }
}

/*
===============
CG_CastFootShadow

Cast complex foot shadow using lights
===============
*/
void CG_CastFootShadow(const vec_t *vLightPos, vec_t *vLightIntensity, int iTag, refEntity_t *model)
{
    int           i;
    float         fAlpha;
    float         fLength;
    float         fWidth;
    float         fAlphaOfs;
    float         fOfs;
    float         fPitchCos;
    vec3_t        vPos;
    vec3_t        vEnd;
    vec3_t        vDelta;
    vec3_t        vLightAngles;
    trace_t       trace;
    orientation_t oFoot;

    VectorCopy(model->origin, vPos);
    oFoot = cgi.TIKI_Orientation(model, iTag);
    VectorMA(oFoot.origin, 2, oFoot.axis[1], vEnd);
    for (i = 0; i < 3; i++) {
        VectorMA(vPos, vEnd[i], model->axis[i], vPos);
    }

    if (cg_shadowdebug->integer) {
        vec3_t vDir;

        //
        // show debug lines
        //
        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[0][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 1.0, 0.0, 0.0, 1.0);

        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[1][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 0.0, 1.0, 0.0, 1.0);

        memset(vDir, 0, sizeof(vDir));
        for (i = 0; i < 3; ++i) {
            VectorMA(vDir, oFoot.axis[2][i], model->axis[i], vDir);
        }
        VectorMA(vPos, 32.0, vDir, vEnd);
        cgi.R_DebugLine(vPos, vEnd, 0.0, 0.0, 1.0, 1.0);
    }

    // calculate the direction
    VectorSubtract(vLightPos, vPos, vDelta);
    VectorNormalizeFast(vDelta);
    vectoangles(vDelta, vLightAngles);

    // normalize to 180 degrees
    if (vLightAngles[0] > 180) {
        vLightAngles[0] -= 360;
    }

    if (vLightAngles[0] > -5.7319679) {
        // FIXME: what is -5.7319679?
        return;
    }

    fPitchCos = cos(DEG2RAD(vLightAngles[0]));
    if (fPitchCos > 0.955) {
        fAlpha = 1.0 - (fPitchCos - 0.955) * 25;
    } else {
        fAlpha = 1.0;
    }

    fLength = fPitchCos * fPitchCos * 32.0 + fPitchCos * 8.0 + 10.0;
    fOfs    = 0.5 - (-4.1 / tan(DEG2RAD(vLightAngles[0])) + 4.0 - fLength) / fLength * 0.5;
    VectorMA(vPos, -96.0, vDelta, vEnd);
    CG_Trace(&trace, vPos, vec3_origin, vec3_origin, vEnd, 0, MASK_FOOTSHADOW, qfalse, qtrue, "CG_CastFootShadow");

    if (cg_shadowdebug->integer) {
        cgi.R_DebugLine(vPos, vLightPos, 0.75, 0.75, 0.5, 1.0);
        cgi.R_DebugLine(vPos, vEnd, 1.0, 1.0, 1.0, 1.0);
    }

    if (trace.fraction == 1.0) {
        return;
    }

    trace.fraction -= 0.0427f;
    if (trace.fraction < 0) {
        trace.fraction = 0;
    }

    fWidth    = 10.f - (1.f - trace.fraction) * 6.f;
    fAlphaOfs = (1.f - trace.fraction) * fAlpha;

    fAlpha = Q_max(vLightIntensity[0], Q_max(vLightIntensity[1], vLightIntensity[2]));

    if (fAlpha < 0.1) {
        vLightIntensity[0] *= 0.1 / fAlpha * fAlphaOfs;
        vLightIntensity[1] *= 0.1 / fAlpha * fAlphaOfs;
        vLightIntensity[2] *= 0.1 / fAlpha * fAlphaOfs;
    } else {
        vLightIntensity[0] *= fAlphaOfs;
        vLightIntensity[1] *= fAlphaOfs;
        vLightIntensity[2] *= fAlphaOfs;
    }

    fAlpha = Q_max(vLightIntensity[0], Q_max(vLightIntensity[1], vLightIntensity[2]));
    if (fAlpha > 0.6) {
        vLightIntensity[0] *= 0.6 / fAlpha;
        vLightIntensity[1] *= 0.6 / fAlpha;
        vLightIntensity[2] *= 0.6 / fAlpha;
    }

    if (vLightIntensity[0] <= 0.01 && vLightIntensity[1] <= 0.01 && vLightIntensity[2] <= 0.01) {
        return;
    }

    CG_ImpactMark(
        cgs.media.footShadowMarkShader,
        trace.endpos,
        trace.plane.normal,
        vLightAngles[1],
        fWidth,
        fLength,
        vLightIntensity[0],
        vLightIntensity[1],
        vLightIntensity[2],
        1.0,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5,
        fOfs
    );
}

/*
===============
CG_CastSimpleFeetShadow

Cast basic feet shadow
===============
*/
void CG_CastSimpleFeetShadow(
    const trace_t *pTrace,
    float          fWidth,
    float          fAlpha,
    int            iRightTag,
    int            iLeftTag,
    const dtiki_t *tiki,
    refEntity_t   *model
)
{
    int           i;
    float         fShadowYaw;
    float         fLength;
    vec3_t        vPos, vRightPos, vLeftPos;
    vec3_t        vDelta;
    orientation_t oFoot;

    //
    // right foot
    //
    VectorCopy(pTrace->endpos, vRightPos);
    oFoot = cgi.TIKI_Orientation(model, iRightTag);
    VectorMA(oFoot.origin, 3, oFoot.axis[1], vPos);

    for (i = 0; i < 3; i++) {
        VectorMA(vRightPos, vPos[i], model->axis[i], vRightPos);
    }

    VectorMA(vRightPos, -2, oFoot.axis[1], vRightPos);

    //
    // left foot
    //
    VectorCopy(pTrace->endpos, vLeftPos);
    oFoot = cgi.TIKI_Orientation(model, iLeftTag);
    VectorMA(oFoot.origin, 3, oFoot.axis[1], vPos);

    for (i = 0; i < 3; i++) {
        VectorMA(vLeftPos, vPos[i], model->axis[i], vLeftPos);
    }

    VectorAdd(vRightPos, vLeftPos, vPos);
    VectorScale(vPos, 0.5, vPos);
    VectorSubtract(vRightPos, vLeftPos, vDelta);
    VectorMA(vLeftPos, 0.5, vDelta, vPos);

    // get the facing yaw
    fShadowYaw = vectoyaw(vDelta);
    fLength    = VectorNormalize(vDelta) * 0.5 + 12;
    if (fLength < fWidth * 0.7) {
        fLength = fWidth * 0.7;
    }

    // add the mark
    CG_ImpactMark(
        cgs.media.shadowMarkShader,
        vPos,
        pTrace->plane.normal,
        fShadowYaw,
        fWidth * 0.7,
        fLength,
        fAlpha,
        fAlpha,
        fAlpha,
        1.0,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5,
        0.5
    );
}

/*
===============
CG_EntityShadow

Returns the Z component of the surface being shadowed

  should it return a full plane instead of a Z?
===============
*/
#define SHADOW_DISTANCE 96

qboolean CG_EntityShadow(centity_t *cent, refEntity_t *model)
{
    int     iTagL, iTagR;
    float   alpha;
    float   fWidth;
    vec3_t  end;
    vec3_t  vMins, vMaxs;
    vec3_t  vSize;
    trace_t trace;

    iTagR = -1;

    if (cg_shadows->integer == 0) {
        return qfalse;
    }

    if (model->renderfx & RF_SKYENTITY) {
        // no shadows on sky entities
        return qfalse;
    }

    // HZM gl2 REAL CHARACTER SHADOWS. renderergl2 publishes r_coopRealShadows = 1 only while
    // it is actually writing skeletal characters into the sun cascade shadow maps
    // (r_charShadows 1 and the sun chain live). Suppress BOTH the HZM Phase-A directional
    // decal immediately below AND every vanilla foot/blob path after it, so the player sees
    // exactly one shadow per actor instead of a real cast shadow with a decal painted on top.
    //
    // GL1-SAFE BY CONSTRUCTION: renderergl1 never sets this cvar, so under gl1 it stays at the
    // "0" default registered right here and this branch is never taken - gl1 behaviour is
    // bit-identical with no reasoning required. This mirrors the existing r_coopSunValid
    // pattern (published by renderergl1/tr_scene.c, consumed just below).
    //
    // NOT cg_shadows: that is the SAME cvar the renderers register their internal r_shadows
    // under, it gates the rend2 pshadow pass, it also gates CG_Splash water marks, and its
    // registration defaults disagree between cgame (0) and both renderers (1).
    //
    // To force the decal back on while real shadows are running: r_charShadowBlob 1
    // (renderer side - it makes the renderer publish 0 here).
    {
        static cvar_t *sRealShadows = NULL;
        if (!sRealShadows) {
            sRealShadows = cgi.Cvar_Get("r_coopRealShadows", "0", 0);
        }
        if (sRealShadows && sRealShadows->integer) {
            return qfalse;
        }
    }

    // HZM coop - PHASE A directional shadow: when coop_shadowDir is on, draw a single elongated,
    // sun-oriented ground decal per model (overrides the straight-down foot/blob shadow). Uses fixed
    // sun-angle cvars (coop_shadowAz/El) so it needs no renderer sun-direction bridge. Pure cgame.
    {
        static cvar_t *sDir, *sAz, *sEl, *sLen, *sAuto, *sSunAz, *sSunEl, *sSunValid;
        if (!sDir) {
            sDir = cgi.Cvar_Get("coop_shadowDir", "1",   CVAR_ARCHIVE);
            sAz  = cgi.Cvar_Get("coop_shadowAz",  "45",  CVAR_ARCHIVE);   // MANUAL sun azimuth (deg)
            sEl  = cgi.Cvar_Get("coop_shadowEl",  "45",  CVAR_ARCHIVE);   // MANUAL sun elevation (deg); lower = longer
            sLen = cgi.Cvar_Get("coop_shadowLen", "1.5", CVAR_ARCHIVE);   // extra length multiplier
            // AUTO: follow the map's REAL sun (published each frame by the renderer, RE_RenderScene) when it has
            // one; otherwise fall back to the manual coop_shadowAz/El above. coop_shadowAuto 0 forces manual.
            sAuto     = cgi.Cvar_Get("coop_shadowAuto", "1",  CVAR_ARCHIVE);
            sSunAz    = cgi.Cvar_Get("r_coopSunAz",     "45", 0);
            sSunEl    = cgi.Cvar_Get("r_coopSunEl",     "45", 0);
            sSunValid = cgi.Cvar_Get("r_coopSunValid",  "0",  0);
        }
        if (sDir->integer) {
            float azDeg = sAz->value;
            float elDeg = sEl->value;
            float w;
            if (sAuto->integer && sSunValid->integer) {
                azDeg = sSunAz->value;   // the map's real sun
                elDeg = sSunEl->value;
            }
            w = model->scale * cgi.R_ModelRadius(model->hModel);
            if (w < 1) {
                return qfalse;
            }
            VectorCopy(model->origin, end);
            end[2] -= SHADOW_DISTANCE;
            cgi.CM_BoxTrace(&trace, model->origin, end, vec3_origin, vec3_origin, 0, MASK_PLAYERSOLID, qfalse);
            if (trace.fraction == 1.0 || trace.startsolid || trace.allsolid) {
                return qfalse;
            }
            alpha = (1.0 - trace.fraction) * 0.65f;
            {
                float elr = elDeg * ((float)M_PI / 180.0f);
                float azr = azDeg * ((float)M_PI / 180.0f);
                // [user 2026-08-20] "shadows are looking gigantic again". The length is
                // 1 + len/tan(elevation), and tan collapses as the sun drops: at the 0.17rad
                // (9.7deg) floor this reaches 9.5x the model radius, which on a low-sun map is a
                // shadow the size of a truck. Clamp the RESULT rather than the elevation, so a
                // low sun still gives a long shadow but never an absurd one, and fade it as it
                // stretches - a real grazing shadow is long AND faint, not long and black.
                float stretch = 1.0f + sLen->value / tan(elr < 0.17f ? 0.17f : elr);
                {
                    static cvar_t *sMax = NULL;
                    float          cap;
                    if (!sMax) {
                        sMax = cgi.Cvar_Get("coop_shadowStretchMax", "3.0", CVAR_ARCHIVE);
                    }
                    cap = sMax->value > 1.0f ? sMax->value : 1.0f;
                    if (stretch > cap) {
                        alpha  *= cap / stretch; // longer than we allow => proportionally fainter
                        stretch = cap;
                    }
                }
                vec3_t sunH, pos;
                sunH[0] = (float)cos(azr);
                sunH[1] = (float)sin(azr);
                sunH[2] = 0.0f;
                // trail the shadow centre away from the sun so it reads as cast behind the model
                VectorMA(trace.endpos, -w * (stretch - 1.0f) * 0.5f, sunH, pos);
                CG_ImpactMark(
                    cgs.media.shadowMarkShader, pos, trace.plane.normal,
                    azDeg, w * stretch, w, alpha, alpha, alpha, 1,
                    qfalse, qtrue, qfalse, qfalse, 0.5f, 0.5f);
            }
            return qtrue;
        }
    }

    if (cg_shadows->integer == 2 && (model->renderfx & RF_SHADOW_PRECISE)) {
        iTagL = cgi.Tag_NumForName(model->tiki, "Bip01 L Foot");
        if (iTagL != -1) {
            iTagR = cgi.Tag_NumForName(model->tiki, "Bip01 R Foot");
        }

        if (iTagR != -1) {
            int    iNumLights, iCurrLight;
            vec3_t avLightPos[16], avLightIntensity[16];

            iNumLights = Q_clamp(cg_shadowscount->integer, 1, 8);
            iNumLights = cgi.R_GatherLightSources(model->origin, avLightPos, avLightIntensity, iNumLights);
            if (iNumLights) {
                for (iCurrLight = 0; iCurrLight < iNumLights; iCurrLight++) {
                    CG_CastFootShadow(avLightPos[iCurrLight], avLightIntensity[iCurrLight], iTagL, model);
                    CG_CastFootShadow(avLightPos[iCurrLight], avLightIntensity[iCurrLight], iTagR, model);
                }

                // shadow was casted properly
                return qtrue;
            }
        }
    }

    // send a trace down from the player to the ground
    VectorCopy(model->origin, end);
    end[2] -= SHADOW_DISTANCE;

    cgi.CM_BoxTrace(&trace, model->origin, end, vec3_origin, vec3_origin, 0, MASK_PLAYERSOLID, qfalse);

    // no shadow if too high
    if (trace.fraction == 1.0) {
        return qfalse;
    }

    // since 2.0: no shadow if solid
    if (trace.startsolid || trace.allsolid) {
        return qfalse;
    }

    if ((cg_shadows->integer == 3) && (model->renderfx & RF_SHADOW_PRECISE)) {
        return qtrue;
    }

    //
    // get the bounds of the current frame
    //
    fWidth = model->scale * cgi.R_ModelRadius(model->hModel);
    if (fWidth < 1) {
        return qfalse;
    }

    // fade the shadow out with height
    alpha = (1.0 - trace.fraction) * 0.65f;

    if (model->renderfx & RF_SHADOW_PRECISE) {
        iTagL = cgi.Tag_NumForName(model->tiki, "Bip01 L Foot");
        if (iTagL != -1) {
            iTagR = cgi.Tag_NumForName(model->tiki, "Bip01 R Foot");
        }

        if (iTagR != -1) {
            if (cg_shadows->integer == 2) {
                alpha *= 0.6f;
            }

            CG_CastSimpleFeetShadow(&trace, fWidth, alpha, iTagR, iTagL, model->tiki, model);
            return qtrue;
        }
    }

    cgi.R_ModelBounds(model->hModel, vMins, vMaxs);
    VectorSubtract(vMaxs, vMins, vSize);
    VectorScale(vSize, 0.6f, vSize);

    // add the mark as a temporary, so it goes directly to the renderer
    // without taking a spot in the cg_marks array
    CG_ImpactMark(
        cgs.media.shadowMarkShader,
        trace.endpos,
        trace.plane.normal,
        cent->lerpAngles[YAW],
        vSize[1],
        vSize[0],
        alpha,
        alpha,
        alpha,
        1,
        qfalse,
        qtrue,
        qfalse,
        qfalse,
        0.5f,
        0.5f
    );

    return qtrue;
}

//
//
// NEW ANIMATION AND THREE PART MODEL SYSTEM
//
//

//=================
//CG_AnimationDebugMessage
//=================
void CG_AnimationDebugMessage(int number, const char *fmt, ...)
{
#ifndef NDEBUG
    if (cg_debugAnim->integer) {
        va_list argptr;
        char    msg[1024];

        va_start(argptr, fmt);
        Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
        va_end(argptr);

        if ((!cg_debugAnimWatch->integer) || ((cg_debugAnimWatch->integer - 1) == number)) {
            if (cg_debugAnim->integer == 2) {
                cgi.DebugPrintf(msg);
            } else {
                cgi.Printf(msg);
            }
        }
    }
#endif
}

/*
======================
CG_AttachEntity

Modifies the entities position and axis by the given
tag location
======================
*/
void CG_AttachEntity(
    refEntity_t *entity, refEntity_t *parent, dtiki_t *tiki, int tagnum, qboolean use_angles, vec3_t attach_offset
)
{
    int i;
    orientation_t or ;
    vec3_t tempAxis[3];
    vec3_t vOrigin;
    vec3_t vDeltaLightOrg;

    or = cgi.TIKI_Orientation(parent, tagnum);
    //cgi.Printf( "th = %d %.2f %.2f %.2f\n", tikihandle, or.origin[ 0 ], or.origin[ 1 ], or.origin[ 2 ] );

    VectorSubtract(entity->lightingOrigin, entity->origin, vDeltaLightOrg);
    VectorCopy(parent->origin, entity->origin);

    for (i = 0; i < 3; i++) {
        VectorMA(entity->origin, or.origin[i], parent->axis[i], entity->origin);
    }

    if (attach_offset[0] || attach_offset[1] || attach_offset[2]) {
        MatrixMultiply(or.axis, parent->axis, tempAxis);

        for (i = 0; i < 3; i++) {
            VectorMA(entity->origin, attach_offset[i], tempAxis[i], entity->origin);
        }
    }

    VectorCopy(entity->origin, entity->oldorigin);

    if (use_angles) {
        MatrixMultiply(entity->axis, or.axis, tempAxis);
        MatrixMultiply(tempAxis, parent->axis, entity->axis);
    }

    entity->scale *= parent->scale;
    entity->renderfx |= (parent->renderfx & ~(RF_FLAGS_NOT_INHERITED | RF_LIGHTING_ORIGIN));

    MatrixTransformVectorRight(entity->axis, vDeltaLightOrg, vOrigin);
    VectorAdd(entity->origin, vOrigin, entity->lightingOrigin);
}

/*
===============
CG_AttachEyeEntity
===============
*/
void CG_AttachEyeEntity(
    refEntity_t *entity, refEntity_t *parent, dtiki_t *tiki, int tagnum, qboolean use_angles, vec_t *attach_offset
)
{
    int i;

    VectorCopy(cg.refdef.vieworg, entity->origin);

    if (use_angles) {
        AnglesToAxis(cg.refdefViewAngles, entity->axis);
    }

    if (attach_offset[0] || attach_offset[1] || attach_offset[2]) {
        for (i = 0; i < 3; i++) {
            VectorMA(entity->origin, attach_offset[i], entity->axis[i], entity->origin);
        }
    }

    VectorCopy(entity->origin, entity->oldorigin);
    entity->scale *= parent->scale;
    entity->renderfx |= (parent->renderfx & ~(RF_FLAGS_NOT_INHERITED | RF_LIGHTING_ORIGIN));
    VectorCopy(parent->lightingOrigin, entity->lightingOrigin);
}

/*
===============
CG_IsValidServerModel
===============
*/
qboolean CG_IsValidServerModel(const char *modelpath)
{
    const char *str;
    int         i;

    for (i = 1; i < MAX_MODELS; i++) {
        str = CG_ConfigString(CS_MODELS + i);
        if (!Q_stricmp(str, modelpath)) {
            return qtrue;
        }
    }

    return qfalse;
}

/*
===============
CG_CheckValidModels

This verifies the allied player model and the german player model:
- If they don't exist on the client, reset to the default allied player model
- If they don't exist on the server, don't allow forceModel so the client explicitly know the skin isn't supported
===============
*/
void CG_CheckValidModels()
{
    const char *modelpath;
    qboolean    isDirty = qfalse;

    if (dm_playermodel->modified) {
        // Check for allied model
        modelpath = va("models/player/%s.tik", dm_playermodel->string);
        if (!cgi.R_RegisterModel(modelpath)) {
            cgi.Printf(
                "Allied model '%s' is invalid, resetting to '%s'\n", dm_playermodel->string, dm_playermodel->resetString
            );

            cgi.Cvar_Set("dm_playermodel", dm_playermodel->resetString);
            modelpath = va("models/player/%s.tik", dm_playermodel->string);
        }

        cg.serverAlliedModelValid = CG_IsValidServerModel(modelpath);
    }

    if (dm_playergermanmodel->modified) {
        // Check for axis model
        modelpath = va("models/player/%s.tik", dm_playergermanmodel->string);
        if (!cgi.R_RegisterModel(modelpath)) {
            cgi.Printf(
                "Allied model '%s' is invalid, resetting to '%s'\n",
                dm_playergermanmodel->string,
                dm_playergermanmodel->resetString
            );

            cgi.Cvar_Set("dm_playergermanmodel", dm_playergermanmodel->resetString);
            modelpath = va("models/player/%s.tik", dm_playergermanmodel->string);
        }

        cg.serverAxisModelValid = CG_IsValidServerModel(modelpath);
    }

    if (dm_playermodel->modified || dm_playergermanmodel->modified) {
        cg_forceModelAllowed = cg.serverAlliedModelValid && cg.serverAxisModelValid;
    }
}

/*
===============
CG_ServerModelLoaded
===============
*/
void CG_ServerModelLoaded(const char *name, qhandle_t handle)
{
    if (!Q_stricmpn(name, "models/player/", 14) && (!cg.serverAlliedModelValid || !cg.serverAxisModelValid)) {
        char modelName[MAX_QPATH];
        COM_StripExtension(name + 14, modelName, sizeof(modelName));

        //
        // The player model has been loaded on the server
        // so try again parsing
        //
        if (!Q_stricmp(modelName, dm_playermodel->string)) {
            dm_playermodel->modified = qtrue;
        }
        if (!Q_stricmp(modelName, dm_playergermanmodel->string)) {
            dm_playergermanmodel->modified = qtrue;
        }
    }
}

/*
===============
CG_ServerModelUnloaded
===============
*/
void CG_ServerModelUnloaded(qhandle_t handle)
{
#if 0
    if (cg.serverAlliedModelValid && handle == cg.hAlliedPlayerModelHandle) {
        dm_playermodel->modified = qtrue;
    }
    if (cg.serverAxisModelValid && handle == cg.hAxisPlayerModelHandle) {
        dm_playergermanmodel->modified = qtrue;
    }
#endif
}

/*
===============
CG_UpdateForceModels
===============
*/
void CG_UpdateForceModels()
{
    qhandle_t hModel;
    char     *pszAlliesPartial;
    char     *pszAxisPartial;
    char      szAlliesModel[256];
    char      szAxisModel[256];
    qboolean  isDirty;

    isDirty = dm_playermodel->modified || dm_playergermanmodel->modified || cg_forceModel->modified;

    if (!cg_forceModelAllowed) {
        if (isDirty) {
            cgi.Printf(
                "One or more of the selected players model don't exist on the server or are not loaded, using the "
                "default skin\n"
            );
        }

        return;
    }

    if (cg.pAlliedPlayerModel && cg.pAxisPlayerModel && !isDirty) {
        return;
    }

    pszAlliesPartial = dm_playermodel->string;
    pszAxisPartial   = dm_playergermanmodel->string;

    Com_sprintf(szAlliesModel, sizeof(szAlliesModel), "models/player/%s.tik", pszAlliesPartial);
    Com_sprintf(szAxisModel, sizeof(szAxisModel), "models/player/%s.tik", pszAxisPartial);

    hModel = cg.serverAlliedModelValid ? cgi.R_RegisterModel(szAlliesModel) : 0;
    if (!hModel) {
        Com_sprintf(szAlliesModel, sizeof(szAlliesModel), "models/player/%s.tik", dm_playermodel->resetString);
        hModel = cgi.R_RegisterModel(szAlliesModel);
    }

    if (hModel) {
        cg.hAlliedPlayerModelHandle = hModel;
        cg.pAlliedPlayerModel       = cgi.R_Model_GetHandle(hModel);
        if (!cg.pAlliedPlayerModel) {
            cg.hAlliedPlayerModelHandle = 0;
        }
    } else {
        cg.hAlliedPlayerModelHandle = 0;
        cg.pAlliedPlayerModel       = NULL;
    }

    hModel = cg.serverAxisModelValid ? cgi.R_RegisterModel(szAxisModel) : 0;
    if (!hModel) {
        Com_sprintf(szAxisModel, sizeof(szAxisModel), "models/player/%s.tik", dm_playergermanmodel->resetString);
        hModel = cgi.R_RegisterModel(szAxisModel);
    }

    if (hModel) {
        cg.hAxisPlayerModelHandle = hModel;
        cg.pAxisPlayerModel       = cgi.R_Model_GetHandle(hModel);
        if (!cg.pAxisPlayerModel) {
            cg.hAxisPlayerModelHandle = 0;
        }
    } else {
        cg.hAxisPlayerModelHandle = 0;
        cg.pAxisPlayerModel       = 0;
    }

    // Clear modified flag
    //dm_playermodel->modified       = qfalse;
    //dm_playergermanmodel->modified = qfalse;
}

/*
===============
CG_ProcessPlayerModel

Checks player models, and update force models
===============
*/
void CG_ProcessPlayerModel()
{
    CG_CheckValidModels();
    if (cg_forceModel->integer) {
        CG_UpdateForceModels();
    }

    // Clear modified flag
    dm_playermodel->modified       = qfalse;
    dm_playergermanmodel->modified = qfalse;
    cg_forceModel->modified        = qfalse;
}

// HZM coop - BAKED per-gun ADS iron-sight tune, dialled in-game with the tuning workbench (numpad pad) and
// captured via adssave. name = weapon configstring (CS_WEAPONS). Standing pitch/yaw/roll + screen shift x/y
// are absolute; crouch values are EXTRA, added on top of the standing values while ducked. Guns NOT listed
// fall back to the global cg_ads* cvars. cg_modelanim applies the rotations; cg_view applies the shift.
static const adsGunTune_t s_adsGunTune[] = {
    //  name                     sP     sY     sR    sSx    sSy      cP     cY    cR    cSx    cSy
    { "Colt 45", -2.5f, -2.0f,  1.5f, -0.02f, -0.02f,   1.5f, -8.5f,  4.0f,  -0.14f,  0.04f },
    { "Walther P38", -1.5f, -0.5f,  1.5f,   0.0f, -0.02f,   0.5f, -9.5f,  1.0f, -0.145f,  0.02f },
    { "Webley Revolver",        2.5f, -1.5f,  0.0f, -0.02f, 0.12f,   0.5f,-10.0f, 1.0f,-0.16f, 0.08f },
    { "Nagant Revolver",        2.5f, -1.5f,  0.0f, -0.02f, 0.08f,   0.5f,-10.0f, 1.0f,-0.16f, 0.02f },
    { "Beretta",                0.0f, -1.0f, -2.0f,  0.005f, 0.075f, -1.0f, -8.5f, 1.0f,-0.14f, 0.02f },
    { "Hi-Standard Silenced",   0.0f, -1.0f, -2.0f, -0.02f, 0.02f,   1.0f, -8.0f, 1.0f,-0.12f, 0.02f },
    { "M1 Garand", -7.5f, -1.0f, -1.0f, -0.02f, -0.22f,   3.5f,-38.5f,  3.5f,  -0.75f,  0.08f },
    { "Mauser KAR 98K", -7.5f, -1.0f, -1.0f, -0.02f, -0.22f,   4.5f,-34.5f,  6.5f, -0.625f,  0.08f },
    { "Lee-Enfield", -7.0f, -1.0f,  1.0f, -0.02f, -0.26f,   2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Mosin Nagant Rifle", -6.5f, -1.5f,  0.5f, -0.02f, -0.24f,   4.0f,-33.5f,  3.0f,  -0.61f, 0.105f },
    { "Carcano", -6.0f, -1.0f,  0.5f, -0.02f, -0.22f,   4.0f,-41.0f,  0.5f, -0.805f, 0.095f },
    { "DeLisle",                1.0f, -3.0f,  2.0f, -0.04f, 0.02f,   4.0f,-34.0f, 3.0f,-0.56f, 0.14f },
    { "Thompson",  1.0f,  2.0f,  0.0f,  0.02f,   0.0f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    { "MP40",  0.5f,  2.0f, -1.5f,  0.02f,   0.0f,   2.5f,-20.0f,  1.5f, -0.335f,  0.05f },
    { "Sten Mark II",           1.0f,  1.5f, -1.5f,  0.02f,-0.04f,   1.0f,-20.0f, 1.5f,-0.28f, 0.02f },
    { "PPSH SMG", -2.5f, 11.0f, -2.0f,  0.14f, -0.08f,   1.5f,-18.0f,  2.5f,  -0.26f,  0.04f },
    { "Moschetto", -8.0f,  6.0f, -2.0f,  0.115f,-0.325f,   2.0f,-18.5f,  1.5f, -0.285f,-0.005f },
    { "BAR",  1.0f, -0.5f,  0.0f,   0.0f,  0.04f,   2.0f,-23.0f,  1.5f,  -0.38f, 0.155f },
    { "StG 44",  0.0f,  2.5f,  0.0f,  0.04f,  0.02f,   2.5f,-20.5f,  2.5f, -0.325f,  0.06f },
    { "Vickers-Berthier",  7.0f,-11.5f, -1.0f, -0.12f,  0.28f,   1.5f,-26.5f,  1.5f, -0.505f,  0.18f },
    { "Bazooka", 11.0f,  7.5f, -1.0f,  0.20f,  0.16f,   1.5f, -7.0f,  3.5f,  -0.06f, 0.055f },
    { "Panzerschreck",         11.0f,  7.5f, -1.0f,  0.08f, 0.30f,   1.5f, -7.0f, 1.0f,-0.06f, 0.04f },
    { "PIAT",                 -12.5f, 21.0f,  2.0f,  0.24f,-0.32f,   1.5f, -7.0f, 1.0f,-0.10f, 0.02f },
    { "shotgun",-12.5f, 12.0f,  2.0f,  0.12f, -0.42f,  -3.0f,-43.0f,  1.0f, -0.715f, -0.02f },
    // [user 07-18] grease guns: dialled on the SILENCED variant; regular M3 copied to match (same gun).
    { "Silenced Grease Gun",    3.0f,  0.0f,  0.0f,   0.0f, 0.095f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    { "M3 Grease Gun",          3.0f,  0.0f,  0.0f,   0.0f, 0.095f,   1.0f,-10.5f,  1.5f, -0.165f,  0.02f },
    // [user 07-18 session 2] STANDING-ONLY tune pass (pistols + silenced variants, rifles, SMGs). The STAND
    // fields are dialled; the CROUCH fields on these NEW rows are the inherited default state the adssave
    // printed (crouch was NOT deliberately tuned this session) - fine as a starting point, refine later.
    // [user 2026-08-18] crouch re-synced to each gun's correct FAMILY donor ("i know i missed some for
    // crouch ads"): silenced pistols match their own base gun, PPS-43 the PPSH, Beretta M38 the Moschetto.
    // Still family guesses, not eyeballed - the standing fields remain the hand-dialled truth.
    { "Silenced Colt .45",      0.0f, -1.0f,  1.5f, -0.015f,-0.005f,   1.5f, -8.5f,  4.0f, -0.14f,  0.04f },
    { "Silenced Walther P38",  -2.0f,  0.0f,  1.5f, -0.005f,-0.025f,   0.5f, -9.5f,  1.0f, -0.145f,  0.02f },
    { "Silenced TT-33",        -2.0f, -1.0f,  1.5f, -0.01f, -0.015f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Silenced Beretta",      -1.0f, -1.5f,  1.5f, -0.005f, 0.01f, -1.0f, -8.5f, 1.0f,-0.14f, 0.02f },
    { "Silenced Luger P08",    -1.5f, -1.5f,  1.5f, -0.01f, -0.025f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Walther PPK",           -3.5f, -0.5f,  3.0f, -0.01f, -0.11f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Luger P08",              0.0f, -1.5f,  0.0f, -0.01f, -0.035f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Nambu Type 14",          1.5f, -1.5f,  0.0f, -0.015f, 0.015f,   1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "TT-33 Tokarev",         -1.0f, -0.5f,  0.0f, -0.015f,-0.01f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "Welrod",                -1.0f, -2.0f,  0.0f, -0.015f,-0.01f,    1.0f, -8.0f,  1.0f, -0.12f,  0.02f },
    { "M1 Carbine",           -10.5f, -0.5f, -1.0f, -0.015f,-0.385f,   4.5f,-34.5f,  6.5f, -0.625f, 0.08f },
    { "Arisaka Type 99",       -7.0f, -1.0f,  1.0f, -0.015f,-0.29f,    2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Springfield M1903",    -18.0f,  0.5f,  1.0f,  0.01f, -0.73f,    2.0f,-34.5f,  0.0f, -0.625f, 0.045f },
    { "Thompson 50rd",          4.5f,  2.0f,  0.0f,  0.02f,  0.08f,    1.0f,-10.5f,  1.5f, -0.165f, 0.02f },
    { "Silenced MP40",          4.5f,  4.0f,  0.0f,  0.045f, 0.095f,   2.5f,-20.0f,  1.5f, -0.335f,  0.05f },
    { "Type 100 SMG",          18.0f, -2.0f,  0.0f,  0.0f,   0.325f,   1.0f,-10.5f,  1.5f, -0.165f, 0.02f },
    { "Silenced PPS-43",       -2.0f,  4.0f,  0.0f,  0.065f, 0.005f,   1.5f,-18.0f,  2.5f,  -0.26f,  0.04f },
    { "Beretta M38",           -8.0f,  6.5f, -1.5f,  0.125f,-0.335f,   2.0f,-18.5f,  1.5f, -0.285f,-0.005f },
    { "Breda",                  1.5f,  2.0f, -3.0f,  0.045f, 0.035f,   2.0f,-23.0f,  1.5f, -0.38f,  0.155f },
};

/*
====================
CoopStripSkinSuffix

HZM coop [user 2026-08-17]. A skin variant is named "<Base Gun> (<Finish>)" - "Thompson (Gold)".
Strips the trailing parenthesised part so a cosmetic variant resolves to its base gun. Returns
qtrue only when something was actually stripped.
====================
*/
qboolean CoopStripSkinSuffix(const char *in, char *out, int outSize)
{
    const char *paren;
    int         len;

    if (!in || !*in || !out || outSize <= 0) {
        return qfalse;
    }
    paren = strstr(in, " (");
    if (!paren) {
        return qfalse;
    }
    len = (int)(paren - in);
    if (len <= 0 || len >= outSize) {
        return qfalse;
    }
    memcpy(out, in, len);
    out[len] = 0;
    return qtrue;
}

// [user 2026-08-18] "I wish there was a simpler way to get these new guns ads done and accurate
// without having to manually do it." DONOR ALIASES: a new gun that is mechanically the same
// family as a hand-dialled one borrows that gun's tune by name - one line here instead of a
// numpad session. Exact rows and the "(Finish)" strip still win, so any alias can be replaced
// by a real dialled row later without touching this list. Scoped guns are absent on purpose:
// their ADS is the scope overlay, not iron sights. Audit tool: docs/tools/ads_audit.py lists
// every shipped weapon name that resolves to no row.
static const char *const s_adsDonor[][2] = {
    {"DP-28",         "Vickers-Berthier"}, // top-magazine LMG, offset irons like the VB
    {"FG 42",         "StG 44"          }, // shoulder-fired automatic rifle
    {"G 43",          "M1 Garand"       }, // semi-auto battle rifle
    {"SVT 40",        "M1 Garand"       }, // semi-auto battle rifle
    {"Johnson M1941", "M1 Garand"       }, // semi-auto battle rifle
    {"Gewehrgranate", "Mauser KAR 98K"  }, // kar98 body with a launcher cup
    {"Mauser C96",    "Luger P08"       }, // German pistol
    {"S&W M10 .38",   "Webley Revolver" }, // top-break-style revolver sight picture
};

static float s_fAdsPose = 0.0f; // eased ADS pose factor (0 = hip, 1 = full sight alignment)

static const adsGunTune_t *CG_AdsTuneExact(const char *wpn)
{
    int i;

    for (i = 0; i < (int)(sizeof(s_adsGunTune) / sizeof(s_adsGunTune[0])); i++) {
        if (!Q_stricmp(wpn, s_adsGunTune[i].name)) {
            return &s_adsGunTune[i];
        }
    }
    return NULL;
}

const adsGunTune_t *CG_FindAdsTune(const char *wpn)
{
    int                 i;
    char                base[64];
    const char         *name;
    const adsGunTune_t *t;

    if (!wpn || !*wpn) {
        return NULL;
    }
    t = CG_AdsTuneExact(wpn);
    if (t) {
        return t;
    }
    //
    // [user 2026-08-17] No exact hit - fall back to the base gun. Without this a skin variant
    // silently loses every hand-dialled sight value in the table above, because the lookup is an
    // exact Q_stricmp and "Thompson (Gold)" is not "Thompson". Exact still wins, so a variant CAN
    // be given its own tuning later simply by adding a row for it.
    //
    name = wpn;
    if (CoopStripSkinSuffix(wpn, base, sizeof(base))) {
        name = base;
        t    = CG_AdsTuneExact(name);
        if (t) {
            return t;
        }
    }
    // [user 2026-08-18] donor stage: runs on the finish-stripped base name, so
    // "FG 42 (Gold)" -> "FG 42" -> StG 44's dialled values.
    for (i = 0; i < (int)(sizeof(s_adsDonor) / sizeof(s_adsDonor[0])); i++) {
        if (!Q_stricmp(name, s_adsDonor[i][0])) {
            return CG_AdsTuneExact(s_adsDonor[i][1]);
        }
    }
    return NULL;
}

/*
===============
CG_ModelAnim
===============
*/
void CG_ModelAnim(centity_t *cent, qboolean bDoShaderTime)
{
    entityState_t *s1;
    entityState_t *sNext = NULL;
    refEntity_t    model;
    int            i;
    vec3_t         vMins, vMaxs, vTmp;
    const char    *szTagName;
    int            iAnimFlags;
    qboolean       bThirdPerson = qfalse;
    qboolean       bCoopHideDraw = qfalse; // HZM coop - process commands/sounds but do not render [219]

    s1 = &cent->currentState;

    // HZM coop - STAGED ADS: first person is forced only when the ADS system asks for it (instant for
    // first-person players / cg_adsShoulder 0; for third-person players the over-the-shoulder aim stage
    // KEEPS the body drawn, and the wheel-up handoff flips this in LOCKSTEP with cg.renderingThirdPerson
    // in cg_view.c - both sides must use CG_AdsForceFirstPerson or you get the camera-in-body bug).
    bThirdPerson |= (cg_3rd_person->integer && !CG_AdsForceFirstPerson()) ? qtrue : qfalse;
    // HZM coop (bug-1234) - the bug-1217 DBNO LOCKSTEP THAT USED TO SIT HERE IS REVERTED.
    // It forced bThirdPerson = qfalse for the whole downed state on the theory that the body
    // draw had to match cg_view.c's camera force. That reasoning was sound in the abstract and
    // WRONG for this project: the user could go third person while downed, deliberately, and
    // relied on it - so the 'fix' removed a working feature to prevent a problem nobody had.
    // If a camera-in-head artefact ever does appear while downed, fix it on the CAMERA side in
    // cg_view.c, where the player still keeps the choice, not by force-hiding their body here.
    // HZM coop - IN COVER auto-3P: draw the own body whenever the cover view force is active
    // (lockstep with cg_view.c renderingThirdPerson - turret-camera-regression rule 2)
    // [user 2026-08-20] cover must not outrank the ADS first-person handoff - lockstep with the
    // matching change in cg_view.c (turret-camera-regression rule 2: if the camera goes first
    // person and the body draw does not, the camera sits inside the drawn head).
    if ((cg.snap->ps.pm_flags & PMF_COOP_COVER) && !CG_AdsForceFirstPerson()) { bThirdPerson = qtrue; }
    // Fixed in OPM
    //  Draw world model body when in camera
    bThirdPerson |= (cg.snap->ps.pm_flags & PMF_CAMERA_VIEW && !(cg.snap->ps.pm_flags & PMF_TURRET));
    // HZM coop [241] - NATIVE ZOOM lockstep: the camera side (cg_view.c) forces FIRST person while
    // scoped (STAT_INZOOM, turrets exempt) - the body draw must match or the 1P camera sits inside
    // the still-drawn head ("sniper scope looks at the back of the player's head"). This is exactly
    // the lockstep rule from the comment above, applied to the zoom term.
    if (cg.snap->ps.stats[STAT_INZOOM] && !(cg.snap->ps.pm_flags & PMF_TURRET)) {
        bThirdPerson = qfalse;
    }
    // HZM coop - REMOVED the 3rd-person MG42 experiment's `bThirdPerson |= PMF_TURRET` line. It force-drew
    // your own body in 3rd person on EVERY turret (MG42 nest, jeep .30cal, halftrack), overriding the
    // upstream line above (which deliberately excludes turrets so the mounted view stays clean first-person).
    // The cg_view.c half of that experiment was reverted but this half was missed = the "camera stuck in my
    // body" regression. Restored to stock: turrets are first person, own body not drawn.

    if ((cg.snap->ps.pm_flags & PMF_INTERMISSION) && s1->number == cg.snap->ps.clientNum && !bThirdPerson) {
        // Don't render the first-person model during intermission
        return;
    }

    memset(&model, 0, sizeof(model));

    if (cent->interpolate) {
        sNext = &cent->nextState;
    }

    // add loop sound only if it is not attached
    if (s1->loopSound && (s1->parent == ENTITYNUM_NONE)) {
        cgi.S_AddLoopingSound(
            cent->lerpOrigin,
            vec3_origin,
            cgs.sound_precache[s1->loopSound],
            s1->loopSoundVolume,
            s1->loopSoundMinDist,
            s1->loopSoundMaxDist,
            s1->loopSoundPitch,
            s1->loopSoundFlags
        );
    }

    if (cent->tikiLoopSound && (s1->parent == ENTITYNUM_NONE)) {
        cgi.S_AddLoopingSound(
            cent->lerpOrigin,
            vec3_origin,
            cent->tikiLoopSound,
            cent->tikiLoopSoundVolume,
            cent->tikiLoopSoundMinDist,
            cent->tikiLoopSoundMaxDist,
            cent->tikiLoopSoundPitch,
            cent->tikiLoopSoundFlags
        );
    }

    if (s1->renderfx & RF_SKYORIGIN) {
        AnglesToAxis(cent->lerpAngles, cg.sky_axis);
        VectorCopy(cent->lerpOrigin, cg.sky_origin);
    }

    // if set to invisible, skip
    if (!s1->modelindex) {
        return;
    }

    // set the entity number
    model.entityNumber = s1->number;

    // take the results of CL_InterpolateEntities
    VectorCopy(cent->lerpOrigin, model.origin);
    VectorCopy(cent->lerpOrigin, model.oldorigin);

    IntegerToBoundingBox(s1->solid, vMins, vMaxs);
    // calculate the light origin
    VectorAdd(vMins, vMaxs, vTmp);
    VectorMA(model.origin, 0.5, vTmp, model.lightingOrigin);
    // calculate the radius
    VectorSubtract(vMins, vMaxs, vTmp);
    model.radius = VectorLength(vTmp) * 0.5;

    if (s1->number == cg.snap->ps.clientNum) {
        if (!bThirdPerson) {
            PmoveAdjustAngleSettings_Client(
                cg.refdefViewAngles, cent->lerpAngles, &cg.predicted_player_state, &cent->currentState
            );
        }

        model.bone_quat = s1->bone_quat;
        model.bone_tag  = s1->bone_tag;
    } else {
        for (i = 0; i < NUM_BONE_CONTROLLERS; i++) {
            if (s1->bone_tag[i] >= 0) {
                if ((cent->interpolate) && (cent->nextState.bone_tag[i] == s1->bone_tag[i])) {
                    SlerpQuaternion(
                        s1->bone_quat[i], cent->nextState.bone_quat[i], cg.frameInterpolation, cent->bone_quat[i]
                    );
                } else {
                    cent->bone_quat[i][0] = s1->bone_quat[i][0];
                    cent->bone_quat[i][1] = s1->bone_quat[i][1];
                    cent->bone_quat[i][2] = s1->bone_quat[i][2];
                    cent->bone_quat[i][3] = s1->bone_quat[i][3];
                }
            }
        }

        model.bone_quat = cent->bone_quat;
        model.bone_tag  = s1->bone_tag;
    }

    // convert angles to axis
    AnglesToAxis(cent->lerpAngles, model.axis);

    // copy shader specific data
    if (s1->shader_data[0]) {
        model.shader_data[0] = s1->shader_data[0];
    } else {
        model.shader_data[0] = s1->tag_num;
    }

    if (s1->shader_data[1]) {
        model.shader_data[1] = s1->shader_data[1];
    } else {
        model.shader_data[1] = s1->skinNum;
    }

    if (bDoShaderTime) {
        if (cent->interpolate) {
            model.shaderTime =
                s1->shader_time + (sNext->shader_time - s1->shader_time) * cg.frameInterpolation + cg.time / 1000.0;
        } else {
            model.shaderTime = cg.time / 1000.0 + s1->shader_time;
        }
    }

    // Interpolated state variables
    if (cent->interpolate) {
        model.scale = s1->scale + cg.frameInterpolation * (cent->nextState.scale - s1->scale);
    } else {
        model.scale = s1->scale;
    }

    model.hOldModel = 0;
    model.tiki      = cgi.R_Model_GetHandle(cgs.model_draw[s1->modelindex]);

    if (s1->number != cg.snap->ps.clientNum && (s1->eType == ET_PLAYER || (s1->eFlags & EF_DEAD))) {
        if (cg_forceModel->integer && cg_forceModelAllowed) {
            //CG_UpdateForceModels();

            if (s1->eFlags & EF_AXIS) {
                model.hModel = cg.hAxisPlayerModelHandle;
                model.tiki   = cg.pAxisPlayerModel;
            } else {
                model.hModel = cg.hAlliedPlayerModelHandle;
                model.tiki   = cg.pAlliedPlayerModel;
            }

            if (model.hModel && model.tiki) {
                model.hOldModel = cgs.model_draw[s1->modelindex];
            } else {
                // fallback to non-forced model
                model.tiki   = cgi.R_Model_GetHandle(cgs.model_draw[s1->modelindex]);
                model.hModel = cgs.model_draw[s1->modelindex];
            }
        } else {
            model.hModel = cgs.model_draw[s1->modelindex];
        }

        if (!model.hModel || !model.tiki) {
            // Use a model in case it still doesn't exist
            if (s1->eFlags & EF_AXIS) {
                model.hModel = cgi.R_RegisterModel(CG_GetPlayerModelTiki(dm_playergermanmodel->resetString));
            } else {
                model.hModel = cgi.R_RegisterModel(CG_GetPlayerModelTiki(dm_playermodel->resetString));
            }
            model.tiki      = cgi.R_Model_GetHandle(model.hModel);
            model.hOldModel = cgs.model_draw[s1->modelindex];
        }
    } else {
        model.hModel = cgs.model_draw[s1->modelindex];
    }

    if (!model.tiki) {
        // still no model
        return;
    }

    // set skin
    model.skinNum = s1->skinNum;
    model.renderfx |= s1->renderfx;
    cgi.TIKI_SetEyeTargetPos(model.tiki, model.entityNumber, s1->eyeVector);

    CG_InterpolateAnimParms(s1, sNext, &model);

    if (cent->currentState.parent != ENTITYNUM_NONE) {
        int          iTagNum;
        refEntity_t *parent;
        dtiki_t     *tiki;

        parent = cgi.R_GetRenderEntity(cent->currentState.parent);
        if (!parent) {
            if (developer->integer > 1) {
                cgi.DPrintf("CG_ModelAnim: Could not find parent entity\n");
            }

            return;
        }

        // HZM coop - a mounted turret's FIRST-PERSON overlay gun (<model>_viewmodel.tik,
        // attached to the local player's "eyes bone" by TurretGun::P_CreateViewModel) must
        // never DRAW in third person - attached to the visible body it renders skewered
        // through the head (jeep .30cal report). DRAW-only skip: the early-return version
        // also killed the viewmodel's client frame commands = the owner's FIRE SOUND went
        // silent in 3P (bug-315). First person keeps it (it IS the gun view).
        if (s1->parent == cg.snap->ps.clientNum && bThirdPerson && model.tiki) {
            const char *szTikiName = cgi.TIKI_Name(model.tiki);
            if (szTikiName && strstr(szTikiName, "_viewmodel")) {
                bCoopHideDraw = qtrue;
            }
        }

        if (s1->parent != cg.snap->ps.clientNum || bThirdPerson) {
            // attach the model to the world model

            // Fixed in OPM
            //  Added checks to make sure the old model's tiki is valid
            if (parent->hOldModel && (tiki = cgi.R_Model_GetHandle(parent->hOldModel))) {
                szTagName = cgi.Tag_NameForNum(tiki, s1->tag_num & TAG_MASK);
                // Fixed in OPM
                //  Added checks to make sure the tag name is valid
                if (szTagName) {
                    tiki    = cgi.R_Model_GetHandle(parent->hModel);
                    iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                } else {
                    iTagNum = 0;
                }
            } else {
                tiki    = cgi.R_Model_GetHandle(parent->hModel);
                iTagNum = s1->tag_num;
            }

            CG_AttachEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);
        } else {
            tiki = cg.pPlayerFPSModel;

            // attach to the first person model
            if (cg.pLastPlayerWorldModel) {
                szTagName = cgi.Tag_NameForNum(cg.pLastPlayerWorldModel, s1->tag_num & TAG_MASK);
            } else {
                szTagName = cgi.Tag_NameForNum(tiki, s1->tag_num & TAG_MASK);
            }

            if (!Q_stricmp(szTagName, "eyes bone")) {
                iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                CG_AttachEyeEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);
            } else if (!Q_stricmp(szTagName, "tag_weapon_right") || !Q_stricmp(szTagName, "tag_weapon_left")) {
                iTagNum = cgi.Tag_NumForName(tiki, szTagName);
                CG_AttachEntity(&model, parent, tiki, iTagNum & TAG_MASK, s1->attach_use_angles, s1->attach_offset);

                // HZM coop: ADS iron-sight aim. A screen shift (r_weaponshift) moves the whole gun
                // uniformly, so it cannot line the REAR aperture up with the FRONT post; rotating the
                // weapon about tag_weapon (near the grip) tilts/angles the barrel so both sights fall on
                // the aim line. cg_adsPitch (up/down) + cg_adsYaw (left/right), live-tunable, ADS only.
                // [user 2026-08-20] "if the animation could be smoother coming out of ads for all
                // guns that would be ideal". The per-gun sight rotation used to be a hard on/off -
                // full alignment the instant ADS engaged, gone the instant it released - so the gun
                // SNAPPED at both ends regardless of how smoothly the zoom eased. Ease a 0..1 pose
                // factor and scale every angle by it: at 1 the sight picture is identical to before,
                // at 0 the block does not run at all, and in between the gun rotates into and out of
                // alignment. Out is deliberately gentler than in - coming down off the sights is a
                // relax, not a snap.
                // [user 2026-08-20] the ease now lives in cg_view.c (CG_AdsFactorAdvance) and is
                // advanced unconditionally once per frame. It used to be advanced HERE, inside the
                // first-person weapon-tag branch - which does not run in third person, in cover, on
                // a cutscene camera, or while dead - so the pose froze and was then re-applied at
                // full strength on the first frame this branch ran again.
                s_fAdsPose = CG_AdsPoseFactor();
                if (s_fAdsPose > 0.001f) {
                    vec3_t      vAdsA, vAdsB;
                    const char *adsWpn   = "";
                    const adsGunTune_t *adsT;
                    qboolean    tune   = (cg_adsTune && cg_adsTune->integer) ? qtrue : qfalse;
                    qboolean    ducked = (cg.predicted_player_state.pm_flags & PMF_DUCKED) ? qtrue : qfalse;
                    float       fAdsPitch, fAdsYaw, fAdsRoll;

                    if (cg.snap->ps.activeItems[1] >= 0) {
                        adsWpn = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
                    }
                    // Per-gun ADS sight values are BAKED in s_adsGunTune (CG_FindAdsTune). TUNE MODE
                    // (cg_adsTune 1) ignores the table and uses the global cg_ads* cvars so the held gun can
                    // be dialled live; any gun NOT in the table also falls back to those globals.
                    adsT      = tune ? NULL : CG_FindAdsTune(adsWpn);
                    fAdsPitch = adsT ? adsT->sPitch : (cg_adsPitch ? cg_adsPitch->value : 0.0f);
                    fAdsYaw   = adsT ? adsT->sYaw   : (cg_adsYaw   ? cg_adsYaw->value   : 0.0f);
                    fAdsRoll  = adsT ? adsT->sRoll  : (cg_adsRoll  ? cg_adsRoll->value  : 0.0f);
                    fAdsPitch *= s_fAdsPose; // scaled by the eased pose: full at 1, nothing at 0
                    fAdsYaw   *= s_fAdsPose;
                    fAdsRoll  *= s_fAdsPose;

                    // pitch: rotate forward + up about the left axis (tilt muzzle up/down)
                    if (fAdsPitch != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[1], model.axis[0], fAdsPitch);
                        RotatePointAroundVector(vAdsB, model.axis[1], model.axis[2], fAdsPitch);
                        VectorCopy(vAdsA, model.axis[0]);
                        VectorCopy(vAdsB, model.axis[2]);
                    }
                    // yaw: rotate forward + left about the up axis (angle muzzle left/right)
                    if (fAdsYaw != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[2], model.axis[0], fAdsYaw);
                        RotatePointAroundVector(vAdsB, model.axis[2], model.axis[1], fAdsYaw);
                        VectorCopy(vAdsA, model.axis[0]);
                        VectorCopy(vAdsB, model.axis[1]);
                    }
                    // roll: rotate up + left about the forward axis (un-tilt the gun) - standing
                    if (fAdsRoll != 0.0f) {
                        RotatePointAroundVector(vAdsA, model.axis[0], model.axis[1], fAdsRoll);
                        RotatePointAroundVector(vAdsB, model.axis[0], model.axis[2], fAdsRoll);
                        VectorCopy(vAdsA, model.axis[1]);
                        VectorCopy(vAdsB, model.axis[2]);
                    }

                    // CROUCH-only EXTRA correction (added on top of the standing rotation): the crouch pose
                    // hunches the body and carries the gun off the standing sight line. Per-gun crouch values
                    // come from the table; tune mode / un-tabled guns fall back to the cg_adsCrouch* cvars.
                    // [user 2026-08-20] THE ADS JOLT. These three were applied at FULL STRENGTH,
                    // never multiplied by s_fAdsPose, for the whole ~0.73s ease-out - and then
                    // vanished in a single frame when the enclosing gate (> 0.001f) closed. The
                    // values are not small: cYaw is -38.5 on the M1 Garand, -34.5 on the KAR98,
                    // -43.0 on the shotgun. Crouched, that is the entire per-gun sight correction
                    // snapping off at once, which is exactly the reported "gun may be way to the
                    // right aiming up ... so it snaps back into its non ADS position".
                    // Now scaled by BOTH the ADS factor and the eased crouch blend, so the pose
                    // is continuous when aiming in/out AND when crouching/standing while aimed.
                    // The guard also has to admit the blend-OUT frames after PMF_DUCKED clears,
                    // or standing up while aimed would still cut the correction off at once.
                    if (ducked || CG_AdsCrouchBlend() > 0.001f) {
                        float fCrouchMix = s_fAdsPose * CG_AdsCrouchBlend();
                        float cp = (adsT ? adsT->cPitch : (cg_adsCrouchPitch ? cg_adsCrouchPitch->value : 0.0f)) * fCrouchMix;
                        float cy = (adsT ? adsT->cYaw   : (cg_adsCrouchYaw   ? cg_adsCrouchYaw->value   : 0.0f)) * fCrouchMix;
                        float cr = (adsT ? adsT->cRoll  : (cg_adsCrouchRoll  ? cg_adsCrouchRoll->value  : 0.0f)) * fCrouchMix;
                        if (cp != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[1], model.axis[0], cp);
                            RotatePointAroundVector(vAdsB, model.axis[1], model.axis[2], cp);
                            VectorCopy(vAdsA, model.axis[0]);
                            VectorCopy(vAdsB, model.axis[2]);
                        }
                        if (cy != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[2], model.axis[0], cy);
                            RotatePointAroundVector(vAdsB, model.axis[2], model.axis[1], cy);
                            VectorCopy(vAdsA, model.axis[0]);
                            VectorCopy(vAdsB, model.axis[1]);
                        }
                        if (cr != 0.0f) {
                            RotatePointAroundVector(vAdsA, model.axis[0], model.axis[1], cr);
                            RotatePointAroundVector(vAdsB, model.axis[0], model.axis[2], cr);
                            VectorCopy(vAdsA, model.axis[1]);
                            VectorCopy(vAdsB, model.axis[2]);
                        }
                    }
                }
            } else {
                // Don't show the model at all
                return;
            }
        }

        if (s1->loopSound) {
            cgi.S_AddLoopingSound(
                model.origin,
                vec3_origin,
                cgs.sound_precache[s1->loopSound],
                s1->loopSoundVolume,
                s1->loopSoundMinDist,
                s1->loopSoundMaxDist,
                s1->loopSoundPitch,
                s1->loopSoundFlags
            );
        }

        if (cent->tikiLoopSound) {
            cgi.S_AddLoopingSound(
                cent->lerpOrigin,
                vec3_origin,
                cent->tikiLoopSound,
                cent->tikiLoopSoundVolume,
                cent->tikiLoopSoundMinDist,
                cent->tikiLoopSoundMaxDist,
                cent->tikiLoopSoundPitch,
                cent->tikiLoopSoundFlags
            );
        }

        // set the attached model to have the same render FX
        // HZM coop (bug-1217) - RF_THIRD_PERSON was written TWICE in both masks where
        // RF_FIRST_PERSON belongs. The pair means "the child's view-visibility is EXACTLY the
        // parent's": line 1 drops whatever the child was carrying, line 2 takes the parent's.
        // With the typo the first-person bit could never be DROPPED, only inherited. Restores the
        // intended semantics; verified bit-for-bit inert on today's data, so this is a latent-only
        // correctness fix - nothing in fgame ever sets RF_FIRST_PERSON (the renderEffects script
        // token table in entity.cpp has no name for it), and both CG_AttachEntity and
        // CG_AttachEyeEntity already OR the parent's copy in (RF_FIRST_PERSON is deliberately NOT
        // in RF_FLAGS_NOT_INHERITED), so child == parent either way for every entity that exists.
        model.renderfx &= ~(RF_FIRST_PERSON | RF_THIRD_PERSON | RF_DEPTHHACK);
        model.renderfx |= parent->renderfx & (RF_FIRST_PERSON | RF_THIRD_PERSON | RF_DEPTHHACK);
    }

    for (i = 0; i < 3; i++) {
        model.shaderRGBA[i] = cent->color[i] * 255;
    }
    model.shaderRGBA[3] = s1->alpha * 255;

    // set surfaces
    memcpy(model.surfaces, s1->surfaces, MAX_MODEL_SURFACES);

    // HZM coop (bug-1208) - THIS IS A VIEW-MODEL HIDER, so it must not run in third person.
    // The stock parenthesisation was `((!cg_drawviewmodel->integer && !bThirdPerson) || STAT_INZOOM)`:
    // !bThirdPerson guarded ONLY the cg_drawviewmodel clause, so the STAT_INZOOM clause fired
    // unconditionally and nodraw'd EVERY surface of EVERY entity attached to the local player even
    // while the 3rd-person body was on screen - the switcher helmet (attached to "Bip01 Head",
    // coop_mod/helmet.scr), holstered weapons, gear, all of it. Reachable because zoom normally
    // forces first person (see the STAT_INZOOM term where bThirdPerson is computed) EXCEPT on a
    // turret: PMF_TURRET is exempt there, and VehicleTurretGun force-zooms its gunner purely to pin
    // the fov (fgame/player.cpp ToggleZoom), so mounting any MG42 / jeep .30cal / halftrack in 3rd
    // person stripped the player's helmet and every other attached prop.
    // Hoisting !bThirdPerson out is bit-IDENTICAL in first person (it is already true there), so the
    // viewmodel/zoom behaviour this block exists for is unchanged; it simply stops firing in 3P,
    // where there is no viewmodel to hide.
    if (!(s1->renderfx & RF_ALWAYSDRAW) && s1->parent != ENTITYNUM_NONE && s1->parent == cg.snap->ps.clientNum
        && !bThirdPerson && (!cg_drawviewmodel->integer || cg.snap->ps.stats[STAT_INZOOM])) {
        // hide all surfaces while zooming or if the viewmodel shouldn't be shown
        for (i = 0; i < MAX_MODEL_SURFACES; i++) {
            model.surfaces[i] |= MDL_SURFACE_NODRAW;
        }
    }

    if (!(s1->renderfx & RF_DONTDRAW) && !bCoopHideDraw && (model.renderfx & RF_SHADOW)) {
        // add the shadow
        CG_EntityShadow(cent, &model);
    }

    iAnimFlags = 0;

    // combine anim flags from all frame infos
    for (i = 0; i < MAX_FRAMEINFOS; i++) {
        if (model.frameInfo[i].weight && model.frameInfo[i].index >= 0) {
            iAnimFlags |= cgi.Anim_Flags(model.tiki, model.frameInfo[i].index);
        }
    }

    if (iAnimFlags & TAF_AUTOSTEPS) {
        int iTagNum;
        // Automatically calculate the footsteps sounds

        if (cent->bFootOnGround_Right) {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 R Foot");
            if (iTagNum >= 0) {
                cent->bFootOnGround_Right = cgi.TIKI_IsOnGround(&model, iTagNum, 13.653847f);
            } else {
                cent->bFootOnGround_Right = qtrue;
            }
        } else {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 R Foot");
            if (iTagNum >= 0) {
                if (cgi.TIKI_IsOnGround(&model, iTagNum, 13.461539f)) {
                    CG_Footstep(
                        "Bip01 R Foot",
                        cent,
                        &model,
                        (iAnimFlags & TAF_AUTOSTEPS_RUNNING),
                        (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT)
                    );
                    cent->bFootOnGround_Right = qtrue;
                }
            } else {
                cent->bFootOnGround_Right = qtrue;
            }
        }

        if (cent->bFootOnGround_Left) {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 L Foot");
            if (iTagNum >= 0) {
                cent->bFootOnGround_Left = cgi.TIKI_IsOnGround(&model, iTagNum, 13.653847f);
            } else {
                cent->bFootOnGround_Left = qtrue;
            }
        } else {
            iTagNum = cgi.Tag_NumForName(model.tiki, "Bip01 L Foot");
            if (iTagNum >= 0) {
                if (cgi.TIKI_IsOnGround(&model, iTagNum, 13.461539f)) {
                    CG_Footstep(
                        "Bip01 L Foot",
                        cent,
                        &model,
                        (iAnimFlags & TAF_AUTOSTEPS_RUNNING),
                        (iAnimFlags & TAF_AUTOSTEPS_EQUIPMENT)
                    );

                    cent->bFootOnGround_Left = qtrue;
                }
            } else {
                cent->bFootOnGround_Left = qtrue;
            }
        }
    } else {
        cent->bFootOnGround_Left  = qtrue;
        cent->bFootOnGround_Right = qtrue;
    }

    if (cent->currentState.eType == ET_PLAYER && !(cent->currentState.eFlags & EF_DEAD)) {
        CG_PlayerTeamIcon(&model, &cent->currentState);
    }


    if ((cent->currentState.eType == ET_MODELANIM || cent->currentState.eType == ET_MODELANIM_SKEL)
        && (cent->currentState.renderfx & RF_COOP_BOSS)
        && !(cent->currentState.eFlags & EF_DEAD)) {
        int iconType;
        /* The officer is additionally tagged "+additivedynamiclight" (RF_ADDITIVE_DLIGHT)
         * by the coop script as an officer marker. That bit is NOT set by default on
         * sentients (RF_SHADOW_PRECISE is -> it put the eagle over every actor AND player),
         * has no visual effect without an attached dlight, and the stock game.dll already
         * supports the token (a brand-new renderfx token would need an fgame rebuild this
         * build tree can't produce). It gets the eagle icon. */
        if (cent->currentState.renderfx & RF_ADDITIVE_DLIGHT) {
            iconType = 2; /* officer -> Reichsadler eagle */
        } else if (cent->currentState.eFlags & EF_AXIS) {
            iconType = 1; /* axis -> swastika */
        } else {
            iconType = 0; /* ally -> star */
        }
        CG_ActorOverheadIcon(&model, iconType);
    }

    if (s1->number == cg.snap->ps.clientNum) {
        if ((!cg.bFPSModelLastFrame && !bThirdPerson) || (cg.bFPSModelLastFrame && bThirdPerson)) {
            // reset the animations when toggling 3rd person
            for (i = 0; i < MAX_FRAMEINFOS; i++) {
                cent->animLast[i] = -1;
            }

            cent->animLastWeight = 0;
            cent->usageIndexLast = 0;

            cg.bFPSModelLastFrame = !bThirdPerson;
        }

        // player footsteps, walking/falling
        if (cg.bFPSOnGround != cg.predicted_player_state.walking) {
            cg.bFPSOnGround = cg.predicted_player_state.walking;
            if (cg.predicted_player_state.walking) {
                CG_LandingSound(cent, &model, 1.0, 1);
            } else {
                if (cent->iNextLandTime < cg.time) {
                    CG_Footstep(0, cent, &model, 1, 1);
                }

                cent->iNextLandTime = cg.time + 200;
            }
        }

        if (!bThirdPerson) {
            // first person view

            if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)
                && (cg.snap->ps.stats[STAT_HEALTH] <= 0 || cg_animationviewmodel->integer)) {
                // use world position for this case
                CG_OffsetFirstPersonView(&model, qtrue);
            }

            if (!cg.pLastPlayerWorldModel || cg.pLastPlayerWorldModel != model.tiki) {
                qhandle_t hModel;
                char      fpsname[128];

                COM_StripExtension(model.tiki->a->name, fpsname, sizeof(fpsname));
                Q_strcat(fpsname, sizeof(fpsname), "_fps.tik");

                hModel = cgi.R_RegisterModel(fpsname);
                if (hModel) {
                    cg.hPlayerFPSModelHandle = hModel;
                    cg.pPlayerFPSModel       = cgi.R_Model_GetHandle(hModel);
                    if (!cg.pPlayerFPSModel) {
                        cg.pPlayerFPSModel = model.tiki;
                    }
                } else {
                    if (cg.snap->ps.stats[STAT_TEAM] == TEAM_AXIS) {
                        hModel = cgi.R_RegisterModel(CG_GetPlayerLocalModelTiki(dm_playergermanmodel->resetString));
                    } else {
                        hModel = cgi.R_RegisterModel(CG_GetPlayerLocalModelTiki(dm_playermodel->resetString));
                    }

                    if (hModel) {
                        cg.hPlayerFPSModelHandle = hModel;
                        cg.pPlayerFPSModel       = cgi.R_Model_GetHandle(hModel);

                        if (!cg.pPlayerFPSModel) {
                            cg.pPlayerFPSModel = model.tiki;
                        }
                    } else {
                        cg.hPlayerFPSModelHandle = cgs.model_draw[s1->modelindex];
                        cg.pPlayerFPSModel       = model.tiki;
                    }
                }

                cg.pLastPlayerWorldModel = model.tiki;
            }

            model.tiki   = cg.pPlayerFPSModel;
            model.hModel = cg.hPlayerFPSModelHandle;
            memset(model.surfaces, 0, sizeof(model.surfaces));

            CG_ViewModelAnimation(&model);
            model.renderfx |= RF_FRAMELERP;
            cgi.ForceUpdatePose(&model);

            if ((cent->currentState.eFlags & EF_UNARMED) || cg_drawviewmodel->integer <= 1
                || cg.snap->ps.stats[STAT_INZOOM] || cg.snap->ps.stats[STAT_HEALTH] <= 0) {
                // unarmed or zooming, hide the arms
                for (i = 0; i < MAX_MODEL_SURFACES; i++) {
                    model.surfaces[i] |= MDL_SURFACE_NODRAW;
                }
            } else {
                // show/hide the garand hand depending if it's a rifle or not
                // so the hand can hold the rifle correctly

                const char *weaponstring = "";
                int         iSurfaceNum;

                if (cg.snap->ps.activeItems[1] >= 0) {
                    weaponstring = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
                }

                if (!Q_stricmp(weaponstring, "M1 Garand") || !Q_stricmp(weaponstring, "Springfield '03 Sniper")
                    || !Q_stricmp(weaponstring, "Mauser KAR 98K") || !Q_stricmp(weaponstring, "KAR98 - Sniper")) {
                    // show the garand hands

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                    }

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] &= ~MDL_SURFACE_NODRAW;
                    }
                } else {
                    // hide the garand hands

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                    }

                    iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                    if (iSurfaceNum >= 0) {
                        model.surfaces[iSurfaceNum] &= ~MDL_SURFACE_NODRAW;
                    }
                }

                // HZM coop - ADS off-hand HIDE. The raised aim poses bake the support (off) hand for a
                // different gun's foregrip, so on many weapons it hovers off the gun while aiming. Instead
                // of re-posing every weapon, just HIDE the support hand while aiming down the sights (the
                // trigger hand + gun stay, sight alignment + tuning unchanged). Covers both the normal
                // "lefthand" and the rifle "garandhand" support surfaces. Toggle: cg_adsHideOffHand 0.
                // ONLY while the steady aim pose (VM_ANIM_CHARGE) is playing - during a bolt rechamber,
                // reload, or weapon switch the vm anim changes away from charge, so the support hand
                // re-appears to work the bolt / magazine (bolt rifles like the Kar98 need this).
                {
                    cvar_t *pHideOff = cgi.Cvar_Get("cg_adsHideOffHand", "1", CVAR_ARCHIVE);
                    if (pHideOff && pHideOff->integer && CG_AimingDownSights()
                        && cg.snap->ps.iViewModelAnim == VM_ANIM_CHARGE) {
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "lefthand");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "garandhand");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                        // also hide the off-arm: "viewsleeves" is the forearm/sleeve geometry that was
                        // left dangling once the support hand was hidden. Leaves trigger hand + gun only.
                        iSurfaceNum = cgi.Surface_NameToNum(model.tiki, "viewsleeves");
                        if (iSurfaceNum >= 0) {
                            model.surfaces[iSurfaceNum] |= MDL_SURFACE_NODRAW;
                        }
                    }
                }
            }

            if (!(s1->eFlags & EF_CLIMBWALL)) {
                // when the player is not climbing ladders show the entity
                model.renderfx |= RF_DEPTHHACK;
            }

            if (!(cg.predicted_player_state.pm_flags & PMF_CAMERA_VIEW)) {
                if (cg.snap->ps.stats[STAT_HEALTH] > 0 && !cg_animationviewmodel->integer) {
                    CG_OffsetFirstPersonView(&model, qfalse);
                }

                AnglesToAxis(cg.refdefViewAngles, cg.refdef.viewaxis);
            }

            model.renderfx &= ~(RF_FIRST_PERSON | RF_THIRD_PERSON);
            // set the first person render flag
            model.renderfx |= RF_FIRST_PERSON;
        }
    }

    model.reType = RT_MODEL;
    if (!(s1->renderfx & RF_DONTDRAW) && !bCoopHideDraw) {
        cgi.R_Model_GetHandle(model.hModel);
        if (VectorCompare(model.origin, vec3_origin)) {
            VectorCopy(s1->origin, model.origin);
            AngleVectors(s1->angles, model.axis[0], model.axis[1], model.axis[2]);
        }

        // add to refresh list
        cgi.R_AddRefEntityToScene(&model, s1->parent);
    }

    CG_UpdateEntityEmitters(s1->number, &model, cent);

    // HZM coop [225] - the hidden 1P viewmodel still processes its frame commands for the FIRE
    // SOUND (bug-315 kept them), but its VISUAL commands (tagdlight + tagspawnlinked muzzle
    // flash) spawned at the player's eyes-bone gun - "the burst comes from the left" while the
    // real turret flashes at its own barrel (shot0020). Mute non-sound commands for the whole
    // command dispatch below when this entity is the draw-skipped viewmodel.
    cg_bCoopMuteVisualCmds = bCoopHideDraw;

    if (s1->usageIndex == cent->usageIndexLast) {
        // process the exit commands of the last animations
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            if ((cent->animLastWeight >> i) & 1) {
                if (!model.frameInfo[i].weight || model.frameInfo[i].index != cent->animLast[i]) {
                    CG_ProcessEntityCommands(TIKI_FRAME_EXIT, cent->animLast[i], s1->number, &model, cent);
                }
            }
        }
    }

    for (i = 0; i < MAX_FRAMEINFOS; i++) {
        // process the entry commands of the current anim
        if (model.frameInfo[i].weight) {
            if (!((cent->animLastWeight >> i) & 1) || model.frameInfo[i].index != cent->animLast[i]) {
                CG_ProcessEntityCommands(TIKI_FRAME_ENTRY, model.frameInfo[i].index, s1->number, &model, cent);
                if (cent->animLastTimes[i] == -1) {
                    cent->animLast[i]      = model.frameInfo[i].index;
                    cent->animLastTimes[i] = model.frameInfo[i].time;
                } else {
                    cent->animLastTimes[i] = 0;
                }
            }

            CG_ClientCommands(&model, cent, i);
        }

        cent->animLastTimes[i] = model.frameInfo[i].time;
        cent->animLast[i]      = model.frameInfo[i].index;

        if (model.frameInfo[i].weight) {
            cent->animLastWeight |= 1 << i;
        } else {
            cent->animLastWeight &= ~(1 << i);
        }
    }

    cg_bCoopMuteVisualCmds = qfalse; // HZM coop [225] - never leak the mute past this entity

    cent->usageIndexLast = cent->currentState.usageIndex;
}
