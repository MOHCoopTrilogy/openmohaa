/*
===========================================================================
Copyright (C) 2015-2024 the OpenMoHAA team

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

#include "ui_local.h"

// [HZM keynav-next] keyboard navigation of the mouse-only menus (arrows / Tab move focus like the pad's D-pad, Enter and
// Space click the widget under the cursor). ui_keyNav 0 switches it off. Never taken while a text field has focus.
static bool s_keyNavShown  = false;   // the keyboard has moved the UI cursor: draw it, and Enter / Space may click
static int  s_keyClickUpAt = 0;       // Sys_Milliseconds() at which the synthetic MOUSE1 is released (0 = none)

Event W_MouseExited
(
    "mouse_exited",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse exiting a widget event"
);
Event W_MouseEntered
(
    "mouse_entered",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse Entered a widget event"
);
Event W_LeftMouseDragged
(
    "left_mousedragged",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse was moved in a widget with the left button down"
);
Event W_RightMouseDragged
(
    "right_mousedragged",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse was moved in a widget with the right button down"
);
Event W_CenterMouseDragged
(
    "center_mousedragged",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse was moved in a widget with the center button down"
);
Event W_MouseMoved
(
    "mouse_moved",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Mouse was moved in a widget with no buttons down"
);
Event W_LeftMouseDown
(
    "left_mouse_down",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Left mouse button has been pressed down"
);
Event W_LeftMouseUp
(
    "left_mouse_up",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Left mouse button has been released"
);
Event W_RightMouseDown
(
    "right_mouse_down",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Right mouse button has been pressed down"
);
Event W_RightMouseUp
(
    "right_mouse_up",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Right mouse button has been released"
);
Event W_CenterMouseDown
(
    "center_mouse_down",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Center mouse button has been pressed down"
);
Event W_CenterMouseUp
(
    "center_mouse_up",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Center mouse button has been released"
);
Event W_AnyMouseDown
(
    "any_mouse_down",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Any mouse button has been pressed"
);
Event W_AnyMouseUp
(
    "any_mouse_up",
    EV_DEFAULT,
    "ffi",
    "xpos ypos buttons",
    "Any mouse button has been released"
);

UIWindowManager uWinMan;

CLASS_DECLARATION(UIWidget, UIWindowManager, NULL) {
    {NULL, NULL}
};

UIWindowManager::UIWindowManager()
{
    m_amidead          = false;
    m_lastbuttons      = 0;
    m_padClick         = 0;
    m_oldview          = NULL;
    m_firstResponder   = NULL;
    m_backgroundwidget = NULL;
    m_cursor           = NULL;
    m_showcursor       = true;
    m_font             = NULL;
    m_bindactive       = NULL;
    setBorderStyle(border_none);
}

UIWindowManager::~UIWindowManager()
{
    m_amidead = true;
}

void UIWindowManager::ViewEvent(UIWidget *view, Event& event, UIPoint2D& pos, int buttons)
{
    if (!view) {
        // use the first responder instead
        view = m_firstResponder;
        if (!view) {
            return;
        }
    }

    if (!view->isEnabled()) {
        // HZM coop: part of the ui_clickdebug tracer - a dropped event on a disabled
        // widget is exactly the failure mode stacked cvar-gated buttons produce.
        if (UI_GetCvarInt("ui_clickdebug", 0)) {
            uii.Sys_Printf("^~^~^ UICLICK DROPPED (disabled) view '%s'\n", view->getName());
        }
        return;
    }

    Event *ev = new Event(event);
    ev->AddFloat(pos.x);
    ev->AddFloat(pos.y);
    ev->AddInteger(buttons);
    // send the event to the view
    view->ProcessEvent(ev);
}

UIWidget *UIWindowManager::getResponder(UIPoint2D& pos)
{
    UIWidget *subview;
    UIWidget *responder;
    int       i;

    if (!m_firstResponder) {
        m_firstResponder = this;
    }

    responder = m_firstResponder;

    if (responder != this &&
        // Fixed in OPM
        //  Ignore the first responder if it's disabled
        //  this prevents the UI system from being blocked
        //  trying to activate the control
        responder->CanActivate() && responder->isEnabled()) {
        return responder;
    }

    for (i = m_children.NumObjects(); i > 0; i--) {
        subview = m_children.ObjectAt(i);
        if (subview != m_backgroundwidget) {
            responder = subview->FindResponder(pos);
            if (responder) {
                return responder;
            }
        }
    }

    if (m_backgroundwidget) {
        return m_backgroundwidget->FindResponder(pos);
    }

    return this;
}

void UIWindowManager::Init(const UIRect2D& frame, const char *fontname)
{
    setFrame(frame);
    m_font = new UIFont(fontname);
}

void UIWindowManager::setBackgroundWidget(UIWidget *widget)
{
    m_backgroundwidget = widget;
    if (widget) {
        widget->setParent(this);
        widget->setFrame(m_frame);
    }
}

void UIWindowManager::UpdateViews(void)
{
    int i;
    int n;
    int x, y;

    // [HZM keynav-next] release the synthetic click a short while after the press (the UI polls the button state per frame)
    if (s_keyClickUpAt && Sys_Milliseconds() >= s_keyClickUpAt) {
        s_keyClickUpAt = 0;
        Com_QueueEvent(Sys_Milliseconds(), SE_KEY, K_MOUSE1, qfalse, 0, NULL);
    }

    if (m_backgroundwidget) {
        m_backgroundwidget->Display(m_frame, 1.0);

        n = m_children.NumObjects();
        for (i = 1; i <= n; i++) {
            if (m_children.ObjectAt(i) != m_backgroundwidget) {
                m_children.ObjectAt(i)->Display(m_frame, 1.0);
            }
        }
    } else {
        Display(m_frame, 1.0);
    }

    if (m_cursor && m_showcursor && uid.uiHasMouse) {
        vec4_t col;

        set2D();

        // Added in OPM
        //  Draw the mouse if it has been grabbed
        // [gamepad-next] also while the pad drives the menu in a window: the OS cursor is not in screenshots / capture
        if (IN_IsCursorActive() || UI_GetCvarInt("in_padActive", 0) || s_keyNavShown) {
            VectorSet4(col, 1, 1, 1, 1);
            uii.Rend_SetColor(col);

            uii.Rend_DrawPicStretched(uid.mouseX, uid.mouseY, 0, 0, 0, 0, 1, 1, m_cursor->GetMaterial());
        }

        m_font->setColor(UWhite);

        if (UI_GetCvarInt("ui_drawcoords", 0)) {
            x = uid.mouseX + 10;
            y = uid.mouseY - 10;
            m_font->Print(x, y, va("%d:%d", uid.mouseX, uid.mouseY), -1, m_bVirtual ? m_vVirtualScale : NULL);
        }
    }
}

void UIWindowManager::ServiceEvents(void)
{
    UIPoint2D         pos;
    unsigned int      buttons;
    SafePtr<UIWidget> view;

    pos.x   = uid.mouseX;
    pos.y   = uid.mouseY;
    buttons = uid.mouseFlags;
    // [HZM gamepad-next] PadTab click: frame 3 hover, frame 2 button down, frame 1 button up
    if (m_padClick > 0) {
        if (m_padClick == 2) {
            buttons |= 1;
        }
        m_padClick--;
    }

    if (m_bindactive) {
        view = m_bindactive;
    } else {
        view = getResponder(pos);
    }

    SafePtr<UIWidget> b = m_oldview;

    if (b != view) {
        ViewEvent(m_oldview, W_MouseExited, pos, buttons);
        ViewEvent(view, W_MouseEntered, pos, buttons);
    }

    if (m_oldpos != pos) {
        if ((buttons & 1) && (m_lastbuttons & 1)) {
            ViewEvent(view, W_LeftMouseDragged, pos, buttons);
        }
        if ((buttons & 2) && (m_lastbuttons & 2)) {
            ViewEvent(view, W_RightMouseDragged, pos, buttons);
        }
        if ((buttons & 4) && (m_lastbuttons & 4)) {
            ViewEvent(view, W_CenterMouseDragged, pos, buttons);
        }
        if (!buttons) {
            ViewEvent(view, W_MouseMoved, pos, 0);
        }
    }

    // HZM coop: click routing tracer (set ui_clickdebug 1) - logs where every left
    // down/up lands, its enabled/activatable state, and any live responder capture.
    if (((buttons & 1) != (m_lastbuttons & 1)) && UI_GetCvarInt("ui_clickdebug", 0)) {
        UIWidget *cap = (m_firstResponder && m_firstResponder != this) ? (UIWidget *)m_firstResponder : NULL;
        uii.Sys_Printf(
            "^~^~^ UICLICK %s pos %d,%d view '%s' en=%d act=%d cap='%s'\n",
            (buttons & 1) ? "DOWN" : "UP",
            (int)pos.x,
            (int)pos.y,
            view ? view->getName() : "NULL",
            view ? (int)view->isEnabled() : -1,
            view ? (int)view->CanActivate() : -1,
            cap ? cap->getName() : "-"
        );
    }

    if ((buttons & 1) != (m_lastbuttons & 1)) {
        if (buttons & 1) {
            uii.UI_WantsKeyboard();
            ActivateControl(view);
            ViewEvent(view, W_LeftMouseDown, pos, buttons);
        } else {
            ViewEvent(view, W_LeftMouseUp, pos, buttons);
        }
    }
    if ((buttons & 2) != (m_lastbuttons & 2)) {
        if (buttons & 2) {
            ViewEvent(view, W_RightMouseDown, pos, buttons);
        } else {
            ViewEvent(view, W_RightMouseUp, pos, buttons);
        }
    }
    if ((buttons & 4) != (m_lastbuttons & 4)) {
        if (buttons & 4) {
            ViewEvent(view, W_CenterMouseDown, pos, buttons);
        } else {
            ViewEvent(view, W_CenterMouseUp, pos, buttons);
        }
    }
    if (buttons != m_lastbuttons) {
        ViewEvent(view, W_AnyMouseDown, pos, buttons);
        ViewEvent(view, W_AnyMouseUp, pos, buttons);
    }

    m_lastbuttons = buttons;
    m_oldpos      = pos;
    m_oldview     = view;

    if (!m_cursor) {
        // initialize the cursor
        setCursor("gfx/2d/mouse_cursor.tga");
    }
}

void UIWindowManager::setFirstResponder(UIWidget *responder)
{
    if (!responder) {
        m_firstResponder = this;
    } else {
        m_firstResponder = responder;
    }
}

UIWidget *UIWindowManager::getFirstResponder(void)
{
    if (m_firstResponder != this) {
        return m_firstResponder;
    }
    return NULL;
}

UIWidget *UIWindowManager::ActiveControl(void)
{
    return m_activeControl;
}

void UIWindowManager::ActivateControl(UIWidget *control)
{
    UIWidget *wid;

    for (wid = control; wid && wid != this; wid = wid->getParent()) {
        if (wid->CanActivate() && wid->isEnabled()) {
            break;
        }
    }

    if (!wid || wid == this) {
        return;
    }

    if (wid->CanActivate() && wid->isEnabled()) {
        UIWidget *active = ActiveControl();

        if (wid != active) {
            if (active) {
                m_activeControl = NULL;
                active->SendSignal(W_Deactivated);
                active->SetHovermaterialActive(false);
                active->SetPressedmaterialActive(false);
            }

            if (!active || !ActiveControl()) {
                m_activeControl = wid;
                wid->SetHovermaterialActive(true);
                wid->ActivateOrder();
                wid->BringToFrontPropogated();
                wid->SendSignal(W_Activated);
            }
        }
    }
}

void UIWindowManager::DeactivateCurrentControl(void)
{
    UIWidget *wid = m_activeControl;

    if (wid) {
        m_activeControl = NULL;
        wid->SendSignal(W_Deactivated);
    }
}

UIWidget *UIWindowManager::FindNextControl(UIWidget *control)
{
    UIWidget *widget;
    bool      back_up;

    if (!control) {
        control = this;
    }

    while (control) {
        widget = control->getFirstChild();
        if (widget) {
            if (widget->isEnabled() && widget->CanActivate()) {
                return widget;
            }

            control = widget;
        } else {
            widget = control->getNextSibling();
            if (widget) {
                if (widget->isEnabled() && widget->CanActivate()) {
                    return widget;
                }

                control = widget;
            } else {
                back_up = true;

                while (control->getParent() && back_up) {
                    control = control->getParent();
                    if (control->getNextSibling()) {
                        control = control->getNextSibling();

                        // there is an error in the fakk2 code where the second check if the control is enabled
                        // while the second should check if the control can activate
                        // this especially affects MOHAA
                        if (control->isEnabled() && control->CanActivate()) {
                            return control;
                        }

                        back_up = false;
                    }
                }

                if (control == this) {
                    break;
                }
            }
        }
    }

    return NULL;
}

UIWidget *UIWindowManager::FindPrevControl(UIWidget *control)
{
    UIWidget *tmp_control;
    UIWidget *next_control;

    if (control) {
        tmp_control = this;
        do {
            next_control = tmp_control;
            tmp_control  = FindNextControl(tmp_control);

            if (!tmp_control) {
                return NULL;
            }
        } while (tmp_control != control);
    } else {
        tmp_control = this;
        do {
            next_control = tmp_control;
            tmp_control  = FindNextControl(tmp_control);
        } while (tmp_control);
    }

    if (next_control != this) {
        return next_control;
    }

    return NULL;
}

void UIWindowManager::setCursor(const char *string)
{
    m_cursor = RegisterShader(string);

    if (m_cursor) {
        m_cursorname = string;
    } else {
        uii.Sys_Printf("Could not register shader '%s'\n", string);
    }

    // Added in OPM
    refreshCursor();
}

void UIWindowManager::showCursor(bool show)
{
    m_showcursor = show;
}

void UIWindowManager::CharEvent(int ch)
{
    if (m_activeControl) {
        m_activeControl->CharEvent(ch);
    }
}

extern void CL_SetMousePos(int x, int y);

/*
====================
PadProbe

A widget is reachable if SOME point of it hits the widget itself (a passive label laid over half of a button must not
hide the button, e.g. coop_startHzmInfo over APPLY). Tries the centre, then points above / below / left / right of it.
Returns the point in (outX, outY).
====================
*/
bool UIWindowManager::PadProbe(UIWidget *w, float *outX, float *outY)
{
    UIRect2D& r = w->m_clippedframe;
    static const float fx[9] = {0.5f, 0.5f, 0.5f, 0.25f, 0.75f, 0.5f, 0.15f, 0.85f, 0.5f};
    static const float fy[9] = {0.5f, 0.25f, 0.75f, 0.5f, 0.5f, 0.1f, 0.5f, 0.5f, 0.9f};
    int                k;

    for (k = 0; k < 9; k++) {
        UIPoint2D c(r.pos.x + r.size.width * fx[k], r.pos.y + r.size.height * fy[k]);

        if (getResponder(c) == w) {
            *outX = c.x;
            *outY = c.y;
            return true;
        }
    }
    return false;
}

