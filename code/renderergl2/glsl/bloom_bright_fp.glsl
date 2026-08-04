// HZM gl2 POST-FX PORT: bloom bright-pass. Byte-port of renderergl1's BRIGHT_FS
// (tr_postprocess_gl1.c) so gl2 reproduces the bloom the OG trilogy has on gl1, driven by the
// SAME Advanced-Graphics levers (r_ppBloom / r_ppBloomThreshold / r_ppBloomIntensity).
//
// Keeps only what exceeds the threshold, with a soft knee via a per-pixel scale rather than a
// hard cut, so a pixel just over the threshold contributes just a little.
//
// The threshold arrives in u_Color.x: FBO_Blit hands its `color` argument over as UNIFORM_COLOR,
// and GLSL_InitUniforms resolves uniforms by the NAME in tr_glsl.c uniformsInfo - which is
// u_Color. Naming it anything else yields location -1 and a silently zeroed uniform (bug-1148).
//
// COLOUR-DOMAIN PARITY (bug-1159): gl1's bright-pass samples an already-clamped LDR backbuffer -
// tr_postprocess_gl1.c copies from the default (fixed-point) framebuffer into an RGBA8 scratch
// texture (RB_PostFxApply, tr_postprocess_gl1.c ~L866-871), and GL hard-clamps any write to a
// fixed-point colour target to [0,1] - so BRIGHT_FS never sees a channel above 1.0, and
// r_ppBloomThreshold is documented as a 0..1 value (renderergl1 tr_init.c comment: bloom
// brightness threshold, 0 to 1). gl2's source, by contrast, is tr.renderFbo/tr.msaaResolveFbo, which is
// genuinely RGBA16F HDR whenever r_hdr is on and the GPU has float textures (tr_image.c hdrFormat
// selection; r_hdr defaults to 1 - this is the SHIPPED default, not an edge case). texture2D()
// fetches from a float texture are NOT clamped, so without the clamp below this shader thresholds
// raw unbounded linear HDR values against the same 0.6 that gl1 thresholds a clamped LDR value
// against - two different domains, same slider. Clamping the sample here (not a Reinhard/ACES
// curve - gl1's own clamp is a hard clip, not a smooth compressive curve, and a smooth curve would
// reclassify pixels differently near the threshold and need a re-derived constant) makes the
// comparison run in the SAME domain on both renderers with no threshold recalibration needed, and
// is a no-op on the LDR path (r_hdr 0 -> tr.renderImage is already RGBA8 / already clamped).
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C string
// literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (threshold, unused, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	vec3  c = clamp(texture2D(u_TextureMap, var_TexCoords).rgb, 0.0, 1.0);
	float b = max(c.r, max(c.g, c.b));
	float f = max(b - u_Color.x, 0.0) / max(b, 1e-4);

	gl_FragColor = vec4(c * f, 1.0);
}
