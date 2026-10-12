uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;

uniform vec4      u_Color;


uniform vec2      u_AutoExposureMinMax;
uniform vec3      u_ToneMinAvgMaxLinear;

varying vec2      var_TexCoords;
varying float     var_InvWhite;

// HZM gl2 volumetric clouds (tr_hzm_clouds.c RB_HZM_CloudsToneDither; plan 2.4 step 4): an ANIMATED cloud-masked
// triangular dither right before the RGBA8 write. u_VclDither.x = amplitude / 255 (0 = off: this pass is then
// byte-identical), y = the golden-ratio frame offset, z = 1 - the sky's fog fraction. The mask is the cloud presence
// from the tent target x sky (depth at the far plane) - vc_post.py chain(mask=presence*sky*(1-f), dframe=frame).
uniform sampler2D u_VclCloud;
uniform sampler2D u_ScreenDepthMap;
uniform vec4      u_VclDither;
uniform vec4      u_VclVp;
uniform vec4      u_VclMisc;
uniform vec4      u_VclView;

float VclIgn(vec2 p)
{
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

vec3 VclDither(vec3 c)
{
	if (u_VclDither.x <= 0.0)
	{
		return c;
	}
	vec2 uv = (gl_FragCoord.xy - u_VclVp.xy) * u_VclVp.zw;
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
	{
		return c;
	}
	if (texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_VclView.zw).r < 1.0)
	{
		return c;
	}
	vec2 tp = clamp(uv * u_VclMisc.zw, vec2(0.5), u_VclMisc.zw - vec2(0.5));
	vec4 cl = texture2D(u_VclCloud, tp * u_VclView.xy);
	float pres = clamp(max(1.0 - cl.a, max(cl.r, max(cl.g, cl.b)) * 4.0) * 8.0, 0.0, 1.0) * u_VclDither.z;
	vec2 p = floor(gl_FragCoord.xy) + vec2(u_VclDither.y);
	float n = VclIgn(p) + VclIgn(p + vec2(17.0, 31.0)) - 1.0;
	return c + vec3(n * u_VclDither.x * pres);
}

const vec3  LUMINANCE_VECTOR =   vec3(0.2125, 0.7154, 0.0721); //vec3(0.299, 0.587, 0.114);

float FilmicTonemap(float x)
{
	const float SS  = 0.22; // Shoulder Strength
	const float LS  = 0.30; // Linear Strength
	const float LA  = 0.10; // Linear Angle
	const float TS  = 0.20; // Toe Strength
	const float TAN = 0.01; // Toe Angle Numerator
	const float TAD = 0.30; // Toe Angle Denominator

	return ((x*(SS*x+LA*LS)+TS*TAN)/(x*(SS*x+LS)+TS*TAD)) - TAN/TAD;
}

void main()
{
	vec4 color = texture2D(u_TextureMap, var_TexCoords) * u_Color;

#if defined(USE_PBR)
	color.rgb *= color.rgb;
#endif

	vec3 minAvgMax = texture2D(u_LevelsMap, var_TexCoords).rgb;
	vec3 logMinAvgMaxLum = clamp(minAvgMax * 20.0 - 10.0, -u_AutoExposureMinMax.y, -u_AutoExposureMinMax.x);

	float invAvgLum = u_ToneMinAvgMaxLinear.y * exp2(-logMinAvgMaxLum.y);

	color.rgb = color.rgb * invAvgLum - u_ToneMinAvgMaxLinear.xxx;
	color.rgb = max(vec3(0.0), color.rgb);

	color.r = FilmicTonemap(color.r);
	color.g = FilmicTonemap(color.g);
	color.b = FilmicTonemap(color.b);

	color.rgb = clamp(color.rgb * var_InvWhite, 0.0, 1.0);

#if defined(USE_PBR)
	color.rgb = sqrt(color.rgb);
#endif

	// add a bit of dither to reduce banding
	color.rgb += vec3(1.0/510.0 * mod(gl_FragCoord.x + gl_FragCoord.y, 2.0) - 1.0/1020.0);

	gl_FragColor = vec4(VclDither(color.rgb), color.a);
}
