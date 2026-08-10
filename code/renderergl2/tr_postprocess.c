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

#include "tr_local.h"

void RB_ToneMap(FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure)
{
	ivec4_t srcBox, dstBox;
	vec4_t color;
	static int lastFrameCount = 0;

	if (autoExposure)
	{
		if (lastFrameCount == 0 || tr.frameCount < lastFrameCount || tr.frameCount - lastFrameCount > 5)
		{
			// determine average log luminance
			FBO_t *srcFbo, *dstFbo, *tmp;
			int size = 256;

			lastFrameCount = tr.frameCount;

			VectorSet4(dstBox, 0, 0, size, size);

			FBO_Blit(hdrFbo, hdrBox, NULL, tr.textureScratchFbo[0], dstBox, &tr.calclevels4xShader[0], NULL, 0);

			srcFbo = tr.textureScratchFbo[0];
			dstFbo = tr.textureScratchFbo[1];

			// downscale to 1x1 texture
			while (size > 1)
			{
				VectorSet4(srcBox, 0, 0, size, size);
				//size >>= 2;
				size >>= 1;
				VectorSet4(dstBox, 0, 0, size, size);

				if (size == 1)
					dstFbo = tr.targetLevelsFbo;

				//FBO_Blit(targetFbo, srcBox, NULL, tr.textureScratchFbo[nextScratch], dstBox, &tr.calclevels4xShader[1], NULL, 0);
				FBO_FastBlit(srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);

				tmp = srcFbo;
				srcFbo = dstFbo;
				dstFbo = tmp;
			}
		}

		// blend with old log luminance for gradual change
		VectorSet4(srcBox, 0, 0, 0, 0);

		color[0] = 
		color[1] =
		color[2] = 1.0f;
		if (glRefConfig.textureFloat)
			color[3] = 0.03f;
		else
			color[3] = 0.1f;

		FBO_Blit(tr.targetLevelsFbo, srcBox, NULL, tr.calcLevelsFbo, NULL,  NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}

	// tonemap
	color[0] =
	color[1] =
	color[2] = pow(2, r_cameraExposure->value - autoExposure); //exp2(r_cameraExposure->value);
	color[3] = 1.0f;

	// HZM gl2 PARITY GRADE (r_tonemapMode 1): run gl1's exact ACES grade instead of rend2's
	// Hable, driven by the SAME r_pp* cvars gl1 uses, so gl2 reproduces the OG on-screen look.
	// gl1's exposure term replaces r_cameraExposure, so u_Color carries the grade, not a gain.
	{
		static cvar_t *r_tonemapMode = NULL;
		static cvar_t *r_ppExposure = NULL, *r_ppContrast = NULL, *r_ppSaturation = NULL, *r_ppTemp = NULL;
		static cvar_t *r_ppTonemap = NULL, *r_ppGrade = NULL;

		if (!r_tonemapMode) {
			r_tonemapMode  = ri.Cvar_Get("r_tonemapMode",  "0",        CVAR_ARCHIVE);
			r_ppExposure   = ri.Cvar_Get("r_ppExposure",   "0.889971", CVAR_ARCHIVE);
			r_ppContrast   = ri.Cvar_Get("r_ppContrast",   "0.951289", CVAR_ARCHIVE);
			r_ppSaturation = ri.Cvar_Get("r_ppSaturation", "1.031519", CVAR_ARCHIVE);
			r_ppTemp       = ri.Cvar_Get("r_ppTemp",       "0",        CVAR_ARCHIVE);
			// HZM fix: the postfx menu's Tonemap/Grade checkbox and Grade 0-4 preset slider
			// (r_ppTonemap/r_ppGrade) were never read anywhere in gl2 - only the console-only,
			// undocumented-in-any-menu r_tonemapMode==1 gated this path, so the menu controls
			// were silent no-ops. gl1's gate is `r_ppTonemap->integer || r_ppGrade->integer`
			// (tr_postprocess_gl1.c:750-751) - mirror it here instead of requiring r_tonemapMode.
			r_ppTonemap    = ri.Cvar_Get("r_ppTonemap",    "0",        CVAR_ARCHIVE);
			r_ppGrade      = ri.Cvar_Get("r_ppGrade",      "0",        CVAR_ARCHIVE);
		}

		if (r_tonemapMode->integer == 1 || r_ppTonemap->integer || r_ppGrade->integer) {
			vec4_t grade;
			float  expo = r_ppExposure->value;
			float  cont = r_ppContrast->value;
			float  sat  = r_ppSaturation->value;
			float  temp = r_ppTemp->value;

			// Color-grade presets override the manual sliders (war-film looks), matching gl1
			// tr_postprocess_gl1.c:1082-1089 exactly. 0 = use the manual cvars above.
			switch (r_ppGrade->integer) {
			case 1: expo = 1.0f;  cont = 1.05f; sat = 1.0f;  temp =  0.00f; break; // Neutral
			case 2: expo = 1.05f; cont = 1.10f; sat = 1.05f; temp =  0.10f; break; // Warm "Normandy"
			case 3: expo = 0.95f; cont = 1.10f; sat = 0.85f; temp = -0.10f; break; // Cold "Ardennes"
			case 4: expo = 1.0f;  cont = 1.35f; sat = 0.55f; temp = -0.02f; break; // Bleach bypass
			default: break;
			}

			grade[0] = expo;
			grade[1] = cont;
			grade[2] = sat;
			grade[3] = temp;

			FBO_Blit(hdrFbo, hdrBox, NULL, ldrFbo, ldrBox, &tr.tonemapHzmShader, grade, 0);
			return;
		}
	}

	if (autoExposure)
		GL_BindToTMU(tr.calcLevelsImage,  TB_LEVELSMAP);
	else
		GL_BindToTMU(tr.fixedLevelsImage, TB_LEVELSMAP);

	FBO_Blit(hdrFbo, hdrBox, NULL, ldrFbo, ldrBox, &tr.tonemapShader, color, 0);
}

/*
=============
RB_GlobalFog

HZM gl2 re-port: MOHAA GLOBAL farplane distance fog as a screen-space depth pass. gl2's GLSL
pipeline only fogs Q3 fog-BRUSH volumes; the global atmospheric distance fog (viewParms.farplane_*)
was never applied. Mixes the scene toward farplane_color by linear eye-distance, mirroring gl1's
fixed-function GL_FOG_LINEAR (START = farplane_bias, END = farplane_distance). src and dst must differ.

v2 (fog-parity pass). Three changes from the first port, all of them "gl2's fog is thinner
than gl1's" candidates:

 1. The eye distance is reconstructed from the PROJECTION MATRIX ACTUALLY USED to rasterise
    the world (latched in RB_SetupGlobalFog), not from r_znear + backEnd.viewParms.zFar. The
    old form assumed the projection's near plane is r_znear (further clamped to >= 1) and its
    far plane is the zFar carried by the RC_POSTPROCESS command's copy of tr.viewParms. Any
    mismatch scales the whole distance curve, and the fog fraction with it.
 2. Everything is driven off rb_globalFog, latched from the MAIN world view, so a portal /
    sky-portal / shadow sub-view can never supply the parameters.
 3. dstFbo must not have tr.renderDepthImage attached - see tr.globalFogFbo in tr_fbo.c.
    Sampling a depth image that is attached to the bound draw framebuffer is a rendering
    feedback loop and the fetch is undefined.
=============
*/
void RB_GlobalFog(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t viewInfo, fogColor, fogDist, fogDepth;
	float  ooRange;

	if (!rb_globalFog.active)
		return;

	ooRange = 1.0f / (rb_globalFog.end - rb_globalFog.start);

	// (projMat[10], projMat[14], fog-the-sky, debug mode)
	VectorSet4(viewInfo,
		rb_globalFog.projMat10,
		rb_globalFog.projMat14,
		(r_globalFogSky && !r_globalFogSky->integer) ? 0.0f : 1.0f,
		r_globalFogDebug ? (float)r_globalFogDebug->integer : 0.0f);

	VectorSet4(fogColor, rb_globalFog.color[0], rb_globalFog.color[1], rb_globalFog.color[2], 1.0f);

	// (start, end, 1/(end-start), overall fraction scale)
	VectorSet4(fogDist, rb_globalFog.start, rb_globalFog.end, ooRange,
		r_globalFogScale ? r_globalFogScale->value : 1.0f);

	// (1/projMat[0], 1/projMat[5], radial-distance toggle, unused)
	VectorSet4(fogDepth,
		(rb_globalFog.projMat0 != 0.0f) ? (1.0f / rb_globalFog.projMat0) : 0.0f,
		(rb_globalFog.projMat5 != 0.0f) ? (1.0f / rb_globalFog.projMat5) : 0.0f,
		(r_globalFogRadial && r_globalFogRadial->integer) ? 1.0f : 0.0f,
		0.0f);

	if (r_globalFogDebug && r_globalFogDebug->integer == 1)
	{
		static int nextLog = 0;
		int now = ri.Milliseconds();

		if (now >= nextLog)
		{
			float centerDepth = -1.0f, centerDist = -1.0f;
			FBO_t *oldFbo = glState.currentFBO;

			// read the raw window depth at the centre of the view so the log shows the
			// value the shader is actually reconstructing from
			FBO_Bind(srcFbo);
			qglReadPixels(srcBox[0] + srcBox[2] / 2, srcBox[1] + srcBox[3] / 2, 1, 1,
				GL_DEPTH_COMPONENT, GL_FLOAT, &centerDepth);
			FBO_Bind(oldFbo);

			{
				float denom = rb_globalFog.projMat10 + (2.0f * centerDepth - 1.0f);
				if (denom > -1e-6f)
					denom = -1e-6f;
				centerDist = rb_globalFog.projMat14 / denom;
			}

			ri.Printf(PRINT_ALL,
				"^~^~^ GLOBALFOG start=%.1f end=%.1f color=%.3f %.3f %.3f "
				"projZNear=%.3f projZFar=%.1f vpZFar=%.1f r_znear=%.2f "
				"centerZw=%.6f centerDist=%.1f centerFrac=%.3f identityLight=%.3f\n",
				rb_globalFog.start, rb_globalFog.end,
				rb_globalFog.color[0], rb_globalFog.color[1], rb_globalFog.color[2],
				rb_globalFog.zNear, rb_globalFog.zFar,
				backEnd.viewParms.zFar, r_znear->value,
				centerDepth, centerDist,
				(centerDist - rb_globalFog.start) * ooRange,
				tr.identityLight);

			nextLog = now + 1000;
		}
	}

	// scene colour arrives on TB_COLORMAP via FBO_Blit(src); depth goes on TB_LEVELSMAP.
	// GLSL_SetUniform* upload straight to the program object (qglProgramUniform*EXT), so
	// setting them before FBO_Blit's internal bind is fine.
	GL_BindToTMU(tr.renderDepthImage, TB_LEVELSMAP);
	GLSL_SetUniformVec4(&tr.globalFogShader, UNIFORM_VIEWINFO,     viewInfo);
	GLSL_SetUniformVec4(&tr.globalFogShader, UNIFORM_FOGCOLORMASK, fogColor);
	GLSL_SetUniformVec4(&tr.globalFogShader, UNIFORM_FOGDISTANCE,  fogDist);
	GLSL_SetUniformVec4(&tr.globalFogShader, UNIFORM_FOGDEPTH,     fogDepth);

	FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, &tr.globalFogShader, fogColor, 0);
}

