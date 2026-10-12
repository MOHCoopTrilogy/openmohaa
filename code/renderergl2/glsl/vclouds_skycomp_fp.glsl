// HZM gl2 volumetric clouds v1.1 (2026-10-05): the PROCEDURAL profile's composite. A full-screen triangle drawn at the
// end of the profile sky's sky pass (tr_sky.c RB_StageIteratorSky -> tr_hzm_clouds.c RB_HZM_CloudsSkyComposite) at depth
// range (1,1) with the depth test on, so it lands only where nothing nearer has been drawn: the sky. Blend ONE, SRC_ALPHA
// (alpha written as T but masked off by the caller): the box and its stages behind, already fogged, keep T of their light.
//   rgb = L * (1 - f) + F * f * (1 - T),  a = T
// f = the sky fog model's fraction at this pixel (generic_fp.glsl HzmFogDist), so cloud fades into the fog exactly as
// the sky behind it does. GLSL >= 1.30 only. No double quotes anywhere in this file (tools/stringify).

uniform sampler2D u_VclCloud;         // the tent target (premultiplied L, T)
uniform vec4      u_VclVp;            // x,y the traced view's viewport origin (scene px), z,w 1/viewport size
uniform vec4      u_VclMisc;          // x unused, y debug mode, z,w the traced sub-rectangle size
uniform vec4      u_VclView;          // x,y 1/target size
uniform vec4      u_GlobalFogColor;
uniform vec4      u_GlobalFogParams;
uniform vec4      u_HzmSkyFog;
uniform vec4      u_HzmSkyVp;
uniform vec4      u_HzmSkyTan;

void main()
{
	vec2 uv = (gl_FragCoord.xy - u_VclVp.xy) * u_VclVp.zw;
	vec2 tp = clamp(uv * u_VclMisc.zw, vec2(0.5), u_VclMisc.zw - vec2(0.5));
	vec4 cl = texture2D(u_VclCloud, tp * u_VclView.xy);
	vec3 L = cl.rgb;
	float T = cl.a;

	float f = 0.0;
	if (u_GlobalFogColor.a > 0.0)
	{
		float dist;
		if (u_HzmSkyTan.z > 0.5)
		{
			vec2 ndc = (gl_FragCoord.xy - u_HzmSkyVp.xy) * u_HzmSkyVp.zw * 2.0 - 1.0;
			vec3 ray = normalize(vec3(ndc.x * u_HzmSkyTan.x, ndc.y * u_HzmSkyTan.y, -1.0));
			dist = u_HzmSkyFog.w / max(dot(ray, u_HzmSkyFog.xyz), 0.0001);
		}
		else
		{
			float denom = u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0);
			dist = u_GlobalFogParams.y / min(denom, -1e-6);
		}
		f = clamp((dist - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0);
		f = clamp(f * u_GlobalFogColor.a, 0.0, 1.0);
	}
	vec3 F = u_GlobalFogColor.rgb;

	vec3 rgb = L * (1.0 - f) + F * f * (1.0 - T);
	float a = T;
	if (u_VclMisc.y > 0.5 && u_VclMisc.y < 1.5)        // r_hzmCloudsDebug 1: clouds over black (dst x 0)
	{
		rgb = L; a = 0.0;
	}
	else if (u_VclMisc.y > 3.5 && u_VclMisc.y < 4.5)   // 4: transmittance
	{
		rgb = vec3(T); a = 0.0;
	}
	gl_FragColor = vec4(rgb, a);
}