/*
====================
UIWindowManager::PadNavigate

[HZM gamepad-next] D-pad focus move. Every widget that can be activated, is enabled and is really hit-testable at
its own centre (getResponder(centre) == widget, so nothing hidden behind another widget) is a candidate. The
cursor jumps to the centre of the best one in the pressed direction: nearest along the direction, with sideways
offset weighted double. The normal hover / click path does the rest, so every menu, custom or vanilla, works.
====================
*/
bool UIWindowManager::PadNavigate(int dx, int dy)
{
    struct Cand {
        UIWidget *w;
        float     x, y;
    };
    enum { MAXC = 512, MAXSTACK = 1024 };
    static Cand       cand[MAXC];
    static UIWidget  *stack[MAXSTACK];
    int               ncand = 0, sp = 0, i;
    float             px = (float)uid.mouseX, py = (float)uid.mouseY;
    float             bestScore = 1e30f, bx = 0, by = 0;
    bool              found = false;

    stack[sp++] = this;
    while (sp > 0) {
        UIWidget *w = stack[--sp];

        for (i = 1; i <= w->m_children.NumObjects(); i++) {
            if (sp < MAXSTACK) {
                stack[sp++] = w->m_children.ObjectAt(i);
            }
        }
        if (w == this || w == m_backgroundwidget || ncand >= MAXC) {
            continue;
        }
        if (UI_GetCvarInt("ui_clickdebug", 0) >= 3 && w->getName() && strstr(w->getName(), "apply")) {
            UIPoint2D c3(w->m_clippedframe.pos.x + w->m_clippedframe.size.width * 0.5f,
                         w->m_clippedframe.pos.y + w->m_clippedframe.size.height * 0.5f);
            UIWidget *r3 = getResponder(c3);

            uii.Sys_Printf("^~^~^ PADWHY %s activ=%d en=%d vis=%d show=%d w=%d at %d,%d resp='%s'\n", w->getName(),
                           (int)w->CanActivate(), (int)w->isEnabled(), (int)w->IsVisible(), (int)w->getShow(),
                           (int)w->m_clippedframe.size.width, (int)c3.x, (int)c3.y, r3 ? r3->getName() : "-");
        }
        // sliders and check boxes are not "activatable" in this UI (the mouse drags / toggles them) but the pad must reach them
        if (!w->isEnabled() || !(w->CanActivate() || w->isSubclassOf(UISlider) || w->isSubclassOf(UICheckBox))) {
            continue;
        }
        if (w->m_clippedframe.size.width < 2 || w->m_clippedframe.size.height < 2) {
            continue;
        }
        // [HZM keynav-next] a dot-sized button (under 6 x 6 menu units: the retail c3 easter egg) is not a focus stop
        {
            const float sx = m_frame.size.width / 640.0f, sy = m_frame.size.height / 480.0f;   // px per menu unit

            if (w->m_clippedframe.size.width < 6.0f * sx && w->m_clippedframe.size.height < 6.0f * sy) {
                continue;
            }
        }
        {
            float cx, cy;

            if (!PadProbe(w, &cx, &cy)) {
                continue;
            }
            cand[ncand].w = w;
            cand[ncand].x = cx;
            cand[ncand].y = cy;
            ncand++;
        }
    }

    // start from the centre of the widget under the cursor, if it is one of the candidates
    {
        UIPoint2D here((float)uid.mouseX, (float)uid.mouseY);
        UIWidget *under = getResponder(here);

        for (i = 0; i < ncand; i++) {
            if (cand[i].w == under) {
                px = cand[i].x;
                py = cand[i].y;
                break;
            }
        }
    }

    if (UI_GetCvarInt("ui_clickdebug", 0) >= 2) {
        for (i = 0; i < ncand; i++) {
            uii.Sys_Printf("^~^~^ PADCAND %s %d %d\n", cand[i].w->getName(), (int)cand[i].x, (int)cand[i].y);
        }
    }

    for (i = 0; i < ncand; i++) {
        float rx = cand[i].x - px, ry = cand[i].y - py;
        float fwd = rx * dx + ry * dy;
        float lat = fabsf(dx ? ry : rx);

        if (fwd < 2.0f) {
            continue;
        }
        if (fwd + lat * 2.0f < bestScore) {
            bestScore = fwd + lat * 2.0f;
            bx        = cand[i].x;
            by        = cand[i].y;
            found     = true;
        }
    }

    if (found) {
        CL_SetMousePos((int)bx, (int)by);
    }
    if (UI_GetCvarInt("ui_clickdebug", 0)) {
        UIPoint2D at(bx, by);
        UIWidget *w = found ? getResponder(at) : NULL;

        uii.Sys_Printf("^~^~^ PADNAV dir %d,%d cands %d -> %s (%d,%d) widget '%s'\n", dx, dy, ncand, found ? "moved" : "none",
                       (int)bx, (int)by, w ? w->getName() : "-");
    }
    return found;
}

