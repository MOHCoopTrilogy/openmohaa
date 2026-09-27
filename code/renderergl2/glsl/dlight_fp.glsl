uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

varying vec2      var_Tex1;
varying vec4      var_Color;
varying vec3      var_HzmSpotDir;

// HZM gl2 [2026-09-26] Phase S1 SPOT CONE (renderergl2/tr_hzm_spot.c). u_HzmLightSpot = (cone axis * k, cosOuter),
// k = 1 / (cosInner - cosOuter); cosOuter + 4 = the r_hzmSpotDebug 2 magenta tint. ALL ZERO = an omni light, which is
// what every other draw of this program carries (GPU uniforms start at 0): the cone is then exactly 1.0 and the tint
// exactly vec3(1.0). The same two helpers are in lightall_vp, lightall_fp and dlight_fp - each file is stringified on
// its own (no includes), and none of them may contain a double quote.
uniform vec4      u_HzmLightSpot;

float HzmSpotCone(vec3 toSurf)
{
	float k = length(u_HzmLightSpot.xyz);
	if (k <= 0.0)
		return 1.0;
	float cosOuter = u_HzmLightSpot.w;
	if (cosOuter > 2.0)
		cosOuter -= 4.0;
	float t = clamp((dot(toSurf, u_HzmLightSpot.xyz) / k - cosOuter) * k, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

vec3 HzmSpotTint()
{
	return (u_HzmLightSpot.w > 2.0) ? vec3(1.0, 0.15, 1.0) : vec3(1.0);
}


void main()
{
	vec4 color = texture2D(u_DiffuseMap, var_Tex1);

	float alpha = color.a * var_Color.a;
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
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	// HZM gl2 [2026-09-26] Phase S1: the spot cone per pixel (vet F11); exactly x1.0 for an omni light
	gl_FragColor.rgb *= HzmSpotTint() * HzmSpotCone(var_HzmSpotDir * inversesqrt(max(dot(var_HzmSpotDir, var_HzmSpotDir), 1e-6)));
	gl_FragColor.a = alpha;
}
