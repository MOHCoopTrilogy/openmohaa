// HZM coop [scopes1025] PICTURE-IN-PICTURE SCOPE - composite of the lens render into the wide scene.
//
// Runs at the top of the post chain on the HDR scene (before bloom / tone map), driven by cgame through r_ppScopeState.
//   u_TextureMap : the MAIN scene, rendered at the wide hip fov
//   u_LevelsMap  : the LENS scene, rendered along the same ray at the optic's magnified fov into an N x N square that
//                  covers the lens circle plus a 4 percent margin
//   inside       : the lens picture (slight barrel, radial chromatic fringe, glass tint, rim shading, crisp dark ring)
//   outside      : the wide world out of focus: two rings of taps, darkened, the tube shadow deepest at the rim
//   pupil        : the exit pupil shadow (sway / breathing / movement) darkens the LENS only, never the wide world
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;
uniform vec4      u_Color;      // (amount 0..1, lens radius / screen height, pupil x, pupil y) - pupil in lens radii, +y down
uniform vec4      u_HzmParams;  // (pupil radius in lens radii, glass warmth, chromatic amount, outside brightness 0..1)
uniform vec2      u_InvTexRes;  // 1 / scene size in texels

varying vec2      var_TexCoords;

vec3 lensAt(vec2 qq)
{
	// qq in lens radii, +y down; the square covers 2 * 1.04 lens radii
	return texture2D(u_LevelsMap, vec2(0.5 + qq.x / 2.08, 0.5 - qq.y / 2.08)).rgb;
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

	vec3 scene = texture2D(u_TextureMap, var_TexCoords).rgb;

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
	inside  *= 1.0 - 0.18 * pow(smoothstep(0.55, 1.0, r), 1.5);   // tube vignette toward the rim
	inside  *= 1.0 - 0.92 * smoothstep(0.965, 0.995, r);          // the scope ring: a thin dark crisp edge

	// ---- outside: the real wide-angle world, out of focus and dim
	vec3  outside = scene;
	if (amt > 0.0)
	{
		vec2  o1  = vec2(0.010 / aspect, 0.010);
		vec2  o2  = vec2(0.024 / aspect, 0.024);
		vec3  b   = scene * 0.12;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 1.0,  0.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-1.0,  0.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.0,  1.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.0, -1.0)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.7,  0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-0.7,  0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2( 0.7, -0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o1 * vec2(-0.7, -0.7)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.92,  0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.92,  0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.92, -0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.92, -0.38)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.38,  0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.38,  0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2( 0.38, -0.92)).rgb * 0.055;
		b += texture2D(u_TextureMap, var_TexCoords + o2 * vec2(-0.38, -0.92)).rgb * 0.055;
		float rimShade = mix(0.30, 1.0, smoothstep(1.0, 1.9, r));
		outside = b * clamp(u_HzmParams.w, 0.0, 1.0) * rimShade;
	}

	// debug views (r_ppScopeCA): 10 = the lens texture only, 20 = the blurred outside only, 30 = the plain wide scene
	if (u_HzmParams.z > 5.0)
	{
		if (u_HzmParams.z < 15.0) { gl_FragColor = vec4(texture2D(u_LevelsMap, var_TexCoords).rgb, 1.0); return; }
		if (u_HzmParams.z < 25.0) { gl_FragColor = vec4(outside, 1.0); return; }
		gl_FragColor = vec4(scene, 1.0); return;
	}

	vec3 shadowCol = outside * 0.15;
	vec3 lensCol   = mix(shadowCol, inside, pupil);
	vec3 col       = mix(outside, lensCol, lens);
	gl_FragColor   = vec4(mix(scene, col, amt), 1.0);
}