/*
====================
UIWindowManager::PadTab

[HZM gamepad-next] LB / RB. Tabs in this UI are plain buttons whose names start with "tab" or "foldertab" (Armory
categories, loadout pages, options folders). The row is ordered by position; the current tab is the one under the
cursor, else the one the pad last picked. The cursor jumps to the previous / next tab and a click is injected.
====================
*/
bool UIWindowManager::PadTab(int dir)
{
    struct Cand {
        UIWidget *w;
        float     x, y;
    };
    enum { MAXC = 64, MAXSTACK = 1024 };
    static Cand      cand[MAXC];
    static UIWidget *stack[MAXSTACK];
    static float     lastX = -1, lastY = -1;
    int              n = 0, sp = 0, i, j, cur = -1;

    stack[sp++] = this;
    while (sp > 0) {
        UIWidget *w = stack[--sp];

        for (i = 1; i <= w->m_children.NumObjects(); i++) {
            if (sp < MAXSTACK) {
                stack[sp++] = w->m_children.ObjectAt(i);
            }
        }
        if (w == this || n >= MAXC) {
            continue;
        }
        {
            const char *nm = w->getName();

            if (!nm || (Q_stricmpn(nm, "tab", 3) && Q_stricmpn(nm, "foldertab", 9))) {
                continue;
            }
        }
        if (!w->CanActivate() || !w->isEnabled() || w->m_clippedframe.size.width < 2) {
            continue;
        }
        {
            float cx, cy;

            if (!PadProbe(w, &cx, &cy)) {
                continue;
            }
            cand[n].w = w;
            cand[n].x = cx;
            cand[n].y = cy;
            n++;
        }
    }
    if (n < 2) {
        return false;
    }

    // order: rows (y bucketed to 24 px), then x
    for (i = 1; i < n; i++) {
        Cand t = cand[i];

        for (j = i - 1; j >= 0; j--) {
            long rj = (long)(cand[j].y / 24.0f), rt = (long)(t.y / 24.0f);

            if (rj < rt || (rj == rt && cand[j].x <= t.x)) {
                break;
            }
            cand[j + 1] = cand[j];
        }
        cand[j + 1] = t;
    }

    {
        UIPoint2D here((float)uid.mouseX, (float)uid.mouseY);
        UIWidget *under = getResponder(here);

        for (i = 0; i < n; i++) {
            if (cand[i].w == under) {
                cur = i;
                break;
            }
        }
        if (cur < 0 && lastX >= 0) {
            for (i = 0; i < n; i++) {
                if (fabsf(cand[i].x - lastX) < 2 && fabsf(cand[i].y - lastY) < 2) {
                    cur = i;
                    break;
                }
            }
        }
    }
    if (cur < 0) {
        cur = (dir > 0) ? -1 : 0;
    }
    cur = (cur + dir + n) % n;
    lastX = cand[cur].x;
    lastY = cand[cur].y;
    CL_SetMousePos((int)lastX, (int)lastY);
    m_padClick = 3;       // ServiceEvents: hover frame, press frame, release frame
    if (UI_GetCvarInt("ui_clickdebug", 0)) {
        uii.Sys_Printf("^~^~^ PADTAB dir %d -> '%s' (%d of %d)\n", dir, cand[cur].w->getName(), cur + 1, n);
    }
    return true;
}

