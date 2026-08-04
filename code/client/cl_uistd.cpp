/*
===========================================================================
Copyright (C) 2015 the OpenMoHAA team

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
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110EV_DEFAULT301  USA
===========================================================================
*/

#include "cl_ui.h"
#include "../qcommon/localization.h"

Event EV_Layout_PlayerStat
(
    "playerstat",
    EV_DEFAULT,
    "i",
    "playerstatValue",
    "set playerstat for layout\n"
);
// Added in 2.0
Event EV_Layout_PlayerStatAlpha
(
    "playerstatalpha",
    EV_DEFAULT,
    "i",
    "playerstatValue",
    "set playerstat to control alpha for layout\n"
);
Event EV_Layout_PlayerStatConfigstring
(
    "playerstatconfigstring",
    EV_DEFAULT,
    "i",
    "playerstatValue",
    "set playerstat for layout - will print out the corresponding configstring\n"
);
Event EV_Layout_MaxPlayerStat
(
    "maxplayerstat",
    EV_DEFAULT,
    "i",
    "maxPlayerstatValue",
    "set the playerstat that holds the max value for that stat.  Used for statbars\n"
);
Event EV_Layout_ItemIndex
(
    "itemstat",
    EV_DEFAULT,
    "i",
    "itemstatValue",
    "set itemstat for layout\n"
);
Event EV_Layout_InventoryRenderModelIndex
(
    "invmodelhand",
    EV_DEFAULT,
    "i",
    "handindex",
    "set the specified hand index to render a 3d model from\n"
);
Event EV_Layout_Statbar
(
    "statbar",
    EV_DEFAULT,
    "sII",
    "orientation min max",
    "make this label present the stat using a graphical bar.  Optionally specify a min and max value"
);
Event EV_Layout_StatbarTileShader
(
    "statbar_tileshader",
    EV_DEFAULT,
    "s",
    "shader",
    "set the tile shader for this statbar"
);
Event EV_Layout_StatbarTileShader_Flash
(
    "statbar_tileshader_flash",
    EV_DEFAULT,
    "s",
    "shader",
    "set the flash tile shader for this statbar"
);
Event EV_Layout_StatbarShader
(
    "statbar_shader",
    EV_DEFAULT,
    "s",
    "shader",
    "set the shader for this statbar"
);
Event EV_Layout_StatbarShader_Flash
(
    "statbar_shader_flash",
    EV_DEFAULT,
    "s",
    "shader",
    "set the flash shader for this statbar"
);
// Added in 2.0
Event EV_Layout_StatbarShader_Marker
(
    "statbar_shader_marker",
    EV_DEFAULT,
    "s",
    "shader",
    "set the marker shader for this statbar\nThis is drawn at the end of the status bar"
);
Event EV_Layout_Statbar_EndAngles
(
    "statbar_endangles",
    EV_DEFAULT,
    "ff",
    "startang endang",
    "Sets the start and end angles for a circular stat bar"
);
Event EV_Layout_Statbar_NeedleWidth
(
    "statbar_needlewidth",
    EV_DEFAULT,
    "f",
    "width",
    "Sets the width of the needle for a needle statbar"
);
Event EV_Layout_Statbar_RotatorSize
(
    "statbar_rotatorsize",
    EV_DEFAULT,
    "f",
    "width",
    "Sets the width of the needle for a needle statbar"
);
Event EV_Layout_DrawModelName
(
    "invmodelname",
    EV_DEFAULT,
    "s",
    "name",
    "set the name of the 3d model to render\n"
);
Event EV_Layout_RenderModel
(
    "rendermodel",
    EV_DEFAULT,
    "b",
    "bool",
    "Render the model specified by the cvar."
);
// HZM coop: weapon previews (armory) - guns are authored with wildly different origins
// and poses, so the hardcoded standing-player box crops/mislocates most of them. This
// flag frames the model's REAL bounds instead (the same auto-fit the inventory HUD uses).
Event EV_Layout_RenderModelFit
(
    "rendermodelfit",
    EV_DEFAULT,
    "b",
    "bool",
    "Frame the rendered model by its real bounds instead of the standing-player box."
);
Event EV_Layout_RenderModelOffset
(
    "modeloffset",
    EV_DEFAULT,
    "v",
    "offset",
    "Render model offset"
);
Event EV_Layout_RenderModelRotateOffset
(
    "modelrotateoffset",
    EV_DEFAULT,
    "v",
    "offset",
    "Render model rotation offset"
);
Event EV_Layout_RenderModelAngles
(
    "modelangles",
    EV_DEFAULT,
    "v",
    "angles",
    "Render model angles"
);
Event EV_Layout_RenderModelScale
(
    "modelscale",
    EV_DEFAULT,
    "f",
    "scale",
    "Render model scale"
);
Event EV_Layout_RenderModelAnim
(
    "modelanim",
    EV_DEFAULT,
    "s",
    "anim",
    "Render model anim"
);
// HZM coop FIT-TUNE: read this widget's transform LIVE from a cvar holding 7 floats
// "offX offY offZ scale pitch yaw roll". Lets the armory position every preview model via
// cvars (per-gun baked values, nudged live by the `uifit` console command). Empty/short = static.
Event EV_Layout_RenderModelXformCvar
(
    "modelxformcvar",
    EV_DEFAULT,
    "s",
    "cvar",
    "HZM coop: read offset(3)+scale(1)+angles(3) live from a 7-float cvar (fit-tune)."
);
// HZM coop: composite a second model onto a bone tag of this widget's model (helmet on the operator).
// modelattachcvar names a cvar holding the attach model's .tik path (e.g. coop_loHelm); modelattachtag
// names the bone (e.g. "Bip01 Head"). The attach model tracks the idle anim and is nudged by coop_loXfmCH.
Event EV_Layout_RenderModelAttachCvar
(
    "modelattachcvar",
    EV_DEFAULT,
    "s",
    "cvar",
    "HZM coop: cvar naming a model path to composite onto the attach tag (helmet on operator)."
);
Event EV_Layout_RenderModelAttachTag
(
    "modelattachtag",
    EV_DEFAULT,
    "s",
    "tag",
    "HZM coop: bone tag the attach model rides (e.g. \"Bip01 Head\")."
);
// HZM coop: SECOND attach slot - composite the inspected weapon into the operator's hand.
Event EV_Layout_RenderModelAttachCvar2
(
    "modelattachcvar2",
    EV_DEFAULT,
    "s",
    "cvar",
    "HZM coop: cvar naming a 2nd model path to composite onto attach tag 2 (weapon in operator's hand)."
);
Event EV_Layout_RenderModelAttachTag2
(
    "modelattachtag2",
    EV_DEFAULT,
    "s",
    "tag",
    "HZM coop: bone tag the 2nd attach model rides (e.g. \"tag_weapon_right\")."
);
// HZM coop: read the model's anim name LIVE from a cvar (overrides the static modelanim). Lets the armory
// swap the operator's STANCE per inspected weapon class (rifle hold vs pistol hold) by setting one cvar.
// Empty/unset falls back to the static modelanim.
Event EV_Layout_RenderModelAnimCvar
(
    "modelanimcvar",
    EV_DEFAULT,
    "s",
    "cvar",
    "HZM coop: read the model anim name live from a cvar (per-weapon operator stance)."
);
// HZM coop: click-and-drag to rotate the rendered model. Names a cvar whose value (degrees) is ADDED to
// the model's yaw each frame; a horizontal left-drag over the widget adds to it (sensitivity = cvar
// coop_loSpinSens). Setting it makes this label a mouse responder (AllowActivate). Used by the armory
// operator so the player can spin the character to inspect the loadout from any angle.
Event EV_Layout_RenderModelSpinCvar
(
    "modelspincvar",
    EV_DEFAULT,
    "s",
    "cvar",
    "HZM coop: click-drag rotate - horizontal mouse drag adds degrees to this cvar (model yaw)."
);
Event EV_ClearInvItemReference
(
    "clearinvitemref",
    EV_DEFAULT,
    NULL,
    NULL,
    "used internally when the inventory is reset\n"
);

