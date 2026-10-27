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
uniform sampler2D u_SpecularMap;      // the optic's reticle (black RGBA over the lens square), drawn INSIDE the picture
uniform sampler2D u_ScreenDepthMap;   // pass 0: the ocular-glass coverage mask (white = the rifle's real glass); pass 1: the finished picture of pass 0, glass coverage in alpha
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outside brightness 0..1)
uniform vec4      u_ViewInfo;   // (lens circle centre offset x, y in screen-height units, +y down: it follows the rifle's eyepiece; pass mode)
uniform vec4      u_FogDistance; // (blur radius scale, 0, 0, 0)
uniform vec2      u_InvTexRes;  // 1 / scene size in texels

varying vec2      var_TexCoords;

// the glass coverage, softened over a 3x3 so the glass fan's 12 edges do not show as facets
float glassMask()
{
	vec2  px = u_InvTexRes * 1.6;
	float m  = texture2D(u_ScreenDepthMap, var_TexCoords).r * 4.0;
	m += texture2D(u_ScreenDepthMap, var_TexCoords + vec2(px.x, 0.0)).r + texture2D(u_ScreenDepthMap, var_TexCoords - vec2(px.x, 0.0)).r;
	m += texture2D(u_ScreenDepthMap, var_TexCoords + vec2(0.0, px.y)).r + texture2D(u_ScreenDepthMap, var_TexCoords - vec2(0.0, px.y)).r;
	m += texture2D(u_ScreenDepthMap, var_TexCoords + px).r + texture2D(u_ScreenDepthMap, var_TexCoords - px).r + texture2D(u_ScreenDepthMap, var_TexCoords + vec2(px.x, -px.y)).r + texture2D(u_ScreenDepthMap, var_TexCoords + vec2(-px.x, px.y)).r;
	return m / 12.0;
}