qboolean UIWindowManager::KeyEvent(int key, unsigned int time)
{
    UIWidget *selwidget = NULL;

    // [HZM gamepad-next] an open pad pull-down owns the D-pad (up / down = highlight, A = pick, B = cancel)
    if (UIPulldownMenu::PadKey(key)) {
        return true;
    }

    // [HZM gamepad-next] LB / RB = previous / next tab; with no tab row they page the focused list.
    if ((key == K_PAD0_LEFTSHOULDER || key == K_PAD0_RIGHTSHOULDER) && !m_bindactive) {
        if (!PadTab(key == K_PAD0_RIGHTSHOULDER ? 1 : -1) && m_activeControl && m_activeControl != this) {
            m_activeControl->KeyEvent(key == K_PAD0_RIGHTSHOULDER ? K_PGDN : K_PGUP, time);
        }
        return true;
    }

    // [HZM gamepad-next] right-stick scroll arrives as wheel ticks: give them to the list UNDER the cursor, not
    // only to the focused control (a list nobody clicked would never scroll). Pad input only.
    if ((key == K_MWHEELUP || key == K_MWHEELDOWN) && !m_bindactive && UI_GetCvarInt("in_padActive", 0)) {
        UIPoint2D here((float)uid.mouseX, (float)uid.mouseY);
        UIWidget *w = getResponder(here);

        for (; w && w != this; w = w->getParent()) {
            if (w->KeyEvent(key, time)) {
                return true;
            }
        }
    }

    // [HZM gamepad-next] D-pad: the focused widget gets the arrow first (lists, sliders), else focus moves.
    if (key >= K_PAD0_DPAD_UP && key <= K_PAD0_DPAD_RIGHT && !m_bindactive) {
        static const int arrow[4] = {K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW};
        static const int ddx[4]   = {0, 0, -1, 1};
        static const int ddy[4]   = {-1, 1, 0, 0};
        int              d        = key - K_PAD0_DPAD_UP;

        // the widget UNDER the cursor gets the arrow first (a slider the D-pad just landed on: left / right = value)
        {
            UIPoint2D here((float)uid.mouseX, (float)uid.mouseY);
            UIWidget *w = getResponder(here);

            for (; w && w != this; w = w->getParent()) {
                if (w->isSubclassOf(UISlider) && w->KeyEvent(arrow[d], time)) {
                    if (UI_GetCvarInt("ui_clickdebug", 0)) {
                        uii.Sys_Printf("^~^~^ PADNAV dpad %d -> slider '%s'\n", d, w->getName());
                    }
                    return true;
                }
            }
        }
        if (m_activeControl && m_activeControl != this && m_activeControl->KeyEvent(arrow[d], time)) {
            if (UI_GetCvarInt("ui_clickdebug", 0)) {
                uii.Sys_Printf("^~^~^ PADNAV dpad %d eaten by '%s'\n", d, m_activeControl->getName());
            }
            return true;
        }
        PadNavigate(ddx[d], ddy[d]);
        return true;
    }

    // [HZM keynav-next] keyboard: arrows / Tab = focus to the next widget in that direction (the pad's D-pad logic), Enter /
    // Space = click what the cursor is on. Lists, sliders and pull-downs that take the arrow themselves get it first, and
    // a text field keeps every key (the legacy Up / Down field order is unchanged).
    if (!m_bindactive && UI_GetCvarInt("ui_keyNav", 1)) {
        const bool typing = m_activeControl && m_activeControl != this
                         && (m_activeControl->isSubclassOf(UIField) || m_activeControl->isSubclassOf(UIMultiLineEdit));

        if (!typing) {
            const bool shift = uii.Sys_IsKeyDown(K_SHIFT) != 0;
            int        dx = 0, dy = 0;
            bool       nav = false;

            switch (key) {
            case K_UPARROW:    dy = -1; nav = true; break;
            case K_DOWNARROW:  dy = 1;  nav = true; break;
            case K_LEFTARROW:  dx = -1; nav = true; break;
            case K_RIGHTARROW: dx = 1;  nav = true; break;
            case K_TAB:
                if (!uii.Sys_IsKeyDown(K_CTRL)) {
                    dx  = shift ? -1 : 1;
                    nav = true;
                }
                break;
            default: break;
            }

            if (nav) {
                if (key != K_TAB) {
                    UIPoint2D here((float)uid.mouseX, (float)uid.mouseY);
                    UIWidget *w = getResponder(here);

                    for (; w && w != this; w = w->getParent()) {
                        if (w->isSubclassOf(UISlider) && w->KeyEvent(key, time)) {
                            return true;
                        }
                    }
                    if (m_activeControl && m_activeControl != this && m_activeControl->KeyEvent(key, time)) {
                        return true;
                    }
                }
                bool moved = PadNavigate(dx, dy);

                if (!moved && key == K_TAB) {
                    moved = PadNavigate(0, shift ? -1 : 1);   // end of a row: continue down the next one
                }
                if (moved) {
                    if (!s_keyNavShown) {
                        UI_SetCvarInt("ui_keyNavActive", 1);   // menus may hide pad / keyboard-unfriendly bits (main.urc c3)
                    }
                    s_keyNavShown = true;
                    return true;
                }
            } else if (s_keyNavShown && (key == K_ENTER || key == K_KP_ENTER || key == K_SPACE)) {
                if (m_activeControl && m_activeControl != this && m_activeControl->KeyEvent(key, time)) {
                    return true;
                }
                Com_QueueEvent(Sys_Milliseconds(), SE_KEY, K_MOUSE1, qtrue, 0, NULL);
                s_keyClickUpAt = Sys_Milliseconds() + 110;
                return true;
            }
        }
    }

    if (key == K_TAB && uii.Sys_IsKeyDown(K_CTRL)) {
        UIWidget *wid;

        if (m_activeControl && m_activeControl != this) {
            for (selwidget = m_activeControl; selwidget != NULL; selwidget = selwidget->getParent()) {
                if (selwidget->getParent() == this) {
                    break;
                }
            }
        }

        if (!selwidget) {
            selwidget = getFirstChild();
        }

        for (wid = getNextChild(selwidget); wid != NULL; wid = getNextChild(wid)) {
            if (!(selwidget->m_flags & WF_ALWAYS_BOTTOM) && selwidget->CanActivate()) {
                ActivateControl(wid);
                return true;
            }
        }
        for (wid = getFirstChild(); wid != selwidget && wid != NULL; wid = getNextChild(wid)) {
            if (!(selwidget->m_flags & WF_ALWAYS_BOTTOM) && selwidget->CanActivate()) {
                ActivateControl(wid);
                return true;
            }
        }
    } else {
        if (m_bindactive) {
            m_bindactive->KeyEvent(key, time);
        } else if (!(m_activeControl && m_activeControl->KeyEvent(key, time))
                   && (key == K_DOWNARROW || key == K_UPARROW)) {
            selwidget = m_activeControl;
            if (selwidget && selwidget != this) {
                for (selwidget = m_activeControl; selwidget != NULL; selwidget = selwidget->getParent()) {
                    if (selwidget->getParent() == this) {
                        break;
                    }
                }
            }

            if (selwidget != NULL) {
                if (selwidget->isSubclassOf(UIWidgetContainer)) {
                    UIWidgetContainer *widcon = (UIWidgetContainer *)selwidget;

                    if (key == K_DOWNARROW) {
                        selwidget = widcon->GetNextWidgetInOrder();
                    } else if (key == K_UPARROW) {
                        selwidget = widcon->GetPrevWidgetInOrder();
                    }

                    if (selwidget) {
                        ActivateControl(selwidget);
                    }
                }
            }
        }
    }

    return true;
}