/*
=============
RB_BokehBlur


Blurs a part of one framebuffer to another.

Framebuffers can be identical. 
=============
*/
void RB_BokehBlur(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur)
{
//	ivec4_t srcBox, dstBox;
	vec4_t color;
	
	blur *= 10.0f;

	if (blur < 0.004f)
		return;

	if (glRefConfig.framebufferObject)
	{
		// bokeh blur
		if (blur > 0.0f)
		{
			ivec4_t quarterBox;

			quarterBox[0] = 0;
			quarterBox[1] = tr.quarterFbo[0]->height;
			quarterBox[2] = tr.quarterFbo[0]->width;
			quarterBox[3] = -tr.quarterFbo[0]->height;

			// create a quarter texture
			//FBO_Blit(NULL, NULL, NULL, tr.quarterFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(src, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}

#ifndef HQ_BLUR
		if (blur > 1.0f)
		{
			// create a 1/16th texture
			//FBO_Blit(tr.quarterFbo[0], NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
#endif

		if (blur > 0.0f && blur <= 1.0f)
		{
			// Crossfade original with quarter texture
			VectorSet4(color, 1, 1, 1, blur);

			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
#ifndef HQ_BLUR
		// ok blur, but can see some pixelization
		else if (blur > 1.0f && blur <= 2.0f)
		{
			// crossfade quarter texture with 1/16th texture
			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, NULL, 0);

			VectorSet4(color, 1, 1, 1, blur - 1.0f);

			FBO_Blit(tr.textureScratchFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
		else if (blur > 2.0f)
		{
			// blur 1/16th texture then replace
			int i;

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = ((blur - 2.0f) / 2.0f) / 3.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 0.5f;
				color[3] = 1.0f;

				if (i != 0)
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
				else
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, 0);
			}

			FBO_Blit(tr.textureScratchFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#else // higher quality blur, but slower
		else if (blur > 1.0f)
		{
			// blur quarter texture then replace
			int i;

			src = tr.quarterFbo[0];
			dst = tr.quarterFbo[1];

			VectorSet4(color, 0.5f, 0.5f, 0.5f, 1);

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = (blur - 1.0f) / 2.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 1.0f;
				if (i != 0)
					color[3] = 1.0f;
				else
					color[3] = 0.5f;

				FBO_Blit(tr.quarterFbo[0], NULL, blurTexScale, tr.quarterFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			}

			FBO_Blit(tr.quarterFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#endif
	}
}


static void RB_RadialBlur(FBO_t *srcFbo, FBO_t *dstFbo, int passes, float stretch, float x, float y, float w, float h, float xcenter, float ycenter, float alpha)
{
	ivec4_t srcBox, dstBox;
	int srcWidth, srcHeight;
	vec4_t color;
	const float inc = 1.f / passes;
	const float mul = powf(stretch, inc);
	float scale;

	alpha *= inc;
	VectorSet4(color, alpha, alpha, alpha, 1.0f);

	srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
	srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

	VectorSet4(srcBox, 0, 0, srcWidth, srcHeight);

	VectorSet4(dstBox, x, y, w, h);
	FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

	--passes;
	scale = mul;
	while (passes > 0)
	{
		float iscale = 1.f / scale;
		float s0 = xcenter * (1.f - iscale);
		float t0 = (1.0f - ycenter) * (1.f - iscale);

		srcBox[0] = s0 * srcWidth;
		srcBox[1] = t0 * srcHeight;
		srcBox[2] = iscale * srcWidth;
		srcBox[3] = iscale * srcHeight;
			
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		scale *= mul;
		--passes;
	}
}


static qboolean RB_UpdateSunFlareVis(void)
{
	GLuint sampleCount = 0;
	if (!glRefConfig.occlusionQuery)
		return qtrue;

	tr.sunFlareQueryIndex ^= 1;
	if (!tr.sunFlareQueryActive[tr.sunFlareQueryIndex])
		return qtrue;

	/* debug code */
	if (0)
	{
		int iter;
		for (iter=0 ; ; ++iter)
		{
			GLint available = 0;
			qglGetQueryObjectiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
				break;
		}

		ri.Printf(PRINT_DEVELOPER, "Waited %d iterations\n", iter);
	}
	
	// Note: On desktop OpenGL this is a sample count (glRefConfig.occlusionQueryTarget == GL_SAMPLES_PASSED)
	// but on OpenGL ES this is a boolean (glRefConfig.occlusionQueryTarget == GL_ANY_SAMPLES_PASSED)
	qglGetQueryObjectuiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT, &sampleCount);
	return sampleCount > 0;
}

void RB_SunRays(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;
	float sunRayScale = 1.0f;

//	float w, h, w2, h2;
	mat4_t mvp;
	vec4_t pos, hpos;

	dot = DotProduct(tr.sunDirection, backEnd.viewParms.or.axis[0]);
	if (dot < cutoff)
		return;

	// HZM fix: many MOHAA night maps still carry a worldspawn sundirection/suncolor purely to
	// tint ambient/sphere lighting toward moonlight (e.g. m1l1 ships suncolor "10 15 30" - a
	// near-black value, nothing like an actual visible sun). The sun bridge (bug-1154) correctly
	// treats that as "a sun exists" for lighting purposes, but this ray effect only ever checked
	// VIEW DIRECTION, never the light's actual brightness, so a near-black "sun" still drew full-
	// intensity god ray shafts across a starry night sky. Scale ray strength by how bright the
	// sun light actually is instead of an on/off gate, so real dusk/dawn sun keeps a soft falloff
	// rather than popping, and drop the pass entirely once it would be invisible anyway.
	{
		float sunLum = 0.299f * tr.sunLight[0] + 0.587f * tr.sunLight[1] + 0.114f * tr.sunLight[2];
		// m1l1's night-map moonlight tint (suncolor "10 15 30") luminances to ~15 - well under a
		// real dawn/dusk/daylight sun (raw worldspawn suncolor values are typically 150-255-ish
		// on at least one channel). 48 draws the line comfortably above moonlight-tier data.
		if (sunLum < 48.0f)
			return;
		sunRayScale = sunLum / 128.0f;
		if (sunRayScale > 1.0f)
			sunRayScale = 1.0f;
	}

	if (!RB_UpdateSunFlareVis())
		return;

	// From RB_DrawSun()
	{
		float dist;
		mat4_t trans, model;

		Mat4Translation( backEnd.viewParms.or.origin, trans );
		Mat4Multiply( backEnd.viewParms.world.modelMatrix, trans, model );
		Mat4Multiply(backEnd.viewParms.projectionMatrix, model, mvp);

		dist = backEnd.viewParms.zFar / 1.75;		// div sqrt(3)

		VectorScale( tr.sunDirection, dist, pos );
	}

	// project sun point
	//Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, mvp);
	Mat4Transform(mvp, pos, hpos);

	// transform to UV coords
	hpos[3] = 0.5f / hpos[3];

	pos[0] = 0.5f + hpos[0] * hpos[3];
	pos[1] = 0.5f + hpos[1] * hpos[3];

	// initialize quarter buffers
	{
		float mul = sunRayScale;
		ivec4_t rayBox, quarterBox;
		int srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
		int srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

		VectorSet4(color, mul, mul, mul, 1);

		rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / srcHeight;
		rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / srcHeight;

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		// first, downsample the framebuffer
		if (colorize)
		{
			FBO_FastBlit(srcFbo, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			FBO_Blit(tr.sunRaysFbo, rayBox, NULL, tr.quarterFbo[0], quarterBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
		}
		else
		{
			FBO_FastBlit(tr.sunRaysFbo, rayBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i=0; i<2; ++i)
		{
			RB_RadialBlur(tr.quarterFbo[i&1], tr.quarterFbo[(~i) & 1], 5, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f);
			stretch += stretch_add;
		}
	}
	
	// add result back on top of the main buffer
	{
		float mul = 1.f;

		VectorSet4(color, mul, mul, mul, 1);

		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_BlurAxis(FBO_t *srcFbo, FBO_t *dstFbo, float strength, qboolean horizontal)
{
	float dx, dy;
	float xmul, ymul;
	float weights[3] = {
		0.227027027f,
		0.316216216f,
		0.070270270f,
	};
	float offsets[3] = {
		0.f,
		1.3846153846f,
		3.2307692308f,
	};

	xmul = horizontal;
	ymul = 1.f - xmul;

	xmul *= strength;
	ymul *= strength;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, weights[0], weights[0], weights[0], 1.0f);
		VectorSet4(srcBox, 0, 0, srcFbo->width, srcFbo->height);
		VectorSet4(dstBox, 0, 0, dstFbo->width, dstFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

		VectorSet4(color, weights[1], weights[1], weights[1], 1.0f);
		dx = offsets[1] * xmul;
		dy = offsets[1] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

		VectorSet4(color, weights[2], weights[2], weights[2], 1.0f);
		dx = offsets[2] * xmul;
		dy = offsets[2] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_HBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qtrue);
}

static void RB_VBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qfalse);
}

void RB_GaussianBlur(FBO_t *srcFbo, FBO_t *dstFbo, float blur)
{
	//float mul = 1.f;
	float factor = Com_Clamp(0.f, 1.f, blur);

	if (factor <= 0.f)
		return;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, 1, 1, 1, 1);

		// first, downsample the framebuffer
		FBO_FastBlit(srcFbo, NULL, tr.quarterFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);

		// set the alpha channel
		qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		FBO_BlitFromTexture(tr.whiteImage, NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, color, GLS_DEPTHTEST_DISABLE);
		qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// blur the tiny buffer horizontally and vertically
		RB_HBlur(tr.textureScratchFbo[0], tr.textureScratchFbo[1], factor);
		RB_VBlur(tr.textureScratchFbo[1], tr.textureScratchFbo[0], factor);

		// finally, merge back to framebuffer
		VectorSet4(srcBox, 0, 0, tr.textureScratchFbo[0]->width, tr.textureScratchFbo[0]->height);
		VectorSet4(dstBox, 0, 0, glConfig.vidWidth,              glConfig.vidHeight);
		color[3] = factor;
		FBO_Blit(tr.textureScratchFbo[0], srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
}

/*
=============
RB_HZMBloom

HZM gl2 POST-FX PORT (bug-1149): renderergl1's bloom, driven by the SAME Advanced-Graphics levers
(r_ppBloom / r_ppBloomThreshold / r_ppBloomIntensity) so the menu and the player's saved config
control both renderers identically and cannot drift apart.

gl1's chain (tr_postprocess_gl1.c RB_PostFxApply) is: bright-pass the scene into a half-res target,
blur it horizontally then vertically with a 9-tap Gaussian, then composite it back over the scene
additively. This is a straight port of that, using gl2's existing FBO_Blit plumbing:

  * tr.quarterFbo[0..1] are already half-res RGBA8 colour targets - gl1 sizes its bloom buffers at
    width/2 x height/2, so they match exactly.
  * the blur direction rides UNIFORM_INVTEXRES: FBO_Blit divides its inSrcTexScale argument by the
    source image's dimensions, so (1,0) becomes (1/srcW, 0) and (0,1) becomes (0, 1/srcH) -
    identical to gl1's u_dir, with no new uniform.
  * the additive composite needs no shader of its own: FBO_Blit with a NULL program uses
    tr.textureColorShader (texture * u_Color), which IS gl1's ADD_FS.

Placement: gl1 applies bloom BEFORE its tonemap/grade, so the caller runs this immediately ahead of
the tone stage. src and dst are the same FBO - the scene is read, and the blurred highlights are
blended back onto it.
=============
*/
void RB_HZMBloom(FBO_t *srcFbo, ivec4_t srcBox)
{
	static cvar_t *r_ppBloom = NULL, *r_ppBloomThreshold = NULL, *r_ppBloomIntensity = NULL;
	ivec4_t quarterBox;
	vec2_t  dir;
	vec4_t  color;

	if (!r_ppBloom) {
		// same names and same defaults as renderergl1 tr_init.c, so one set of levers drives both
		r_ppBloom          = ri.Cvar_Get("r_ppBloom",          "1",   CVAR_ARCHIVE);
		r_ppBloomThreshold = ri.Cvar_Get("r_ppBloomThreshold", "0.6", CVAR_ARCHIVE);
		r_ppBloomIntensity = ri.Cvar_Get("r_ppBloomIntensity", "1.3", CVAR_ARCHIVE);
	}

	if (!r_ppBloom->integer)
		return;

	if (!srcFbo || !tr.quarterFbo[0] || !tr.quarterFbo[1])
		return;

	VectorSet4(quarterBox, 0, 0, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height);

	// 1) bright-pass the scene into the half-res target (threshold rides u_Color.x)
	VectorSet4(color, r_ppBloomThreshold->value, 0.0f, 0.0f, 1.0f);
	FBO_Blit(srcFbo, srcBox, NULL, tr.quarterFbo[0], quarterBox, &tr.bloomBrightShader, color, 0);

	// 2) horizontal then vertical 9-tap Gaussian, ping-ponging between the two half-res targets
	VectorSet2(dir, 1.0f, 0.0f);
	FBO_Blit(tr.quarterFbo[0], quarterBox, dir, tr.quarterFbo[1], quarterBox, &tr.bloomBlurShader, NULL, 0);

	VectorSet2(dir, 0.0f, 1.0f);
	FBO_Blit(tr.quarterFbo[1], quarterBox, dir, tr.quarterFbo[0], quarterBox, &tr.bloomBlurShader, NULL, 0);

	// 3) additive composite back over the scene at full res (intensity rides u_Color)
	color[0] =
	color[1] =
	color[2] = r_ppBloomIntensity->value;
	color[3] = 1.0f;
	FBO_Blit(tr.quarterFbo[0], quarterBox, NULL, srcFbo, srcBox, NULL, color,
		GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
}

/*
=============
RB_HZMScreenFx

HZM gl2 POST-FX PORT (bug-1150): the screen-space tail of renderergl1's chain - FXAA, then the
contrast-adaptive sharpen, then rain-on-lens - in gl1's order, after the tone/grade stage, driven
by the same Advanced-Graphics levers so one set of sliders controls both renderers.

Each stage is a full-screen pass that must not read and write the same attachment (a rendering
feedback loop is undefined), so each one blits THROUGH tr.screenScratchFbo and back - exactly the
round-trip RB_ToneMap already uses.

Rain wetness comes from r_ppRainWet, which the CGAME publishes per frame (0..1 = rain intensity AND
under open sky, eased so beads build and dry). The cgame is renderer-agnostic, so that signal is
already there when gl2 is active - it was simply never consumed, which is why the lens stayed dry
on gl2 while gl1 showed beads.
=============
*/
/*
=============
RB_HZMDizzyAmount

HZM (bug-1233) - the 0..1 "how badly hurt are you" factor, shared by the dizzy double-vision pass
and the injury chromatic aberration. Factored out deliberately: both consumers live in DIFFERENT
post-process functions, so a cached value would depend on which one ran first. This recomputes from
the two cvars cgame publishes, so it is correct regardless of call order and costs two cvar reads.
Returns 0 when the feature is off or the player is healthy.
=============
*/
static float RB_HZMDizzyAmount(void)
{
	static cvar_t *pOn = NULL, *pAmt = NULL, *pStart = NULL, *pFrac = NULL, *pDbno = NULL;
	float dz, frac, start;

	if (!pOn) {
		pOn    = ri.Cvar_Get("r_ppDizzy",       "1",   CVAR_ARCHIVE);
		pAmt   = ri.Cvar_Get("r_ppDizzyAmount", "1",   CVAR_ARCHIVE);
		pStart = ri.Cvar_Get("r_ppDizzyStart",  "0.8", CVAR_ARCHIVE);
		pFrac  = ri.Cvar_Get("r_ppHealthFrac",  "1",   0);   // cgame-published
		pDbno  = ri.Cvar_Get("coop_dbnoView",   "0",   0);   // cgame-published
	}

	if (!pOn->integer) {
		return 0.0f;
	}

	dz    = 0.0f;
	frac  = pFrac->value;
	start = pStart->value;
	if (start < 0.05f) start = 0.05f;

	if (frac < start) {
		dz = (start - frac) / start;
	}
	if (pDbno->integer && dz < 0.85f) {
		dz = 0.85f;
	}

	dz *= pAmt->value;
	if (dz < 0.0f) dz = 0.0f;
	if (dz > 1.0f) dz = 1.0f;
	return dz;
}

void RB_HZMScreenFx(FBO_t *srcFbo, ivec4_t srcBox)
{
	static cvar_t *r_ppFXAA = NULL, *r_ppSharpen = NULL, *r_ppSharpenAmount = NULL;
	static cvar_t *r_ppRainDrops = NULL, *r_ppRainWet = NULL, *r_ppRainAmount = NULL;
	static cvar_t *r_ppMotionBlur = NULL, *r_ppMotionBlurAmount = NULL;
	vec2_t texScale;
	vec4_t color;

	if (!r_ppFXAA) {
		// same names and defaults as renderergl1 tr_init.c
		r_ppFXAA          = ri.Cvar_Get("r_ppFXAA",          "1",    CVAR_ARCHIVE);
		r_ppSharpen       = ri.Cvar_Get("r_ppSharpen",       "1",    CVAR_ARCHIVE);
		r_ppSharpenAmount = ri.Cvar_Get("r_ppSharpenAmount", "0.35", CVAR_ARCHIVE);
		r_ppRainDrops     = ri.Cvar_Get("r_ppRainDrops",     "1",    CVAR_ARCHIVE);
		r_ppRainAmount    = ri.Cvar_Get("r_ppRainAmount",    "1",    CVAR_ARCHIVE);
		r_ppRainWet       = ri.Cvar_Get("r_ppRainWet",       "0",    0);   // cgame-published, per frame
		// [user 07-29] camera motion blur. Plain CVAR_ARCHIVE, deliberately NOT CVAR_LATCH: this pass
		// allocates nothing at init, so there is no reason to demand a vid_restart - and vid_restart from
		// an open menu is a known crash in this renderer (bug-1181). Registered exactly ONCE: Cvar_Get
		// OR-combines flags across duplicate registrations, which is how r_lodscale silently became
		// cheat-protected (bug-1125).
		r_ppMotionBlur       = ri.Cvar_Get("r_ppMotionBlur",       "0",    CVAR_ARCHIVE);   // opt-in
		r_ppMotionBlurAmount = ri.Cvar_Get("r_ppMotionBlurAmount", "0.35", CVAR_ARCHIVE);
	}

	if (!srcFbo || !tr.screenScratchFbo)
		return;

	// one texel: FBO_Blit divides this by the source dimensions, giving gl1's u_rcpFrame / u_rcp
	VectorSet2(texScale, 1.0f, 1.0f);

	// HZM [user 07-29] CAMERA MOTION BLUR. Runs FIRST in this chain - before FXAA - on purpose: the
	// blur is a resampling operation and leaves faint tap stepping on high-contrast edges, which FXAA
	// then cleans up for free. Reversed, FXAA's work would simply be smeared away.
	//
	// The motion vector comes from REPROJECTION, not from differencing Euler angles: keep the previous
	// frame's forward vector, express it in the CURRENT frame's view basis, perspective-divide. That
	// gives exactly where the old view centre moved to on screen, and it is immune to Euler wraparound
	// - a yaw crossing 359->1 is a 2 degree turn, but subtracting the angles says 358 and would flash a
	// full-screen smear once per spin.
	if (r_ppMotionBlur->integer && r_ppMotionBlurAmount->value > 0.0001f)
	{
		static vec3_t   prevFwd   = { 0.0f, 0.0f, 0.0f };
		static int      prevTime  = 0;
		static qboolean prevValid = qfalse;
		vec4_t          mbColor;
		float           px, py, pz, sx, sy, tx, ty, mag;
		int             now = backEnd.refdef.time;

		// view-space coordinates of the PREVIOUS forward direction. viewaxis is [forward, left, up]
		// (Quake3 convention), so px is a LEFT-handed lateral term - the sign is irrelevant here because
		// the shader samples symmetrically about the fragment.
		pz = DotProduct(prevFwd, backEnd.refdef.viewaxis[0]);
		px = DotProduct(prevFwd, backEnd.refdef.viewaxis[1]);
		py = DotProduct(prevFwd, backEnd.refdef.viewaxis[2]);

		sx = sy = 0.0f;
		if (prevValid && pz > 0.01f && now > prevTime)
		{
			// project onto the near plane, then divide by the half-extent there so the result lands in
			// 0..1 UV units rather than NDC
			tx = tan(DEG2RAD(backEnd.refdef.fov_x) * 0.5f);
			ty = tan(DEG2RAD(backEnd.refdef.fov_y) * 0.5f);
			if (tx > 0.0001f && ty > 0.0001f)
			{
				sx = ((px / pz) / tx) * 0.5f;
				sy = ((py / pz) / ty) * 0.5f;
			}
		}

		// CUT REJECTION. Without it, the first frame of every map load, every teleport/warp and every
		// cutscene camera switch smears the whole screen once - which reads as a graphical fault, not as
		// motion. A real mouse turn cannot move the view centre a third of a screen in one frame, so
		// anything past the cap is a discontinuity rather than a turn.
		mag = sqrt(sx * sx + sy * sy);
		if (mag > 0.35f)
		{
			sx = sy = 0.0f;
		}

		if (sx != 0.0f || sy != 0.0f)
		{
			// FRAMERATE: sx/sy are PER-FRAME deltas, so they already scale with frame time - which is
			// physically what a fixed shutter angle does, since a longer frame exposes longer. No further
			// normalisation is wanted; adding one would suppress the effect at low framerates, which is
			// exactly where it helps most.
			VectorSet4(mbColor, r_ppMotionBlurAmount->value, sx, sy, 0.0f);
			FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.motionBlurShader, mbColor, 0);
			FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		VectorCopy(backEnd.refdef.viewaxis[0], prevFwd);
		prevTime  = now;
		prevValid = qtrue;
	}

	if (r_ppFXAA->integer)
	{
		FBO_Blit(srcFbo, srcBox, texScale, tr.screenScratchFbo, srcBox, &tr.fxaaShader, NULL, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	if (r_ppSharpen->integer)
	{
		VectorSet4(color, r_ppSharpenAmount->value, 0.0f, 0.0f, 1.0f);
		FBO_Blit(srcFbo, srcBox, texScale, tr.screenScratchFbo, srcBox, &tr.sharpenShader, color, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	// ---- heat haze + localized muzzle shimmer (gl1 order: after sharpen, before rain)
	{
		static cvar_t *r_ppHeatHaze = NULL, *r_ppHeat = NULL, *r_ppHeatAmount = NULL;
		static cvar_t *r_ppMuzzleHeat = NULL, *r_ppMuzzleX = NULL, *r_ppMuzzleY = NULL, *r_ppMuzzleRadius = NULL;
		float heat, muzHeat, muzR;

		if (!r_ppHeatHaze) {
			r_ppHeatHaze     = ri.Cvar_Get("r_ppHeatHaze",     "1",    CVAR_ARCHIVE);
			r_ppHeat         = ri.Cvar_Get("r_ppHeat",         "0",    0);   // cgame-published, per frame
			r_ppHeatAmount   = ri.Cvar_Get("r_ppHeatAmount",   "1",    CVAR_ARCHIVE);
			r_ppMuzzleHeat   = ri.Cvar_Get("r_ppMuzzleHeat",   "0",    CVAR_ARCHIVE);
			r_ppMuzzleX      = ri.Cvar_Get("r_ppMuzzleX",      "0.5",  CVAR_ARCHIVE);
			r_ppMuzzleY      = ri.Cvar_Get("r_ppMuzzleY",      "0.6",  CVAR_ARCHIVE);
			r_ppMuzzleRadius = ri.Cvar_Get("r_ppMuzzleRadius", "0.28", CVAR_ARCHIVE);
		}

		if (r_ppHeatHaze->integer)
		{
			heat = r_ppHeat->value * r_ppHeatAmount->value;
			if (heat < 0.0f) heat = 0.0f;
			if (heat > 1.0f) heat = 1.0f;

			muzHeat = r_ppMuzzleHeat->value * r_ppHeatAmount->value;
			if (muzHeat < 0.0f) muzHeat = 0.0f;
			if (muzHeat > 1.0f) muzHeat = 1.0f;

			muzR = r_ppMuzzleRadius->value;
			if (muzR < 0.02f) muzR = 0.02f;

			if (heat > 0.001f || muzHeat > 0.001f)
			{
				vec4_t muz;

				VectorSet4(muz, r_ppMuzzleX->value, r_ppMuzzleY->value, 0.0f, 0.0f);
				FBO_SetHzmParams(muz);

				color[0] = heat;
				color[1] = backEnd.refdef.floatTime;
				color[2] = muzHeat;
				color[3] = muzR;

				FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.heatHazeShader, color, 0);
				FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				FBO_SetHzmParams(NULL);
			}
		}
	}

	if (r_ppRainDrops->integer)
	{
		float wet = r_ppRainWet->value * r_ppRainAmount->value;

		if (wet < 0.0f) wet = 0.0f;
		if (wet > 1.0f) wet = 1.0f;

		// gl1 skips the pass entirely below this, so the lens is untouched when it is not raining
		if (wet > 0.003f)
		{
			float h = (srcBox && srcBox[3]) ? (float)srcBox[3] : (float)glConfig.vidHeight;
			float w = (srcBox && srcBox[2]) ? (float)srcBox[2] : (float)glConfig.vidWidth;

			color[0] = wet;
			color[1] = backEnd.refdef.floatTime;      // seconds; gl1 feeds the same clock
			color[2] = (h > 0.0f) ? (w / h) : 1.0f;   // aspect
			color[3] = 1.0f;

			FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.rainDropsShader, color, 0);
			FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
	}

	// ---- low health: desaturate + luminance-proportional red vignette, with a heartbeat pulse.
	// The ramp, the 0.35 onset floor at the threshold and the beat rate are gl1's exactly
	// (renderergl1 tr_postprocess_gl1.c), so the effect reads identically on both renderers.
	{
		static cvar_t *r_ppLowHealth = NULL, *r_ppHealthFrac = NULL, *r_ppLowHealthStart = NULL;
		static cvar_t *r_ppLowHealthAmount = NULL, *r_ppLowHealthBeat = NULL;

		if (!r_ppLowHealth) {
			r_ppLowHealth       = ri.Cvar_Get("r_ppLowHealth",       "1",    CVAR_ARCHIVE);
			r_ppHealthFrac      = ri.Cvar_Get("r_ppHealthFrac",      "1",    0);   // cgame-published
			r_ppLowHealthStart  = ri.Cvar_Get("r_ppLowHealthStart",  "0.5",  CVAR_ARCHIVE);
			r_ppLowHealthAmount = ri.Cvar_Get("r_ppLowHealthAmount", "1",    CVAR_ARCHIVE);
			r_ppLowHealthBeat   = ri.Cvar_Get("r_ppLowHealthBeat",   "0.25", CVAR_ARCHIVE);
		}

		if (r_ppLowHealth->integer)
		{
			float frac  = r_ppHealthFrac->value;
			float start = r_ppLowHealthStart->value;
			float amt   = r_ppLowHealthAmount->value;

			if (start < 0.05f) start = 0.05f;

			if (frac < start)
			{
				float ramp = (start - frac) / start;
				float hurt, depth, beatRate;

				if (ramp < 0.0f) ramp = 0.0f;
				if (ramp > 1.0f) ramp = 1.0f;

				hurt = (0.35f + 0.65f * ramp) * amt;
				if (hurt > 1.0f) hurt = 1.0f;

				depth = r_ppLowHealthBeat->value;
				if (depth < 0.0f) depth = 0.0f;
				if (depth > 1.0f) depth = 1.0f;
				beatRate = 1.8f + hurt * 1.6f;
				hurt *= (1.0f - depth) + depth * (float)sin(backEnd.refdef.time * 0.001f * beatRate);

				if (hurt < 0.0f) hurt = 0.0f;
				if (hurt > 1.0f) hurt = 1.0f;

				if (hurt > 0.001f)
				{
					VectorSet4(color, hurt, 0.0f, 0.0f, 1.0f);
					FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.lowHealthShader, color, 0);
					FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				}
			}
		}
	}

	// ---- DIZZY / concussed double vision (HZM, bug-1203).
	// Origin: while chasing the stuck "ghost of my first person view" bug the user said
	// "its actually kinda cool and would be good for a dizzy effect if you get shot" - so this
	// is that, on purpose and under control. The accidental version was a portal-visibility
	// leak drawing a real entity twice (fixed in tr_main.c); this one is a deliberate screen
	// pass, so it is bounded, decays with health, and always clears.
	//
	// Look: one semi-transparent copy of the frame, offset from centre, SWAYING on a slow
	// lissajous so it drifts rather than sitting still - a static duplicate reads as a render
	// bug, a moving one reads as concussion. Brightness is preserved by weighting base and
	// ghost to sum to 1, so the screen never washes out however strong the effect gets.
	//
	// Driven by BOTH signals cgame already publishes: r_ppHealthFrac (1 = full health) below
	// r_ppDizzyStart, and coop_dbnoView which pins it near maximum while downed but not out.
	{
		static cvar_t *r_ppDizzy = NULL, *r_ppDizzyAmount = NULL, *r_ppDizzyStart = NULL;
		static cvar_t *r_ppDizzyMaxPix = NULL, *r_ppDizzyRate = NULL;
		static cvar_t *r_ppHealthFracD = NULL, *r_ppDbnoView = NULL;

		if (!r_ppDizzy) {
			r_ppDizzy       = ri.Cvar_Get("r_ppDizzy",       "1",    CVAR_ARCHIVE);
			r_ppDizzyAmount = ri.Cvar_Get("r_ppDizzyAmount", "1",    CVAR_ARCHIVE);
			r_ppDizzyStart  = ri.Cvar_Get("r_ppDizzyStart",  "0.8",  CVAR_ARCHIVE);
			r_ppDizzyMaxPix = ri.Cvar_Get("r_ppDizzyMaxPix", "22",   CVAR_ARCHIVE);
			r_ppDizzyRate   = ri.Cvar_Get("r_ppDizzyRate",   "0.9",  CVAR_ARCHIVE);
			r_ppHealthFracD = ri.Cvar_Get("r_ppHealthFrac",  "1",    0);   // cgame-published
			r_ppDbnoView    = ri.Cvar_Get("coop_dbnoView",   "0",    0);   // cgame-published
		}

		if (r_ppDizzy->integer && tr.screenScratchFbo)
		{
			// shared with the injury chromatic aberration in RB_HZMExtraFx - see RB_HZMDizzyAmount
			float dz    = RB_HZMDizzyAmount();
			float frac  = r_ppHealthFracD->value;
			float start = r_ppDizzyStart->value;

			(void)frac; (void)start;

			if (dz > 0.01f)
			{
				float t     = backEnd.refdef.time * 0.001f * r_ppDizzyRate->value;
				float amp   = r_ppDizzyMaxPix->value * dz;
				// lissajous: x and y on different periods so the ghost traces a slow wander
				// instead of a straight line back and forth.
				float dx    = (float)sin(t)        * amp;
				float dy    = (float)cos(t * 0.7f) * amp * 0.5f;
				// ghost opacity tops out below half so the real frame always dominates
				float w     = 0.42f * dz;
				ivec4_t ghostBox;

				VectorSet4(ghostBox, srcBox[0] + (int)dx, srcBox[1] + (int)dy, srcBox[2], srcBox[3]);

				// base frame at (1 - w)...
				VectorSet4(color, 1.0f - w, 1.0f - w, 1.0f - w, 1.0f);
				FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, NULL, color, 0);
				// ...plus the offset ghost at w, additive. Weights sum to 1 -> no brightness shift.
				VectorSet4(color, w, w, w, 1.0f);
				FBO_Blit(srcFbo, ghostBox, NULL, tr.screenScratchFbo, srcBox, NULL, color,
				         GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

				FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
		}
	}

	// ---- suppression: desaturate + peripheral blur + tunnel vignette while pinned down
	{
		static cvar_t *r_ppSuppression = NULL, *r_ppSuppress = NULL, *r_ppSuppressAmount = NULL;

		if (!r_ppSuppression) {
			r_ppSuppression    = ri.Cvar_Get("r_ppSuppression",    "1", CVAR_ARCHIVE);
			r_ppSuppress       = ri.Cvar_Get("r_ppSuppress",       "0", 0);   // cgame-published, decaying
			r_ppSuppressAmount = ri.Cvar_Get("r_ppSuppressAmount", "1", CVAR_ARCHIVE);
		}

		if (r_ppSuppression->integer)
		{
			float sup = r_ppSuppress->value * r_ppSuppressAmount->value;

			if (sup < 0.0f) sup = 0.0f;
			if (sup > 1.0f) sup = 1.0f;

			if (sup > 0.001f)
			{
				VectorSet4(color, sup, 0.0f, 0.0f, 1.0f);
				FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.suppressionShader, color, 0);
				FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
		}

		// ---- HZM coop [user 08-02] ON-HIT BLOOD -------------------------------------------
		// Requested alongside the suppression work: "we need light blood effects on screen when
		// you get shot". Suppression is the sustained under-fire state; this is the sharp cue for
		// the instant a round lands on you. Deliberately LAST in the chain so the splats sit on
		// top of the suppression desaturate rather than being washed out by it.
		// r_ppHit is cgame-published and decays there (same contract as r_ppSuppress, which is
		// why it is registered with no CVAR_ARCHIVE - it is state, not a setting).
		{
			static cvar_t *r_ppHitBlood = NULL, *r_ppHit = NULL, *r_ppHitAmount = NULL;

			if (!r_ppHitBlood) {
				r_ppHitBlood  = ri.Cvar_Get("r_ppHitBlood",  "1", CVAR_ARCHIVE);
				r_ppHit       = ri.Cvar_Get("r_ppHit",       "0", 0);   // cgame-published, decaying
				r_ppHitAmount = ri.Cvar_Get("r_ppHitAmount", "1", CVAR_ARCHIVE);
			}

			if (r_ppHitBlood->integer)
			{
				float hit = r_ppHit->value * r_ppHitAmount->value;

				if (hit < 0.0f) hit = 0.0f;
				if (hit > 1.0f) hit = 1.0f;

				if (hit > 0.001f)
				{
					VectorSet4(color, hit, 0.0f, 0.0f, 1.0f);
					FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.hitBloodShader, color, 0);
					FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
				}
			}
		}
	}
}

/*
=============
RB_HZMSsao

HZM gl2 SSAO BLACK-SCREEN FIX (bug-1177 follow-up).

This pass used to live inside RB_DrawSurfs, nested in the block gated on
`r_depthPrepass->integer || isShadowView`, and additionally inside that block's `if (!isShadowView)`
arm. The AO COMPOSITE in RB_PostProcess, however, was gated only on the cvars plus NULL-ness of
the buffers. With r_depthPrepass 0 - which is what the gl2 sandbox archives - generation therefore
never ran for the main view while the composite still ran every frame.

That is fatal rather than merely inert, because tr.screenSsaoImage is created with pic = NULL:
R_CreateImage2 allocates storage with a NULL upload and only uploads `if (pic)`, so the texture is
driver zero-fill = pure BLACK. The composite is a multiply (GLS_SRCBLEND_DST_COLOR |
GLS_DSTBLEND_ZERO, i.e. dst_rgb = dst_rgb * src_rgb), so every pixel of the frame was multiplied by
0 and the presented image was uniformly black - with no crash and no GL error to point at it.
(ssao_fp.glsl clamps its result to a floor of 1 - intensity, so the shader mathematically cannot
output black: a black screen was always proof the pass had not run at all.)

The fix is the same one that already makes DoF work at r_depthPrepass 0: take our OWN depth
snapshot instead of relying on the prepass block to have made one. Every full-size FBO has
tr.renderDepthImage bound as its DEPTH attachment and the AO passes render into quarter-size
colour targets, so the snapshot is also what keeps this off a rendering feedback loop.

Placement: called from RB_PostProcess after the MSAA resolve (so tr.renderDepthImage holds
RESOLVED depth) and immediately before the AO composite, which is the head of the post chain -
generate then consume, in that order, within one frame. That matches gl1's SSAO -> DoF -> bloom
ordering.
=============
*/
void RB_HZMSsao(FBO_t *srcFbo, ivec4_t srcBox)
{
	vec4_t  quadVerts[4];
	vec2_t  texCoords[4];
	vec4_t  ssaoParams;
	vec4_t  viewInfo;
	vec4_t  srcTexCoords;
	FBO_t  *oldFbo;
	int     blurIdx;
	float   aoRad, aoInt, aoBias;

	// HZM gl2 (bug-1177): r_ppSSAO is CVAR_LATCH, so ->integer is the same value R_InitImages read
	// when it decided whether to allocate - gate and allocation cannot desync. The explicit NULL
	// tests are belt-and-braces: tr is Com_Memset to 0 in R_Init, so an unallocated resource is
	// reliably NULL rather than garbage.
	if (!(r_ssao->integer || (r_ppSSAO && r_ppSSAO->integer)))
		return;

	if (!srcFbo || !tr.hdrDepthImage || !tr.hdrDepthFbo || !tr.renderDepthImage
	    || !tr.screenSsaoImage || !tr.screenSsaoFbo
	    || !tr.quarterImage[0] || !tr.quarterImage[1]
	    || !tr.quarterFbo[0] || !tr.quarterFbo[1])
		return;

	// no world = no meaningful depth to occlude against
	if (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
		return;

	oldFbo = glState.currentFBO;

	VectorSet4(viewInfo, backEnd.viewParms.zFar / r_znear->value, backEnd.viewParms.zFar, 0.0, 0.0);

	// snapshot the (already MSAA-resolved) scene depth into a sampleable colour target. This is
	// the whole reason the pass no longer needs r_depthPrepass - see RB_HZMDof, same move.
	VectorSet4(srcTexCoords, 0.0f, 0.0f, 1.0f, 1.0f);
	FBO_BlitFromTexture(tr.renderDepthImage, srcTexCoords, NULL, tr.hdrDepthFbo, NULL, NULL, NULL, 0);

	// clamp to the coop_postfx.urc slider ranges (plus headroom) so a hand-edited cvar
	// cannot push the shader somewhere degenerate
	aoRad  = r_ppSSAORadius    ? r_ppSSAORadius->value    : 16.0f;
	aoInt  = r_ppSSAOIntensity ? r_ppSSAOIntensity->value : 1.0f;
	aoBias = r_ppSSAOBias      ? r_ppSSAOBias->value      : 0.5f;
	if (aoRad  < 1.0f)  aoRad  = 1.0f;  else if (aoRad  > 64.0f) aoRad  = 64.0f;
	if (aoInt  < 0.0f)  aoInt  = 0.0f;  else if (aoInt  > 4.0f)  aoInt  = 4.0f;
	if (aoBias < 0.01f) aoBias = 0.01f; else if (aoBias > 8.0f)  aoBias = 8.0f;
	VectorSet4(ssaoParams, aoRad, aoInt, aoBias, 0.0f);

	// depthBlurShader[0]/[1] are compiled WITH USE_DEPTH (edge-preserving, tr_glsl.c),
	// [2]/[3] without - exactly gl1's ssaoBlurProgram-vs-blurProgram choice. The shadow-blur
	// pass in RB_DrawSurfs also uses [0]/[1] and is deliberately NOT retargeted: this toggle is
	// an SSAO control only, same scope as on gl1.
	blurIdx = (r_ppSSAODepthAware && !r_ppSSAODepthAware->integer) ? 2 : 0;

	viewInfo[2] = 1.0f / ((float)(tr.quarterImage[0]->width)  * tan(backEnd.viewParms.fovX * M_PI / 360.0f) * 2.0f);
	viewInfo[3] = 1.0f / ((float)(tr.quarterImage[0]->height) * tan(backEnd.viewParms.fovY * M_PI / 360.0f) * 2.0f);
	viewInfo[3] *= (float)backEnd.viewParms.viewportHeight / (float)backEnd.viewParms.viewportWidth;

	FBO_Bind(tr.quarterFbo[0]);

	qglViewport(0, 0, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height);
	qglScissor(0, 0, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height);

	VectorSet4(quadVerts[0], -1,  1, 0, 1);
	VectorSet4(quadVerts[1],  1,  1, 0, 1);
	VectorSet4(quadVerts[2],  1, -1, 0, 1);
	VectorSet4(quadVerts[3], -1, -1, 0, 1);

	texCoords[0][0] = 0; texCoords[0][1] = 1;
	texCoords[1][0] = 1; texCoords[1][1] = 1;
	texCoords[2][0] = 1; texCoords[2][1] = 0;
	texCoords[3][0] = 0; texCoords[3][1] = 0;

	GL_State( GLS_DEPTHTEST_DISABLE );

	GLSL_BindProgram(&tr.ssaoShader);

	GL_BindToTMU(tr.hdrDepthImage, TB_COLORMAP);

	GLSL_SetUniformVec4(&tr.ssaoShader, UNIFORM_VIEWINFO, viewInfo);
	GLSL_SetUniformVec4(&tr.ssaoShader, UNIFORM_HZMPARAMS, ssaoParams);

	RB_InstantQuad2(quadVerts, texCoords);


	viewInfo[2] = 1.0f / (float)(tr.quarterImage[0]->width);
	viewInfo[3] = 1.0f / (float)(tr.quarterImage[0]->height);

	FBO_Bind(tr.quarterFbo[1]);

	qglViewport(0, 0, tr.quarterFbo[1]->width, tr.quarterFbo[1]->height);
	qglScissor(0, 0, tr.quarterFbo[1]->width, tr.quarterFbo[1]->height);

	GLSL_BindProgram(&tr.depthBlurShader[blurIdx]);

	GL_BindToTMU(tr.quarterImage[0],  TB_COLORMAP);
	GL_BindToTMU(tr.hdrDepthImage, TB_LIGHTMAP);

	GLSL_SetUniformVec4(&tr.depthBlurShader[blurIdx], UNIFORM_VIEWINFO, viewInfo);

	RB_InstantQuad2(quadVerts, texCoords);


	FBO_Bind(tr.screenSsaoFbo);

	qglViewport(0, 0, tr.screenSsaoFbo->width, tr.screenSsaoFbo->height);
	qglScissor(0, 0, tr.screenSsaoFbo->width, tr.screenSsaoFbo->height);

	GLSL_BindProgram(&tr.depthBlurShader[blurIdx + 1]);

	GL_BindToTMU(tr.quarterImage[1],  TB_COLORMAP);
	GL_BindToTMU(tr.hdrDepthImage, TB_LIGHTMAP);

	GLSL_SetUniformVec4(&tr.depthBlurShader[blurIdx + 1], UNIFORM_VIEWINFO, viewInfo);

	RB_InstantQuad2(quadVerts, texCoords);

	// tr.screenSsaoFbo now holds AO produced from THIS frame's depth. Only now may the composite
	// multiply the frame by it - see backEndState_t::ssaoValid.
	backEnd.ssaoValid = qtrue;

	// restore what the raw qglViewport/qglScissor calls above trampled (mirrors RB_DrawSurfs'
	// FBO_Bind(oldFbo) + SetViewportAndScissor(), which is static to tr_backend.c)
	FBO_Bind(oldFbo);
	GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
	qglViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
	qglScissor( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}

/*
=============
RB_HZMDof

HZM gl2 POST-FX PORT (bug-1157): renderergl1's depth of field, driven by the same
r_ppDoF / r_ppDoFFocus / r_ppDoFRange / r_ppDoFIntensity levers.

gl1's recipe (tr_postprocess_gl1.c): blur the scene at half res, then composite it back over the
sharp frame with per-pixel alpha = circle of confusion from depth, letting the fixed-function
blender do the mix. Ported straight across, with two gl2-specific details:

  * DEPTH MUST BE A COPY. Every full-size FBO here (renderFbo, msaaResolveFbo, screenScratchFbo,
    sunRaysFbo) has tr.renderDepthImage bound as its DEPTH attachment, and this pass renders INTO
    one of them - sampling the original while doing so is a rendering feedback loop and the fetched
    depth is undefined. So the depth is first blitted into tr.hdrDepthImage (a colour R32F target),
    exactly as the SSAO path does, and the shader samples that. gl1 solves the same problem with
    its own glCopyTexSubImage2D into s.sceneDepth.
  * tr.hdrDepthImage is only allocated upstream for shadow blur / SSAO, so tr_image.c now also
    allocates it when r_ppDoF is set (both of those ship off here).

Placement: gl1's order is SSAO -> DoF -> bloom -> god rays -> grade, so the caller runs this
immediately before RB_HZMBloom.
=============
*/
void RB_HZMDof(FBO_t *srcFbo, ivec4_t srcBox)
{
	static cvar_t *r_ppDoFFocus = NULL, *r_ppDoFRange = NULL, *r_ppDoFIntensity = NULL;
	ivec4_t quarterBox;
	vec4_t  srcTexCoords;
	vec2_t  dir;
	vec4_t  color, params;
	float   zNear, zFar;

	if (!r_ppDoFFocus) {
		// same names and defaults as renderergl1 tr_init.c
		r_ppDoFFocus     = ri.Cvar_Get("r_ppDoFFocus",     "0",    CVAR_ARCHIVE);
		r_ppDoFRange     = ri.Cvar_Get("r_ppDoFRange",     "1200", CVAR_ARCHIVE);
		r_ppDoFIntensity = ri.Cvar_Get("r_ppDoFIntensity", "0.5",  CVAR_ARCHIVE);
	}

	if (!r_ppDoF || !r_ppDoF->integer)
		return;

	if (!srcFbo || !tr.quarterFbo[0] || !tr.quarterFbo[1] || !tr.hdrDepthFbo || !tr.hdrDepthImage)
		return;

	// gl1's fallbacks, so the two renderers agree even when a cvar is out of range
	zNear = r_znear->value;
	if (zNear <= 0.1f)
		zNear = 4.0f;

	zFar = backEnd.viewParms.zFar;
	if (zFar <= 1.0f)
		zFar = 2048.0f;

	VectorSet4(quarterBox, 0, 0, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height);

	// 1) snapshot the scene depth into a sampleable colour target (see the feedback-loop note)
	VectorSet4(srcTexCoords, 0.0f, 0.0f, 1.0f, 1.0f);
	FBO_BlitFromTexture(tr.renderDepthImage, srcTexCoords, NULL, tr.hdrDepthFbo, NULL, NULL, NULL, 0);

	// 2) half-res copy of the scene, then the same separable Gaussian bloom uses
	FBO_Blit(srcFbo, srcBox, NULL, tr.quarterFbo[0], quarterBox, NULL, NULL, 0);

	VectorSet2(dir, 1.0f, 0.0f);
	FBO_Blit(tr.quarterFbo[0], quarterBox, dir, tr.quarterFbo[1], quarterBox, &tr.bloomBlurShader, NULL, 0);

	VectorSet2(dir, 0.0f, 1.0f);
	FBO_Blit(tr.quarterFbo[1], quarterBox, dir, tr.quarterFbo[0], quarterBox, &tr.bloomBlurShader, NULL, 0);

	// 3) composite over the sharp frame, alpha = circle of confusion.
	//    FBO_Blit binds the colour to TB_COLORMAP itself and never touches TB_LIGHTMAP, so the
	//    depth bound here survives into the draw.
	GL_BindToTMU(tr.hdrDepthImage, TB_LIGHTMAP);

	VectorSet4(color, zNear, zFar, r_ppDoFFocus->value, r_ppDoFRange->value);
	VectorSet4(params, r_ppDoFIntensity->value, 0.0f, 0.0f, 0.0f);
	FBO_SetHzmParams(params);

	FBO_Blit(tr.quarterFbo[0], quarterBox, NULL, srcFbo, srcBox, &tr.dofShader, color,
		GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

	FBO_SetHzmParams(NULL);
}

/*
=============
RB_HZMExtraFx

HZM gl2 NEW POST-FX (bug-1158): cinematic-look additions with no gl1 equivalent - underwater/
slime/lava screen distortion, frost-on-lens while it snows, chromatic aberration, film grain.
Called from RB_PostProcess after RB_HZMScreenFx's gl1-parity chain (FXAA/sharpen/heat-haze/rain/
low-health/suppression), so the environmental effects (underwater, frost) sit alongside rain -
same round-trip-through-tr.screenScratchFbo pattern - and the two pure image-grade effects
(chromatic aberration, grain) run last, over everything including the vignettes, matching how a
real lens/sensor artifact would sit on top of the whole image.

Underwater and frost share their cgame-side publish idiom with rain (cg_view.c: r_ppRainWet is
the existing pattern; r_ppUnderwater and r_ppFrostAmt are new eased 0..1 signals published right
alongside it, off the SAME precip/content detection cgame already computes for its own FOV-warp
and rain gating - no new detection logic, only new publishes).
=============
*/
void RB_HZMExtraFx(FBO_t *srcFbo, ivec4_t srcBox)
{
	static cvar_t *r_ppUnderwaterFx = NULL, *r_ppUnderwater = NULL;
	static cvar_t *r_ppFrost = NULL, *r_ppFrostAmt = NULL;
	static cvar_t *r_ppChromaticAberration = NULL, *r_ppChromaticAberrationAmount = NULL;
	static cvar_t *r_ppFilmGrain = NULL, *r_ppFilmGrainAmount = NULL;
	static cvar_t *r_ppDizzyChroma = NULL;
	vec4_t color;
	float aberr;

	if (!r_ppUnderwaterFx) {
		r_ppUnderwaterFx              = ri.Cvar_Get("r_ppUnderwaterFx",              "1",   CVAR_ARCHIVE);
		r_ppUnderwater                = ri.Cvar_Get("r_ppUnderwater",                "0",   0);   // cgame-published
		r_ppFrost                     = ri.Cvar_Get("r_ppFrost",                     "0",   CVAR_ARCHIVE);   // [user 08-07] frost removed; cgame no longer publishes r_ppFrostAmt
		r_ppFrostAmt                  = ri.Cvar_Get("r_ppFrostAmt",                  "0",   0);   // cgame-published
		r_ppChromaticAberration       = ri.Cvar_Get("r_ppChromaticAberration",       "0",   CVAR_ARCHIVE);   // opt-in
		r_ppChromaticAberrationAmount = ri.Cvar_Get("r_ppChromaticAberrationAmount", "0.35", CVAR_ARCHIVE);
		r_ppFilmGrain                 = ri.Cvar_Get("r_ppFilmGrain",                 "0",   CVAR_ARCHIVE);   // opt-in
		r_ppFilmGrainAmount           = ri.Cvar_Get("r_ppFilmGrainAmount",           "0.35", CVAR_ARCHIVE);
		r_ppDizzyChroma               = ri.Cvar_Get("r_ppDizzyChroma",               "0.6",  CVAR_ARCHIVE);
	}

	if (!srcFbo || !tr.screenScratchFbo)
		return;

	if (r_ppUnderwaterFx->integer && r_ppUnderwater->value > 0.001f)
	{
		VectorSet4(color, r_ppUnderwater->value, backEnd.refdef.floatTime, 0.0f, 1.0f);
		FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.underwaterShader, color, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	if (r_ppFrost->integer && r_ppFrostAmt->value > 0.001f)
	{
		VectorSet4(color, r_ppFrostAmt->value, backEnd.refdef.floatTime, 0.0f, 1.0f);
		FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.frostShader, color, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	// [user 07-28 / bug-1233] Chromatic aberration has TWO independent sources that share one blit:
	//   * r_ppChromaticAberration - the flat artistic look, opt-in, off by default.
	//   * r_ppDizzyChroma         - injury. Colour fringing rises with the SAME 0..1 hurt factor that
	//                               drives the dizzy double-vision, so the two effects stay in lockstep
	//                               as the player bleeds down toward DBNO instead of drifting apart.
	// The injury term is deliberately NOT gated behind r_ppChromaticAberration: that cvar means "I want
	// aberration all the time", and requiring it would force an always-on fringe just to get the injury
	// cue. They sum, so someone running both gets base fringing that worsens as they get hurt.
	aberr = 0.0f;
	if (r_ppChromaticAberration->integer) {
		aberr = r_ppChromaticAberrationAmount->value;
	}
	if (r_ppDizzyChroma->value > 0.0001f) {
		aberr += RB_HZMDizzyAmount() * r_ppDizzyChroma->value;
	}
	if (aberr > 2.0f) {
		aberr = 2.0f;   // past ~2 the channels separate far enough to read as a broken frame, not an injury
	}

	if (aberr > 0.0001f)
	{
		VectorSet4(color, aberr * 0.02f, 0.0f, 0.0f, 1.0f);
		FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.chromabShader, color, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	if (r_ppFilmGrain->integer && r_ppFilmGrainAmount->value > 0.0001f)
	{
		VectorSet4(color, r_ppFilmGrainAmount->value, backEnd.refdef.floatTime, 0.0f, 1.0f);
		FBO_Blit(srcFbo, srcBox, NULL, tr.screenScratchFbo, srcBox, &tr.filmgrainShader, color, 0);
		FBO_FastBlit(tr.screenScratchFbo, srcBox, srcFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
}
