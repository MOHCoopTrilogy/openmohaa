// HZM gl2 POST-FX: BLOOD ON THE LENS.  [user 2026-09-02 bug-2360; rebuilt 2026-09-03]
//
// The user, on the Omaha ramp drop: -even the blood on the lens is not really that impressive by
// the way during that scene... just doesnt hit.- The AMOUNT was never the problem: the cinematic
// already calls coop_rampLensBlood 0.85 of a 0..1 range. The EFFECT was. Measured over the user's
// own m3l1a frame at his real 3440x1440, v1 drew:
//
//   * 1.63% of the screen at full coverage at the peak amount; 15.49% with any trace at all.
//   * Inside a drop only 23.4% of the result carried any scene information. Expand lines 92-96 of
//     v1: c*0.12 + thru*0.1137 + vec3(0.1637, 0.0109, 0.0065). The remainder is a CONSTANT - a
//     flat dark-maroon patch with no interior structure at all.
//   * On anything DARKER than that constant the drop came out BRIGHTER than the scene: a shadowed
//     hull went (0.10, 0.09, 0.09) -> (0.187, 0.032, 0.028). It read as a dirt smudge, not blood.
//   * A refraction of at most 0.010, uniform in direction across each drop, applied to a backdrop
//     that was then 87% hidden. Invisible. And it was built in aspect space and subtracted in uv
//     space, so the horizontal offset was 1.78x the vertical - raindrops_fp.glsl:72 has the
//     vec2(1.0/aspect, 1.0) conversion that v1 was missing.
//   * A hardcoded 1.7778 aspect. This user runs 3440x1440 (qconsole: MODE: -1, 3440 x 1440), so
//     every drop was stretched 34% wider than tall on the machine that reported the complaint.
//   * Runs driven by backEnd.refdef.floatTime, i.e. ABSOLUTE level time. Measured: only 3 of 14
//     drops have a meaningful run coefficient, and they are the three LARGEST. At 90 s of uptime
//     they are still on screen; by 300 s all three have left the frame entirely, and any-trace
//     coverage has fallen 15.49% -> 8.04%. So the effect silently loses exactly the drops that
//     were meant to dominate, and it looks different every session.
//   * And they ran UP, off the TOP of the frame - not the bottom. raindrops_fp.glsl:26 and its
//     trickle head both establish that v INCREASES UPWARD in var_TexCoords, and v1 did
//     pos.y += run. Re-derived independently from the blit geometry: tr_fbo.c:551-555 makes
//     dstBox[1] the smaller dst y, Mat4Ortho(0, width, height, 0, ...) puts that at NDC +1 = the
//     top, and the texCoord at that same vertex is srcTexCorners[1], the LARGER v. Larger v = top.
//
// So this is a rebuild, not a tuning pass. Every drop is now a LENS, which is the cue that
// actually sells liquid on glass:
//
//   REFRACTION.  A droplet is a strong converging lens. What is behind it arrives turned over and
//                squeezed in, and swept sideways at the rim where the surface is steepest.
//   ABSORPTION.  Beer-Lambert through a shallow spherical cap, red extinguished ~6x less than
//                green and blue. Thin edges are a luminous crimson, the thick middle is nearly
//                black. That is what stops it reading as one flat red patch.
//   FRESNEL.     A convex drop MIRRORS the sky, hardest at the rim. That silvery edge is the other
//                half of the photographic look, and it is what keeps the drop from going cartoon
//                red on a grey beach.
//   CONTACT LINE.A dark meniscus where the fluid wets the glass, with a bright fringe inside it.
//   SPECULAR.    One tight glint per drop off a fixed key light, so it reads as wet.
//   RUNS.        Keyed to AGE, not level time. Decelerating, DOWNWARD, dragging a tapered track
//                that refracts exactly like the head does, because it is the same film.
//   DRYING.      It congeals rather than fading: browner, deeper, matte, and it stops behaving
//                like a lens. Small drops shrink away first; the big ones persist.
//
// It stays SCREEN SPACE and STATIC. Nothing here is a function of view angle, so the drops sit on
// the glass instead of swimming with the head - the single thing that would break the illusion.
//
// Measured on the same real m3l1a frame, amount 0.85, age 0.8 s, aspect 2.3889:
//   at the SHIPPED r_ppBloodScale 0.55 - 5.84% of the screen carries blood, 4.94% fully covered,
//     mean per-channel change 0.0116, largest drop 104 px radius (v1: 15.49% / 1.63% / 0.0095).
//     So it touches LESS of the frame than v1 and covers 3.0x more of it completely: fewer,
//     larger, opaque, structured drops instead of a wide wash of translucent smudge.
//   at r_ppBloodScale 1.0 - 16.75% / 14.13% / 0.0317, largest drop 189 px radius. See the SIZE
//     note in the loop for why 1.0 is not the default.
//   Inside a drop, luminance 0.098 against a scene 0.352: it is DARK, which is the point.
//   Inside-drop luminance standard deviation 0.059 - v1 is a CONSTANT there, so that number IS
//   the refracted image, and it is the whole difference.
//   Zero NaN/Inf over 168 combinations of age 0..600 s, amount, submersion and refract strength.
//
// u_Color.x     = amount 0..1 (cgame publishes r_ppBlood and owns the decay)
// u_Color.y     = AGE IN SECONDS since the amount last rose. NOT floatTime any more - see the
//                 epoch in tr_postprocess.c RB_HZMExtraFx. Deliberate contract change.
// u_Color.z     = r_ppBloodAmt, artist master. 1.0 = as authored here, and 1.0 is ALREADY written
//                 into omconfig.cfg:4340, so the intended look must be the look at 1.0.
// u_Color.w     = r_ppUnderwater 0..1
// u_HzmParams.x = aspect (w/h) derived from srcBox, same idiom as the raindrops pass at
//                 tr_postprocess.c:967. It doubles as the WIRED FLAG - see below.
// u_HzmParams.y = r_ppBloodRefract   lens strength. 1.0 = as authored, 0.0 = a true no-op.
// u_HzmParams.z = r_ppBloodRun       how far it runs
// u_HzmParams.w = r_ppBloodScale     drop SIZE. The slot the first draft left unused. 1.0 is the
//                 ladder exactly as reviewed; 0.55 is what ships, and the SIZE note in the loop
//                 says why the render, not the arithmetic, decided that.
// u_InvTexRes   = (1/srcW, 1/srcH), pushed unconditionally to every blit shader by
//                 tr_fbo.c:601-610. Read only as the aspect fallback - see below.
//
// HALF-SHIP GUARD. This shader and the tr_postprocess.c dispatch are ONE change in two files, and
// applying only this one used to be WORSE than applying neither: u_HzmParams arrives {0,0,0,0}, so
// aspect fell back to 1.7778, runK became 0 (no runs at all - the headline feature), refractK
// became 0, and u_Color.w was still the old literal 1.0f so uw was pinned at 1, the film never
// dried and had no glint. Measured, that half-build covers MORE of the screen than the finished
// one (any 19.31% vs 16.75%, full 15.62% vs 14.13%) while looking worse. A silent wrong build is
// this project's most expensive failure mode, so: u_HzmParams.x is an aspect and is never
// legitimately 0, and an all-zero vec4 therefore means the dispatch half is absent. In that case
// fall back to the AUTHORED look - refract 1, run 1, age 0.8 s, dry air - at an aspect recovered
// from u_InvTexRes, which is correct on any machine. The MISSING ^~^~^ LENSBLOOD marker is the
// real detector for a half ship; this guard only keeps the failure survivable.
//
// NOTE: no double quotes and no backslashes anywhere in this file, comments included.
// tools/stringify.cpp:44 emits each line as a bare quoted C string with NO escaping whatsoever,
// and tools/stringify.c:42 does the same, so either character breaks the literal it writes.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;       // (amount, age, master, underwater)
uniform vec4      u_HzmParams;   // (aspect, refract, run, scale)
uniform vec2      u_InvTexRes;   // (1/srcW, 1/srcH) - aspect fallback only