void UIWindowManager::CreateMenus(void)
{
    int i;
    int n;

    menuManager.DeleteAllMenus();

    // HZM coop (bug-767): drop DUPLICATE same-named widget containers before building menus.
    // Every URC scan path ("new UILayout" in cl_ui.cpp: the CL_InitializeUI ui/*.urc loop and
    // the "loadmenu" command) only ADDS containers to uWinMan; nothing removes a same-named
    // container from an earlier scan. This routine then builds one Menu per container with no
    // name dedup, so after any second scan every menu name would exist twice: only the first
    // copy is reachable through menuManager.FindMenu/PushMenu, while the stale twin can still
    // be drawn and still receives mouse input (input hit-testing walks uWinMan children, not
    // the menu stack) = doubled/garbled menu text and dead buttons. Keep the FIRST container
    // (preserves FindMenu order for existing callers); tear later copies down with the
    // standard recursive Shutdown()+delete recipe (the widget destructor unlinks itself from
    // uWinMan.m_children, so the downward walk stays valid).
    for (i = uWinMan.m_children.NumObjects(); i >= 2; i--) {
        UIWidget *w = uWinMan.m_children.ObjectAt(i);
        int       j;

        if (!w->isSubclassOf(UIWidgetContainer) || !w->getName() || !*w->getName()) {
            continue;
        }

        for (j = 1; j < i; j++) {
            UIWidget *earlier = uWinMan.m_children.ObjectAt(j);

            if (earlier->isSubclassOf(UIWidgetContainer) && earlier->getName()
                && !str::icmp(earlier->getName(), w->getName())) {
                uii.Sys_Printf("UIWindowManager::CreateMenus: dropping duplicate menu container '%s'\n", w->getName());
                w->Shutdown();
                delete w;
                break;
            }
        }
    }

    n = uWinMan.m_children.NumObjects();
    for (i = 1; i <= n; i++) {
        UIWidget *w = uWinMan.m_children.ObjectAt(i);
        if (w->isSubclassOf(UIWidgetContainer)) {
            UIWidgetContainer *wid = (UIWidgetContainer *)w;
            Menu              *m   = new Menu(w->getName());

            m->setFullscreen(wid->isFullscreen());
            m->setVidMode(wid->getVidMode());
            m->AddMenuItem(w);
            m->ForceHide();
        }
    }
}