void main()
{
	float amt    = clamp(u_Color.x, 0.0, 1.0);
	float aspect = u_InvTexRes.y / u_InvTexRes.x;            // width / height
	vec2  d      = var_TexCoords - vec2(0.5);
	vec2  p      = d * vec2(aspect, -1.0);                     // screen-height units, +y down
	float R      = max(u_Color.y, 0.05);
	vec2  q      = (p - u_ViewInfo.xy) / R;                    // lens radii, from the lens centre (the eyepiece)
	float r      = length(q);
	float vw     = u_ViewInfo.w;
	bool  glass  = (mod(vw, 2.0) > 0.5);       // the rifle has a real glass surface (coverage mask)
	bool  adsMode = (vw > 1.5);                // 1: MAIN = the magnified picture, aux = the wide world; 0: MAIN = the hip view, aux = the magnified LENS picture

	// ---- the rim, pass 1
	float band  = smoothstep(0.995, 1.004, r) * (1.0 - smoothstep(1.50, 1.58, r));   // out to the bell's outer radius: its faceted silhouette is covered
	vec2  qn    = q / max(r, 0.0001);
	float arc   = pow(max(dot(qn, normalize(vec2(-0.55, -0.83))), 0.0), 6.0);
	vec3  metal = vec3(0.016, 0.017, 0.020) * mix(1.6, 0.6, smoothstep(1.0, 1.5, r)) + vec3(0.075) * arc * smoothstep(1.02, 1.10, r) * (1.0 - smoothstep(1.22, 1.45, r));
	float ringK = band;

	vec3  scene = texture2D(u_TextureMap, var_TexCoords).rgb;
	if (u_ViewInfo.z > 0.5)
	{
		// whatever the weapon drew INSIDE the circle (the tube interior, the objective disc seen down the tube) is replaced by the finished picture again
		vec4  pic     = texture2D(u_ScreenDepthMap, var_TexCoords);
		float gm      = pic.a;
		float restore = glass ? gm : (1.0 - smoothstep(0.985, 1.0, r));
		vec3  base    = mix(scene, pic.rgb, restore);
		gl_FragColor = vec4(mix(base, metal, glass ? 0.0 : ringK), 1.0);
		return;
	}

	// ---- pass 0
	float lens   = glass ? smoothstep(0.3, 0.7, glassMask()) : (1.0 - smoothstep(0.985, 1.0, r));   // the rifle's real glass surface, else the circle
	float pr     = length(q - u_Color.zw) / max(u_HzmParams.x, 0.5);
	float pupil  = 1.0 - smoothstep(0.86, 1.0, pr);

	vec3  mainC  = texture2D(u_TextureMap, var_TexCoords).rgb;
	vec3  auxC   = texture2D(u_LevelsMap, var_TexCoords).rgb;
	vec3  wide   = adsMode ? auxC : mainC;                            // the wide world (no weapon)

	// inside the lens: the magnified main picture
	float r2   = min(r * r, 1.0);
	float bar  = 1.0 - 0.015 * r2;
	float ca   = min(u_HzmParams.z, 4.0) * 0.004 * r2;
	vec3  inside;
	vec2  dl = d - vec2(u_ViewInfo.x / aspect, -u_ViewInfo.y);   // the picture rides with the lens (the rifle's real glass), not the screen centre
	vec2  c0 = vec2(0.5) + dl * (bar + ca), c1 = vec2(0.5) + dl * bar, c2 = vec2(0.5) + dl * (bar - ca);   // the magnified picture: MAIN in ADS mode, the aux lens view otherwise
	if (adsMode) { inside.r = texture2D(u_TextureMap, c0).r; inside.g = texture2D(u_TextureMap, c1).g; inside.b = texture2D(u_TextureMap, c2).b; }
	else         { inside.r = texture2D(u_LevelsMap, c0).r;   inside.g = texture2D(u_LevelsMap, c1).g;   inside.b = texture2D(u_LevelsMap, c2).b; }
	float w  = u_HzmParams.y;
	inside  *= vec3(1.0 + 0.25 * w, 1.0 + 0.05 * w, 1.0 - w);
	if (u_FogDistance.y > 0.5)   // the reticle is part of the lens image: centred on the glass, clipped to it, dimmed by the same shadows
	{
		vec2  ruv = vec2(0.5) + q * 0.5;
		vec2  ho  = vec2(u_InvTexRes.y / R) * 0.5 * 1.6;
		vec4  rt  = texture2D(u_SpecularMap, ruv);
		float hl  = max(max(texture2D(u_SpecularMap, ruv + vec2(ho.x, 0.0)).a, texture2D(u_SpecularMap, ruv - vec2(ho.x, 0.0)).a),
		                max(texture2D(u_SpecularMap, ruv + vec2(0.0, ho.y)).a, texture2D(u_SpecularMap, ruv - vec2(0.0, ho.y)).a));
		float ra = clamp(rt.a * 2.0, 0.0, 1.0);                       // the lines are thin: firm them up, BLACK
		inside = mix(inside, inside * 0.25, hl * (1.0 - ra) * 0.5);    // a faint dark halo only (keeps the line readable on bright scenes)
		inside = mix(inside, vec3(0.0), ra);
	}
	float rIn = (glass && !adsMode) ? r * 0.55 : r;   // the glass is seen at an angle at the hip: its real mask ends the picture, the circle's falloff must not darken it
	inside  *= 1.0 - 0.45 * pow(smoothstep(0.62, 1.0, rIn), 1.6);   // soft inner shadow toward the rim (the tube wall)
	inside  *= 1.0 - 0.85 * smoothstep(0.972, 0.996, rIn);          // the glass edge goes dark into the ring

	// outside: the wide-angle world, out of focus and dim. The source is the 1/4 box-downsampled copy (each texel already averages 4 x 4 wide texels, so
	// the foliage speckle is gone before the blur); 24 Vogel taps rotated per pixel smooth the rest.
	vec3  outside = wide;
	if (amt > 0.0)
	{
		float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		float br  = 0.05 * u_FogDistance.x;
		vec3  b   = min(adsMode ? texture2D(u_ScreenImageMap, var_TexCoords).rgb : mainC, vec3(1.5)) * 0.04;
		for (int k = 0; k < 24; k++)
		{
			float rr = sqrt((float(k) + 0.5) / 24.0);
			float th = float(k) * 2.39996323 + ign * 6.2831853;
			vec2  tc = var_TexCoords + vec2(cos(th) / aspect, sin(th)) * rr * br;
			b += min(adsMode ? texture2D(u_ScreenImageMap, tc).rgb : texture2D(u_TextureMap, tc).rgb, vec3(1.5)) * 0.04;
		}
		float rimShade = mix(0.62, 1.0, smoothstep(1.0, 1.9, r));   // the tube shadow: deepest at the ring, gone toward the screen edge
		outside = b * clamp(u_HzmParams.w, 0.0, 1.0) * rimShade;
	}

	// debug views (r_ppScopeCA): 10 = the wide view alone, 20 = the blurred outside, 30 = the main picture
	if (u_HzmParams.z > 5.0)
	{
		if (u_HzmParams.z < 15.0) { gl_FragColor = vec4(wide, 1.0); return; }
		if (u_HzmParams.z < 25.0) { gl_FragColor = vec4(outside, 1.0); return; }
		if (u_HzmParams.z < 35.0) { gl_FragColor = vec4(mainC, 1.0); return; }
		if (u_HzmParams.z < 45.0) { gl_FragColor = vec4(vec3(glassMask()), 1.0); return; }
		gl_FragColor = vec4(texture2D(u_ScreenImageMap, var_TexCoords).rgb, 1.0); return;
	}

	vec3 shadowCol = vec3(0.012);                                     // the pupil shadow is the dark inside of the tube, never the world
	vec3 lensCol   = mix(shadowCol, inside, pupil);

	vec3  ring     = mix(wide, outside, amt);
	float lensFade = lens;   // the live magnified picture is on the glass ALWAYS (hip, raise, bolt, reload)
	gl_FragColor   = vec4(mix(ring, lensCol, lensFade), lens);   // alpha = the lens / glass coverage, for the restore in pass 1
}
