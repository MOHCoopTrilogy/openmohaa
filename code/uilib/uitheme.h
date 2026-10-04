/*
===========================================================================
HZM E2 (menu_system_remaster): the UI colour table for the engine-drawn parts of the menus.

The list headers, list selection bars, server-list rows, scrollbars and pulldown popups are drawn by the engine with
hard-coded colours; no .urc keyword reaches them. UI_ThemeColor() returns the colour for one of these slots:

  - Classic menu style (ui_menuRoom <= 0), or a slot the table leaves out: the CALLER'S classic constant, unchanged -
    so the classic menus are byte-identical to before.
  - War Room style (ui_menuRoom > 0): the slot's value from the cvar ui_themeColors, a flat list of "r g b a" groups in
    uiThemeColor_t order (0..1 floats). A group whose first token is "-" keeps the classic colour for that slot. The
    mod ships the table in a cfg; the engine hard-codes no theme colour.
===========================================================================
*/

#pragma once

#include "ucolor.h"

typedef enum {
    UITC_LIST_HEADER_BG,   // list column header fill          classic (0.07 0.06 0.005)
    UITC_LIST_HEADER_FG,   // list column header text + rule   classic UHudColor
    UITC_LIST_SEL_BG,      // list selection bar               classic (0.21 0.18 0.015)
    UITC_LIST_SEL_FG,      // list selected text               classic (0.9 0.8 0.6)
    UITC_SRV_ROW_BG,       // server row, queried              classic (0.02 0.07 0.004/5)
    UITC_SRV_ROW_FG,       //                                  classic UHudColor
    UITC_SRV_SEL_BG,       // server row, queried, selected    classic (0.2 0.18 0.015)
    UITC_SRV_SEL_FG,       //                                  classic (0.9 0.8 0.6)
    UITC_SRV_NEW_BG,       // server row, not yet queried      classic (0.005 0.07 0.02)
    UITC_SRV_NEW_FG,       //                                  classic (0.05 0.5 0.6)
    UITC_SRV_NEWSEL_BG,    // not yet queried, selected        classic (0.15 0.18 0.18)
    UITC_SRV_NEWSEL_FG,    //                                  classic (0.6 0.7 0.8)
    UITC_SRV_FAIL_BG,      // server row, query failed         classic (0.1 0 0)
    UITC_SRV_FAIL_FG,      //                                  classic (0.55 0 0)
    UITC_SRV_FAILSEL_BG,   // query failed, selected           classic (0.2 0 0)
    UITC_SRV_FAILSEL_FG,   //                                  classic (0.9 0 0)
    UITC_SCROLL_BG,        // scrollbar trough + arrow boxes   classic (0.15 0.196 0.278)
    UITC_SCROLL_BORDER,    // scrollbar solid border           classic (0.075 0.098 0.139)
    UITC_SCROLL_THUMB,     // scrollbar thumb                  classic (0.15 0.196 0.278)
    UITC_SCROLL_ARROW,     // scrollbar arrow glyphs           classic UHudColor
    UITC_POPUP_BG,         // pulldown popup body              classic (0.02 0.07 0.005)
    UITC_POPUP_FG,         // pulldown popup text              classic UHudColor
    UITC_POPUP_SEL_BG,     // pulldown popup highlight bar     classic (0.21 0.18 0.015)
    UITC_POPUP_SEL_FG,     // pulldown popup highlight text    classic (0.9 0.8 0.6)
    UITC_COUNT
} uiThemeColor_t;

bool   UI_ThemeActive(void);
UColor UI_ThemeColor(int slot, const UColor& classic);
