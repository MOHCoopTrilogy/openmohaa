uniform sampler2D u_DiffuseMap;

// HZM gl2 re-port (bug-gl2-nextbundle2): second texture bundle (MOHAA
// 'nextbundle' single-pass multitexture). u_LightMap is the fixed TMU 1
// sampler the C side already binds bundle[1] to; u_Texture1Env selects the
// gl1 texEnv combine (0 = off, 1 = GL_MODULATE, 2 = GL_ADD).
uniform sampler2D u_LightMap;
uniform int       u_Texture1Env;

uniform int       u_AlphaTest;

varying vec2      var_DiffuseTex;
varying vec2      var_Tex2;

varying vec4      var_Color;

// HZM gl2 FORWARD GLOBAL FOG (r_globalFogForward, bug-1306) - see the long note in
// lightall_fp.glsl. Duplicated rather than factored out because these two fragment shaders
// are stringified independently and there is no include mechanism. Keep the bodies identical.
uniform vec4      u_GlobalFogColor;
uniform vec4      u_GlobalFogParams;

// HZM coop [2026-09-27] realistic lightning: fog IN-SCATTER toward the strike (tr_hzm_lightning.c). The lit cloud
// region lights the haze between it and the eye, so fogged distance in the strike's direction brightens in a 35-degree
// lobe - never a flat band. u_HzmLtFog = (strike direction in EYE space, energy); w 0 = inert (the default: every
// uniform starts at 0), so with lightning off the fog colour below is exactly u_GlobalFogColor.rgb.
uniform vec4      u_HzmLtFog;
uniform vec4      u_HzmLtView;
uniform vec4      u_HzmLtProj;

vec3 HzmLtFogColor()
{
	vec3 fogCol = u_GlobalFogColor.rgb;

	if (u_HzmLtFog.w > 0.0)
	{
		vec2  ndc = (gl_FragCoord.xy - u_HzmLtView.xy) * u_HzmLtView.zw * 2.0 - 1.0;
		vec3  ray = normalize(vec3(ndc.x * u_HzmLtProj.x, ndc.y * u_HzmLtProj.y, -1.0));
		float ang = acos(clamp(dot(ray, u_HzmLtFog.xyz), -1.0, 1.0));
		float lob = exp(-0.5 * ang * ang / (0.611 * 0.611));

		fogCol += vec3(0.86, 0.90, 1.0) * (u_HzmLtFog.w * (lob + 0.10));
	}

	return fogCol;
}

// HZM coop [2026-10-05] SKY FOG MODEL (tr_shade.c RB_HZM_SkyFogUniforms, docs/proposals/volumetric_clouds_2026-09-26/v11.md).
// The sky is drawn at depth 1, so the depth fraction fogged it as if it hung at zFar - the farthest VISIBLE corner of
// the world, which shrinks in a street or a room: a thickly fogged map then showed a clear sky through every gap
// above the fog wall. A sky draw (u_HzmSkyTan.z 1: the box, its cloud stages, the sun, a portal-sky view) instead
// takes the path through a fog LAYER of height H toward its own elevation: the zenith is fogged as at H, the
// horizon fully - camera position no longer matters. Every other draw keeps its depth (the z 0 default).
uniform vec4      u_HzmSkyFog;
uniform vec4      u_HzmSkyVp;
uniform vec4      u_HzmSkyTan;

float HzmFogDist()
{
	if (u_HzmSkyTan.z > 0.5)
	{
		vec2 ndc = (gl_FragCoord.xy - u_HzmSkyVp.xy) * u_HzmSkyVp.zw * 2.0 - 1.0;
		vec3 ray = normalize(vec3(ndc.x * u_HzmSkyTan.x, ndc.y * u_HzmSkyTan.y, -1.0));
		return u_HzmSkyFog.w / max(dot(ray, u_HzmSkyFog.xyz), 0.0001);
	}
	float denom = u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0);
	return u_GlobalFogParams.y / min(denom, -1e-6);
}

