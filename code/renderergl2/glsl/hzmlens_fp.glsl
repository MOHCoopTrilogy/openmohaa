// HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE - composite of the lens render into the wide scene.
//
// Runs at the top of the post chain on the HDR scene (before bloom / tone map), driven by cgame through r_ppScopeState.
//   u_TextureMap : the MAIN scene, rendered at the wide hip fov
//   u_LevelsMap  : the LENS scene, rendered along the same ray at the optic's magnified fov into an N x N square (N = 1.5 x the
//                  on-screen lens) that covers the lens circle plus a 4 percent margin
//   inside       : the lens picture - box-filtered down from the supersampled render (foliage speckle), slight barrel, radial
//                  chromatic fringe, glass tint, a soft inner shadow toward the rim
//   rifle        : the REAL scope body (eyepiece bell, tube, receiver) from the rifle layer, crisp and only a touch dimmed - it is the frame
//                  around the lens; everything that is not rifle is the world, blurred
//   outside      : the wide world out of focus: three rings of taps, dimmed, the tube shadow deepest at the ring
//   pupil        : the exit pupil shadow (sway / breathing / movement) darkens the LENS only, never the wide world
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;
uniform sampler2D u_ScreenImageMap;   // the RIFLE LAYER: the view weapon and arms alone, rendered at the main view's size
uniform sampler2D u_ScreenDepthMap;   // ...and its depth (< 1 = a rifle pixel)
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outside brightness 0..1)
uniform vec4      u_ViewInfo;   // (1 / lens texture size, 0, 0, 0)
uniform vec2      u_InvTexRes;  // 1 / scene size in texels

varying vec2      var_TexCoords;

// the lens texture at qq (lens radii, +y down; the square covers 2 * 1.04 lens radii), box-filtered over 2 x 2 source texels
vec3 lensAt(vec2 qq)
{
	vec2  uv = vec2(0.5 + qq.x / 2.08, 0.5 - qq.y / 2.08);
	float t  = 0.5 * u_ViewInfo.x;
	vec3  c  = texture2D(u_LevelsMap, uv + vec2(-t, -t)).rgb;
	c += texture2D(u_LevelsMap, uv + vec2( t, -t)).rgb;
	c += texture2D(u_LevelsMap, uv + vec2(-t,  t)).rgb;
	c += texture2D(u_LevelsMap, uv + vec2( t,  t)).rgb;
	return c * 0.25;
}

void main()
{
	float amt    = clamp(u_Color.x, 0.0, 1.0);
	float aspect = u_InvTexRes.y / u_InvTexRes.x;            // width / height
	vec2  d      = var_TexCoords - vec2(0.5);
	vec2  p      = d * vec2(aspect, -1.0);                     // screen-height units, +y down
	float R      = max(u_Color.y, 0.05);
	vec2  q      = p / R;                                      // lens radii
	float r      = length(q);

	float lens   = 1.0 - smoothstep(0.985, 1.0, r);            // crisp edge
	float pr     = length(q - u_Color.zw) / max(u_HzmParams.x, 0.5);
	float pupil  = 1.0 - smoothstep(0.86, 1.0, pr);

	vec3  scene  = texture2D(u_TextureMap, var_TexCoords).rgb;      // the world (the rifle is NOT in it while the layer is on)
	float gunDepth = texture2D(u_ScreenDepthMap, var_TexCoords).r;
	float gunA   = 1.0 - smoothstep(0.9990, 0.99995, gunDepth);       // rifle coverage
	vec3  gunCol = texture2D(u_ScreenImageMap, var_TexCoords).rgb;
	vec3  sharp  = mix(scene, gunCol, gunA);                           // the plain wide view with the rifle back in

	// ---- inside the lens
	float r2   = min(r * r, 1.0);
	float bar  = 1.0 - 0.015 * r2;
	float ca   = min(u_HzmParams.z, 4.0) * 0.004 * r2;
	vec3  inside;
	inside.r = lensAt(q * (bar + ca)).r;
	inside.g = lensAt(q * bar).g;
	inside.b = lensAt(q * (bar - ca)).b;
	float w  = u_HzmParams.y;
	inside  *= vec3(1.0 + 0.25 * w, 1.0 + 0.05 * w, 1.0 - w);
	inside  *= 1.0 - 0.45 * pow(smoothstep(0.62, 1.0, r), 1.6);   // soft inner shadow toward the rim (the tube wall)
	inside  *= 1.0 - 0.85 * smoothstep(0.972, 0.996, r);          // the glass edge goes dark into the ring

	// ---- outside: the real wide-angle world, out of focus and dim
	vec3  outside = scene;
	if (amt > 0.0)
	{
		// three rings of 8 taps (1.2, 2.8 and 5 percent of the screen height): a real out-of-focus blur, not a smear
		vec3  b   = scene * 0.10;
		for (int k = 0; k < 8; k++)
		{
			float an = 0.7853982 * float(k);
			vec2  dir = vec2(cos(an), sin(an));
			b += texture2D(u_TextureMap, var_TexCoords + dir * vec2(0.012 / aspect, 0.012)).rgb * 0.0375;
			b += texture2D(u_TextureMap, var_TexCoords + dir * vec2(0.028 / aspect, 0.028)).rgb * 0.0375;
			b += texture2D(u_TextureMap, var_TexCoords + dir * vec2(0.050 / aspect, 0.050)).rgb * 0.0375;
		}
		float rimShade = mix(0.42, 1.0, smoothstep(1.0, 1.9, r));   // the tube shadow: deepest at the ring, gone toward the screen edge
		outside = b * clamp(u_HzmParams.w, 0.0, 1.0) * rimShade;
	}
	outside = mix(outside, gunCol * 0.92, gunA);                        // the real scope body: crisp, barely dimmed

	// debug views (r_ppScopeCA): 10 = the lens texture only, 20 = the blurred outside only, 30 = the plain wide scene
	if (u_HzmParams.z > 5.0)
	{
		if (u_HzmParams.z < 15.0) { gl_FragColor = vec4(texture2D(u_LevelsMap, var_TexCoords).rgb, 1.0); return; }
		if (u_HzmParams.z < 25.0) { gl_FragColor = vec4(outside, 1.0); return; }
		gl_FragColor = vec4(sharp, 1.0); return;
	}

	vec3 shadowCol = vec3(0.012);                                     // the pupil shadow is the dark inside of the tube, never the world
	vec3 lensCol   = mix(shadowCol, inside, pupil);
	vec3 col       = mix(outside, lensCol, lens);

	gl_FragColor   = vec4(mix(sharp, col, amt), 1.0);
}
