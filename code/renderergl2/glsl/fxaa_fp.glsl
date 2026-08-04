// HZM gl2 POST-FX PORT (bug-1150): byte-port of renderergl1's FXAA_FS
// (tr_postprocess_gl1.c), driven by the same r_ppFXAA lever. Runs after the grade, exactly
// where gl1 runs it.
//
// gl1 passes 1/frame-size as its own u_rcpFrame; gl2 gets the same thing for free from
// FBO_Blit, which divides its inSrcTexScale argument by the source image dimensions and hands
// it over as UNIFORM_INVTEXRES - so passing (1,1) yields (1/w, 1/h).
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C
// string literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform vec2      u_InvTexRes;   // = gl1's u_rcpFrame

varying vec2      var_TexCoords;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
	vec2 px   = u_InvTexRes;
	vec3 rgbM = texture2D(u_TextureMap, var_TexCoords).rgb;
	float lM  = luma(rgbM);
	float lNW = luma(texture2D(u_TextureMap, var_TexCoords + vec2(-px.x, -px.y)).rgb);
	float lNE = luma(texture2D(u_TextureMap, var_TexCoords + vec2( px.x, -px.y)).rgb);
	float lSW = luma(texture2D(u_TextureMap, var_TexCoords + vec2(-px.x,  px.y)).rgb);
	float lSE = luma(texture2D(u_TextureMap, var_TexCoords + vec2( px.x,  px.y)).rgb);

	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

	vec2 dir;
	dir.x = -((lNW + lNE) - (lSW + lSE));
	dir.y =  ((lNW + lSW) - (lNE + lSE));

	float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.0625, 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;

	vec3 rgbA = 0.5 * (texture2D(u_TextureMap, var_TexCoords + dir * (1.0 / 3.0 - 0.5)).rgb
	                 + texture2D(u_TextureMap, var_TexCoords + dir * (2.0 / 3.0 - 0.5)).rgb);
	vec3 rgbB = rgbA * 0.5 + 0.25 * (texture2D(u_TextureMap, var_TexCoords + dir * (-0.5)).rgb
	                               + texture2D(u_TextureMap, var_TexCoords + dir * ( 0.5)).rgb);

	float lB = luma(rgbB);

	gl_FragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}