vec3 ApplyGlobalFog(vec3 color)
{
	if (u_GlobalFogColor.a <= 0.0)
	{
		return color;
	}

	float dist  = HzmFogDist();
	float frac  = clamp((dist - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0);

	frac = clamp(frac * u_GlobalFogColor.a, 0.0, 1.0);

	vec3 operand = (frac > 0.0) ? clamp(color, 0.0, 1.0) : color;

	return mix(operand, HzmLtFogColor(), frac);
}

// HZM gl2 soft particles (r_softParticles). u_ScreenDepthMap is the scene-depth snapshot bound on
// TB_SCREENDEPTH (TMU 7); u_InvTexRes is 1/scene-FBO size so uv = gl_FragCoord.xy * u_InvTexRes.
// u_SoftParticle = (1/fadeDistance, projMat[10], projMat[14], mode). mode 0 leaves this inert, so
// with r_softParticles off the fragment output is byte-identical. Body kept identical to the copy
// in lightall_fp.glsl (these two fragment shaders are stringified independently - no includes).
uniform sampler2D u_ScreenDepthMap;
uniform vec2      u_InvTexRes;
uniform vec4      u_SoftParticle;

void ApplySoftParticle(inout vec3 rgb, inout float a)
{
	if (u_SoftParticle.w < 0.5)
	{
		return;
	}

#if defined(MSAA_DEPTH_MINMAX)
	// HZM gl2 MSAA (plan P2b, MH-B1) two-surface fade: under MSAA the bound snapshot holds the edge pixel's nearest (r)
	// and farthest (g) depth. A particle behind the nearest surface is hidden on those samples by the multisample
	// depth test anyway, so it fades against the far one. g is 0 on a single-sample snapshot (the HOME bypass),
	// which keeps today's r exactly.
	vec2  zmm = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).rg;
	float zs  = (zmm.g > 0.0 && gl_FragCoord.z > zmm.r) ? zmm.g : zmm.r;
#else
	float zs = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).r;
#endif
	float ds = u_SoftParticle.z / min(u_SoftParticle.y + (2.0 * zs - 1.0), -1e-6);
	float df = u_SoftParticle.z / min(u_SoftParticle.y + (2.0 * gl_FragCoord.z - 1.0), -1e-6);
	float k  = clamp((ds - df) * u_SoftParticle.x, 0.0, 1.0);

	if (u_SoftParticle.w > 3.5)
	{
		rgb = vec3(k);                // debug: the fade factor as greyscale
		a   = 1.0;
	}
	else if (u_SoftParticle.w > 2.5)
	{
		rgb = mix(vec3(1.0), rgb, k); // mode 3: modulate, fade toward white
	}
	else if (u_SoftParticle.w > 1.5)
	{
		rgb *= k;                     // mode 2: additive, fade toward black
	}
	else
	{
		a *= k;                       // mode 1: alpha blend
	}
}

void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
	vec3  rgb   = color.rgb * var_Color.rgb;

	// HZM gl2 re-port (bug-gl2-nextbundle2): apply the second bundle exactly
	// like gl1's fixed-function unit 1 (after the unit 0 modulate-by-color):
	// MODULATE multiplies rgb+alpha, ADD adds rgb and multiplies alpha.
	if (u_Texture1Env != 0)
	{
		vec4 color2 = texture2D(u_LightMap, var_Tex2);

		if (u_Texture1Env == 2)
		{
			rgb += color2.rgb;
		}
		else
		{
			rgb *= color2.rgb;
		}
		alpha *= color2.a;
	}

	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
#if defined(MSAA_DEPTH_MINMAX)
	else if (u_AlphaTest >= 4)
	{
		// HZM gl2 MSAA P4a (MH-B2): alpha-to-coverage. The coverage ramp is a pixel wide in alpha (fwidth), capped
		// so a nomip cutout cannot smear; 4 centres it on the old 0.5 threshold, 5 (light / unknown matte) keeps
		// it on the opaque side so no texel more contaminated than today's test ever shows
		float hzmA2c = (alpha - 0.5) / max(min(fwidth(alpha), 0.5), 0.0001);
		if (u_AlphaTest == 4)
			hzmA2c += 0.5;
		hzmA2c = clamp(hzmA2c, 0.0, 1.0);
		if (hzmA2c <= 0.0)
			discard;
		alpha = hzmA2c;
	}
#endif

	ApplySoftParticle(rgb, alpha);

	gl_FragColor.rgb = ApplyGlobalFog(rgb);
	gl_FragColor.a = alpha;
}