UIReggedMaterial *UIWindowManager::RegisterShader(const str& name)
{
    int               i;
    UIReggedMaterial *regged;

    for (i = 1; i <= m_materiallist.NumObjects(); i++) {
        regged = m_materiallist.ObjectAt(i);
        if (regged->GetName() == name) {
            return regged;
        }
    }

    regged = new UIReggedMaterial;
    regged->SetMaterial(name);

    m_materiallist.AddObject(regged);
    return regged;
}

UIReggedMaterial *UIWindowManager::RefreshShader(const str& name)
{
    int               i;
    UIReggedMaterial *regged;

    for (i = 1; i <= m_materiallist.NumObjects(); i++) {
        regged = m_materiallist.ObjectAt(i);
        if (regged->GetName() == name) {
            regged->RefreshMaterial();
            return regged;
        }
    }

    regged = new UIReggedMaterial;
    regged->SetMaterial(name);

    m_materiallist.AddObject(regged);
    return regged;
}

void UIWindowManager::CleanupShadersFromList(void)
{
    int i;
    int num;

    num = m_materiallist.NumObjects();
    for (i = 1; i <= num; i++) {
        UIReggedMaterial *regged = m_materiallist.ObjectAt(i);
        regged->CleanupMaterial();
    }
}

void UIWindowManager::RefreshShadersFromList(void)
{
    int i;
    int num;

    num = m_materiallist.NumObjects();
    for (i = 1; i <= num; i++) {
        UIReggedMaterial *regged = m_materiallist.ObjectAt(i);
        regged->ReregisterMaterial();
    }

    refreshCursor();
}

