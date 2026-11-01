// HZM coop - visual filters (r_ppFilter 1..10), flipped with KP* / KP/ or the filter command.
// Runs last on the 3D scene only (the HUD is drawn afterwards). Shared look with gl1's FILTER_FS in
// tr_postprocess_gl1.c - keep the two in step. Names live in code/qcommon/hzm_filters.h.
//
// u_Color.x = filter (1..10), u_Color.y = time (s), u_Color.z = strength (0..1), u_Color.w = aspect (w/h)
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;

varying vec2      var_TexCoords;

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float vignette(vec2 uv, float amt)
{
	vec2 d = uv - 0.5;
	return 1.0 - amt * smoothstep(0.30, 0.78, length(d) * 1.1);
}

float grain(vec2 uv, float t)
{
	return h21(uv * 4096.0 + vec2(t * 97.0, t * 57.0)) - 0.5;
}

void main()
{
	float mode = floor(u_Color.x + 0.5);
	float t    = u_Color.y;
	vec2  uv   = var_TexCoords;
	float fr   = floor(t * 24.0); // film frame counter (24 fps)

	// old film: a little gate weave
	if (mode == 8.0) {
		uv.y += (h21(vec2(fr, 3.0)) - 0.5) * 0.0025;
		uv.x += (h21(vec2(fr, 7.0)) - 0.5) * 0.0012;
	}

	vec3  src = texture2D(u_TextureMap, var_TexCoords).rgb;
	vec3  c   = texture2D(u_TextureMap, uv).rgb;
	float l   = luma(c);
	float flick = h21(vec2(fr, 1.0)) - 0.5;

	if (mode == 1.0) {          // Newsreel: black and white, punchy, grain, flicker
		l = clamp((l - 0.5) * 1.25 + 0.5, 0.0, 1.0);
		l *= 1.0 + 0.05 * flick;
		l += grain(uv, t) * 0.10;
		c = vec3(l) * vignette(uv, 0.55);
	} else if (mode == 2.0) {   // Sepia
		vec3 s = vec3(l) * vec3(1.07, 0.88, 0.66) + vec3(0.05, 0.025, 0.0);
		c = s * vignette(uv, 0.40) + grain(uv, t) * 0.035;
	} else if (mode == 3.0) {   // Noir: hard black and white, crushed blacks, heavy vignette
		l = smoothstep(0.07, 0.85, l);
		l = pow(l, 1.15);
		l += grain(uv, t) * 0.06;
		c = vec3(l) * vignette(uv, 0.80);
	} else if (mode == 4.0) {   // Technicolor: rich, saturated, warm
		c = mix(vec3(l), c, 1.55);
		c = (c - 0.5) * 1.08 + 0.5;
		c *= vec3(1.04, 1.0, 0.93);
	} else if (mode == 5.0) {   // Bleach bypass: silver, desaturated, contrasty
		vec3 lum = vec3(l);
		vec3 lo  = 2.0 * c * lum;
		vec3 hi  = 1.0 - 2.0 * (1.0 - c) * (1.0 - lum);
		vec3 ov  = mix(lo, hi, step(0.5, l));
		c = mix(c, ov, 0.85);
		c = mix(vec3(luma(c)), c, 0.55);
	} else if (mode == 6.0) {   // Cold front: steel blue, desaturated
		c = mix(vec3(l), c, 0.70);
		c = c * vec3(0.88, 0.98, 1.12) + vec3(0.0, 0.01, 0.03);
		c = (c - 0.5) * 1.08 + 0.5;
		c *= vignette(uv, 0.25);
	} else if (mode == 7.0) {   // Golden hour: warm Normandy light
		c = mix(vec3(l), c, 1.12);
		c *= vec3(1.12, 1.0, 0.80);
		c = pow(clamp(c, 0.0, 1.0), vec3(0.95));
		c *= vignette(uv, 0.25);
	} else if (mode == 8.0) {   // Old film: faded, yellowed, grain, scratches, dust
		c = mix(vec3(l), c, 0.45);
		c = c * 0.80 + vec3(0.11, 0.085, 0.04);
		c *= 1.0 + 0.06 * flick;
		c += grain(uv, t) * 0.13;
		// one or two vertical scratches some frames
		for (int i = 0; i < 2; i++) {
			float fi = float(i);
			if (h21(vec2(fr, 11.0 + fi)) > 0.55) {
				float sx = h21(vec2(fr, 21.0 + fi));
				float w  = 0.0008 + 0.0012 * h21(vec2(fr, 31.0 + fi));
				float k  = 1.0 - smoothstep(0.0, w, abs(uv.x - sx));
				c = mix(c, vec3(0.92, 0.88, 0.78), k * 0.55);
			}
		}
		// dust specks
		{
			vec2  cell = floor(uv * vec2(220.0, 124.0));
			float d    = h21(cell + vec2(fr * 1.37, fr * 0.71));
			if (d > 0.9988) {
				c *= 0.25;
			}
		}
		c *= vignette(uv, 0.65);
	} else if (mode == 9.0) {   // Gun camera: black and white, soft, very grainy, dark corners
		vec2 px = vec2(0.0015, 0.0015 * u_Color.w);
		vec3 b  = c + texture2D(u_TextureMap, uv + vec2(px.x, 0.0)).rgb + texture2D(u_TextureMap, uv - vec2(px.x, 0.0)).rgb
		            + texture2D(u_TextureMap, uv + vec2(0.0, px.y)).rgb + texture2D(u_TextureMap, uv - vec2(0.0, px.y)).rgb;
		l = luma(b * 0.2);
		l = clamp((l - 0.5) * 1.35 + 0.5, 0.0, 1.0);
		l *= 1.0 + 0.08 * flick;
		l += grain(uv, t) * 0.18;
		c = vec3(l) * vignette(uv, 0.95);
	} else if (mode == 10.0) {  // Cinematic: teal shadows, warm highlights, 2.39:1 bars
		vec3 sh = c * vec3(0.86, 1.0, 1.08);
		vec3 hl = c * vec3(1.10, 1.0, 0.86);
		c = mix(sh, hl, smoothstep(0.2, 0.8, l));
		c = (c - 0.5) * 1.10 + 0.5;
		c *= vignette(uv, 0.20);
		if (u_Color.w < 2.38) {
			float bar = 0.5 * (1.0 - u_Color.w / 2.39);
			if (var_TexCoords.y < bar || var_TexCoords.y > 1.0 - bar) {
				c = vec3(0.0);
			}
		}
	} else {
		c = src;
	}

	c = clamp(c, 0.0, 1.0);
	// strength fades the look in; the cinema bars stay solid
	if (mode != 10.0) {
		c = mix(src, c, clamp(u_Color.z, 0.0, 1.0));
	}
	gl_FragColor = vec4(c, 1.0);
}
