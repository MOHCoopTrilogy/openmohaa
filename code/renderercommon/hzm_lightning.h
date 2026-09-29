/*
===========================================================================
HZM coop [2026-09-27] REALISTIC LIGHTNING - the shared switches and the cgame -> renderer state contract.
docs/proposals/lightning_2026-09-27/plan.md (v1), vet.md.

ONE header, read by cgame.dll and renderer_opengl2.dll, so the two halves can never disagree about what "auto" means or
how the per-frame state string is laid out (TRAPS T4). game.dll does not include it: its half is the configstring
CS_HZM_LIGHTNING (fgame/bg_public.h), which cgame parses.

  server  global/weather.scr (coop_ltMode 2) -> setcvar coop_ltStrike "<kind> <yaw> <distM> <n> <seed>"
  game    G_HzmLtPublish (g_main.cpp): validates, stamps seq + server time -> configstring CS_HZM_LIGHTNING (28)
  cgame   cg_hzmlightning.c: expands the strike from its seed (same on every client), runs the flash limiter, plays
          the thunder per listener, and publishes r_hzmLtState every frame while a strike is lit
  gl2     tr_hzm_lightning.c: parses r_hzmLtState; sky pass (lit cloud lobe + far bolt at sky depth), fog in-scatter
          and the outdoor surface light in lightall_fp.glsl

1) THE AUTO DEFAULTS. Every switch is registered "-1" = auto with flags 0 (never saved), the headlight pattern:
   -1 resolves to the define below, 0 / 1 force it. While HZM_LIGHTNING_AUTO is 0 NOTHING changes for any player and
   the renderer is byte-identical (no uniform is non-zero, the sky pass never runs). Flip here, rebuild BOTH DLLs.
2) OMAHA. Both halves refuse m3l1a/m3l1b through HZM_LightRestoreMapProtected (hzm_light_restore.h). Those maps have
   no weather; this makes a strike there impossible by construction (user decision D7).
3) THE STATE STRING (cgame writes, renderer reads; "0" or empty = off):
     r_hzmLtState "<ver> <cgTimeMs> <eSky> <eWorld> <eHaze> <eBolt> <sx> <sy> <sz> <boltSeed> <boltYawDeg> <boltTopElDeg>"
   s = unit direction to the lit cloud region, WORLD space (Q3 axes, z up). Energies are already envelope x distance x
   map scale; the renderer only multiplies by its flash colour. cgTimeMs is refdef time when written: the renderer
   ignores a state more than HZM_LT_STALE_MS older than the frame it draws (vet V10: the publish site is skipped on
   snapshot loss and in cinematics, and cgame also clears it in the bug-1202 list at CG_Init).
===========================================================================
*/
#ifndef HZM_LIGHTNING_H
#define HZM_LIGHTNING_H

#define HZM_LIGHTNING_AUTO      1   // cg_hzmLightning / r_hzmLightning -1 means this. ON since the 2026-09-28 in-engine A/B.
#define HZM_LTBOLTS_AUTO        1   // cg_hzmLightningBolts -1 means this. User 2026-09-27: ON together with HZM_LIGHTNING_AUTO
                                    // ("the distant bolt adds to it") - both flipped to 1 after the A/B passed (bug-3171/3172 fixed).

#define HZM_LT_STATE_VERSION    1
#define HZM_LT_STALE_MS         2000

// the flash colour (a cool white, the look-dev's), shared so cgame's thunder band log and the renderer agree
#define HZM_LT_COLOR_R          0.86f
#define HZM_LT_COLOR_G          0.90f
#define HZM_LT_COLOR_B          1.00f

// bolts are drawn only from this elevation up (never across the middle of the screen when looking level, D3)
#define HZM_LT_BOLT_MIN_EL_DEG  2.0f

#endif