void UIWindowManager::DeactivateCurrentSmart(void)
{
    UIWidget *toset;
    UIWidget *checking;
    UIWidget *sibling;

    toset = NULL;

    if (!m_activeControl) {
        return;
    }

    for (checking = m_activeControl; checking; checking = checking->getParent()) {
        if (!checking->getParent() || (checking->getParent() && checking->getParent()->IsDying())) {
            continue;
        }

        for (sibling = checking->getPrevSibling(); sibling && !toset; sibling = sibling->getPrevSibling()) {
            if (sibling->CanActivate()) {
                toset = sibling;
            }
        }

        if (toset) {
            break;
        }

        for (sibling = checking->getLastSibling(); sibling && sibling != checking && !toset;
             sibling = sibling->getPrevSibling()) {
            if (sibling->CanActivate()) {
                toset = sibling;
            }
        }

        if (toset) {
            break;
        }
    }

    if (!toset) {
        uWinMan.DeactivateCurrentControl();
        return;
    }

    if (toset == &uWinMan) {
        Menu     *menu;
        UIWidget *newWid = NULL;

        menu = menuManager.CurrentMenu();

        if (menu) {
            newWid = menu->GetContainerWidget();
        }

        if (newWid) {
            toset = newWid;
        }
    }

    uWinMan.ActivateControl(toset);
}

bool UIWindowManager::IsDead(void)
{
    return m_amidead;
}

int UIWindowManager::AddBinding(str binding)
{
    int      i;
    Binding *bind;

    for (i = 1; i <= bindings.NumObjects(); i++) {
        Binding *b = bindings.ObjectAt(i);

        if (b->binding == binding) {
            return i;
        }
    }

    bind          = new Binding;
    bind->binding = binding;
    uii.Key_GetKeysForCommand(binding, &bind->key1, &bind->key2);
    bindings.AddObject(bind);

    return bindings.IndexOfObject(bind);
}