CLASS_DECLARATION(UILabel, UIFakkLabel, NULL) {
    {&EV_Layout_PlayerStat,                &UIFakkLabel::LayoutPlayerStat             },
    {&EV_Layout_PlayerStatAlpha,           &UIFakkLabel::LayoutPlayerStatAlpha        },
    {&EV_Layout_PlayerStatConfigstring,    &UIFakkLabel::LayoutPlayerStatConfigstring },
    {&EV_Layout_MaxPlayerStat,             &UIFakkLabel::LayoutMaxPlayerStat          },
    {&EV_Layout_ItemIndex,                 &UIFakkLabel::LayoutItemIndex              },
    {&EV_Layout_InventoryRenderModelIndex, &UIFakkLabel::InventoryRenderModelIndex    },
    {&EV_Layout_Statbar,                   &UIFakkLabel::LayoutStatbar                },
    {&EV_Layout_StatbarTileShader,         &UIFakkLabel::LayoutStatbarTileShader      },
    {&EV_Layout_StatbarShader,             &UIFakkLabel::LayoutStatbarShader          },
    {&EV_Layout_StatbarTileShader_Flash,   &UIFakkLabel::LayoutStatbarTileShader_Flash},
    {&EV_Layout_StatbarShader_Marker,      &UIFakkLabel::LayoutStatbarShader_Marker   },
    {&EV_Layout_StatbarShader_Flash,       &UIFakkLabel::LayoutStatbarShader_Flash    },
    {&EV_Layout_Statbar_EndAngles,         &UIFakkLabel::LayoutStatbarEndAngles       },
    {&EV_Layout_Statbar_NeedleWidth,       &UIFakkLabel::LayoutStatbarNeedleWidth     },
    {&EV_Layout_Statbar_RotatorSize,       &UIFakkLabel::LayoutStatbarRotatorSize     },
    {&EV_Layout_DrawModelName,             &UIFakkLabel::LayoutModelName              },
    {&EV_Layout_RenderModel,               &UIFakkLabel::LayoutRenderModel            },
    {&EV_Layout_RenderModelFit,            &UIFakkLabel::LayoutRenderModelFit         },
    {&EV_Layout_RenderModelOffset,         &UIFakkLabel::LayoutRenderModelOffset      },
    {&EV_Layout_RenderModelRotateOffset,   &UIFakkLabel::LayoutRenderModelRotateOffset},
    {&EV_Layout_RenderModelAngles,         &UIFakkLabel::LayoutRenderModelAngles      },
    {&EV_Layout_RenderModelScale,          &UIFakkLabel::LayoutRenderModelScale       },
    {&EV_Layout_RenderModelAnim,           &UIFakkLabel::LayoutRenderModelAnim        },
    {&EV_Layout_RenderModelXformCvar,      &UIFakkLabel::LayoutRenderModelXformCvar   },
    {&EV_Layout_RenderModelAttachCvar,     &UIFakkLabel::LayoutRenderModelAttachCvar  },
    {&EV_Layout_RenderModelAttachTag,      &UIFakkLabel::LayoutRenderModelAttachTag   },
    {&EV_Layout_RenderModelAttachCvar2,    &UIFakkLabel::LayoutRenderModelAttachCvar2 },
    {&EV_Layout_RenderModelAttachTag2,     &UIFakkLabel::LayoutRenderModelAttachTag2  },
    {&EV_Layout_RenderModelAnimCvar,       &UIFakkLabel::LayoutRenderModelAnimCvar    },
    {&EV_Layout_RenderModelSpinCvar,       &UIFakkLabel::LayoutRenderModelSpinCvar    },
    {&W_LeftMouseDown,                     &UIFakkLabel::OnSpinPressed                },
    {&W_LeftMouseDragged,                  &UIFakkLabel::OnSpinDragged                },
    {&W_LeftMouseUp,                       &UIFakkLabel::OnSpinReleased               },
    {&EV_ClearInvItemReference,            &UIFakkLabel::ClearInvItemReference        },
    {NULL,                                 NULL                                       }
};

UIFakkLabel::UIFakkLabel()
{
    m_statbar_min = m_statbar_max = -1.0;
    m_stat                        = -1;
    m_stat_alpha                  = -1;
    m_stat_configstring           = -1;
    m_maxstat                     = -1;
    m_itemindex                   = -1;
    m_inventoryrendermodelindex   = -1;
    m_rendermodel                 = false;
    m_rendermodelfit              = false;
    m_statbar_or                  = L_STATBAR_NONE;
    m_statbar_material            = NULL;
    m_statbar_material_flash      = NULL;
    m_statbar_material_marker     = NULL;
    m_lastitemindex               = -1;
    m_spinStartVal                = 0.0f;   // HZM coop: click-drag rotate state
    m_spinStartX                  = 0;
    m_spinning                    = false;
}

void UIFakkLabel::LayoutPlayerStat(Event *ev)
{
    m_stat = ev->GetInteger(1);
}

void UIFakkLabel::LayoutPlayerStatAlpha(Event *ev)
{
    m_stat_alpha = ev->GetInteger(1);
}

void UIFakkLabel::LayoutPlayerStatConfigstring(Event *ev)
{
    m_stat_configstring = ev->GetInteger(1);
}

void UIFakkLabel::LayoutMaxPlayerStat(Event *ev)
{
    m_maxstat = ev->GetInteger(1);
}

void UIFakkLabel::LayoutItemIndex(Event *ev)
{
    m_itemindex = ev->GetInteger(1);
}

void UIFakkLabel::InventoryRenderModelIndex(Event *ev)
{
    m_inventoryrendermodelindex = ev->GetInteger(1);
}

void UIFakkLabel::LayoutModelName(Event *ev)
{
    m_sDrawModelName = ev->GetString(1);
}

void UIFakkLabel::ClearInvItemReference(Event *ev)
{
    m_lastitem      = NULL;
    m_lastitemindex = -1;
}

