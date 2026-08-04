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
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#pragma once

typedef enum {
    L_STATBAR_NONE,
    L_STATBAR_HORIZONTAL,
    L_STATBAR_VERTICAL,
    L_STATBAR_VERTICAL_REVERSE,
    L_STATBAR_VERTICAL_STAGGER_EVEN,
    L_STATBAR_VERTICAL_STAGGER_ODD,
    L_STATBAR_CIRCULAR,
    L_STATBAR_NEEDLE,
    L_STATBAR_ROTATOR,
    L_STATBAR_COMPASS,
    L_STATBAR_SPINNER,
    L_STATBAR_HEADING_SPINNER
} statbar_orientation_t;

extern Event EV_ClearInvItemReference;

class UIFakkLabel : public UILabel
{
protected:
    SafePtr<inventory_item_t> m_lastitem;
    int                       m_lastitemindex;
    int                       m_stat;
    int                       m_stat_alpha;
    int                       m_stat_configstring;
    int                       m_maxstat;
    int                       m_itemindex;
    int                       m_inventoryrendermodelindex;
    str                       m_sDrawModelName;
    qboolean                  m_rendermodel;
    qboolean                  m_rendermodelfit; // HZM coop: frame the model's REAL bounds, not the standing-player box
    statbar_orientation_t     m_statbar_or;
    float                     m_statbar_min;
    float                     m_statbar_max;
    float                     m_lastfrac;
    float                     m_flashtime;
    vec3_t                    m_offset;
    vec3_t                    m_rotateoffset;
    vec3_t                    m_angles;
    float                     m_scale;
    str                       m_anim;
    str                       m_xformcvar; // HZM coop: live FIT-TUNE - read offset(3)+scale(1)+angles(3) from a 7-float cvar
    str                       m_attachcvar; // HZM coop: cvar naming a model path to composite onto m_attachtag (helmet on operator)
    str                       m_attachtag;  // HZM coop: bone tag the attach model rides (e.g. "Bip01 Head")
    str                       m_attachcvar2; // HZM coop: 2nd attach - cvar naming a model path (weapon in operator's hand)
    str                       m_attachtag2;  // HZM coop: 2nd attach - bone tag (e.g. "tag_weapon_right")
    str                       m_animcvar;    // HZM coop: live anim override - read the model anim name from this cvar (per-weapon operator stance)
    str                       m_spincvar;    // HZM coop: click-drag rotate - horizontal drag adds degrees to this cvar (model yaw)
    float                     m_spinStartVal; // HZM coop: yaw cvar value captured at drag start
    int                       m_spinStartX;   // HZM coop: mouse X captured at drag start
    bool                      m_spinning;      // HZM coop: mid drag-rotate
    UIReggedMaterial         *m_statbar_material;
    UIReggedMaterial         *m_statbar_material_flash;
    UIReggedMaterial         *m_statbar_material_marker;

public:
    CLASS_PROTOTYPE(UIFakkLabel);

    UIFakkLabel();

    void LayoutPlayerStat(Event *ev);
    void LayoutPlayerStatAlpha(Event *ev); // Added in 2.0
    void LayoutPlayerStatConfigstring(Event *ev);
    void LayoutMaxPlayerStat(Event *ev);
    void LayoutItemIndex(Event *ev);
    void InventoryRenderModelIndex(Event *ev);
    void LayoutModelName(Event *ev);
    void ClearInvItemReference(Event *ev);
    void LayoutStatbar(Event *ev);
    void LayoutStatbarShader(Event *ev);
    void LayoutStatbarTileShader(Event *ev);
    void LayoutStatbarShader_Flash(Event *ev);
    void LayoutStatbarShader_Marker(Event *ev); // Added in 2.0
    void LayoutStatbarEndAngles(Event *ev);
    void LayoutStatbarNeedleWidth(Event *ev);
    void LayoutStatbarRotatorSize(Event *ev);
    void LayoutStatbarTileShader_Flash(Event *ev);
    void LayoutRenderModel(Event *ev);
    void LayoutRenderModelFit(Event *ev); // HZM coop
    void LayoutRenderModelOffset(Event *ev);
    void LayoutRenderModelRotateOffset(Event *ev);
    void LayoutRenderModelAngles(Event *ev);
    void LayoutRenderModelScale(Event *ev);
    void LayoutRenderModelAnim(Event *ev);
    void LayoutRenderModelXformCvar(Event *ev); // HZM coop fit-tune
    void LayoutRenderModelAttachCvar(Event *ev); // HZM coop: helmet-on-operator composite
    void LayoutRenderModelAttachTag(Event *ev);  // HZM coop: helmet-on-operator composite
    void LayoutRenderModelAttachCvar2(Event *ev); // HZM coop: weapon-in-hand composite
    void LayoutRenderModelAttachTag2(Event *ev);  // HZM coop: weapon-in-hand composite
    void LayoutRenderModelAnimCvar(Event *ev);    // HZM coop: live per-weapon operator stance
    void LayoutRenderModelSpinCvar(Event *ev);    // HZM coop: click-drag rotate
    void OnSpinPressed(Event *ev);                // HZM coop: click-drag rotate (W_LeftMouseDown)
    void OnSpinDragged(Event *ev);                // HZM coop: click-drag rotate (W_LeftMouseDragged)
    void OnSpinReleased(Event *ev);               // HZM coop: click-drag rotate (W_LeftMouseUp)

    void DrawStatbar(float frac);
    void DrawStatCircle(float frac);
    void DrawStatNeedle(float frac);
    void DrawStatRotator(float frac);
    void DrawStatCompass(float frac);
    void DrawStatSpinner(float frac);
    void StatCircleTexCoord(float fAng, vec3_t vTexCoord);

    void Draw(void) override;
};
