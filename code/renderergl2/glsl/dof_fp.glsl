// HZM gl2 POST-FX PORT (bug-1157): byte-port of renderergl1's DOF_FS
// (tr_postprocess_gl1.c). Composites the half-res blurred scene over the sharp frame with a
// per-pixel alpha = circle of confusion derived from depth, so the fixed-function blender does
// the mix: result = sharp*(1-coc) + blurred*coc. Driven by the same r_ppDoF / r_ppDoFFocus /
// r_ppDoFRange / r_ppDoFIntensity levers gl1 uses.
//
// DEPTH SOURCE: u_ScreenDepthMap is a COPY of the scene depth (tr.hdrDepthImage), not
// tr.renderDepthImage itself. That is deliberate and load-bearing - every full-size FBO
// (renderFbo, msaaResolveFbo, screenScratchFbo, sunRaysFbo) has tr.renderDepthImage bound as its
// DEPTH attachment, and this pass renders INTO one of them, so sampling the original would be a
// rendering feedback loop with undefined results. gl1 dodges the same problem by copying depth
// into its own s.sceneDepth every frame.
//
// u_Color   = (zNear, zFar, focus, range)      focus <= 0 means auto-focus on centre screen
// u_HzmParams.x = intensity (max blur mix 0..1)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;      // blurred scene, half res
uniform sampler2D u_ScreenDepthMap;  // full-res copy of scene depth
uniform vec4      u_Color;           // (zNear, zFar, focus, range)
uniform vec4      u_HzmParams;       // (intensity, unused, unused, unused)

varying vec2      var_TexCoords;

float lin(vec2 uv)
{
	float d = texture2D(u_ScreenDepthMap, uv).r;
	return (u_Color.x * u_Color.y) / (u_Color.y - d * (u_Color.y - u_Color.x));
}

void main()
{
	float fd  = (u_Color.z > 0.0) ? u_Color.z : lin(vec2(0.5, 0.5));
	float d   = lin(var_TexCoords);
	float coc = clamp(abs(d - fd) / max(u_Color.w, 1.0), 0.0, 1.0) * u_HzmParams.x;

	gl_FragColor = vec4(texture2D(u_TextureMap, var_TexCoords).rgb, coc);
}
