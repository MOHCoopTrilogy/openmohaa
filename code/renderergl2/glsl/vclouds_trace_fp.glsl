// HZM gl2 volumetric clouds (docs/proposals/volumetric_clouds_2026-09-26/plan.md 2.4 step 1): the half-res trace.
// A line-for-line port of the P0 twin p0_stills/tools/vc_render.py trace_clouds / density_at / density_light /
// dome_st_pv / pv_st_dir (V1 compares the two). Profile constants arrive as VCL_* defines (tr_hzm_clouds.c).
// Built only on GLSL >= 1.30 (textureLod and texelFetch in loops), TRY-compiled: a failure turns the feature off.
// NOTE: no double quotes anywhere in this file - tools/stringify embeds it verbatim.

uniform sampler2D u_VclCov;       // private coverage copy, R8, REPEAT, LINEAR_MIPMAP_LINEAR, explicit mips
uniform sampler2D u_VclCovMax;    // conservative max-filtered mips of the same (NEAREST), for the empty-ray skip
uniform sampler3D u_VclNoise;     // 64^3 R8 Worley fBm, REPEAT, trilinear + mips
uniform sampler2D u_VclVerts;     // 9 x (9*6) RG32F: today's dome (s,t) at the 9x9 vertices of each box face

uniform vec4 u_VclRight;          // xyz view right, w tan(fovX/2)
uniform vec4 u_VclUp;             // xyz view up,    w tan(fovY/2)
uniform vec4 u_VclFwd;            // xyz view fwd,   w trace pixel angle (rad)
uniform vec4 u_VclScroll;         // x,y wrapped scroll offsets in scaled texture units, z debug mode, w 1/traceH
uniform vec4 u_VclView;           // x trace W, y trace H, z 1/W, w unused

#define R_DOME 4096.0
#define H_OLD  512.0

// v1.1 (2026-10-05) VCL_PROC: the PROCEDURAL profile for painted boxes (tr_hzm_clouds.c R_HZM_CloudsProcUniforms). The
// march below is shared; the per-map constants become uniforms, coverage is the generated 512^2 tileable rank-normalised
// map (a threshold CLO covers 1 - CLO of the sky) under a planar wind mapping, and a fully fogged pixel exits at once.
// Without VCL_PROC every line compiles exactly as the africanight P1 trace (the V1 twin).
#ifdef VCL_PROC
uniform vec4 u_VclProcA;          // x CLO, y CHI (coverage ramp), z SIGMA (extinction), w top erosion (base 0.55 w)
uniform vec4 u_VclProcB;          // x LMAX (knee), y 1 / coverage tile (dome units), z noise repeats per tile, w unused
uniform vec4 u_VclLdir;           // xyz toward the sun / moon (world), w SILVER: forward-scatter rim on thin cloud
uniform vec4 u_VclLcol;           // rgb its light on the cloud, w DEEP: weight of the deep multiple-scatter octave
uniform vec4 u_VclAmbTop;         // rgb sky light from above (the box zenith), w SHRINK: outline narrowing with height
uniform vec4 u_VclAmbBot;         // rgb sky light from the side / below (the box horizon), w POWDER 0..1
uniform vec4 u_VclFog;            // x fog-layer height H, y fog start, z 1/(end-start), w fraction scale (0 = no fog)
uniform vec4 u_VclProcC;          // x outline-billow frequency (per tile), y base crispness 0..1, z BASE1, w TOPK
#define VCL_CLO u_VclProcA.x
#define VCL_CHI u_VclProcA.y
#define VCL_SIGMA u_VclProcA.z
#define VCL_EROS0 (0.55 * u_VclProcA.w)
#define VCL_EROS1 u_VclProcA.w
#define VCL_LMAX u_VclProcB.x
#define VCL_LDIR u_VclLdir.xyz
#define VCL_LCOL u_VclLcol.rgb
#define VCL_AMBTOP u_VclAmbTop.rgb
#define VCL_AMBBOT u_VclAmbBot.rgb
#define VCL_BASE1 u_VclProcC.z
#define VCL_SHRINK u_VclAmbTop.w
#define VCL_TOPK u_VclProcC.w
// v2 shape (look-dev twin tools/vc11_twin.py): a cumulus base is FLAT and crisp, its top billows
#define VCL_EDGEB (VCL_EDGE * (1.0 - u_VclProcC.y))
#else
#define VCL_EDGEB VCL_EDGE
#endif

