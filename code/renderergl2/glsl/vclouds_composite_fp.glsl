// HZM gl2 volumetric clouds (plan 2.4 step 3): the composite, drawn in the sky pass in place of today's talonclouds
// stage (blend ONE, SRC_ALPHA; alpha written as T but masked off by the caller). With T = 1, L = 0 it reduces to
// today's additive stage exactly: gauze = talon, rgb = talon * (1 - f), dst * 1 (the planted-identity gate).
//   rgb = (gauze * T + L) * (1 - f) + F * f * (1 - T),  a = T,  gauze = cap + (talon - cap) * T^2, cap = min(talon, CLO)
// f and F: the forward global fog of the sky shell (generic_fp.glsl ApplyGlobalFog's fraction; F = the opaque-class
// fog colour the box is fogged toward). No double quotes anywhere in this file (tools/stringify).

uniform sampler2D u_DiffuseMap;       // today's layer image (bundle 0 as bound today, incl. the HD twin)
uniform sampler2D u_VclCloud;         // the tent target (premultiplied L, T)
uniform vec4      u_BaseColor;        // today's stage colour (ComputeShaderColors: rgbGen identity = overbright)
uniform vec4      u_VclVp;            // x,y the traced view's viewport origin (scene px), z,w 1/viewport size
uniform vec4      u_VclMisc;          // x coverage threshold (cap), y debug mode, z,w the traced sub-rectangle size
uniform vec4      u_VclView;          // x,y 1/target size
uniform vec4      u_GlobalFogColor;
uniform vec4      u_GlobalFogParams;
uniform vec4      u_HzmSkyFog;        // [2026-10-05] the sky fog model (generic_fp.glsl HzmFogDist): xyz up (eye), w H
uniform vec4      u_HzmSkyVp;
uniform vec4      u_HzmSkyTan;        // z 1 = the model is on for this draw

varying vec2      var_DiffuseTex;
varying vec3      var_Dir;

void main()
{
	vec3 tal = texture2D(u_DiffuseMap, var_DiffuseTex).rgb * u_BaseColor.rgb;   // first: implicit derivatives, uniform flow

	// the traced sub-rectangle: ray i sits at (i + 0.5) * viewport / traced size (plan 2.4, vet R2-11); clamp the
	// bilinear footprint inside it so no unwritten texel is ever read
	vec2 uv = (gl_FragCoord.xy - u_VclVp.xy) * u_VclVp.zw;
	vec2 tp = clamp(uv * u_VclMisc.zw, vec2(0.5), u_VclMisc.zw - vec2(0.5));
	vec4 cl = texture2D(u_VclCloud, tp * u_VclView.xy);
	vec3 L = cl.rgb;
	float T = cl.a;

	// the fog fraction exactly as ApplyGlobalFog computes it (generic_fp.glsl HzmFogDist: the sky fog model, or depth)
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

	// the kept gauze: today's layer where there is no cloud, capped under cloud (continuous in T)
	vec3 cap = min(tal, vec3(u_VclMisc.x));
	vec3 gauze = cap + (tal - cap) * (T * T);
	// nothing below the horizon (user decision 4): the gauze fades out over +-0.75 deg
	float elev = degrees(asin(clamp(normalize(var_Dir).z, -1.0, 1.0)));
	gauze *= clamp((elev + 0.75) / 1.5, 0.0, 1.0);

	vec3 rgb = (gauze * T + L) * (1.0 - f) + F * f * (1.0 - T);

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
