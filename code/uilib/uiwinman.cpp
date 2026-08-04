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
        if (IN_IsCursorActive()) {
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

qboolean UIWindowManager::KeyEvent(int key, unsigned int time)
{
    UIWidget *selwidget = NULL;

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
