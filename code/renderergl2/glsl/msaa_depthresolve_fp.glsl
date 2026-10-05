// HZM gl2 MSAA (plan P2a infrastructure, used by P2b RB_MSAAResolveDepth).
// Custom depth resolve of the multisample scene depth: gl_FragDepth takes the NEAREST sample (min), so
// every depth-sampling pass sees the front-most surface of an edge pixel, and the colour output carries
// (min, max) for the passes that need the far side of an edge (underwater, legacy fog, soft particles).
// Built only on the new path at >= 2 samples, with MSAA_SAMPLES defined by tr_glsl.c; GLSL 1.50 core
// (sampler2DMS / texelFetch). Drawn with GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK_TRUE into a target whose
// depth is tr.renderDepthImage and whose colour is tr.msaaDepthMinMaxImage.
// No double quotes anywhere in this file: tools/stringify embeds it verbatim.

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 2
#endif

uniform sampler2DMS u_ScreenDepthMap;

void main()
{
	ivec2 tc = ivec2(gl_FragCoord.xy);
	float zMin = 1.0;
	float zMax = 0.0;

	for (int i = 0; i < MSAA_SAMPLES; i++)
	{
		float z = texelFetch(u_ScreenDepthMap, tc, i).r;
		zMin = min(zMin, z);
		zMax = max(zMax, z);
	}

	gl_FragDepth = zMin;
	gl_FragColor = vec4(zMin, zMax, 0.0, 1.0);
}