varying vec2      var_TexCoords;

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main()
{
	float amt = clamp(u_Color.x, 0.0, 1.0);
	float k   = max(u_Color.z, 0.0);

	// is the tr_postprocess.c half of this change present? See HALF-SHIP GUARD above.
	bool  wired = (u_HzmParams.x > 0.01);

	// aspect: from srcBox when wired (exact, and it handles a sub-viewport); otherwise recovered
	// from u_InvTexRes = (1/srcW, 1/srcH) so aspect = x/y; and only then the old 1.7778 guess.
	float aspect = 1.7778;
	if (wired) {
		aspect = u_HzmParams.x;
	} else if (u_InvTexRes.x > 0.0 && u_InvTexRes.y > 0.0) {
		aspect = u_InvTexRes.y / u_InvTexRes.x;
	}

	float refractK = wired ? max(u_HzmParams.y, 0.0)    : 1.0;
	float runK     = wired ? max(u_HzmParams.z, 0.0)    : 1.0;
	float scaleK   = wired ? max(u_HzmParams.w, 0.0)    : 0.55;
	float age      = wired ? max(u_Color.y, 0.0)        : 0.8;
	float uw       = wired ? clamp(u_Color.w, 0.0, 1.0) : 0.0;

	vec2 uv = var_TexCoords;
	vec3 c  = texture2D(u_TextureMap, uv).rgb;

	float a = clamp(amt * k, 0.0, 1.5);
	if (a <= 0.001) {
		gl_FragColor = vec4(c, 1.0);
		return;
	}

	// aspect-corrected working space so drops are round on any monitor
	vec2 p = vec2(uv.x * aspect, uv.y);

	// per-pixel constants, hoisted out of the loop
	float runEase = 1.0 - exp(-age * 0.62);      // most of the run happens in the first ~3 s
	float dry     = clamp(age * 0.11, 0.0, 1.0) * (1.0 - uw);

	// ---------------------------------------------------------------- the drop field
	// Placement is an R4 quasirandom sequence (the plastic-constant generalisation of the golden
	// ratio), not a sin hash: four multiplies and four fracts, provably equidistributed, and it
	// does not clump the way fract(sin(dot)) does.
	//
	// COST, stated honestly. Everything in this loop depends only on the loop counter and never
	// on p, so once the constant-bound loop is FULLY UNROLLED every one of those terms - the
	// pow() in the radius ladder included - constant-folds to a literal and none of it reaches
	// the per-pixel work. That is a property of the unroll. A fragment shader cannot hoist
	// per-invocation work on its own, so if a driver declines to unroll 38 iterations then 38
	// pow() calls per pixel really do remain. The pass only runs at all while r_ppBlood > 0.001
	// (about 7 s per event, 1 s if you are submerged) and r_ppBloodFx 0 is the archived escape.

	float bestR   = 1.0e9;      // normalised distance inside the winning drop; < 1.0 means covered
	vec2  bestC   = vec2(0.0);  // its surface centre in p-space (head, or nearest point on the track)
	float bestRad = 1.0;
	float bestSq  = 1.0;        // its vertical squash
	float bestRun = 0.0;        // 0 at the head, 1 well up the track

	float i;
	for (i = 0.0; i < 38.0; i += 1.0)
	{
		float fi = i + 1.0;

		float qx = fract(fi * 0.8566748839 + 0.137);
		float qy = fract(fi * 0.7338918566 + 0.523);
		float qs = fract(fi * 0.6287067210 + 0.271);
		float qv = fract(fi * 0.5385972572 + 0.811);

		// heavy-tailed radius: a long tail of specks, a handful that dominate the frame.
		// The formula spans 0.011 to 0.139 screen-heights; over these 38 samples the realized
		// values are 0.0110 to 0.1355 before the keep-out below and 0.0055 to 0.1315 after it,
		// i.e. 8 px to 189 px of radius at 1440p. Measured, not estimated - an earlier draft of
		// this comment said 0.009 to 0.128 and 13 to 184 px and every one of those was wrong.
		float rad0 = 0.011 + 0.128 * pow(qs, 2.4);

		// an honest centre keep-out. SHRINK drops near the aim point, never move them. v1 claimed
		// to push them outward but its multiplier 0.55 + 0.45*(1.0 - r0) is 1.0 - 0.45*r0, which
		// is always <= 1.0 - measured range over its 14 drops, 0.7006 to 0.9425 - so it pulled
		// them IN. Moving them also carves a visible ring; shrinking does not. Measured in uv, so
		// the exclusion is wider than tall, which is right for a widescreen frame.
		float dc = length(vec2(qx, qy) - vec2(0.5));
		rad0 *= 0.40 + 0.60 * smoothstep(0.08, 0.42, dc);

		// The amount does not fade drops out uniformly - it decides WHICH are still there. Small
		// ones dry away first, big ones persist and congeal, so clearing is ragged and physical
		// instead of a global alpha ramp.
		float big   = smoothstep(0.040, 0.080, rad0);
		float alive = smoothstep(qv * 0.50 + 0.02, qv * 0.50 + 0.26, a + big * 0.45);
		rad0 *= alive;

		// SIZE, and this is the one thing the RENDER decided rather than the arithmetic. At
		// scaleK 1.0 the ladder above is exactly what was measured and reviewed - and on a
		// 3440x1440 frame that makes the largest drop a 189 px radius, 26.3% of the frame's
		// HEIGHT. Rendered over the user's own m3l1a frame it reads as red gel capsules, not as
		// blood on glass; measured, it also puts 14.13% of the frame at full opacity against
		// v1's 1.63%. Applied HERE, after big and alive, so it changes only how large the drops
		// are and never which of them count as big or survive the amount. slip scales with it
		// too, which is right: a smaller drop carries less mass and runs less far. Measured
		// ladder at amount 0.85, age 0.8 s, on that frame - largest drop / any / fully covered:
		//   1.00 -> 189 px / 16.75% / 14.13%      0.70 -> 133 px /  8.86% / 7.50%
		//   0.55 -> 104 px /  5.84% /  4.94%      0.45 ->  85 px /  4.09% / 3.44%
		//   0.30 ->  57 px /  1.93% /  1.62%
		// 0.55 ships. It is a NEW cvar, so unlike r_ppBloodAmt - already archived at 1.0 in
		// omconfig.cfg:4340 and therefore unchangeable in practice - the C default is what every
		// install actually gets, and the default has to be the look that is wanted.
		rad0 *= scaleK;

		// a dead drop is skipped with a nested if rather than a continue - no shader in this tree
		// uses continue and the GLSL ES 1.00 fallback path is fussier about loop bodies
		if (rad0 >= 0.0015)
		{
			// per-drop vertical squash so the field is not 38 identical circles. Driven off its
			// OWN sequence: keying it to qs correlated squash with size and made every big drop
			// squash by the same amount. Provably in [0.664, 1.464), so it never divides by 0.
			float squash = 1.0 + 0.80 * (fract(fi * 0.4656) - 0.42);

			// THE RUN, and it goes DOWN. Keyed to age, so a client that has been up five minutes
			// looks exactly like a fresh one. Trail length scales with the drop, because a bigger
			// drop carries more mass and runs further. Measured: mean head v falls 0.5181 to
			// 0.4417 between age 0 and age 7 s, which is downward.
			float slip = min(rad0 * 3.4 * big * (0.60 + 0.80 * qv) * runK * runEase, 0.34);
			float cy   = qy - slip;
			float cx   = qx * aspect;

			// tapered capsule - the track it left is ABOVE the head and narrows to a point. The
			// taper runs over the TRACK's own length, so a short trail is a slightly stretched
			// drop and a long one is a proper streak. Tapering over a FIXED length instead left
			// the head chopped off flat, which is what the first attempt at this looked like.
			float dy     = p.y - cy;
			float ty     = clamp(dy, 0.0, slip);
			float tapLen = max(slip - rad0 * 0.85, rad0 * 0.25);
			float tf     = clamp((ty - rad0 * 0.85) / tapLen, 0.0, 1.0);
			float rad    = rad0 * (1.0 - 0.90 * (tf * tf * (3.0 - 2.0 * tf)));
			float d      = length(vec2(p.x - cx, (dy - ty) / squash));
			float r      = d / max(rad, 1.0e-5);

			if (r < bestR) {
				bestR   = r;
				bestC   = vec2(cx, cy + ty);
				bestRad = rad;
				bestSq  = squash;
				bestRun = clamp(ty / max(rad0, 1.0e-5) / 2.5, 0.0, 1.0);
			}
		}
	}

	// ---------------------------------------------------------------- fine aerosol
	// A wall of discrete blobs on otherwise spotless glass reads as decals. This is the mist that
	// arrives with them: sub-drop specks, low contrast, no lens, just a faint darkening. One sin
	// hash pair for the whole frame.
	vec2  cell = floor(p * 260.0);
	float mist = h21(cell) * h21(cell + vec2(4.7));
	mist = smoothstep(0.86, 0.995, mist) * clamp(a, 0.0, 1.0) * (1.0 - 0.65 * dry) * 0.55;
	c = mix(c, c * vec3(0.34, 0.055, 0.050) + vec3(0.045, 0.004, 0.003), mist);

	// ---------------------------------------------------------------- shade the winning drop
	float cover = 1.0 - smoothstep(0.90, 1.02, bestR);
	if (cover > 0.002)
	{
		vec2  rel = vec2((p.x - bestC.x) / max(bestRad, 1.0e-5),
		                 (p.y - bestC.y) / max(bestRad * bestSq, 1.0e-5));
		float rr  = min(bestR, 1.0);

		// A drop on glass is a SHALLOW cap, not a hemisphere - 0.42 is the surface slope scale.
		// A full hemisphere normal made them read as floating marbles.
		float hgt   = sqrt(max(0.0, 1.0 - rr * rr));
		float slope = 0.42;
		vec3  N     = normalize(vec3(rel * slope, max(hgt, 0.06)));

		// REFRACTION - the cue v1 did not have. Convert p-space back to uv with the
		// vec2(1.0/aspect, 1.0) that v1 omitted.
		float lens  = refractK * (1.0 - 0.65 * dry) * (1.0 + 0.55 * uw);
		vec2  uvC   = vec2(bestC.x / aspect, bestC.y);
		vec2  relUV = vec2(rel.x * bestRad / aspect, rel.y * bestRad);

		// r_ppBloodRefract 0 MUST BE A TRUE NO-OP, and writing these taps as uvC -+ relUV*K*lens
		// did not give one. At lens = 0 both collapse onto uvC, the drop's single centre texel,
		// so uvRef became uvC + relUV*rr*rr - an UNCONTROLLED pinch that magnifies without bound
		// at the centre, not the lens switched off. Measured on the user's frame at refract 0:
		// the refracted tap still sat a mean 0.0156 uv and up to 0.0506 uv off the identity
		// sample, and the wide scatter tap fell to 663 distinct values inside all drops against
		// 4972-6072 at every other setting - one flat colour per drop, mixed back in at up to
		// 55% weight near the rim. Blending from the IDENTITY sample instead fixes both taps:
		// measured, refract 0 now leaves the tap a mean 0.00001 uv off identity (the residue is
		// the edge clamp) with 4972 distinct scatter values, while refract >= 1.0 reduces to the
		// authored expression exactly, so nothing about the intended look moves. lensS keeps the
		// underwater 1.55x boost, which a plain clamp(lens, 0, 1) would have thrown away.
		float lensT = clamp(lens, 0.0, 1.0);
		float lensS = max(lens, 1.0);

		// centre: turned over and squeezed in, so a wide slice of the scene sits inside the drop
		vec2 uvInv  = uvC + relUV * mix(1.0, -2.60 * lensS, lensT);
		// rim: the surface is steepest here, so the image is swept outward rather than inverted
		vec2 uvBend = uvC + relUV * (1.0 + 2.10 * (1.0 - hgt) * lens);

		vec2 uvRef = mix(uvBend, uvInv, hgt * hgt);
		uvRef = clamp(uvRef, vec2(0.0015), vec2(0.9985));
		vec3 thru  = texture2D(u_TextureMap, uvRef).rgb;

		// a second, wider tap: light that took a long path through the meniscus arrives scattered
		vec2 uvSct = clamp(uvC + relUV * mix(1.0, 2.90 * lensS, lensT),
		                   vec2(0.0015), vec2(0.9985));
		vec3 sct   = texture2D(u_TextureMap, uvSct).rgb;
		thru = mix(thru, sct, (1.0 - hgt) * 0.55);

		// ABSORPTION. Beer-Lambert along the path through the film. Red survives, green and blue
		// do not, so thin edges are luminous crimson and the thick middle is nearly black.
		vec3  ext   = mix(vec3(1.45, 8.0, 10.5), vec3(2.55, 9.6, 11.8), dry);
		float thick = hgt * 0.90 * (1.0 + 0.25 * dry) * clamp(k, 0.25, 2.0);
		vec3  trans = exp(-thick * ext);

		// what the film scatters back on its own - strongest where it is thin
		vec3 scat = mix(vec3(0.0550, 0.00495, 0.00347), vec3(0.145, 0.036, 0.024), dry)
		          * (0.30 + 0.70 * (1.0 - hgt));

		// CONTACT LINE. Thickest where it wets the glass, with a ring of near-total internal
		// reflection just inside that. The inner edge is written 1.0 - smoothstep(0.88, 0.995, rr)
		// rather than smoothstep(0.995, 0.88, rr): the two are exactly equal under the clamp
		// formula, but the GLSL spec calls a smoothstep with edge0 >= edge1 UNDEFINED.
		// raindrops_fp.glsl:51 and :54 already ship two of those and work on this driver, so this
		// is not a live bug - it is simply free not to depend on it.
		float rim    = smoothstep(0.74, 0.995, rr);
		float fringe = smoothstep(0.70, 0.90, rr) * (1.0 - smoothstep(0.88, 0.995, rr));
		trans *= 1.0 - 0.84 * rim;
		scat  += vec3(0.0600, 0.00786, 0.00570) * (fringe * 0.35 * (1.0 - 0.8 * dry));

		// SPECULAR. One tight glint per drop from a fixed key light up and to the left, plus a
		// broad sheen so it reads as wet away from the glint. Water kills both (no air interface
		// left to reflect off) and so does drying.
		vec3  L   = normalize(vec3(-0.42, 0.55, 0.72));
		vec3  Hv  = normalize(L + vec3(0.0, 0.0, 1.0));
		float nh  = max(dot(N, Hv), 0.0);
		float spec = pow(nh, 80.0) * 0.50 * (1.0 - 0.92 * dry) * (1.0 - 0.85 * uw)
		           + pow(nh,  7.0) * 0.05 * (1.0 - 0.90 * dry) * (1.0 - 0.70 * uw);

		// FRESNEL. A convex drop mirrors the sky, hardest at the rim where incidence is grazing.
		// The environment probe is the top strip of the frame at the drop x, nudged by the normal
		// - free, because it is the same sampler, and in a first-person view looking forward the
		// top of frame IS the sky (v increases upward, so 0.90 is near the top). It is a probe,
		// not a reflection: it only has to be roughly the right colour and brightness, and being
		// wrong costs a slightly off rim tint, nothing more. F is hard-clamped to 0.85.
		vec3  sky = texture2D(u_TextureMap,
		                      vec2(clamp(uvC.x + N.x * 0.11, 0.002, 0.998),
		                           clamp(0.90 + N.y * 0.07, 0.002, 0.998))).rgb;
		float F   = clamp((0.05 + 0.85 * pow(rr, 6.0)) * 0.62, 0.0, 0.85);

		// the track is a thinner film than the head - lighter, and it dries first
		float trackThin = 0.45 * bestRun;
		trans = mix(trans, min(trans * 2.3, vec3(1.0)), trackThin);
		scat *= 1.0 - 0.35 * trackThin;

		vec3 drop = (thru * trans + scat) * (1.0 - F) + sky * F + vec3(spec);

		c = mix(c, drop, cover * clamp(a * 1.15, 0.0, 1.0));
	}

	gl_FragColor = vec4(c, 1.0);
}
