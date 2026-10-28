// HZM gl2 POST-FX PORT (bug-1151): byte-port of renderergl1's HEATHAZE_FS
// (tr_postprocess_gl1.c). A fullscreen animated shimmer the cgame spikes near explosions
// (r_ppHeat), plus a tight LOCALIZED ripple around the gun muzzle (r_ppMuzzleHeat at
// r_ppMuzzleX/Y with r_ppMuzzleRadius) so gunfire heat reads as rising off the barrel instead of
// warping the whole frame. Master switch r_ppHeatHaze, strength r_ppHeatAmount.
//
// u_Color carries (heat, time, muzzleHeat, muzzleRadius); the muzzle screen point rides
// u_HzmParams.xy, the spare vec4 FBO_Blit pushes for these ported stages.
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;      // (heat, time seconds, muzzleHeat, muzzleRadius)
uniform vec4      u_HzmParams;  // (muzzleX, muzzleY, aspect w/h, depthOk); centre in UV, radius = fraction of height
uniform sampler2D u_LevelsMap;  // scene depth (TB_LEVELSMAP), only read when u_HzmParams.w > 0.5

uniform vec4      u_HzHaze0, u_HzHaze1, u_HzHaze2, u_HzHaze3, u_HzHaze4, u_HzHaze5, u_HzHaze6, u_HzHaze7;   // fx_1025 spots: (u, v, radius/height, strength)

varying vec2      var_TexCoords;

// HZM fx_1025: one localized, one-sided, rising shimmer spot (same shape as the muzzle one) - fires, wrecks, blasts
vec2 hzSpot(vec4 sp, float t, float aspect)
{
	if (sp.w <= 0.001) {
		return vec2(0.0);
	}
	vec2  dm = (var_TexCoords - sp.xy) * vec2(aspect, 1.0);
	float R  = max(sp.z, 0.001);
	float dy = -dm.y;                                   // texture v runs top-down: up the screen is -v
	float wy = smoothstep(-0.15 * R, 0.6 * R, dy) * (1.0 - smoothstep(1.4 * R, 3.2 * R, dy));
	float wx = clamp(1.0 - abs(dm.x) / (0.9 * R + 0.25 * max(dy, 0.0)), 0.0, 1.0);
	float f  = sp.w * wx * wy;
	f = f * f;
	if (f <= 0.0) {
		return vec2(0.0);
	}
	float a = f * 0.032;
	vec2  d;
	d.x = sin(var_TexCoords.y * 120.0 + t * 15.0 + sp.x * 40.0) * a + sin(var_TexCoords.y * 61.0 - t * 9.0) * a * 0.6;
	d.y = sin(var_TexCoords.x * 100.0 + t * 12.0) * a * 0.7;
	return d;
}

void main()
{
	float heat    = u_Color.x;
	float t       = u_Color.y;
	float muzHeat = u_Color.z;
	float muzRad  = u_Color.w;
	vec2  muzCtr  = u_HzmParams.xy;

	float amp = heat * 0.030;                 // fullscreen warp (explosions)
	vec2  uv  = var_TexCoords;

	uv.x += sin(var_TexCoords.y * 42.0 + t * 8.0) * amp;
	uv.y += sin(var_TexCoords.x * 34.0 + t * 6.2) * amp * 0.7;
	uv.x += sin(var_TexCoords.y * 17.0 - t * 4.5) * amp * 0.5;   // slower second wave

	// localized muzzle shimmer: ONE-SIDED. Zero at and below the muzzle point, it grows upward over ~1 radius
	// and fades out by ~2.2 (heat rises; the sight post and the gun below the muzzle are never touched).
	vec2  dm = (var_TexCoords - muzCtr) * vec2(max(u_HzmParams.z, 0.1), 1.0);
	float R  = max(muzRad, 0.001);
	float wy = smoothstep(0.0, R, dm.y) * (1.0 - smoothstep(R, 2.2 * R, dm.y));
	float wx = clamp(1.0 - abs(dm.x) / (0.8 * R), 0.0, 1.0);
	float mf = muzHeat * wx * wy;
	mf = mf * mf;

	float mamp = mf * 0.045;
	vec2  d = vec2(0.0);
	d.x += sin(var_TexCoords.y * 95.0 + t * 17.0) * mamp;
	d.y += sin(var_TexCoords.x * 85.0 + t * 14.0) * mamp;
	d.x += sin(var_TexCoords.y * 50.0 - t * 11.0) * mamp * 0.6;

	// VIEW-WEAPON MASK: the viewmodel is drawn in depth range 0..0.3, so a pixel below that is gun. A gun pixel
	// is never displaced, and no gun pixel is ever pulled into the world around it - the aim point stays crisp.
	if (mf > 0.0 && u_HzmParams.w > 0.5) {
		if (texture2D(u_LevelsMap, var_TexCoords).r < 0.2995 || texture2D(u_LevelsMap, uv + d).r < 0.2995) {
			d = vec2(0.0);
		}
	}
	d += hzSpot(u_HzHaze0, t, u_HzmParams.z) + hzSpot(u_HzHaze1, t, u_HzmParams.z) + hzSpot(u_HzHaze2, t, u_HzmParams.z) + hzSpot(u_HzHaze3, t, u_HzmParams.z)
	   + hzSpot(u_HzHaze4, t, u_HzmParams.z) + hzSpot(u_HzHaze5, t, u_HzmParams.z) + hzSpot(u_HzHaze6, t, u_HzmParams.z) + hzSpot(u_HzHaze7, t, u_HzmParams.z);
	uv += d;

	gl_FragColor = vec4(texture2D(u_TextureMap, uv).rgb, 1.0);
}
