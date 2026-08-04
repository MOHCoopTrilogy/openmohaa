// HZM coop - ON-HIT BLOOD  [user 08-02]
//
// Requested: We need light blood effects on screen/suppression effects when you get shot.
// Suppression already exists as its own pass (tunnel vignette + desaturate + peripheral blur);
// this is the separate, sharper cue for the moment a round actually lands on YOU.
//
// Design notes:
//  - PROCEDURAL, no texture. A splat texture would need a second sampler bound through the blit
//    path and a new art asset shipped in the pk3; the value-noise blobs below cost neither and
//    scale cleanly at any resolution.
//  - Weighted to the screen EDGES via the same vig term the low-health pass uses, so the centre
//    of the screen (where you are aiming) stays readable. This is a feedback cue, not an
//    obscuring effect - the same reason the low-health tint was toned down.
//  - Darkens and tints rather than painting bright red over the frame: blood on a lens reads as
//    a dark wet smear, and a pure additive red washes out in bright exteriors.
//  - u_Color.x is the hit level 0..1, published by cgame as r_ppHit and decayed there.
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (hit 0..1, unused, unused, unused)

varying vec2      var_TexCoords;

// cheap deterministic value noise - no texture fetch
float h21(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// one soft blob centred at c with radius r, returns 0..1 coverage
float blob(vec2 uv, vec2 c, float r)
{
	float d = length((uv - c) * vec2(1.0, 0.62));   // squash vertically - drips read as runs
	return 1.0 - smoothstep(r * 0.35, r, d);
}

void main()
{
	float hit = clamp(u_Color.x, 0.0, 1.0);
	vec3  c   = texture2D(u_TextureMap, var_TexCoords).rgb;

	if (hit <= 0.001) {
		gl_FragColor = vec4(c, 1.0);
		return;
	}

	vec2  uv  = var_TexCoords;
	vec2  d   = uv - vec2(0.5);
	float vig = clamp(dot(d, d) * 2.4, 0.0, 1.0);   // 0 centre -> 1 edges

	// A handful of splats pinned to fixed pseudo-random spots so they do not crawl frame to
	// frame - only their STRENGTH varies with hit, which is what makes it read as one impact
	// rather than animated noise.
	float cover = 0.0;
	for (int i = 0; i < 7; i++) {
		float fi = float(i);
		vec2  sc = vec2(h21(vec2(fi, 1.0)), h21(vec2(fi, 2.0)));
		// push the splats outward, away from the aiming centre
		sc = vec2(0.5) + (sc - vec2(0.5)) * 1.55;
		float sr = 0.055 + 0.075 * h21(vec2(fi, 3.0));
		cover += blob(uv, sc, sr * (0.55 + 0.45 * hit));
	}
	cover = clamp(cover, 0.0, 1.0);

	// splats only really appear toward the edges, and only as hard as the hit was
	float amt = cover * hit * (0.35 + 0.65 * vig);

	// wet, dark arterial red - multiply-biased so it darkens rather than glows
	vec3 blood = vec3(0.42, 0.03, 0.035);
	c = mix(c, c * blood * 2.0 + blood * 0.25, clamp(amt, 0.0, 1.0));

	// a light overall edge wash on top, so a hit registers even where no splat landed
	c = mix(c, c * vec3(0.85, 0.62, 0.60), hit * vig * 0.45);

	gl_FragColor = vec4(c, 1.0);
}