void UIFakkLabel::LayoutStatbar(Event *ev)
{
    str _or = ev->GetString(1);

    if (ev->NumArgs() > 1) {
        m_statbar_min = ev->GetFloat(2);
        m_statbar_max = ev->GetFloat(3);
    }

    if (!str::icmp(_or, "horizontal")) {
        m_statbar_or = L_STATBAR_HORIZONTAL;
    } else if (!str::icmp(_or, "vertical_stagger_even")) {
        m_statbar_or = L_STATBAR_VERTICAL_STAGGER_EVEN;
    } else if (!str::icmp(_or, "vertical_stagger_odd")) {
        m_statbar_or = L_STATBAR_VERTICAL_STAGGER_ODD;
    } else if (!str::icmp(_or, "vertical")) {
        m_statbar_or = L_STATBAR_VERTICAL;
    } else if (!str::icmp(_or, "vertical_reverse")) {
        m_statbar_or = L_STATBAR_VERTICAL_REVERSE;
    } else if (!str::icmp(_or, "circular")) {
        m_statbar_or = L_STATBAR_CIRCULAR;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else if (!str::icmp(_or, "needle")) {
        m_statbar_or = L_STATBAR_NEEDLE;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else if (!str::icmp(_or, "rotator")) {
        m_statbar_or = L_STATBAR_ROTATOR;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else if (!str::icmp(_or, "compass")) {
        m_statbar_or = L_STATBAR_COMPASS;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else if (!str::icmp(_or, "spinner")) {
        m_statbar_or = L_STATBAR_SPINNER;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else if (!str::icmp(_or, "headingspinner")) {
        m_statbar_or = L_STATBAR_HEADING_SPINNER;

        if (!m_statbar_material) {
            m_statbar_material = uWinMan.RegisterShader("blank");
        }
    } else {
        warning("LayoutPlayerStat", "Invalid statbar orientation: \"%s\"", _or.c_str());
    }
}

void UIFakkLabel::LayoutStatbarShader(Event *ev)
{
    m_statbar_material = uWinMan.RegisterShader(ev->GetString(1));
}

void UIFakkLabel::LayoutStatbarTileShader(Event *ev)
{
    m_statbar_material = uWinMan.RegisterShader(ev->GetString(1));
    AddFlag(WF_TILESHADER);
}

void UIFakkLabel::LayoutStatbarShader_Flash(Event *ev)
{
    m_statbar_material_flash = uWinMan.RegisterShader(ev->GetString(1));
}

void UIFakkLabel::LayoutStatbarShader_Marker(Event *ev)
{
    m_statbar_material_marker = uWinMan.RegisterShader(ev->GetString(1));
}

void UIFakkLabel::LayoutStatbarEndAngles(Event *ev)
{
    m_angles[0] = ev->GetFloat(1);
    m_angles[1] = ev->GetFloat(2);
}

void UIFakkLabel::LayoutStatbarNeedleWidth(Event *ev)
{
    m_angles[2] = ev->GetFloat(1);
}

void UIFakkLabel::LayoutStatbarRotatorSize(Event *ev)
{
    m_angles[2] = ev->GetFloat(1);

    if (ev->NumArgs() > 1) {
        m_scale = ev->GetFloat(2);
    } else {
        m_scale = m_angles[2];
    }
}

void UIFakkLabel::LayoutStatbarTileShader_Flash(Event *ev)
{
    m_statbar_material_flash = uWinMan.RegisterShader(ev->GetString(1));
    AddFlag(WF_TILESHADER);
}

void UIFakkLabel::LayoutRenderModel(Event *ev)
{
    m_rendermodel = ev->GetBoolean(1);
}

void UIFakkLabel::LayoutRenderModelFit(Event *ev)
{
    m_rendermodelfit = ev->GetBoolean(1);
}

void UIFakkLabel::LayoutRenderModelOffset(Event *ev)
{
    Vector vec = ev->GetVector(1);
    VectorCopy(vec, m_offset);
}

void UIFakkLabel::LayoutRenderModelRotateOffset(Event *ev)
{
    Vector vec = ev->GetVector(1);
    VectorCopy(vec, m_rotateoffset);
}

void UIFakkLabel::LayoutRenderModelAngles(Event *ev)
{
    Vector vec = ev->GetVector(1);
    VectorCopy(vec, m_angles);
}

void UIFakkLabel::LayoutRenderModelScale(Event *ev)
{
    m_scale = ev->GetFloat(1);
}

void UIFakkLabel::LayoutRenderModelAnim(Event *ev)
{
    m_anim = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelXformCvar(Event *ev)
{
    m_xformcvar = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelAttachCvar(Event *ev)
{
    m_attachcvar = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelAttachTag(Event *ev)
{
    m_attachtag = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelAttachCvar2(Event *ev)
{
    m_attachcvar2 = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelAttachTag2(Event *ev)
{
    m_attachtag2 = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelAnimCvar(Event *ev)
{
    m_animcvar = ev->GetString(1);
}

void UIFakkLabel::LayoutRenderModelSpinCvar(Event *ev)
{
    m_spincvar = ev->GetString(1);
    // become a mouse responder so left-drag events reach this label (labels are non-activatable by default)
    AllowActivate(true);
}

void UIFakkLabel::OnSpinPressed(Event *ev)
{
    if (!m_spincvar.length()) {
        return;
    }
    m_spinning     = true;
    m_spinStartX   = uid.mouseX;
    m_spinStartVal = atof(Cvar_VariableString(m_spincvar.c_str()));
    uWinMan.setFirstResponder(this); // capture so the drag keeps coming even if the cursor leaves the panel
}

void UIFakkLabel::OnSpinDragged(Event *ev)
{
    if (!m_spincvar.length() || !m_spinning) {
        return;
    }
    float sens = Cvar_Get("coop_loSpinSens", "0.6", CVAR_ARCHIVE)->value; // user-tunable, persists
    if (sens <= 0.0f) {
        sens = 0.6f;
    }
    float yaw = m_spinStartVal + (float)(uid.mouseX - m_spinStartX) * sens;
    Cvar_Set(m_spincvar.c_str(), va("%f", yaw));
}

void UIFakkLabel::OnSpinReleased(Event *ev)
{
    if (!m_spincvar.length()) {
        return;
    }
    m_spinning = false;
    if (uWinMan.getFirstResponder() == this) {
        uWinMan.setFirstResponder(NULL);
    }
}

void UIFakkLabel::DrawStatbar(float frac)
{
    vec4_t    col;
    float     alpha;
    qhandle_t hMat;
    float     w, h;
    float     fvWidth, fvHeight;

    col[0] = col[1] = col[2] = col[3] = 1.0;

    switch (m_statbar_or) {
    case L_STATBAR_CIRCULAR:
        DrawStatCircle(frac);
        return;
    case L_STATBAR_NEEDLE:
        DrawStatNeedle(frac);
        return;
    case L_STATBAR_ROTATOR:
        DrawStatRotator(frac);
        return;
    case L_STATBAR_COMPASS:
        DrawStatCompass(frac);
        return;
    case L_STATBAR_SPINNER:
    case L_STATBAR_HEADING_SPINNER:
        DrawStatSpinner(frac);
        return;
    default:
        break;
    }

    if (!frac) {
        return;
    }

    if (frac != m_lastfrac) {
        m_flashtime = uid.time;
    }
    m_lastfrac = frac;

    alpha = 1.0 - ((float)uid.time - m_flashtime) / 1500.0;
    alpha = Q_clamp_float(alpha, 0, 1);

    // HZM coop - HUD fade: the main bar used to rely on the render color left set by
    // UIWidget::Display (which happened to carry m_local_alpha); set it explicitly, and
    // scale the change-flash overlay by the same widget alpha so it can't pop full-bright
    // through a faded panel.
    {
        vec4_t colBase;
        colBase[0] = colBase[1] = colBase[2] = 1.0;
        colBase[3] = m_local_alpha;
        re.SetColor(colBase);
    }
    col[3] = alpha * m_local_alpha;

    if (m_statbar_material) {
        if (m_flags & WF_TILESHADER) {
            switch (m_statbar_or) {
            case L_STATBAR_HORIZONTAL:
                {
                    float width = frac * m_frame.size.width;

                    fvWidth = m_frame.size.width / m_vVirtualScale[0]
                            / uii.Rend_GetShaderWidth(m_statbar_material->GetMaterial());
                    fvHeight = m_frame.size.height / m_vVirtualScale[1]
                             / uii.Rend_GetShaderHeight(m_statbar_material->GetMaterial());

                    re.DrawStretchPic(
                        0.0, 0.0, width, m_frame.size.height, 0, 0, fvWidth, fvHeight, m_statbar_material->GetMaterial()
                    );
                    //re.DrawTilePic(0.0, 0.0, width, m_frame.size.height, m_statbar_material->GetMaterial());

                    if (alpha != 0.0 && m_statbar_material_flash) {
                        re.SetColor(col);

                        fvWidth = m_frame.size.width / m_vVirtualScale[0]
                                / uii.Rend_GetShaderWidth(m_statbar_material_flash->GetMaterial());
                        fvHeight = m_frame.size.height / m_vVirtualScale[1]
                                 / uii.Rend_GetShaderHeight(m_statbar_material_flash->GetMaterial());

                        re.DrawStretchPic(
                            0.0,
                            0.0,
                            width,
                            m_frame.size.height,
                            0,
                            0,
                            fvWidth,
                            fvHeight,
                            m_statbar_material_flash->GetMaterial()
                        );
                        //re.DrawTilePic(0.0, 0.0, width, m_frame.size.height, m_statbar_material_flash->GetMaterial());
                    }

                    if (m_statbar_material_marker) {
                        hMat = m_statbar_material_marker->GetMaterial();
                        w    = re.GetShaderWidth(hMat);
                        h    = re.GetShaderHeight(hMat);

                        re.DrawStretchPic(
                            m_frame.size.width * frac - w * 0.5,
                            m_frame.size.height * 0.5 - h * 0.5,
                            w,
                            h,
                            0,
                            0,
                            1,
                            1,
                            hMat
                        );
                    }
                    break;
                }
            case L_STATBAR_VERTICAL:
            case L_STATBAR_VERTICAL_REVERSE:
            case L_STATBAR_VERTICAL_STAGGER_EVEN:
            case L_STATBAR_VERTICAL_STAGGER_ODD:
                {
                    float y = m_frame.size.height * (1.0 - frac);

                    fvWidth = m_frame.size.width / m_vVirtualScale[0]
                            / uii.Rend_GetShaderWidth(m_statbar_material->GetMaterial());
                    fvHeight = m_frame.size.height / m_vVirtualScale[1]
                             / uii.Rend_GetShaderHeight(m_statbar_material->GetMaterial());

                    re.DrawStretchPic(
                        0.0,
                        y,
                        m_frame.size.width,
                        m_frame.size.height,
                        0,
                        0,
                        fvWidth,
                        fvHeight,
                        m_statbar_material->GetMaterial()
                    );
                    //re.DrawTilePic(0.0, y, m_frame.size.width, m_frame.size.height, m_statbar_material->GetMaterial());

                    if (alpha != 0.0 && m_statbar_material_flash) {
                        re.SetColor(col);

                        fvWidth = m_frame.size.width / m_vVirtualScale[0]
                                / uii.Rend_GetShaderWidth(m_statbar_material_flash->GetMaterial());
                        fvHeight = m_frame.size.height / m_vVirtualScale[1]
                                 / uii.Rend_GetShaderHeight(m_statbar_material_flash->GetMaterial());

                        re.DrawStretchPic(
                            0.0,
                            y,
                            m_frame.size.width,
                            m_frame.size.height,
                            0,
                            0,
                            fvWidth,
                            fvHeight,
                            m_statbar_material_flash->GetMaterial()
                        );
                        //re.DrawTilePic(
                        //    0.0, y, m_frame.size.width, m_frame.size.height, m_statbar_material_flash->GetMaterial()
                        //);
                    }

                    if (m_statbar_material_marker) {
                        hMat = m_statbar_material_marker->GetMaterial();
                        w    = re.GetShaderWidth(hMat);
                        h    = re.GetShaderHeight(hMat);

                        re.DrawStretchPic(
                            m_frame.size.width * frac - w * 0.5,
                            m_frame.size.height * 0.5 - h * 0.5,
                            w,
                            h,
                            0,
                            0,
                            1,
                            1,
                            hMat
                        );
                    }
                    break;
                }
            default:
                break;
            }
        } else {
            switch (m_statbar_or) {
            case L_STATBAR_HORIZONTAL:
                {
                    float width = frac * m_frame.size.width;

                    re.DrawStretchPic(
                        0.0, 0.0, width, m_frame.size.height, 0.0, 0.0, 1.0, 1.0, m_statbar_material->GetMaterial()
                    );

                    if (alpha != 0.0 && m_statbar_material_flash) {
                        re.SetColor(col);
                        re.DrawStretchPic(
                            0.0,
                            0.0,
                            width,
                            m_frame.size.height,
                            0.0,
                            0.0,
                            1.0,
                            1.0,
                            m_statbar_material_flash->GetMaterial()
                        );
                    }

                    if (m_statbar_material_marker) {
                        hMat = m_statbar_material_marker->GetMaterial();
                        w    = re.GetShaderWidth(hMat);
                        h    = re.GetShaderHeight(hMat);

                        re.DrawStretchPic(
                            m_frame.size.width * frac - w * 0.5,
                            m_frame.size.height * 0.5 - h * 0.5,
                            w,
                            h,
                            0,
                            0,
                            1,
                            1,
                            hMat
                        );
                    }
                    break;
                }
            case L_STATBAR_VERTICAL:
            case L_STATBAR_VERTICAL_STAGGER_EVEN:
            case L_STATBAR_VERTICAL_STAGGER_ODD:
                {
                    float y      = m_frame.size.height * (1.0 - frac);
                    float height = m_frame.size.height * frac;

                    re.DrawStretchPic(
                        0.0, y, m_frame.size.width, height, 0.0, 1.0 - frac, 1.0, 1.0, m_statbar_material->GetMaterial()
                    );

                    if (alpha != 0.0 && m_statbar_material_flash) {
                        re.SetColor(col);
                        re.DrawStretchPic(
                            0.0,
                            y,
                            m_frame.size.width,
                            height,
                            0.0,
                            1.0 - frac,
                            1.0,
                            1.0,
                            m_statbar_material_flash->GetMaterial()
                        );
                    }

                    if (m_statbar_material_marker) {
                        hMat = m_statbar_material_marker->GetMaterial();
                        w    = re.GetShaderWidth(hMat);
                        h    = re.GetShaderHeight(hMat);

                        re.DrawStretchPic(
                            m_frame.size.width * frac - w * 0.5,
                            m_frame.size.height * 0.5 - h * 0.5,
                            w,
                            h,
                            0,
                            0,
                            1,
                            1,
                            hMat
                        );
                    }
                    break;
                }
            case L_STATBAR_VERTICAL_REVERSE:
                {
                    float height = m_frame.size.height * frac;

                    re.DrawStretchPic(
                        0.0, 0.0, m_frame.size.width, height, 0.0, 0.0, 1.0, frac, m_statbar_material->GetMaterial()
                    );

                    if (alpha != 0.0 && m_statbar_material_flash) {
                        re.SetColor(col);
                        re.DrawStretchPic(
                            0.0,
                            0.0,
                            m_frame.size.width,
                            height,
                            0.0,
                            0.0,
                            1.0,
                            frac,
                            m_statbar_material_flash->GetMaterial()
                        );
                    }
                    break;
                }
            default:
                break;
            }
        }
    } else {
        switch (m_statbar_or) {
        case L_STATBAR_VERTICAL:
        case L_STATBAR_VERTICAL_STAGGER_EVEN:
        case L_STATBAR_VERTICAL_STAGGER_ODD:
            DrawBox(
                0.0,
                m_frame.size.height * (1.0 - frac),
                m_frame.size.width,
                m_frame.size.height,
                m_foreground_color,
                1.0
            );
            break;
        case L_STATBAR_HORIZONTAL:
            DrawBox(0.0, 0.0, frac * m_frame.size.width, m_frame.size.height, m_foreground_color, 1.0);
            break;
        default:
            break;
        }
    }
}

void UIFakkLabel::DrawStatCircle(float frac)
{
    vec4_t col;
    float  alpha;
    float  fCurrAng, fEndAng, fNextAng;
    vec2_t vVerts[3];
    vec2_t vTexCoords[3];

    col[0] = 1.f;
    col[1] = 1.f;
    col[2] = 1.f;
    col[3] = 1.f;

    if (m_lastfrac != frac) {
        m_flashtime = uid.time;
    }
    m_lastfrac = frac;

    alpha = 1.f - (uid.time - m_flashtime) / 1500.f;
    if (alpha < 0.f) {
        alpha = 0.f;
    } else if (alpha > 1.f) {
        alpha = 1.f;
    }

    col[3] = alpha;

    vVerts[0][0]     = m_frame.size.width * 0.5f;
    vVerts[0][1]     = m_frame.size.height * 0.5f;
    vTexCoords[0][0] = 0.5f;
    vTexCoords[0][1] = 0.5f;

    if (ui_health_start->value && ui_health_end->value) {
        m_angles[0] = ui_health_start->value;
        m_angles[1] = ui_health_end->value;
    }

    fCurrAng = m_angles[0];
    fEndAng  = frac * (m_angles[1] - fCurrAng) + fCurrAng;
    if (m_angles[1] > fCurrAng) {
        StatCircleTexCoord(fCurrAng, vTexCoords[1]);
        vVerts[1][0] = m_frame.size.width * vTexCoords[1][0];
        vVerts[1][1] = m_frame.size.height * vTexCoords[1][1];

        if ((int)fCurrAng - (((int)fCurrAng - 45) % 90 + 90) % 90
            == (int)fEndAng - (((int)fEndAng - 45) % 90 + 90) % 90) {
            StatCircleTexCoord(fEndAng, vTexCoords[2]);
            vVerts[2][0] = m_frame.size.width * vTexCoords[2][0];
            vVerts[2][1] = m_frame.size.height * vTexCoords[2][1];

            if (alpha && m_statbar_material_flash) {
                re.SetColor(m_foreground_color);
            }

            re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

            if (alpha && m_statbar_material_flash) {
                re.SetColor(col);
                re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
            }
        } else {
            while (fCurrAng > fEndAng) {
                fNextAng = ((int)fCurrAng - (((int)fCurrAng - 45) % 90 + 90) % 90) + 90.0;
                if (fNextAng > fEndAng) {
                    fNextAng = fEndAng;
                }

                StatCircleTexCoord(fNextAng, vTexCoords[2]);
                vVerts[2][0] = m_frame.size.width * vTexCoords[2][0];
                vVerts[2][1] = m_frame.size.height * vTexCoords[2][1];

                if (alpha && m_statbar_material_flash) {
                    re.SetColor(m_foreground_color);
                }

                re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

                if (alpha && m_statbar_material_flash) {
                    re.SetColor(col);
                    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
                }

                vTexCoords[1][0] = vTexCoords[2][0];
                vTexCoords[1][1] = vTexCoords[2][1];
                vVerts[1][0]     = vVerts[2][0];
                vVerts[1][1]     = vVerts[2][1];
                fCurrAng         = fNextAng;
            }
        }
    } else {
        StatCircleTexCoord(fCurrAng, vTexCoords[2]);
        vVerts[2][0] = m_frame.size.width * vTexCoords[2][0];
        vVerts[2][1] = m_frame.size.height * vTexCoords[2][1];
        if ((int)fCurrAng - (((int)fCurrAng - 45) % 90 + 90) % 90
            == (int)fEndAng - (((int)fEndAng - 45) % 90 + 90) % 90) {
            StatCircleTexCoord(fEndAng, vTexCoords[1]);
            vVerts[1][0] = m_frame.size.width * vTexCoords[1][0];
            vVerts[1][1] = m_frame.size.height * vTexCoords[1][1];

            if (alpha && m_statbar_material_flash) {
                re.SetColor(m_foreground_color);
            }

            re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

            if (alpha && m_statbar_material_flash) {
                re.SetColor(col);
                re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
            }
        } else {
            while (fCurrAng > fEndAng) {
                fNextAng = ((int)fCurrAng - (((int)fCurrAng + 45) % 90 + 90) % 90);
                if (fNextAng == fCurrAng) {
                    fNextAng -= 90.f;
                }

                if (fNextAng < fEndAng) {
                    fNextAng = fEndAng;
                }

                StatCircleTexCoord(fNextAng, vTexCoords[1]);
                vVerts[1][0] = m_frame.size.width * vTexCoords[1][0];
                vVerts[1][1] = m_frame.size.height * vTexCoords[1][1];

                if (alpha && m_statbar_material_flash) {
                    re.SetColor(m_foreground_color);
                }

                re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

                if (alpha && m_statbar_material_flash) {
                    re.SetColor(col);
                    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
                }

                vTexCoords[2][0] = vTexCoords[1][0];
                vTexCoords[2][1] = vTexCoords[1][1];
                vVerts[2][0]     = vVerts[1][0];
                vVerts[2][1]     = vVerts[1][1];
                fCurrAng         = fNextAng;
            }
        }
    }
}

void UIFakkLabel::DrawStatNeedle(float frac)
{
    vec4_t col;
    float  alpha;
    float  fTargAng;
    float  fSinVal;
    float  fCosVal;
    vec2_t vCenter;
    vec2_t vVerts[3];
    vec2_t vTexCoords[3];
    vec2_t vNeedleDir;
    vec2_t vSideDir;

    col[0] = 1.f;
    col[1] = 1.f;
    col[2] = 1.f;
    col[3] = 1.f;

    if (m_lastfrac != frac) {
        m_flashtime = uid.time;
    }
    m_lastfrac = frac;

    alpha = 1.f - (uid.time - m_flashtime) / 1500.f;
    if (alpha < 0.f) {
        alpha = 0.f;
    } else if (alpha > 1.f) {
        alpha = 1.f;
    }

    col[3] = alpha;

    fTargAng = DEG2RAD(frac * (m_angles[1] - m_angles[0]) + m_angles[0]);
    fSinVal  = sin(fTargAng);
    fCosVal  = cos(fTargAng);

    vSideDir[0]   = m_frame.size.width * 0.5f;
    vSideDir[1]   = m_frame.size.height * 0.5f;
    vCenter[0]    = m_frame.size.width + fSinVal * vSideDir[0];
    vCenter[1]    = m_frame.size.height + -fCosVal * vSideDir[1];
    vNeedleDir[0] = m_angles[2] * fCosVal;
    vNeedleDir[1] = m_angles[2] * fSinVal;

    vVerts[0][0] = vCenter[0] - vNeedleDir[0];
    vVerts[0][1] = vCenter[1] - vNeedleDir[1];
    vVerts[1][0] = vCenter[0] + vNeedleDir[0];
    vVerts[1][1] = vCenter[1] + vNeedleDir[1];
    vVerts[2][0] = vSideDir[0] - vNeedleDir[1] - vNeedleDir[0];
    vVerts[2][1] = vSideDir[1] + vNeedleDir[0] - vNeedleDir[1];

    vTexCoords[0][0] = 0.f;
    vTexCoords[0][1] = 0.f;
    vTexCoords[1][0] = 1.f;
    vTexCoords[1][1] = 0.f;
    vTexCoords[2][0] = 0.f;
    vTexCoords[2][1] = 1.f;

    if (alpha != 0.f) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(col);
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }

    vVerts[0][0] = vVerts[2][0];
    vVerts[0][1] = vVerts[2][1];
    vVerts[2][0] = m_angles[2] * vCenter[0] + m_frame.size.width * 0.5f - m_angles[2] * vCenter[1];
    vVerts[2][1] = m_angles[2] * vCenter[1] + m_frame.size.height * 0.5f - m_angles[2] * -vCenter[0];

    vTexCoords[0][0] = vTexCoords[2][0];
    vTexCoords[0][1] = vTexCoords[2][1];

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }
}

void UIFakkLabel::DrawStatRotator(float frac)
{
    vec4_t col;
    float  alpha;
    float  fTargAng;
    float  fSinVal, fCosVal;
    vec2_t vCenter;
    vec2_t vVerts[3];
    vec2_t vTexCoords[3];
    vec2_t vNeedleDir;
    vec2_t vSize;

    col[0] = 1.f;
    col[1] = 1.f;
    col[2] = 1.f;
    col[3] = 1.f;

    if (m_lastfrac != frac) {
        m_flashtime = uid.time;
    }
    m_lastfrac = frac;

    alpha = 1.f - (uid.time - m_flashtime) / 1500.f;
    if (alpha < 0.f) {
        alpha = 0.f;
    } else if (alpha > 1.f) {
        alpha = 1.f;
    }

    col[3] = alpha;

    fTargAng      = DEG2RAD(frac * (m_angles[1] - m_angles[0]) + m_angles[0]);
    fSinVal       = sin(fTargAng);
    fCosVal       = cos(fTargAng);
    vNeedleDir[0] = fSinVal;
    vNeedleDir[1] = -fCosVal;

    vSize[0] = (m_frame.size.width + m_frame.size.height) / m_frame.size.width * m_angles[2] * getVirtualScale()[0];
    vSize[1] = (m_frame.size.width + m_frame.size.height) / m_frame.size.height * m_scale * getVirtualScale()[1];

    vCenter[0] = (m_frame.size.width * 0.5f - vSize[0]) * vNeedleDir[0] + m_frame.size.width * 0.5f;
    vCenter[1] = (m_frame.size.height * 0.5f - vSize[1]) * vNeedleDir[1] + m_frame.size.height * 0.5f;

    vVerts[0][0] = vCenter[0] - vSize[0];
    vVerts[0][1] = vCenter[1] - vSize[1];
    vVerts[1][0] = vCenter[0] + vSize[0];
    vVerts[1][1] = vCenter[1] - vSize[1];
    vVerts[2][0] = vCenter[0] - vSize[0];
    vVerts[2][1] = vCenter[1] + vSize[1];

    vTexCoords[0][0] = 0.f;
    vTexCoords[0][1] = 0.f;
    vTexCoords[1][0] = 1.f;
    vTexCoords[1][1] = 0.f;
    vTexCoords[2][0] = 0.f;
    vTexCoords[2][1] = 1.f;

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }

    vVerts[0][0] = vVerts[2][0];
    vVerts[0][1] = vVerts[2][1];
    vVerts[2][0] = vCenter[0] + vSize[0];
    vVerts[2][1] = vCenter[1] + vSize[1];

    vTexCoords[0][0] = vTexCoords[2][0];
    vTexCoords[0][1] = vTexCoords[2][1];
    vTexCoords[2][0] = 1.f;
    vTexCoords[2][1] = 1.f;

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }
}

void UIFakkLabel::DrawStatCompass(float frac)
{
    vec4_t col;
    float  fTargAng;
    float  fSinVal, fCosVal;
    vec2_t vCenter;
    vec2_t vVerts[3];
    vec2_t vTexCoords[3];
    vec2_t vCompassDir;
    vec2_t vSideDir;

    col[0] = col[1] = col[2] = col[3] = 1.0;

    if (!cge) {
        return;
    }

    static int   iLastCompassTime = -9999, iLastTimeDelta = 0;
    static float fLastPitch = 0, fLastYaw = 0, fLastYawDelta = 0;
    static float fYawOffset = 0, fYawSpeed = 0;
    static float fNeedleOffset = 0, fNeedleSpeed = 0;
    int          iTimeCount, iTimeDelta;
    float        fYawDelta, fYawDeltaDiff;
    vec3_t       vViewAngles;

    cge->CG_EyeAngles(&vViewAngles);

    vViewAngles[1] -= cl.snap.ps.stats[STAT_COMPASSNORTH] / 182.0f;

    iTimeCount = uid.time - iLastCompassTime;
    if (iTimeCount <= 1000) {
        fYawDelta = AngleSubtract(vViewAngles[1], fLastYaw);
        if (fYawDelta > 0.1) {
            if ((fYawDelta > 0.0 && fLastYawDelta < fYawDelta) || (fYawDelta < 0.0 && fLastYawDelta > fYawDelta)) {
                fYawSpeed += (fYawDelta - fLastYawDelta) * 0.75;
                fYawOffset += (fYawDelta - fLastYawDelta) * 0.75;
            }

            if (fYawSpeed > 90.0) {
                fYawSpeed = 90.0;
            } else if (fYawSpeed < 0.05) {
                fYawSpeed = 0.05;
            }
        }

        while (iTimeCount > 0) {
            iTimeDelta = iTimeCount;
            if (iTimeCount > 15) {
                iTimeDelta = 15;
            }
            iTimeCount -= iTimeDelta;

            if (fabs(fYawOffset) >= 0.1 || fabs(fYawSpeed) >= 0.01) {
                fYawOffset += fYawSpeed * iTimeDelta;
                if (fYawOffset > 0.0) {
                    fYawSpeed -= iTimeDelta * 0.00175;
                } else if (fYawOffset < 0.0) {
                    fYawSpeed += iTimeDelta * 0.00175;
                }

                if (fYawOffset > 40.0) {
                    fYawOffset = 40.0;
                    fYawSpeed  = 0.0;
                } else if (fYawOffset < -40.0) {
                    fYawOffset = -40.0;
                    fYawSpeed  = 0.0;
                }

                fYawSpeed -= iTimeDelta * 0.003;
                if (fYawSpeed > 0.0) {
                    fYawSpeed -= iTimeDelta * 0.0004;
                } else {
                    fYawSpeed += iTimeDelta * 0.0004;
                    if (fYawSpeed > 0.0) {
                        fYawSpeed = 0.0;
                    }
                }
            } else {
                iTimeCount = 0;
                fYawOffset = 0.0;
                fYawSpeed  = 0.0;
            }
        }

        fLastYawDelta  = fYawDelta;
        iLastTimeDelta = uid.time - iLastCompassTime;
    } else {
        fLastYawDelta  = 0;
        fYawOffset     = 0;
        fYawSpeed      = 0;
        iLastTimeDelta = 0;
    }

    fLastPitch       = vViewAngles[0];
    fLastYaw         = vViewAngles[1];
    iLastCompassTime = uid.time;

    if (iLastTimeDelta) {
        if (fabs(fLastYawDelta) > 0.1) {
            fNeedleOffset -= fLastYawDelta;
        }

        iTimeCount = iLastTimeDelta;
        while (iTimeCount > 0) {
            iTimeDelta = iTimeCount;
            if (iTimeCount > 15) {
                iTimeDelta = 15;
            }
            iTimeCount -= iTimeDelta;

            if (fabs(fNeedleOffset) >= 0.1 || fabs(fNeedleSpeed) >= 0.01) {
                fNeedleOffset += iTimeDelta * fNeedleSpeed;
                if (fNeedleOffset > 180.0) {
                    fNeedleOffset -= 360.0;
                } else if (fNeedleOffset < -180.0) {
                    fNeedleOffset += 360.0;
                }

                if (fNeedleOffset > 0.0) {
                    fNeedleSpeed -= iTimeDelta * 0.00175;
                } else if (fNeedleOffset < 0.0) {
                    fNeedleSpeed += iTimeDelta * 0.00175;
                }

                fNeedleSpeed -= fNeedleSpeed * 0.0025 * iTimeDelta;
                if (fNeedleSpeed > 0.0) {
                    fNeedleSpeed -= iTimeDelta * 0.00035;
                    if (fNeedleSpeed < 0.0) {
                        fNeedleSpeed = 0.0;
                    }
                } else {
                    fNeedleSpeed += iTimeDelta * 0.00035;
                    if (fNeedleSpeed > 0.0) {
                        fNeedleSpeed = 0.0;
                    }
                }
            } else {
                iTimeCount    = 0;
                fNeedleOffset = 0.0;
                fNeedleSpeed  = 0.0;
            }
        }
    } else {
        fNeedleOffset = 0.0;
        fNeedleSpeed  = 0.0;
    }

    fSinVal = sin(anglemod(fNeedleOffset + fLastYaw) / (180 / M_PI));
    fCosVal = cos(anglemod(fNeedleOffset + fLastYaw) / (180 / M_PI));

    vCompassDir[0] = fSinVal;
    vCompassDir[1] = -fCosVal;

    vCenter[0]       = m_frame.size.width * 0.5;
    vCenter[1]       = m_frame.size.height * 0.5;
    vTexCoords[0][0] = 0.0;
    vTexCoords[0][1] = 0.0;
    vTexCoords[1][0] = 1.0;
    vTexCoords[1][1] = 0.0;
    vTexCoords[2][0] = 0.0;
    vTexCoords[2][1] = 1.0;

    vVerts[0][0] = vCenter[0] + vCompassDir[0] * vCenter[0] - fCosVal * vCenter[1];
    vVerts[1][0] = vCenter[0] + vCompassDir[0] * vCenter[0] + fCosVal * vCenter[1];
    vVerts[2][0] = vCenter[0] - vCompassDir[0] * vCenter[0] - fCosVal * vCenter[1];
    vVerts[0][1] = vCenter[1] + vCompassDir[1] * vCenter[1] - fSinVal * vCenter[0];
    vVerts[1][1] = vCenter[1] + vCompassDir[1] * vCenter[1] + fSinVal * vCenter[0];
    vVerts[2][1] = vCenter[1] - vCompassDir[1] * vCenter[1] - fSinVal * vCenter[0];

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    vVerts[0][0]     = vVerts[2][0];
    vVerts[0][1]     = vVerts[2][1];
    vTexCoords[0][0] = vTexCoords[2][0];
    vTexCoords[0][1] = vTexCoords[2][1];
    vTexCoords[2][0] = 1.0;
    vTexCoords[2][1] = 1.0;

    vVerts[2][0] = vCenter[0] + fCosVal * vCenter[1] - vCompassDir[0] * vCenter[0];
    vVerts[2][1] = vCenter[1] * fSinVal + vCenter[0] - vCompassDir[1] * vCenter[1];

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());
}

void UIFakkLabel::DrawStatSpinner(float frac)
{
    vec4_t col;
    float  alpha;
    float  fTargAng;
    float  fSinVal, fCosVal;
    vec2_t vCenter;
    vec2_t vVerts[3];
    vec2_t vTexCoords[3];
    vec2_t vCompassDir;
    vec2_t vSideDir;

    col[0] = 1.f;
    col[1] = 1.f;
    col[2] = 1.f;
    col[3] = 1.f;

    if (m_lastfrac != frac) {
        m_flashtime = uid.time;
    }
    m_lastfrac = frac;

    alpha = 1.f - (uid.time - m_flashtime) / 1500.f;
    if (alpha < 0.f) {
        alpha = 0.f;
    } else if (alpha > 1.f) {
        alpha = 1.f;
    }

    col[3] = alpha;

    if (m_statbar_or == L_STATBAR_HEADING_SPINNER) {
        if (cge) {
            vec3_t vViewAngles;
            float  fAngAlpha;
            cge->CG_EyeAngles(&vViewAngles);

            frac = (AngleSubtract(vViewAngles[1], frac * 360.f + 180.f) + 180.f) / 360.f;
            if (frac < 0.f || frac > 1.f) {
                frac = 0.f;
            }
        }
    }

    fTargAng = DEG2RAD(frac * (m_angles[1] - m_angles[0]) + m_angles[0]);
    fSinVal  = sin(fTargAng);
    fCosVal  = cos(fTargAng);

    vSideDir[0]    = fCosVal;
    vSideDir[1]    = fSinVal;
    vCompassDir[0] = fSinVal;
    vCompassDir[1] = -fCosVal;
    vCenter[0]     = m_frame.size.width * 0.5f;
    vCenter[1]     = m_frame.size.height * 0.5f;

    vVerts[0][0] = (fSinVal * vCenter[0] + vCenter[0]) - (fCosVal * vCenter[1]);
    vVerts[0][1] = (vCompassDir[1] * vCenter[1] + vCenter[1]) - (vSideDir[1] * vCenter[0]);
    vVerts[1][0] = (fSinVal * vCenter[0] + vCenter[0]) + (fCosVal * vCenter[1]);
    vVerts[1][1] = (vSideDir[1] * vCenter[0]) + (vCompassDir[1] * vCenter[1] + vCenter[1]);
    vVerts[2][0] = vCenter[0] - fSinVal * vCenter[0] - fCosVal * vCenter[1];
    vVerts[2][1] = vCenter[1] - vCompassDir[1] * vCenter[1] - vSideDir[1] * vCenter[0];

    vTexCoords[0][0] = 0.f;
    vTexCoords[0][1] = 0.f;
    vTexCoords[1][0] = 1.f;
    vTexCoords[1][1] = 0.f;
    vTexCoords[2][0] = 0.f;
    vTexCoords[2][1] = 1.f;

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }

    vVerts[0][0] = vVerts[2][0];
    vVerts[0][1] = vVerts[2][1];
    vVerts[2][0] = vCenter[1] * fCosVal + vCenter[0] - vCompassDir[0] * vCenter[0];
    vVerts[2][1] = vCenter[0] * vSideDir[1] + vCenter[1] - vCompassDir[1] * vCenter[1];

    vTexCoords[0][0] = vTexCoords[2][0];
    vTexCoords[0][1] = vTexCoords[2][1];
    vTexCoords[2][0] = 1.f;
    vTexCoords[2][1] = 1.f;

    if (alpha != 0.f && m_statbar_material_flash) {
        re.SetColor(m_foreground_color);
    }

    re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material->GetMaterial());

    if (alpha != 0.f && m_statbar_material_flash) {
        re.DrawTrianglePic(vVerts, vTexCoords, m_statbar_material_flash->GetMaterial());
    }
}

void UIFakkLabel::StatCircleTexCoord(float fAng, vec3_t vTexCoord)
{
    int   iSector;
    float fSinVal, fCosVal;

    iSector = (AngleMod(fAng) + 45.f) / 90;
    fSinVal = sin(DEG2RAD(fAng));
    fCosVal = cos(DEG2RAD(fAng));

    if (iSector >= 0) {
        if (iSector == 0) {
            vTexCoord[0] = 0.f;
            vTexCoord[1] = 0.5f / fCosVal * fSinVal + 0.5f;
        } else if (iSector == 1) {
            vTexCoord[0] = 1.f;
            vTexCoord[1] = -0.5f / fSinVal * fCosVal + 0.5f;
        } else if (iSector == 2) {
            vTexCoord[0] = fSinVal * (-0.5f * fCosVal) + 0.5f;
            vTexCoord[1] = 1.f;
        } else if (iSector == 4) {
            vTexCoord[0] = 0.5f / fCosVal * fSinVal + 0.5f;
            vTexCoord[1] = 0.f;
        } else {
            vTexCoord[0] = 0.f;
            vTexCoord[1] = 0.5f / fSinVal * fCosVal + 0.5f;
        }
    } else {
        vTexCoord[0] = 0.f;
        vTexCoord[1] = 0.5f / fSinVal * fCosVal + 0.5f;
    }
}

void UIFakkLabel::Draw(void)
{
    if (m_stat_alpha != -1) {
        float frac;

        frac = (cl.snap.ps.stats[m_stat_alpha] + 1) / 100.0;
        frac = Q_clamp_float(frac, 0, 1);

        m_alpha = frac;
    }

    if (m_stat == -1 && m_itemindex == -1 && m_inventoryrendermodelindex == -1 && m_stat_configstring == -1
        && !m_rendermodel && m_statbar_or == L_STATBAR_NONE) {
        if (!m_sDrawModelName.length()) {
            UILabel::Draw();
            return;
        }
    }

    if (m_stat_configstring != -1) {
        m_font->setColor(UColor(
            m_foreground_color.r, m_foreground_color.g, m_foreground_color.b,
            m_foreground_color.a * m_local_alpha // HZM coop - HUD fade: stat text follows widget alpha
        ));

        if (m_bOutlinedText) {
            m_font->PrintOutlinedJustified(
                getClientFrame(),
                m_iFontAlignmentHorizontal,
                m_iFontAlignmentVertical,
                Sys_LV_CL_ConvertString(va("%s", CL_ConfigString(cl.snap.ps.stats[m_stat_configstring]))),
                UBlack,
                getVirtualScale()
            );
        } else {
            m_font->PrintJustified(
                getClientFrame(),
                m_iFontAlignmentHorizontal,
                m_iFontAlignmentVertical,
                Sys_LV_CL_ConvertString(va("%s", CL_ConfigString(cl.snap.ps.stats[m_stat_configstring]))),
                getVirtualScale()
            );
        }
        return;
    }

    if (m_stat != -1) {
        int delta = cl.snap.ps.stats[m_stat];

        if (m_statbar_or == L_STATBAR_NONE) {
            m_font->setColor(UColor(
            m_foreground_color.r, m_foreground_color.g, m_foreground_color.b,
            m_foreground_color.a * m_local_alpha // HZM coop - HUD fade: stat text follows widget alpha
        ));

            if (m_bOutlinedText) {
                m_font->PrintOutlinedJustified(
                    getClientFrame(),
                    m_iFontAlignmentHorizontal,
                    m_iFontAlignmentVertical,
                    va("%d", delta),
                    UBlack,
                    getVirtualScale()
                );
            } else {
                m_font->PrintJustified(
                    getClientFrame(),
                    m_iFontAlignmentHorizontal,
                    m_iFontAlignmentVertical,
                    va("%d", delta),
                    getVirtualScale()
                );
            }
            return;
        }

        if (m_statbar_or == L_STATBAR_VERTICAL_STAGGER_EVEN || m_statbar_or == L_STATBAR_VERTICAL_STAGGER_ODD) {
            int stat;

            if (m_maxstat < 0) {
                stat = m_statbar_max;
            } else {
                stat = cl.snap.ps.stats[m_maxstat];
            }

            delta = stat - cl.snap.ps.stats[m_stat];

            if (delta & 1) {
                if (m_statbar_or == L_STATBAR_VERTICAL_STAGGER_EVEN) {
                    delta--;
                } else {
                    delta++;
                }
            }

            delta = stat - delta;

            if (m_maxstat >= 0 || m_statbar_min <= delta) {
                if (delta < 0) {
                    delta = 0;
                }
            } else {
                delta = m_statbar_min;
            }
        }

        float frac;

        if (m_maxstat >= 0) {
            frac = (float)delta / (float)cl.snap.ps.stats[m_maxstat];
        } else {
            frac = (float)delta / (float)(m_statbar_max - m_statbar_min);
        }

        if (frac > 1.0) {
            frac = 1.0;
        } else if (frac < 0.0) {
            frac = 0.0;
        }

        DrawStatbar(frac);
        return;
    }

    float     scale;
    qhandle_t handle;
    vec3_t    origin;
    vec3_t    mins;
    vec3_t    maxs;
    vec3_t    angles;
    vec3_t    offset;
    vec3_t    rotateoffset;
    vec4_t    color;
    str       sAnimName;
    float     height;

    if (m_itemindex != -1) {
        if (m_itemindex < 0) {
            return;
        }

        if (cl.snap.ps.activeItems[m_itemindex] < 0 || cl.snap.ps.activeItems[m_itemindex] >= MAX_WEAPONS) {
            // Fixed in OPM
            //  Make sure to not overflow configstrings
            return;
        }

        m_font->setColor(UColor(
            m_foreground_color.r, m_foreground_color.g, m_foreground_color.b,
            m_foreground_color.a * m_local_alpha // HZM coop - HUD fade: stat text follows widget alpha
        ));

        if (m_bOutlinedText) {
            m_font->PrintOutlinedJustified(
                getClientFrame(),
                m_iFontAlignmentHorizontal,
                m_iFontAlignmentVertical,
                Sys_LV_CL_ConvertString(va("%s", CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[m_itemindex]))),
                UBlack,
                getVirtualScale()
            );
        } else {
            m_font->PrintJustified(
                getClientFrame(),
                m_iFontAlignmentHorizontal,
                m_iFontAlignmentVertical,
                Sys_LV_CL_ConvertString(va("%s", CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[m_itemindex]))),
                getVirtualScale()
            );
        }

        return;
    }

    if (m_inventoryrendermodelindex == -1 && !m_sDrawModelName.length() && !m_rendermodel) {
        float frac = 0.0;

        if (m_statbar_or == L_STATBAR_NONE) {
            return;
        }

        if (m_cvarname.length()) {
            frac = Cvar_VariableValue(m_cvarname) / (m_statbar_max - m_statbar_min);
        }

        if (frac > 1.0) {
            frac = 1.0;
        }
        if (frac < 0.0) {
            frac = 0.0;
        }

        DrawStatbar(frac);
        return;
    }

    VectorSet4(color, 255, 255, 255, 255);

    if (m_rendermodel) {
        if (!m_cvarname.length()) {
            return;
        }

        const char *szPreviewModel = Cvar_VariableString(m_cvarname);
        if (!szPreviewModel[0]) {
            // HZM coop: linked cvar exists but is empty (armory preview before any pick) -
            // registering "" spams "RE_RegisterModel: NULL name" every frame
            return;
        }

        handle = re.RegisterModel(szPreviewModel);
        if (!handle) {
            return;
        }

        VectorCopy(m_angles, angles);
        VectorCopy(m_rotateoffset, rotateoffset);
        VectorCopy(m_offset, offset);
        scale     = m_scale;
        sAnimName = m_anim;

        // HZM coop: if this widget has an anim cvar, use its value as the stance (per-weapon operator pose).
        if (m_animcvar.length()) {
            const char *an = Cvar_VariableString(m_animcvar.c_str());
            if (an && an[0]) {
                sAnimName = an;
            }
        }

        // HZM coop FIT-TUNE: if this widget has a transform cvar, read the live 7-float value
        // "offX offY offZ scale pitch yaw roll" and override. This is how the armory frames every
        // preview model per-item via cvars (p<id>.cfg sets the baked value; `uifit` nudges it live).
        if (m_xformcvar.length()) {
            const char *xs = Cvar_VariableString(m_xformcvar.c_str());
            if (xs && xs[0]) {
                float xf[7];
                if (sscanf(xs, "%f %f %f %f %f %f %f", &xf[0], &xf[1], &xf[2], &xf[3], &xf[4], &xf[5], &xf[6]) == 7) {
                    offset[0] = xf[0]; offset[1] = xf[1]; offset[2] = xf[2];
                    scale     = xf[3];
                    angles[0] = xf[4]; angles[1] = xf[5]; angles[2] = xf[6];
                }
            }
        }

        // Added in 2.11
        //  Tunak easter egg
        if (Cvar_Get("tunak", "0", 0)->integer) {
            // Since 2.15, those animations start has 'zz' prefix
            if (strstr(m_anim, "american")) {
                sAnimName = "zzamericanspecialidle";
            } else {
                // Added in 2.15
                sAnimName = "zzgermanspecialidle";
            }
        }
    } else {
        if (m_sDrawModelName.length()) {
            if (!m_lastitem) {
                m_lastitem = CL_GetInvItemByName(&client_inv, m_sDrawModelName);
            }
        } else {
            if (m_inventoryrendermodelindex >= 0
                && m_lastitemindex != cl.snap.ps.activeItems[m_inventoryrendermodelindex]) {
                m_lastitem = CL_GetInvItemByName(
                    &client_inv, CL_ConfigString(CS_WEAPONS + cl.snap.ps.activeItems[m_inventoryrendermodelindex])
                );
                m_lastitemindex = cl.snap.ps.activeItems[m_inventoryrendermodelindex];
            }
        }

        if (!m_lastitem) {
            return;
        }

        VectorCopy(m_lastitem->hudprops.rotateoffset, rotateoffset);
        VectorCopy(m_lastitem->hudprops.offset, offset);
        scale  = m_lastitem->hudprops.scale;
        handle = re.RegisterModel(m_lastitem->hudprops.model);
        VectorCopy(m_lastitem->hudprops.angledeltas, angles);

        if (m_lastitem->hudprops.move == INV_MOVE_BOB) {
            float frac = uid.time / 600.0;
            angles[0] += sin(frac) * m_lastitem->hudprops.angledeltas[0];
            angles[1] += sin(frac) * m_lastitem->hudprops.angledeltas[1];
            angles[2] += sin(frac) * m_lastitem->hudprops.angledeltas[2];
        } else if (m_lastitem->hudprops.model == INV_MOVE_SPIN) {
            float frac = uid.time / 600.0;
            angles[0] += frac * m_lastitem->hudprops.angledeltas[0];
            angles[1] += frac * m_lastitem->hudprops.angledeltas[1];
            angles[2] += frac * m_lastitem->hudprops.angledeltas[2];
        }
    }

    // HZM coop: click-drag rotate - add the accumulated spin (degrees, set by OnSpinDragged) to the yaw
    // so the player can turn the operator to inspect the loadout from any angle. Framing is unaffected.
    if (m_spincvar.length()) {
        angles[1] += atof(Cvar_VariableString(m_spincvar.c_str()));
    }

    if (m_rendermodel && !m_rendermodelfit) {
        VectorSet(mins, -16, -16, 0);
        VectorSet(maxs, 16, 16, 96);
    } else {
        // inventory items - and rendermodelfit previews (armory guns) - frame the model's
        // REAL bounds, so authoring differences in origin/pose can't crop or shrink it
        re.ModelBounds(handle, mins, maxs);
    }

    origin[1] = (mins[1] + maxs[1]) * 0.5;
    origin[2] = (mins[2] + maxs[2]) * -0.5;

    height = maxs[2] - mins[2];

    if (height < maxs[1] - mins[1]) {
        height = maxs[1] - mins[1];
    }

    if (height < maxs[0] - mins[0]) {
        height = maxs[0] - mins[0];
    }

    origin[0] = height * scale * 0.5 / 0.268f;

    // HZM coop: resolve up to two optional attach models composited onto bone tags of this widget's model.
    // Slot 1 = operator's helmet (m_attachcvar -> coop_loHelm, tag "Bip01 Head"); slot 2 = inspected weapon
    // in the operator's hand (m_attachcvar2 -> coop_loPrev, tag "tag_weapon_right"). Register each cvar's
    // model path and hand the handles + tags to the renderer, which seats them on the posed operator.
    qhandle_t   attachHandle  = 0;
    const char *attachTag     = NULL;
    if (m_attachcvar.length() && m_attachtag.length()) {
        const char *szAttach = Cvar_VariableString(m_attachcvar.c_str());
        if (szAttach && szAttach[0]) {
            attachHandle = re.RegisterModel(szAttach);
            if (attachHandle) {
                attachTag = m_attachtag.c_str();
            }
        }
    }
    qhandle_t   attachHandle2 = 0;
    const char *attachTag2    = NULL;
    if (m_attachcvar2.length() && m_attachtag2.length()) {
        const char *szAttach2 = Cvar_VariableString(m_attachcvar2.c_str());
        if (szAttach2 && szAttach2[0]) {
            attachHandle2 = re.RegisterModel(szAttach2);
            if (attachHandle2) {
                attachTag2 = m_attachtag2.c_str();
            }
        }
    }

    CL_Draw3DModel(
        m_screenframe.pos.x,
        m_screenframe.pos.y,
        m_screenframe.size.width,
        m_screenframe.size.height,
        handle,
        origin,
        rotateoffset,
        offset,
        angles,
        color,
        sAnimName,
        attachHandle,
        attachTag,
        attachHandle2,
        attachTag2
    );

    set2D();
}
