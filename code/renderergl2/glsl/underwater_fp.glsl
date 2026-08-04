// HZM gl2 NEW POST-FX (bug-1158): underwater/slime/lava screen distortion. NOT a gl1 port -
// gl1 only warps the FOV (renderergl1 has no equivalent screen-space pass here); this adds an
// actual on-screen ripple + colour tint while submerged, using the SAME cgame-side detection
// (CONTENTS_WATER/SLIME/LAVA) that already drives the FOV warp in cg_view.c, published as
// r_ppUnderwater (0..1, eased) alongside the existing r_ppRainWet/r_ppHeat cgame->renderer
// bridges. Master switch r_ppUnderwaterFx; the tint colour is fixed per liquid type by the
// content flag the cgame side already resolved into the eased fraction, so this shader only
// ever sees how submerged, not which liquid - a later revision could widen r_ppUnderwater
// into a vec4 (fraction, r, g, b) via UNIFORM_HZMPARAMS if per-liquid tinting is wanted.
//
// u_Color.x = amount (0..1, eased submersion), u_Color.y = time (seconds, animates the ripple)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (amount, time, unused, unused)

varying vec2      var_TexCoords;

void main()
{
	float amt = clamp(u_Color.x, 0.0, 1.0);
	float t   = u_Color.y;

	vec2 uv = var_TexCoords;
	uv.x += sin(var_TexCoords.y * 24.0 + t * 1.3) * 0.006 * amt;
	uv.y += sin(var_TexCoords.x * 18.0 + t * 1.1) * 0.006 * amt;

	vec3 c = texture2D(u_TextureMap, uv).rgb;

	// blue-green tint, deeper toward the edges (vignette-shaped) so it reads as looking through
	// a body of water rather than a flat colour wash
	vec2  d   = var_TexCoords - vec2(0.5);
	float vig = clamp(dot(d, d) * 1.6, 0.0, 1.0);
	vec3  tint = vec3(0.55, 0.85, 0.95);

	c = mix(c, c * tint, amt * (0.35 + 0.25 * vig));

	gl_FragColor = vec4(c, 1.0);
}
