/*
===========================================================================
Copyright (C) 2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifndef TR_POSTPROCESS_H
#define TR_POSTPROCESS_H

#include "tr_fbo.h"

void RB_ToneMap(FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure);
void RB_GlobalFog(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox);
void RB_HZMSsao(FBO_t *srcFbo, ivec4_t srcBox);    // HZM gl1-parity SSAO GENERATION (r_ppSSAO); head of the post chain, before the AO composite
void RB_HZMBloom(FBO_t *srcFbo, ivec4_t srcBox);   // HZM gl1-parity bloom (r_ppBloom), runs before the tone stage
void RB_HZMDof(FBO_t *srcFbo, ivec4_t srcBox);     // HZM gl1-parity depth of field (r_ppDoF), runs before bloom
void RB_HZMScreenFx(FBO_t *srcFbo, ivec4_t srcBox); // HZM gl1-parity FXAA + sharpen + rain-on-lens, after the tone stage
void RB_HZMExtraFx(FBO_t *srcFbo, ivec4_t srcBox);  // HZM NEW: underwater, frost, chromatic aberration, film grain - after RB_HZMScreenFx
void RB_BokehBlur(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur);
void RB_SunRays(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox);
void RB_GaussianBlur(FBO_t *srcFbo, FBO_t *dstFbo, float blur);

#endif
