// HZM gl2 POST-FX PORT: bloom blur. Byte-port of renderergl1's BLUR_FS
// (tr_postprocess_gl1.c) - a 9-tap separable Gaussian with gl1's exact weights.
//
// gl1 passes the per-tap texel offset as its own u_dir uniform. gl2 gets it for free from
// FBO_Blit: the inSrcTexScale argument is divided by the source image dimensions and handed over
// as UNIFORM_INVTEXRES (u_InvTexRes), so passing (1,0) gives (1/srcW, 0) for the horizontal pass
// and (0,1) gives (0, 1/srcH) for the vertical one - identical to gl1's u_dir.
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C string
// literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform vec2      u_InvTexRes;  // per-tap texel offset = gl1's u_dir

varying vec2      var_TexCoords;

void main()
{
	vec2 d = u_InvTexRes;
	vec3 o = texture2D(u_TextureMap, var_TexCoords).rgb * 0.227027;

	o += texture2D(u_TextureMap, var_TexCoords + d * 1.0).rgb * 0.1945946;
	o += texture2D(u_TextureMap, var_TexCoords - d * 1.0).rgb * 0.1945946;
	o += texture2D(u_TextureMap, var_TexCoords + d * 2.0).rgb * 0.1216216;
	o += texture2D(u_TextureMap, var_TexCoords - d * 2.0).rgb * 0.1216216;
	o += texture2D(u_TextureMap, var_TexCoords + d * 3.0).rgb * 0.0540541;
	o += texture2D(u_TextureMap, var_TexCoords - d * 3.0).rgb * 0.0540541;
	o += texture2D(u_TextureMap, var_TexCoords + d * 4.0).rgb * 0.0162162;
	o += texture2D(u_TextureMap, var_TexCoords - d * 4.0).rgb * 0.0162162;

	gl_FragColor = vec4(o, 1.0);
}
