uniform sampler2D u_ShadowMap;

uniform vec3      u_LightForward;
uniform vec3      u_LightUp;
uniform vec3      u_LightRight;
uniform vec4      u_LightOrigin;
uniform float     u_LightRadius;

// HZM gl2 [2026-09-26] S4 SPOT SHADOWS: a headlight (spot) shadow exists only where the spot actually lights - inside
// its cone and its pool - by the SAME law as the pool (tr_hzm_spot.c R_HZM_SpotAttenuation x R_HZM_SpotConeDir,
// lightall HzmSpotCone). u_HzmPshadowSpot = (apex, light radius), u_HzmLightSpot = (cone axis * k, cosOuter). ALL ZERO
// for every other shadow (muzzle flashes, explosions): the mask is then exactly 1.0 and nothing is discarded.
uniform vec4      u_HzmPshadowSpot;
uniform vec4      u_HzmLightSpot;
varying vec3      var_Position;
varying vec3      var_Normal;

float HzmSpotShadowMask(vec3 pos)
{
	if (u_HzmPshadowSpot.w <= 0.0)
		return 1.0;
	vec3 toPos = pos - u_HzmPshadowSpot.xyz;
	float d2 = max(dot(toPos, toPos), 1.0);
	float att = clamp(0.5 * u_HzmPshadowSpot.w * u_HzmPshadowSpot.w / d2 - 0.5, 0.0, 1.0);
	float k = length(u_HzmLightSpot.xyz);
	if (k <= 0.0)
		return att;
	float t = clamp((dot(toPos, u_HzmLightSpot.xyz) / (k * sqrt(d2)) - u_HzmLightSpot.w) * k, 0.0, 1.0);
	return att * t * t * (3.0 - 2.0 * t);
}

void main()
{
	vec3 lightToPos = var_Position - u_LightOrigin.xyz;
	vec2 st = vec2(-dot(u_LightRight, lightToPos), dot(u_LightUp, lightToPos));
	
	float fade = length(st);
	
#if defined(USE_DISCARD)
	if (fade >= 1.0)
	{
		discard;
	}
#endif

	fade = clamp(8.0 - fade * 8.0, 0.0, 1.0);
	
	st = st * 0.5 + vec2(0.5);

#if defined(USE_SOLID_PSHADOWS)
	float intensity = max(sign(u_LightRadius - length(lightToPos)), 0.0);
#else
	float intensity = clamp((1.0 - dot(lightToPos, lightToPos) / (u_LightRadius * u_LightRadius)) * 2.0, 0.0, 1.0);
#endif
	
	float lightDist = length(lightToPos);
	float dist;

#if defined(USE_DISCARD)
	if (dot(u_LightForward, lightToPos) <= 0.0)
	{
		discard;
	}

	if (dot(var_Normal, lightToPos) > 0.0)
	{
		discard;
	}
#else
	intensity *= max(sign(dot(u_LightForward, lightToPos)), 0.0);
	intensity *= max(sign(-dot(var_Normal, lightToPos)), 0.0);
#endif

	intensity *= fade;

	float spotMask = HzmSpotShadowMask(var_Position);
#if defined(USE_DISCARD)
	if (spotMask <= 0.0)
	{
		discard;
	}
#endif
	intensity *= spotMask;

	float part;
#if defined(USE_PCF)
	part  = float(texture2D(u_ShadowMap, st + vec2(-1.0/512.0, -1.0/512.0)).r != 1.0);
	part += float(texture2D(u_ShadowMap, st + vec2( 1.0/512.0, -1.0/512.0)).r != 1.0);
	part += float(texture2D(u_ShadowMap, st + vec2(-1.0/512.0,  1.0/512.0)).r != 1.0);
	part += float(texture2D(u_ShadowMap, st + vec2( 1.0/512.0,  1.0/512.0)).r != 1.0);
#else
	part  = float(texture2D(u_ShadowMap, st).r != 1.0);
#endif

	if (part <= 0.0)
	{
		discard;
	}

#if defined(USE_PCF)
	intensity *= part * 0.25;
#else
	intensity *= part;
#endif

	gl_FragColor.rgb = vec3(0);
	gl_FragColor.a = clamp(intensity, 0.0, 0.75);
}
