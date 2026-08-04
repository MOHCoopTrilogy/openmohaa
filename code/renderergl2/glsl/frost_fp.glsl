// HZM gl2 NEW POST-FX (bug-1158): frost/ice crystals on the lens while it snows - the visual
// counterpart to raindrops_fp.glsl, on the branch the rain effect deliberately excludes (the
// precip system drives both rain and snow off the same cg.rain.density/speed pair; snow is the
// slow branch, gated in cg_view.c and published as r_ppFrostAmt, eased the same way r_ppRainWet
// is). Fern-like crystal tendrils grow in from the screen edges as the amount rises, plus a faint
// cool desaturation and the occasional glint - all procedural, no texture.
//
// u_Color.x = amount (0..1, eased snow accumulation), u_Color.y = time (seconds, for the glints)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (amount, time, unused, unused)

varying vec2      var_TexCoords;

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// value noise: smooth-interpolated hash grid, used at increasing frequency per octave to build
// fern-like branching structure rather than a flat frosted-glass blur
float vnoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);

	float a = h21(i);
	float b = h21(i + vec2(1.0, 0.0));
	float c = h21(i + vec2(0.0, 1.0));
	float d = h21(i + vec2(1.0, 1.0));

	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main()
{
	float amt = clamp(u_Color.x, 0.0, 1.0);
	float t   = u_Color.y;

	vec3 c = texture2D(u_TextureMap, var_TexCoords).rgb;

	if (amt > 0.001) {
		// distance from the nearest screen edge (0 at the border, ~0.5 at centre) - frost grows
		// in from here, so the crystal FIELD is edge-weighted before the noise is even sampled
		vec2  toEdge  = min(var_TexCoords, vec2(1.0) - var_TexCoords);
		float edgeDist = min(toEdge.x, toEdge.y);

		// three octaves of branching value noise, each finer and fainter - fbm shaped toward
		// jagged fern-like crystal tendrils rather than a smooth blob
		float n = 0.0;
		n += vnoise(var_TexCoords * 9.0)  * 0.55;
		n += vnoise(var_TexCoords * 22.0) * 0.30;
		n += vnoise(var_TexCoords * 48.0) * 0.15;

		// crystals reach further in as amt rises (edgeDist threshold grows with amt), and the
		// noise field itself only READS as crystal above a threshold (sharp-ish edges, not fog)
		float reach    = amt * 0.62;
		float edgeMask = smoothstep(reach, reach - 0.10, edgeDist);
		float crystal  = smoothstep(0.42, 0.78, n) * edgeMask * amt;

		vec3 frostColor = vec3(0.88, 0.95, 1.0);
		c = mix(c, frostColor, clamp(crystal, 0.0, 0.9));

		// faint overall cool desaturation even where the crystal mask hasn't reached, so full
		// accumulation reads as looking through a frosted pane rather than just edge decoration
		float l = dot(c, vec3(0.299, 0.587, 0.114));
		c = mix(c, mix(c, vec3(l), 0.4), amt * 0.25);

		// occasional sparkle glint on the crystal edges - cheap, just a high-frequency hash spike
		// gated to only fire where there is already crystal, animated by time
		float sparkleSeed = h21(floor(var_TexCoords * 180.0) + floor(t * 3.0));
		float sparkle = step(0.985, sparkleSeed) * crystal;
		c += sparkle * 0.5;
	}

	gl_FragColor = vec4(c, 1.0);
}