str UIWindowManager::GetKeyStringForCommand(str command, int index, qboolean alternate, int *key1, int *key2)
{
    str      s;
    str      cmd;
    Binding *bind;

    if (index > bindings.NumObjects()) {
        return "Invalid Command";
    }

    bind = bindings.ObjectAt(index);

    if (command == bind->binding) {
        if (bind->key1 < 0 || bind->key2 < 0) {
            uii.Key_GetKeysForCommand(command, &bind->key1, &bind->key2);
        }

        if (bind->key2 >= 0) {
            cmd = uii.Key_GetCommandForKey(bind->key2);
            if (cmd != bind->binding) {
                bind->key2 = -1;
            }
        }
        if (bind->key1 >= 0) {
            cmd = uii.Key_GetCommandForKey(bind->key1);
            if (cmd != bind->binding) {
                if (bind->key2 < 0) {
                    bind->key1 = -1;
                } else {
                    bind->key1 = bind->key2;
                    bind->key2 = -1;
                }
            }
        }

        if (alternate) {
            if (bind->key2 >= 0) {
                s = uii.Key_KeynumToString(bind->key2);
            } else {
                s = "None";
            }
        } else {
            if (bind->key1 >= 0) {
                s = uii.Key_KeynumToString(bind->key1);
            } else {
                s = "None";
            }
        }

        if (key1) {
            *key1 = bind->key1;
        }
        if (key2) {
            *key2 = bind->key2;
        }
    } else {
        s = "Invalid Command";
    }

    return s;
}

void UIWindowManager::BindKeyToCommand(str command, int key, int index, qboolean alternate)
{
    Binding *bind = bindings.ObjectAt(index);

    if (bind->binding == command || !bind->binding.length()) {
        if (!command.length()) {
            uii.Key_SetBinding(key, "");
            return;
        }

        if (!alternate) {
            if (bind->key1 == key) {
                return;
            }

            if (bind->key2 == key) {
                uii.Key_SetBinding(bind->key1, "");
                uii.Key_SetBinding(bind->key2, "");
                bind->key1 = key;
                bind->key2 = -1;
            } else {
                uii.Key_SetBinding(bind->key1, "");
            }

            if (!command.length()) {
                uii.Key_SetBinding(key, "");
            } else {
                uii.Key_SetBinding(key, command.c_str());
            }
        } else {
            if (bind->key2 == key) {
                return;
            }

            if (bind->key1 == key) {
                uii.Key_SetBinding(bind->key2, "");
            } else {
                uii.Key_SetBinding(bind->key2, "");
                bind->key2 = key;

                if (!command.length()) {
                    uii.Key_SetBinding(key, "");
                } else {
                    uii.Key_SetBinding(key, command.c_str());
                }
            }
        }
    } else {
        warning(
            "UIWindowManager::BindKeyToCommand",
            "Invalid bind command '%s' for widget command '%s'\n",
            command.c_str(),
            bind->binding.c_str()
        );
    }
}

UIWidget *UIWindowManager::BindActive(void)
{
    return m_bindactive;
}

void UIWindowManager::SetBindActive(UIWidget *w)
{
    m_bindactive = w;
}

void UIWindowManager::Shutdown(void)
{
    int i;

    for (i = m_materiallist.NumObjects(); i > 0; i--) {
        UIReggedMaterial *mat = m_materiallist.ObjectAt(i);
        m_materiallist.RemoveObjectAt(i);

        delete mat;
    }

    for (i = bindings.NumObjects(); i > 0; i--) {
        Binding *b = bindings.ObjectAt(i);
        bindings.RemoveObjectAt(i);

        delete b;
    }

    m_cursor = NULL;
    IN_FreeCursor();

    UIWidget::Shutdown();
}

void UIWindowManager::DeactiveFloatingWindows(void)
{
    int       i;
    UIWidget *pWidg;

    for (i = m_children.NumObjects(); i > 0; i--) {
        pWidg = m_children.ObjectAt(i);

        if (pWidg->isSubclassOf(UIFloatingWindow)) {
            pWidg->SendSignal(W_Deactivated);
        }

        if (pWidg->isSubclassOf(UIPopupMenu)) {
            pWidg->ProcessEvent(W_Deactivated);
        }
    }
}

bool UIWindowManager::DialogExists(void)
{
    int       i;
    UIWidget *pWidg;

    for (i = 1; i <= m_children.NumObjects(); i++) {
        pWidg = m_children.ObjectAt(i);
        if (pWidg->isSubclassOf(UIDialog)) {
            return true;
        }
    }

    return false;
}

void UIWindowManager::RemoveAllDialogBoxes(void)
{
    int       i;
    UIWidget *pWidg;

    for (i = m_children.NumObjects(); i > 0; i--) {
        pWidg = m_children.ObjectAt(i);
        if (pWidg->isSubclassOf(UIDialog)) {
            UIDialog *pDlg = (UIDialog *)pWidg;

            pDlg->m_cancel->m_command += "\n";
            uii.Cmd_Stuff(pDlg->m_cancel->m_command);
        }
    }
}

void UIWindowManager::refreshCursor()
{
    byte *pic;
    int   width, height;

    // Added in OPM
    //  Load cursor image and use it
    if (!m_cursorname.length()) {
        return;
    }

    if (uii.Rend_LoadRawImage(m_cursorname.c_str(), &pic, &width, &height)) {
        IN_SetCursorFromImage(pic, width, height, uii.Rend_FreeRawImage);
    }
}