float vclSmooth(float a, float b, float x)
{
	float t = clamp((x - a) / (b - a), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

float vclHg(float c, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * c, 1.5);
}

float vclDomeHit(vec3 d, float h)
{
	float b = R_DOME * d.z;
	return -b + sqrt(b * b + 2.0 * R_DOME * h + h * h);
}

// today's per-triangle dome mapping (tr_sky.c R_InitSkyTexCoords + FillCloudySkySide): face by major axis, 9x9 grid,
// triangle split (s,t),(s,t+1),(s+1,t) | (s,t+1),(s+1,t+1),(s+1,t) chosen by fs + ft <= 1. Returns radians.
vec2 vclPvDir(vec3 d)
{
	vec3 a = abs(d);
	int face;
	float s, t;
	if (a.x >= a.y && a.x >= a.z)
	{
		if (d.x > 0.0) { face = 0; s = -d.y / a.x; t = d.z / a.x; }
		else           { face = 1; s =  d.y / a.x; t = d.z / a.x; }
	}
	else if (a.y >= a.z)
	{
		if (d.y > 0.0) { face = 2; s =  d.x / a.y; t = d.z / a.y; }
		else           { face = 3; s = -d.x / a.y; t = d.z / a.y; }
	}
	else
	{
		if (d.z > 0.0) { face = 4; s = -d.y / a.z; t = -d.x / a.z; }
		else           { return vec2(-1.0); }
	}
	float gs = (s + 1.0) * 4.0;
	float gt = (t + 1.0) * 4.0;
	int si = int(clamp(floor(gs), 0.0, 7.0));
	int ti = int(clamp(floor(gt), 0.0, 7.0));
	float fs = gs - float(si);
	float ft = gt - float(ti);
	int row = face * 9;
	if (fs + ft <= 1.0)
	{
		vec2 v00 = texelFetch(u_VclVerts, ivec2(si,     row + ti),     0).rg;
		vec2 v10 = texelFetch(u_VclVerts, ivec2(si + 1, row + ti),     0).rg;
		vec2 v01 = texelFetch(u_VclVerts, ivec2(si,     row + ti + 1), 0).rg;
		return v00 + fs * (v10 - v00) + ft * (v01 - v00);
	}
	vec2 v11 = texelFetch(u_VclVerts, ivec2(si + 1, row + ti + 1), 0).rg;
	vec2 v01b = texelFetch(u_VclVerts, ivec2(si,     row + ti + 1), 0).rg;
	vec2 v10b = texelFetch(u_VclVerts, ivec2(si + 1, row + ti),     0).rg;
	return v11 + (1.0 - fs) * (v01b - v11) + (1.0 - ft) * (v10b - v11);
}

// q = point relative to the dome centre (0,0,-R): coverage is constant along the radial line -> the point on the 512
// sphere, its EYE direction, today's mapping, then the tcMods (scroll then scale; offsets pre-wrapped on the CPU)
#ifdef VCL_PROC
// procedural: the horizontal position in the layer (q.xy is eye-relative: the dome centre is straight below the eye),
// in coverage tiles, plus the wind offset (wrapped on the CPU)
vec2 vclSt(vec3 q)
{
	return q.xy * u_VclProcB.y + u_VclScroll.xy;
}
#define VCL_TEXPERDU (VCL_COVN * u_VclProcB.y)
#define VCL_NOISEPERDU (VCL_NOISEN * u_VclProcB.y * u_VclProcB.z)
#define VCL_NOISEST(st) vec2((st).x * u_VclProcB.z, (st).y * u_VclProcB.z)
#else
vec2 vclSt(vec3 q)
{
	vec3 p = q / length(q) * (R_DOME + H_OLD);
	p.z -= R_DOME;
	vec2 st0 = vclPvDir(p);
	return vec2(st0.x * VCL_SCS + u_VclScroll.x, st0.y * VCL_SCT + u_VclScroll.y);
}
#endif

// returns density; hn via out
float vclDensity(vec3 pos, float lodC, float lodN, bool withNoise, out float hn)
{
	vec3 q = vec3(pos.xy, pos.z + R_DOME);
	float r = length(q);
	hn = (r - (R_DOME + VCL_HB)) / (VCL_HT - VCL_HB);
	if (hn < 0.0 || hn > 1.0)
		return 0.0;
	vec2 st = vclSt(q);
	float cov = textureLod(u_VclCov, st, lodC).r;
#ifdef VCL_PROC
	// v2: the OUTLINE is broken into billows by a finer field before the threshold - smooth coverage alone reads as
	// cotton balls. Only lowers coverage, so the max-mip empty-ray skip stays exact.
	if (u_VclProcB.w > 0.0)
	{
		float dn = textureLod(u_VclNoise, vec3(st * u_VclProcC.x, hn * 0.6), lodN + log2(u_VclProcC.x / u_VclProcB.z)).r;
		cov -= u_VclProcB.w * (1.0 - dn);
	}
#endif
	float c = (cov - VCL_CLO) / (VCL_CHI - VCL_CLO);
	if (c <= 0.0)
		return 0.0;
	c = min(c, 1.0);
	float cvs = textureLod(u_VclCov, st, lodC + VCL_SHAPELOD).r;
	float cs = clamp((cvs - VCL_CLO) / (VCL_CHI - VCL_CLO), 0.0, 1.0);
	cs = 0.5 * (cs + c);
	float base = VCL_BASE0 + (VCL_BASE1 - VCL_BASE0) * (1.0 - cs);
	float top = base + VCL_TOPK * pow(cs, VCL_TOPPOW);
	if (hn <= base || hn >= top)
		return 0.0;
	float prof = vclSmooth(base, base + VCL_EDGEB, hn) * (1.0 - vclSmooth(top - (top - base) * 0.45, top, hn));
#ifdef VCL_PROC
	// v3: the outline narrows with height inside the cloud - a dome, not an extruded prism (the in-engine m2l1 look
	// sweep, 2026-10-05). Only lowers c, so the empty-ray skip stays exact.
	c = clamp((cov - VCL_CLO - VCL_SHRINK * clamp((hn - base) / (top - base), 0.0, 1.0)) / (VCL_CHI - VCL_CLO), 0.0, 1.0);
#endif
	float d = prof * c;
	if (withNoise)
	{
#ifdef VCL_PROC
		float n = textureLod(u_VclNoise, vec3(VCL_NOISEST(st), hn * VCL_NZ), lodN).r;
#else
		float n = textureLod(u_VclNoise, vec3(st.x * VCL_NS, st.y * VCL_NT, hn * VCL_NZ), lodN).r;
#endif
		float er = VCL_EROS0 + (VCL_EROS1 - VCL_EROS0) * hn;
		float a = er * (1.0 - n);
		d = max((d - a) / (1.0 - a), 0.0);
	}
	return d;
}

// light-march density: ONE fetch, the blurred shape coverage (P_LIGHTCHEAP)
float vclDensityLight(vec3 pos, float lodC)
{
	vec3 q = vec3(pos.xy, pos.z + R_DOME);
	float r = length(q);
	float hn = (r - (R_DOME + VCL_HB)) / (VCL_HT - VCL_HB);
	if (hn < 0.0 || hn > 1.0)
		return 0.0;
	vec2 st = vclSt(q);
	float cvs = textureLod(u_VclCov, st, lodC + VCL_SHAPELOD).r;
	float cs = (cvs - VCL_CLO) / (VCL_CHI - VCL_CLO);
	if (cs <= 0.0)
		return 0.0;
	cs = min(cs, 1.0);
	float base = VCL_BASE0 + (VCL_BASE1 - VCL_BASE0) * (1.0 - cs);
	float top = base + VCL_TOPK * pow(cs, VCL_TOPPOW);
	if (hn <= base || hn >= top)
		return 0.0;
	return vclSmooth(base, base + VCL_EDGEB, hn) * (1.0 - vclSmooth(top - (top - base) * 0.45, top, hn)) * cs;
}

// exact empty-ray skip (plan 2.4): the max-filtered coverage over the padded (s,t) box of the ray; if it is at or below
// the threshold, every sample has c <= 0 and the march would return T = 1, L = 0 exactly
bool vclRayEmpty(vec3 d, float t0, float t1, float lodC)
{
	// the (s,t) path of the ray through the slab is a gentle curve: bound it by 5 samples, and the pad covers the rest
	vec2 lo = vec2(1e9);
	vec2 hi = vec2(-1e9);
	for (int k = 0; k < 5; k++)
	{
		vec2 p = vclSt(d * mix(t0, t1, float(k) * 0.25) + vec3(0.0, 0.0, R_DOME));
		lo = min(lo, p);
		hi = max(hi, p);
	}
	lo *= VCL_COVN;
	hi *= VCL_COVN;
	float pad = exp2(ceil(max(lodC, 0.0)) + 2.0) + 2.0;     // bilinear taps of the coarsest level sampled, + margin
	lo -= pad; hi += pad;
	float ext = max(hi.x - lo.x, hi.y - lo.y);
	float lvl = clamp(ceil(log2(max(ext, 1.0))), 0.0, VCL_COVMAXLEVELS - 1.0);
	float cell = exp2(lvl);
	ivec2 c0 = ivec2(floor(lo / cell));
	int nlev = int(VCL_COVN / cell);
	float mx = 0.0;
	for (int j = 0; j < 3; j++)
	{
		for (int i = 0; i < 3; i++)
		{
			ivec2 c = c0 + ivec2(i, j);
			c = ivec2(c.x - nlev * int(floor(float(c.x) / float(nlev))), c.y - nlev * int(floor(float(c.y) / float(nlev))));
			mx = max(mx, texelFetch(u_VclCovMax, c, int(lvl)).r);
		}
	}
	return mx <= VCL_CLO;
}

void main()
{
	float W = u_VclView.x;
	float H = u_VclView.y;
	float x = floor(gl_FragCoord.x);
	float yTop = H - 1.0 - floor(gl_FragCoord.y);   // the twin counts rows from the top
	// static interleaved-gradient step offset (Jimenez 2014), frame 0
	float ign = fract(52.9829189 * fract(0.06711056 * x + 0.00583715 * yTop));
	float cx = ((x + 0.5) / W * 2.0 - 1.0) * u_VclRight.w;
	float cy = (1.0 - (yTop + 0.5) / H * 2.0) * u_VclUp.w;
	vec3 d = normalize(u_VclFwd.xyz + cx * u_VclRight.xyz + cy * u_VclUp.xyz);

	vec4 outc = vec4(0.0, 0.0, 0.0, 1.0);
	float elev = degrees(asin(clamp(d.z, -1.0, 1.0)));
	if (elev <= VCL_FADE0)
	{
		gl_FragColor = outc;
		return;
	}
#ifdef VCL_PROC
	// fully fogged sky (the sky fog model, tr_shade.c RB_HZM_SkyFogUniforms): nothing would show - skip the march
	if (u_VclFog.w > 0.0 && clamp((u_VclFog.x / max(d.z, 0.0001) - u_VclFog.y) * u_VclFog.z, 0.0, 1.0) * u_VclFog.w >= 0.999)
	{
		gl_FragColor = outc;
		return;
	}
#endif
	float t0 = vclDomeHit(d, VCL_HB);
	float t1 = vclDomeHit(d, VCL_HT);
	float dt = (t1 - t0) / VCL_STEPS;
	float pix = u_VclFwd.w;
#ifdef VCL_PROC
	if (vclRayEmpty(d, t0, t1, log2(max(t1 * pix * VCL_TEXPERDU, 1e-6))))
#else
	float lodC0 = log2(max(t0 * pix * VCL_COVN * VCL_SCS / (R_DOME + H_OLD), 1e-6));
	if (vclRayEmpty(d, t0, t1, log2(max(t1 * pix * VCL_COVN * VCL_SCS / (R_DOME + H_OLD), 1e-6))))
#endif
	{
		gl_FragColor = outc;
		return;
	}
	vec3 Ld = VCL_LDIR;
	float cth = dot(d, Ld);
	float ph0 = (1.0 - VCL_GW) * vclHg(cth, VCL_G0) + VCL_GW * vclHg(cth, VCL_G1);
	float ph1 = (1.0 - VCL_GW) * vclHg(cth, VCL_G0 * VCL_MSC) + VCL_GW * vclHg(cth, VCL_G1 * VCL_MSC);
	float T = 1.0;
	vec3 L = vec3(0.0);
	float sig = VCL_SIGMA;
	if (d.z < VCL_ELEVREF_SIN)
		sig = sig * d.z / VCL_ELEVREF_SIN;
	float thick = VCL_HT - VCL_HB;
	float lz = max(Ld.z, 0.25);
	float lsteps = 0.0;
	float msLast = 0.0;
	float lmarch = 0.0;
	float steps = 0.0;
	for (int i = 0; i < VCL_STEPS_I; i++)
	{
		float tt = t0 + (float(i) + ign) * dt;
		float foot = tt * pix;
#ifdef VCL_PROC
		float lodC = log2(max(foot * VCL_TEXPERDU, 1e-6));
		float lodN = log2(max(foot * VCL_NOISEPERDU, 1e-6));
#else
		float lodC = log2(max(foot * VCL_COVN * VCL_SCS / (R_DOME + H_OLD), 1e-6));
		float lodN = log2(max(foot * VCL_NOISEN / 768.0, 1e-6));
#endif
		vec3 pos = d * tt;
		float hn;
		float dens = vclDensity(pos, lodC, lodN, true, hn);
		steps += 1.0;
		if (dens <= 0.0)
			continue;
		lsteps += 1.0;
		float st = dens * sig;
		bool capped = lsteps > VCL_CAPLIT;
		float wlight = vclSmooth(0.15, 0.35, T);
		if (wlight <= 0.0)
			capped = true;
		float ms;
		if (capped)
		{
			ms = msLast;
		}
		else
		{
			float tau = 0.0;
			float prev = 0.0;
			for (int k = 0; k < 4; k++)
			{
				float dist = (k < 3) ? thick * (0.10 + 0.55 * (float(k) + 0.5) / 3.0) / lz : thick * 1.6 / lz;
				float seg = dist - prev;
				prev = dist;
				float dl = vclDensityLight(pos + Ld * dist, lodC + 1.0);
				tau += dl * sig * seg;
			}
			float bl = exp(-tau);
			ms = ph0 * bl + VCL_MSB * ph1 * exp(-tau * VCL_MSA);
#ifdef VCL_PROC
			// look-dev v4 (2026-10-12): a third, near-isotropic octave. Two octaves leave a thick cumulus base at ~4 % of
			// its top - a slate-grey blot seen from the ground, which is the only side a player ever sees. DEEP 0 = v3.
			ms += u_VclLcol.w * exp(-tau * 0.09);
			// SILVER: thin cloud between the eye and the sun scatters forward - the bright rim of a backlit cumulus
			ms += u_VclLdir.w * pow(max(cth, 0.0), 6.0) * exp(-tau * 0.5);
#endif
			if (lmarch > 0.0)
				ms = msLast + (ms - msLast) * wlight;
			msLast = ms;
			lmarch += 1.0;
		}
#ifdef VCL_PROC
		// POWDER: the first skin of a cloud is darker than its body (less in-scatter) - reads as depth in the billows
		float pw = 1.0 - u_VclAmbBot.w * exp(-2.0 * dens * sig * thick * 0.25);
		vec3 S = st * VCL_ALB * (VCL_LCOL * ms * pw + VCL_AMBBOT + (VCL_AMBTOP - VCL_AMBBOT) * hn);
#else
		vec3 S = st * VCL_ALB * (VCL_LCOL * ms + VCL_AMBBOT + (VCL_AMBTOP - VCL_AMBBOT) * hn);
#endif
		float Tr = exp(-st * dt);
		L += T * S * ((1.0 - Tr) / st);
		T *= Tr;
		if (T < VCL_TMIN)
			break;
	}
	// knee shoulder at the painted glow: identity below LMAX/2
	float lm = dot(L, vec3(0.299, 0.587, 0.114));
	float kn = 0.5 * VCL_LMAX;
	if (lm > kn)
	{
		float lm2 = kn + (VCL_LMAX - kn) * (1.0 - exp(-(lm - kn) / (VCL_LMAX - kn)));
		L *= lm2 / lm;
	}
	float fade = vclSmooth(VCL_FADE0, VCL_FADE1, elev);
	outc = vec4(L * fade, 1.0 - (1.0 - T) * fade);

	if (u_VclScroll.z > 0.5)   // r_hzmCloudsDebug: 1 over black is the composite's job; 3 = steps heatmap here
	{
		if (u_VclScroll.z > 2.5 && u_VclScroll.z < 3.5)
			outc = vec4(steps / VCL_STEPS, lmarch / 8.0, lsteps / VCL_STEPS, 1.0);
	}
	gl_FragColor = outc;
}
