/*
===========================================================================
Copyright (C) 2026 HaZardModding coop - OpenMOHAA gl2 renderer

This file is part of OpenMOHAA source code.

OpenMOHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/
// tr_msaa_decide.h - HZM gl2 MSAA (plan P2a): the PURE sample-count decision - the Auto tiers (ME-F14), the
// render-scale clamp (Auto and explicit alike), and the GL-caps clamp. No engine dependencies, so the gfx harness
// compiles this very header into a unit test (the Auto matrix of P2 accept #8). The GL queries, the hard
// requirements and r_msaaForcedOff stay in tr_msaa.c, which fills hzmMsaaIn_t and calls HZM_MsaaDecide.

#ifndef TR_MSAA_DECIDE_H
#define TR_MSAA_DECIDE_H

typedef struct {
	int   want;       // -1 Auto, 0 off, > 0 explicit
	float scale;      // r_renderScale, clamped to 0.5 .. 2.0 exactly as R_CreateBuiltinImages clamps it
	int   caps;       // min(GL_MAX_SAMPLES, GL_MAX_COLOR_TEXTURE_SAMPLES, GL_MAX_DEPTH_TEXTURE_SAMPLES)
	int   vramMB;     // dedicated VRAM; <= 0 = unknown
	int   sceneW;     // scene size after the render scale
	int   sceneH;
	int   colorBpp;   // bytes per colour sample: 8 = RGBA16F (r_hdr), 4 = RGBA8; depth adds 4
	int   intelIGpu;  // Intel integrated graphics (Arc counts as discrete)
} hzmMsaaIn_t;

typedef struct {
	int         n;       // the sample count: 0, or a power of two >= 2
	const char *tier;    // why: auto:..., explicit, explicit-0
	const char *clamp;   // NULL, or the render-scale clamp that lowered n
	int         capped;  // 1 when the GL caps lowered n
	int         mb8x;    // the 8x colour + depth buffers at this scene size, in MB (what Auto weighs)
} hzmMsaaOut_t;

static void HZM_MsaaDecide(const hzmMsaaIn_t *in, hzmMsaaOut_t *out)
{
	int    n = 0;
	double bytes8 = (double)in->sceneW * (double)in->sceneH * (double)(in->colorBpp + 4) * 8.0;

	out->tier   = "explicit";
	out->clamp  = 0;
	out->capped = 0;
	out->mb8x   = (int)(bytes8 / 1048576.0);

	if (in->want < 0)
	{
		// Auto: off first, then the VRAM tiers. 8x only when its buffers fit in 5% of the card.
		if (in->intelIGpu)
		{
			n = 0;
			out->tier = "auto:intel-igpu";
		}
		else if (in->scale >= 1.5f)
		{
			n = 0;
			out->tier = "auto:scale>=1.5";
		}
		else if (in->vramMB <= 0)
		{
			n = 4;
			out->tier = "auto:vram-unknown";
		}
		else if (in->vramMB >= 10240 && bytes8 <= (double)in->vramMB * 1048576.0 * 0.05)
		{
			n = 8;
			out->tier = "auto:vram>=10GB";
		}
		else if (in->vramMB >= 10240)
		{
			n = 4;
			out->tier = "auto:8x-over-5pct-cap";
		}
		else if (in->vramMB >= 4096)
		{
			n = 4;
			out->tier = "auto:vram4-10GB";
		}
		else
		{
			n = 2;
			out->tier = "auto:vram<4GB";
		}
	}
	else if (in->want == 0)
	{
		n = 0;
		out->tier = "explicit-0";
	}
	else
	{
		// explicit: round down to a power of two, then clamp by the render scale (>1.0: 4, >=1.5: 2)
		n = 1;
		while (n * 2 <= in->want && n < 64)
		{
			n *= 2;
		}
		if (n < 2)
		{
			n = 0;
		}
	}

	// the render-scale clamp, for Auto and explicit alike (coordinator 2026-09-27: Auto gets the same clamp -
	// 8x above scale 1.0 costs what 8x at the scaled size costs, so Auto may not pick more than an explicit 8x
	// would get). Auto is already 0 from 1.5, so the 1.5 row only ever touches an explicit value.
	if (in->scale >= 1.5f && n > 2)
	{
		n = 2;
		out->clamp = "scale>=1.5:max2";
	}
	else if (in->scale > 1.0f && n > 4)
	{
		n = 4;
		out->clamp = "scale>1.0:max4";
	}

	if (n >= 2 && n > in->caps)
	{
		while (n > in->caps && n >= 2)
		{
			n >>= 1;
		}
		if (n < 2)
		{
			n = 0;
		}
		out->capped = 1;
	}
	out->n = n;
}

#endif // TR_MSAA_DECIDE_H
