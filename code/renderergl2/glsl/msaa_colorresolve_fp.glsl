// HZM gl2 MSAA (plan P2b, ME-F2): the tone-exact colour resolve, one pass, two outputs (MRT).
//   out_Color  (location 0, tr.renderImage) = the tone stage's input for this pixel:
//              mode 1 (grade path, the shipped default): avg_i(min(c_i * ao, 1)) - exact against the grade's own
//              input clamp, and what gl1's clamped RGBA8 multisample buffer gives;
//              mode 2 (Hable path): Karis weighting, w_i = 1 / (1 + k * max3(c_i * ao)), k = the current exposure;
//              mode 0: the plain box average (no tone stage runs).
//   out_Linear (location 1, tr.sceneLinearImage) = avg_i(c_i) * ao, unclamped linear HDR: the source bloom, DoF
//              and the exposure measure read, so thin bright features keep their energy.
// SSAO is applied HERE, per sample before the clamp (the separate AO composite is skipped under MSAA); ao = 1
// when the AO pass did not run this frame (bug-1211). Built only on the new path at >= 2 samples, with
// MSAA_SAMPLES defined by tr_glsl.c; GLSL 1.50 core. out_Color / out_Linear locations are bound before link.
// No double quotes anywhere in this file: tools/stringify embeds it verbatim.

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 2
#endif

uniform sampler2DMS u_TextureMap;       // the multisample scene colour (TB_COLORMAP)
uniform sampler2D   u_ScreenImageMap;   // the AO image (TB_NORMALMAP), or white
uniform sampler2D   u_LevelsMap;        // auto-exposure levels (TB_LEVELSMAP), mode 2 only
uniform vec4        u_Color;            // x mode (0 box, 1 clamp, 2 Karis), y AO on, z/w unused
uniform vec4        u_HzmParams;        // the view's rectangle in window pixels: x0, y0, x1, y1
uniform vec2        u_InvTexRes;        // 1 / scene size
uniform vec2        u_AutoExposureMinMax;
uniform vec3        u_ToneMinAvgMaxLinear;

out vec4 out_Linear;

void main()
{
	ivec2 tc = ivec2(gl_FragCoord.xy);
	vec3  ao = vec3(1.0);

	if (u_Color.y > 0.5
	    && gl_FragCoord.x >= u_HzmParams.x && gl_FragCoord.y >= u_HzmParams.y
	    && gl_FragCoord.x <  u_HzmParams.z && gl_FragCoord.y <  u_HzmParams.w)
	{
		ao = texture2D(u_ScreenImageMap, gl_FragCoord.xy * u_InvTexRes).rgb;
	}

	float k = 1.0;
	if (u_Color.x > 1.5)
	{
		vec3  mam  = texture2D(u_LevelsMap, vec2(0.5)).rgb;
		vec3  logs = clamp(mam * 20.0 - 10.0, -u_AutoExposureMinMax.y, -u_AutoExposureMinMax.x);
		k = u_ToneMinAvgMaxLinear.y * exp2(-logs.y);
	}

	vec4  sumLinear = vec4(0.0);
	vec4  sumTone   = vec4(0.0);
	float sumW      = 0.0;

	for (int i = 0; i < MSAA_SAMPLES; i++)
	{
		vec4 c  = texelFetch(u_TextureMap, tc, i);
		vec4 ca = vec4(c.rgb * ao, c.a);

		sumLinear += c;

		if (u_Color.x > 1.5)
		{
			float w = 1.0 / (1.0 + k * max(ca.r, max(ca.g, ca.b)));
			sumTone += ca * w;
			sumW    += w;
		}
		else if (u_Color.x > 0.5)
		{
			sumTone += vec4(min(ca.rgb, vec3(1.0)), ca.a);
		}
		else
		{
			sumTone += ca;
		}
	}

	float inv = 1.0 / float(MSAA_SAMPLES);

	gl_FragColor = (u_Color.x > 1.5) ? sumTone / max(sumW, 1e-6) : sumTone * inv;
	out_Linear   = vec4(sumLinear.rgb * inv * ao, sumLinear.a * inv);
}
