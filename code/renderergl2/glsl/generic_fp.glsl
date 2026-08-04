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

vec3 ApplyGlobalFog(vec3 color)
{
	if (u_GlobalFogColor.a <= 0.0)
	{
		return color;
	}

	float denom = u_GlobalFogParams.x + (2.0 * gl_FragCoord.z - 1.0);
	float dist  = u_GlobalFogParams.y / min(denom, -1e-6);
	float frac  = clamp((dist - u_GlobalFogParams.z) * u_GlobalFogParams.w, 0.0, 1.0);

	frac = clamp(frac * u_GlobalFogColor.a, 0.0, 1.0);

	vec3 operand = (frac > 0.0) ? clamp(color, 0.0, 1.0) : color;

	return mix(operand, u_GlobalFogColor.rgb, frac);
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

	gl_FragColor.rgb = ApplyGlobalFog(rgb);
	gl_FragColor.a = alpha;
}
