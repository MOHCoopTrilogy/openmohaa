// HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE - composite of the scope picture, the wide world and the rifle.
//
// Runs at the top of the post chain on the HDR scene (before bloom / tone map), driven by cgame through r_ppScopeState.
//   u_TextureMap : the MAIN scene = the MAGNIFIED scope picture (MSAA, sun shadows: the bug-3462 picture), without the rifle
//   u_LevelsMap  : the WIDE scene: the same ray at the hip fov, rendered as a second view (no rifle)
//   u_ScreenImageMap / u_ScreenDepthMap : the RIFLE LAYER: view weapon and arms alone at the hip projection (depth < 1 = rifle)
//   inside       : the scope picture - slight barrel, radial chromatic fringe, glass tint, a soft inner shadow toward the rim
//   outside      : the wide world out of focus (three rings of taps), dimmed, the tube shadow deepest at the lens
//   rifle        : the REAL scope body (eyepiece bell, tube, receiver, hands) crisp over the blurred world - the frame around the lens
//   pupil        : the exit pupil shadow (sway / breathing / movement) darkens the LENS only
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;
uniform sampler2D u_ScreenImageMap;
uniform sampler2D u_ScreenDepthMap;
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outside brightness 0..1)
uniform vec4      u_ViewInfo;   // (lens circle centre offset x, y in screen-height units, +y down: it follows the rifle's eyepiece)
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

	float lens   = 1.0 - smoothstep(0.985, 1.0, r);            // crisp edge
	float pr     = length(q - u_Color.zw) / max(u_HzmParams.x, 0.5);
	float pupil  = 1.0 - smoothstep(0.86, 1.0, pr);

	vec3  scope  = texture2D(u_TextureMap, var_TexCoords).rgb;       // the magnified picture (no rifle)
	vec3  wide   = texture2D(u_LevelsMap, var_TexCoords).rgb;        // the wide world (no rifle)
	float gunDepth = texture2D(u_ScreenDepthMap, var_TexCoords).r;
	float gunA   = 1.0 - smoothstep(0.9990, 0.99995, gunDepth);       // rifle coverage
	vec4  gunTex = texture2D(u_ScreenImageMap, var_TexCoords);
	vec3  gunCol = gunTex.rgb;
	gunA        *= smoothstep(0.55, 0.97, gunTex.a);                    // the translucent glass writes depth but not full alpha: it keeps the world behind it
	vec3  sharp  = mix(wide, gunCol, gunA);                            // the plain wide view with the rifle in it

	// ---- inside the lens: the magnified main picture
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

	// ---- outside: the wide-angle world, out of focus and dim
	vec3  outside = wide;
	if (amt > 0.0)
	{
		// a 32-tap Vogel disk (5 percent of the screen height), rotated per pixel by interleaved gradient noise: a smooth out-of-focus blur,
		// the 24 fixed taps of the first version drew streaks and ghost copies at this radius
		float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		vec3  b   = wide * 0.04;
		for (int k = 0; k < 32; k++)
		{
			float rr = sqrt((float(k) + 0.5) / 32.0);
			float th = float(k) * 2.39996323 + ign * 6.2831853;
			b += texture2D(u_LevelsMap, var_TexCoords + vec2(cos(th) / aspect, sin(th)) * rr * 0.055).rgb * 0.03;
		}
		float rimShade = mix(0.62, 1.0, smoothstep(1.0, 1.9, r));   // the tube shadow: deepest at the ring, gone toward the screen edge
		outside = b * clamp(u_HzmParams.w, 0.0, 1.0) * rimShade;
	}
	float gunDim = mix(0.50, 0.92, smoothstep(1.0, 1.7, r));           // the bell right around the lens is in shadow, the rifle body below it is lit
	outside = mix(outside, gunCol * gunDim, gunA);                     // the real scope body: crisp

	// debug views (r_ppScopeCA): 10 = the wide view alone, 20 = the blurred outside + rifle, 30 = the plain wide view with the rifle, 40 = the main picture
	if (u_HzmParams.z > 5.0)
	{
		if (u_HzmParams.z < 15.0) { gl_FragColor = vec4(wide, 1.0); return; }
		if (u_HzmParams.z < 25.0) { gl_FragColor = vec4(outside, 1.0); return; }
		if (u_HzmParams.z < 35.0) { gl_FragColor = vec4(sharp, 1.0); return; }
		gl_FragColor = vec4(scope, 1.0); return;
	}

	vec3 shadowCol = vec3(0.012);                                     // the pupil shadow is the dark inside of the tube, never the world
	vec3 lensCol   = mix(shadowCol, inside, pupil);

	// the world / rifle layer cross-fades with the whole amount; the LENS picture saturates earlier, so a steady scope never shows a trace of
	// the wide view through the glass
	vec3 ring      = mix(sharp, outside, amt);
	float lensFade = smoothstep(0.55, 0.92, amt) * lens;
	vec3 col       = mix(ring, lensCol, lensFade);

	// the eyepiece rim as a TRUE circle: dark anodised metal with a soft highlight arc and a soft outer falloff, over the faceted rifle mesh
	float band  = smoothstep(0.995, 1.004, r) * (1.0 - smoothstep(1.14, 1.26, r));
	vec2  qn    = q / max(r, 0.0001);
	float arc   = pow(max(dot(qn, normalize(vec2(-0.55, -0.83))), 0.0), 6.0);
	vec3  metal = vec3(0.018, 0.019, 0.022) + vec3(0.07) * arc * smoothstep(1.0, 1.06, r) * (1.0 - smoothstep(1.06, 1.16, r));
	col = mix(col, metal, band * smoothstep(0.55, 0.92, amt));

	gl_FragColor   = vec4(col, 1.0);
}
