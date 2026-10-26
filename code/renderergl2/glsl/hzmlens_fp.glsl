// HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE - composite of the scope picture and the blurred wide world; then the rim.
//
// Runs at the top of the post chain on the HDR scene (before bloom / tone map), driven by cgame through r_ppScopeState. TWO passes (u_ViewInfo.z):
//   0 : the picture. u_TextureMap = the MAIN scene = the MAGNIFIED scope picture (MSAA, sun shadows: the bug-3462 picture), without the weapon.
//       u_LevelsMap = the WIDE scene (same ray at the hip fov, no weapon). u_ScreenImageMap = the wide scene box-downsampled to 1/4 (the blur source).
//       inside the circle the scope picture (barrel, radial fringe, glass tint, soft inner shadow, pupil shadow); outside the wide world, blurred and dimmed.
//   1 : the rim only, AFTER the view weapon was drawn over the picture: a dark metal ring (soft highlight arc, soft falloff) as a TRUE circle that covers the
//       faceted eyepiece bell of the weapon mesh. u_TextureMap = the scene so far.
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;
uniform sampler2D u_ScreenImageMap;
uniform sampler2D u_ScreenDepthMap;   // rim pass only: the finished picture of pass 0
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outside brightness 0..1)
uniform vec4      u_ViewInfo;   // (lens circle centre offset x, y in screen-height units, +y down: it follows the rifle's eyepiece; pass mode)
uniform vec2      u_InvTexRes;  // 1 / scene size in texels

varying vec2      var_TexCoords;

void main()
{
	float amt    = clamp(u_Color.x, 0.0, 1.0);
	float aspect = u_InvTexRes.y / u_InvTexRes.x;            // width / height
	vec2  d      = var_TexCoords - vec2(0.5);
	vec2  p      = d * vec2(aspect, -1.0);                     // screen-height units, +y down
	float R      = max(u_Color.y, 0.05);
	vec2  q      = (p - u_ViewInfo.xy) / R;                    // lens radii, from the lens centre (the eyepiece)
	float r      = length(q);

	// ---- the rim, pass 1
	float band  = smoothstep(0.995, 1.004, r) * (1.0 - smoothstep(1.395, 1.445, r));   // out to the bell's outer radius: its faceted silhouette is covered
	vec2  qn    = q / max(r, 0.0001);
	float arc   = pow(max(dot(qn, normalize(vec2(-0.55, -0.83))), 0.0), 6.0);
	vec3  metal = vec3(0.016, 0.017, 0.020) * mix(1.6, 0.6, smoothstep(1.0, 1.4, r)) + vec3(0.075) * arc * smoothstep(1.02, 1.10, r) * (1.0 - smoothstep(1.22, 1.38, r));
	float ringK = band * smoothstep(0.55, 0.92, amt);

	vec3  scene = texture2D(u_TextureMap, var_TexCoords).rgb;
	if (u_ViewInfo.z > 0.5)
	{
		// whatever the weapon drew INSIDE the circle (the tube interior, the objective disc seen down the tube) is replaced by the finished picture again
		float restore = (1.0 - smoothstep(0.985, 1.0, r)) * smoothstep(0.55, 0.92, amt);
		vec3  base    = mix(scene, texture2D(u_ScreenDepthMap, var_TexCoords).rgb, restore);
		gl_FragColor = vec4(mix(base, metal, ringK), 1.0);
		return;
	}

	// ---- pass 0
	float lens   = 1.0 - smoothstep(0.985, 1.0, r);            // crisp edge
	float pr     = length(q - u_Color.zw) / max(u_HzmParams.x, 0.5);
	float pupil  = 1.0 - smoothstep(0.86, 1.0, pr);

	vec3  wide   = texture2D(u_LevelsMap, var_TexCoords).rgb;        // the wide world (no weapon)

	// inside the lens: the magnified main picture
	float r2   = min(r * r, 1.0);
	float bar  = 1.0 - 0.015 * r2;
	float ca   = min(u_HzmParams.z, 4.0) * 0.004 * r2;
	vec3  inside;
	inside.r = texture2D(u_TextureMap, vec2(0.5) + d * (bar + ca)).r;
	inside.g = texture2D(u_TextureMap, vec2(0.5) + d * bar).g;
	inside.b = texture2D(u_TextureMap, vec2(0.5) + d * (bar - ca)).b;
	float w  = u_HzmParams.y;
	inside  *= vec3(1.0 + 0.25 * w, 1.0 + 0.05 * w, 1.0 - w);
	inside  *= 1.0 - 0.45 * pow(smoothstep(0.62, 1.0, r), 1.6);   // soft inner shadow toward the rim (the tube wall)
	inside  *= 1.0 - 0.85 * smoothstep(0.972, 0.996, r);          // the glass edge goes dark into the ring

	// outside: the wide-angle world, out of focus and dim. The source is the 1/4 box-downsampled copy (each texel already averages 4 x 4 wide texels, so
	// the foliage speckle is gone before the blur); 24 Vogel taps rotated per pixel smooth the rest.
	vec3  outside = wide;
	if (amt > 0.0)
	{
		float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		vec3  b   = min(texture2D(u_ScreenImageMap, var_TexCoords).rgb, vec3(1.5)) * 0.04;
		for (int k = 0; k < 24; k++)
		{
			float rr = sqrt((float(k) + 0.5) / 24.0);
			float th = float(k) * 2.39996323 + ign * 6.2831853;
			b += min(texture2D(u_ScreenImageMap, var_TexCoords + vec2(cos(th) / aspect, sin(th)) * rr * 0.05).rgb, vec3(1.5)) * 0.04;
		}
		float rimShade = mix(0.62, 1.0, smoothstep(1.0, 1.9, r));   // the tube shadow: deepest at the ring, gone toward the screen edge
		outside = b * clamp(u_HzmParams.w, 0.0, 1.0) * rimShade;
	}

	// debug views (r_ppScopeCA): 10 = the wide view alone, 20 = the blurred outside, 30 = the main picture
	if (u_HzmParams.z > 5.0)
	{
		if (u_HzmParams.z < 15.0) { gl_FragColor = vec4(wide, 1.0); return; }
		if (u_HzmParams.z < 25.0) { gl_FragColor = vec4(outside, 1.0); return; }
		gl_FragColor = vec4(scene, 1.0); return;
	}

	vec3 shadowCol = vec3(0.012);                                     // the pupil shadow is the dark inside of the tube, never the world
	vec3 lensCol   = mix(shadowCol, inside, pupil);

	vec3  ring     = mix(wide, outside, amt);
	float lensFade = smoothstep(0.55, 0.92, amt) * lens;
	gl_FragColor   = vec4(mix(ring, lensCol, lensFade), 1.0);
}
