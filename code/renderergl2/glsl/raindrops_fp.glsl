// HZM gl2 POST-FX PORT (bug-1150): byte-port of renderergl1's RAINDROPS_FS
// (tr_postprocess_gl1.c) - refractive water beads and vertical trickles on the lens while the
// player is out in the rain. Driven by the same levers: master r_ppRainDrops, strength
// r_ppRainAmount, and the per-frame wetness r_ppRainWet that the CGAME publishes (0..1 = rain
// intensity AND under open sky, eased so beads build and dry). The cgame is renderer-agnostic,
// so that signal is already being published when gl2 is the active renderer.
//
// gl1 uses three of its own uniforms; gl2 packs them into u_Color, which FBO_Blit sets as
// UNIFORM_COLOR. The name u_Color is load-bearing - see bug-1148.
//
// NOTE: no double quotes anywhere in this file - the stringify build step emits it as a C
// string literal without escaping, so a quote even inside a comment breaks the build.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (wet 0..1, time in seconds, aspect w/h, unused)

varying vec2      var_TexCoords;

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// one grid layer of beads -> returns (refraction dir .xy, bead mask .z)
vec3 dropLayer(vec2 uv, float cells, float speed, float t, float aspect)
{
	vec2 suv = vec2(uv.x * aspect, uv.y);          // square cells so beads are not oval
	vec2 g   = suv * cells;
	g.y -= t * speed;                              // the layer drifts DOWN (v increases upward here)

	vec2 id = floor(g);
	vec2 f  = fract(g) - 0.5;

	float present = step(0.55, h21(id + 11.3));    // ~45% of cells carry a bead
	vec2  c   = vec2((h21(id + 1.7) - 0.5) * 0.5, (h21(id + 5.1) - 0.5) * 0.5);
	float rad = 0.16 + 0.18 * h21(id + 3.9);
	float dd  = length(f - c);
	float drop = present * smoothstep(rad, rad * 0.3, dd);

	return vec3((f - c) * drop, drop);
}

// vertical TRICKLES: a drop head slides DOWN a column and leaves a fading wet trail above it
vec3 trickleLayer(vec2 uv, float cols, float t, float aspect)
{
	float colf = uv.x * aspect * cols;
	float col  = floor(colf);
	float seed = h21(vec2(col, 7.3));
	float present = step(0.62, h21(vec2(col, 2.1)));   // ~38% of columns carry a trickle
	float spd = 0.10 + seed * 0.22;
	float hy  = 1.0 - fract(t * spd + seed * 11.0);
	float cx  = col + 0.35 + 0.30 * h21(vec2(col, 5.0));
	float dx  = colf - cx;
	float xmask = smoothstep(0.42, 0.0, abs(dx));
	float above = uv.y - hy;
	float trail = smoothstep(0.16 + seed * 0.20, 0.0, above) * step(0.0, above);
	float head  = smoothstep(0.03, 0.0, abs(uv.y - hy));
	float amt   = present * xmask * max(trail * 0.45, head);

	return vec3(-dx * 0.5 * amt, head * 0.6 * amt, amt);
}

void main()
{
	float wet    = clamp(u_Color.x, 0.0, 1.0);
	float t      = u_Color.y;
	float aspect = u_Color.z;

	vec3 a = dropLayer(var_TexCoords,  8.0, 0.06, t,        aspect);
	vec3 b = dropLayer(var_TexCoords, 14.0, 0.10, t + 27.0, aspect);
	vec3 c = trickleLayer(var_TexCoords, 7.0, t, aspect);

	vec2  dir  = a.xy + b.xy * 0.8 + c.xy;
	float mask = clamp(a.z + b.z + c.z, 0.0, 1.0);
	vec2  off  = dir * vec2(1.0 / aspect, 1.0) * 0.05 * wet;   // refraction (undo aspect on x)

	vec3 col = texture2D(u_TextureMap, var_TexCoords + off).rgb;
	col += mask * 0.07 * wet;                                  // faint wet glint on beads + trickles

	gl_FragColor = vec4(col, 1.0);
}
