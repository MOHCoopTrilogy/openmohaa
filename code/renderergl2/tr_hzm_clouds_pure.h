/*
tr_hzm_clouds_pure.h - the PURE parts of the volumetric clouds (tr_hzm_clouds.c): no GL, no engine state, no
allocation. docs/tools/vcl_selftest compiles this header UNCHANGED against reference bytes from the P0 twin (V2), so
everything here must stay a function of its arguments. Ports of p0_stills/tools/vc_render.py (hash32, worley_hash,
make_noise, noise_mips, build_mips quant8) and vc_assets.load_talon's PIL "L" conversion, line for line: Python
evaluates the Worley field in IEEE double (numba fastmath off) and the mip chains in float32, rounds half to even
(np.round), and so does this.
*/
#ifndef TR_HZM_CLOUDS_PURE_H
#define TR_HZM_CLOUDS_PURE_H

#include <math.h>
#include <string.h>

#define VCL_NOISE_SEED	20260927u

static unsigned int R_HZM_VclHash32( unsigned int x, unsigned int y, unsigned int z, unsigned int seed ) {
	unsigned int h = ( x * 0x8da6b343u ) ^ ( y * 0xd8163841u ) ^ ( z * 0xcb1ab31fu ) ^ ( seed * 0x165667b1u );

	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

// np.round: round half to even
static double R_HZM_VclRintD( double v ) {
	double f = floor( v );
	double d = v - f;

	if ( d > 0.5 ) {
		return f + 1.0;
	}
	if ( d < 0.5 ) {
		return f;
	}
	return ( fmod( f, 2.0 ) == 0.0 ) ? f : f + 1.0;
}

static float R_HZM_VclRintF( float v ) {
	float f = (float)floor( v );
	float d = v - f;

	if ( d > 0.5f ) {
		return f + 1.0f;
	}
	if ( d < 0.5f ) {
		return f;
	}
	return ( fmodf( f, 2.0f ) == 0.0f ) ? f : f + 1.0f;
}

static int R_HZM_VclMod( int a, int m ) {
	int r = a % m;
	return r < 0 ? r + m : r;
}

// one Worley octave into out[(i * n + j) * n + k] (i = the u axis, as the twin's vol[i][j][k]); fp = cells^3 * 3
// doubles of scratch (the feature point of every cell, computed once)
static void R_HZM_VclWorley( int n, int cells, unsigned int seed, double *out, double *fp ) {
	int i, j, k, a, b, cc;
	const double s3 = sqrt( 3.0 );

	for ( a = 0; a < cells; a++ ) {
		for ( b = 0; b < cells; b++ ) {
			for ( cc = 0; cc < cells; cc++ ) {
				double *p = fp + ( ( a * cells + b ) * cells + cc ) * 3;
				p[0] = ( R_HZM_VclHash32( a, b, cc, seed * 3u + 0u ) & 0xFFFFFF ) / 16777216.0;
				p[1] = ( R_HZM_VclHash32( a, b, cc, seed * 3u + 1u ) & 0xFFFFFF ) / 16777216.0;
				p[2] = ( R_HZM_VclHash32( a, b, cc, seed * 3u + 2u ) & 0xFFFFFF ) / 16777216.0;
			}
		}
	}
	for ( i = 0; i < n; i++ ) {
		for ( j = 0; j < n; j++ ) {
			for ( k = 0; k < n; k++ ) {
				double px = ( i + 0.5 ) / n * cells, py = ( j + 0.5 ) / n * cells, pz = ( k + 0.5 ) / n * cells;
				int cx = (int)floor( px ), cy = (int)floor( py ), cz = (int)floor( pz );
				double best = 1e9, v;
				int ox, oy, oz;

				for ( ox = -1; ox <= 1; ox++ ) {
					for ( oy = -1; oy <= 1; oy++ ) {
						for ( oz = -1; oz <= 1; oz++ ) {
							int gx = cx + ox, gy = cy + oy, gz = cz + oz;
							const double *p = fp + ( ( R_HZM_VclMod( gx, cells ) * cells + R_HZM_VclMod( gy, cells ) ) * cells
								+ R_HZM_VclMod( gz, cells ) ) * 3;
							double dx = gx + p[0] - px, dy = gy + p[1] - py, dz = gz + p[2] - pz;
							double d = dx * dx + dy * dy + dz * dz;
							if ( d < best ) {
								best = d;
							}
						}
					}
				}
				v = 1.0 - sqrt( best ) / s3;
				out[( i * n + j ) * n + k] = v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v );
			}
		}
	}
}

// make_noise: level-0 bytes, i-major. w = 3 * n^3 doubles, fp = 16^3 * 3 doubles of scratch.
static void R_HZM_VclNoiseBytes( int n, unsigned char *out, double *w, double *fp ) {
	const int n3 = n * n * n;
	double mn = 1e30, mx = -1e30;
	int i, o;

	for ( o = 0; o < 3; o++ ) {
		R_HZM_VclWorley( n, 4 << o, VCL_NOISE_SEED + (unsigned int)o, w + o * n3, fp );
	}
	for ( i = 0; i < n3; i++ ) {
		double f = 0.625 * w[i] + 0.25 * w[n3 + i] + 0.125 * w[2 * n3 + i];
		w[i] = f;
		if ( f < mn ) mn = f;
		if ( f > mx ) mx = f;
	}
	for ( i = 0; i < n3; i++ ) {
		out[i] = (unsigned char)R_HZM_VclRintD( ( w[i] - mn ) / ( mx - mn ) * 255.0 );
	}
}

// noise_mips: the next level of the UNQUANTISED float32 chain (8-tap box, the twin's term order)
static void R_HZM_VclNoiseDown( const float *cur, int n, float *nxt ) {
	int h = n / 2, i, j, k;

	for ( i = 0; i < h; i++ ) {
		for ( j = 0; j < h; j++ ) {
			for ( k = 0; k < h; k++ ) {
#define VCLN(a, b, d) cur[( ( 2 * i + (a) ) * n + ( 2 * j + (b) ) ) * n + ( 2 * k + (d) )]
				float s = VCLN( 0, 0, 0 ) + VCLN( 1, 0, 0 );
				s = s + VCLN( 0, 1, 0 );
				s = s + VCLN( 0, 0, 1 );
				s = s + VCLN( 1, 1, 0 );
				s = s + VCLN( 1, 0, 1 );
				s = s + VCLN( 0, 1, 1 );
				s = s + VCLN( 1, 1, 1 );
#undef VCLN
				nxt[( i * h + j ) * h + k] = 0.125f * s;
			}
		}
	}
}

// a level's R8 bytes, in GL's layout (x fastest: texel (x = i, y = j, z = k) at (k * n + j) * n + i)
static void R_HZM_VclNoiseQuantGL( const float *cur, int n, unsigned char *up ) {
	int i, j, k;

	for ( i = 0; i < n; i++ ) {
		for ( j = 0; j < n; j++ ) {
			for ( k = 0; k < n; k++ ) {
				up[( k * n + j ) * n + i] = (unsigned char)R_HZM_VclRintF( cur[( i * n + j ) * n + k] * 255.0f );
			}
		}
	}
}

// PIL Image.convert("L") (ITU-R 601-2, fixed point, rounded) of one RGBA texel
static unsigned char R_HZM_VclGrey( const unsigned char *rgba ) {
	return (unsigned char)( ( rgba[0] * 19595 + rgba[1] * 38470 + rgba[2] * 7471 + 0x8000 ) >> 16 );
}

// build_mips: the next level of the unquantised float32 coverage chain (4-tap box, the twin's term order)
static void R_HZM_VclCovDown( const float *cur, int x, int y, float *nxt ) {
	int x2 = x / 2, y2 = y / 2, a, b;

	for ( b = 0; b < y2; b++ ) {
		for ( a = 0; a < x2; a++ ) {
			float s = cur[( 2 * b ) * x + 2 * a] + cur[( 2 * b + 1 ) * x + 2 * a];
			s = s + cur[( 2 * b ) * x + 2 * a + 1];
			s = s + cur[( 2 * b + 1 ) * x + 2 * a + 1];
			nxt[b * x2 + a] = 0.25f * s;
		}
	}
}

// the conservative max chain for the empty-ray skip: every level >= every texel of level 0 it covers
static void R_HZM_VclMaxDown( const unsigned char *mx, int x, int y, unsigned char *mxn ) {
	int x2 = x / 2, y2 = y / 2, a, b;

	for ( b = 0; b < y2; b++ ) {
		for ( a = 0; a < x2; a++ ) {
			unsigned char m = mx[( 2 * b ) * x + 2 * a];
			if ( mx[( 2 * b + 1 ) * x + 2 * a] > m ) m = mx[( 2 * b + 1 ) * x + 2 * a];
			if ( mx[( 2 * b ) * x + 2 * a + 1] > m ) m = mx[( 2 * b ) * x + 2 * a + 1];
			if ( mx[( 2 * b + 1 ) * x + 2 * a + 1] > m ) m = mx[( 2 * b + 1 ) * x + 2 * a + 1];
			mxn[b * x2 + a] = m;
		}
	}
}

// RB_CalcScrollTexMatrix's wrap (speed * t - floor, in double), then tcMod scale: the offset in scaled units that the
// trace adds to (s,t) * scale. The wrap jumps by exactly `scale`; the noise addresses (s,t) * NS / NT, so NS * SCS and
// NT * SCT must be integers for the wrap to be seamless (vc_profile.py asserts it; the selftest checks both).
static double R_HZM_VclScroll( double t, double speed, double scale ) {
	double a = speed * t;

	a = a - floor( a );
	return a * scale;
}

static int R_HZM_VclStrIEq( const char *a, const char *b ) {
	for ( ;; a++, b++ ) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if ( ca >= 'A' && ca <= 'Z' ) ca += 'a' - 'A';
		if ( cb >= 'A' && cb <= 'Z' ) cb += 'a' - 'A';
		if ( ca != cb ) return 0;
		if ( !ca ) return 1;
	}
}

static int R_HZM_VclStrIHas( const char *hay, const char *needle ) {
	size_t n = strlen( needle ), i;

	for ( ; *hay; hay++ ) {
		for ( i = 0; i < n; i++ ) {
			int ca = (unsigned char)hay[i], cb = (unsigned char)needle[i];
			if ( !ca ) return 0;
			if ( ca >= 'A' && ca <= 'Z' ) ca += 'a' - 'A';
			if ( cb >= 'A' && cb <= 'Z' ) cb += 'a' - 'A';
			if ( ca != cb ) break;
		}
		if ( i == n ) return 1;
	}
	return 0;
}

/*
v1.1 (2026-10-05) THE PROCEDURAL PROFILE (docs/proposals/volumetric_clouds_2026-09-26/v11.md, part B).

R_HZM_VclProcCoverage: a 512^2 TILEABLE cloud-coverage field, generated (never loaded): a periodic Perlin fBm (5
octaves, 4..64 cells per tile) mixed with an inverted periodic Worley fBm (6/12/24 cells - the billows), then RANK-
NORMALISED through a 65536-bin histogram, so its histogram is flat: a threshold CLO covers exactly 1 - CLO of the sky,
which is what lets one number (a family's cover, raised by a storm) mean the same thing on every map. Deterministic
(integer hashes, float maths, no libc randomness): a pure function of n, like the 3D noise above.
*/
static float R_HZM_VclFade( float t ) {
	return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f );
}

static float R_HZM_VclGrad( int ix, int iy, int p, unsigned int seed, float dx, float dy ) {
	unsigned int h = R_HZM_VclHash32( (unsigned int)R_HZM_VclMod( ix, p ), (unsigned int)R_HZM_VclMod( iy, p ), 0u, seed );
	float a = (float)( h >> 8 ) * ( 6.28318530718f / 16777216.0f );

	return (float)cos( a ) * dx + (float)sin( a ) * dy;
}

// periodic gradient noise, lattice period p, at (x, y) in lattice units
static float R_HZM_VclPerlinP( float x, float y, int p, unsigned int seed ) {
	int xi = (int)floor( x ), yi = (int)floor( y );
	float fx = x - (float)xi, fy = y - (float)yi;
	float u = R_HZM_VclFade( fx ), v = R_HZM_VclFade( fy );
	float n00 = R_HZM_VclGrad( xi,     yi,     p, seed, fx,        fy );
	float n10 = R_HZM_VclGrad( xi + 1, yi,     p, seed, fx - 1.0f, fy );
	float n01 = R_HZM_VclGrad( xi,     yi + 1, p, seed, fx,        fy - 1.0f );
	float n11 = R_HZM_VclGrad( xi + 1, yi + 1, p, seed, fx - 1.0f, fy - 1.0f );
	float a = n00 + ( n10 - n00 ) * u, b = n01 + ( n11 - n01 ) * u;

	return a + ( b - a ) * v;
}

// periodic Worley F1 (nearest feature point), c cells per tile, (x, y) in tile units [0,1); ~0 at a point, ~1 far
static float R_HZM_VclWorleyP( float x, float y, int c, unsigned int seed ) {
	float px = x * (float)c, py = y * (float)c, best = 1e9f;
	int ix = (int)floor( px ), iy = (int)floor( py ), i, j;

	for ( j = -1; j <= 1; j++ ) {
		for ( i = -1; i <= 1; i++ ) {
			int cx = ix + i, cy = iy + j;
			unsigned int h = R_HZM_VclHash32( (unsigned int)R_HZM_VclMod( cx, c ), (unsigned int)R_HZM_VclMod( cy, c ), 1u, seed );
			float fx = (float)cx + (float)( h & 0xffffu ) / 65536.0f - px;
			float fy = (float)cy + (float)( h >> 16 ) / 65536.0f - py;
			float d = fx * fx + fy * fy;
			if ( d < best ) {
				best = d;
			}
		}
	}
	best = (float)sqrt( best );
	return best > 1.0f ? 1.0f : best;
}

// out: n*n bytes; val: n*n floats of scratch; hist: 65536 unsigned ints of scratch
static void R_HZM_VclProcCoverage( int n, unsigned char *out, float *val, unsigned int *hist ) {
	const unsigned int seed = 20261005u;
	int i, j, o, total = n * n;
	unsigned int acc;

	for ( j = 0; j < n; j++ ) {
		for ( i = 0; i < n; i++ ) {
			float x = ( (float)i + 0.5f ) / (float)n, y = ( (float)j + 0.5f ) / (float)n;
			float fbm = 0.0f, amp = 0.5f, w;
			int cells = 4;

			for ( o = 0; o < 5; o++, cells *= 2, amp *= 0.5f ) {
				fbm += amp * R_HZM_VclPerlinP( x * (float)cells, y * (float)cells, cells, seed + (unsigned int)o );
			}
			w = 1.0f - ( 0.625f * R_HZM_VclWorleyP( x, y, 6, seed ) + 0.25f * R_HZM_VclWorleyP( x, y, 12, seed + 7u )
			           + 0.125f * R_HZM_VclWorleyP( x, y, 24, seed + 13u ) );
			fbm = fbm * 0.9f + 0.5f;
			val[j * n + i] = 0.55f * fbm + 0.45f * w;
		}
	}
	memset( hist, 0, sizeof( unsigned int ) * 65536 );
	for ( i = 0; i < total; i++ ) {
		float v = val[i] < 0.0f ? 0.0f : ( val[i] > 1.0f ? 1.0f : val[i] );
		int b = (int)( v * 65535.0f );
		val[i] = (float)b;
		hist[b]++;
	}
	// mid-rank CDF of each bin -> 0..255
	for ( acc = 0, i = 0; i < 65536; i++ ) {
		unsigned int c = hist[i];
		hist[i] = acc + c / 2;
		acc += c;
	}
	for ( i = 0; i < total; i++ ) {
		double r = (double)hist[(int)val[i]] / (double)( total - 1 );
		out[i] = (unsigned char)R_HZM_VclRintD( ( r > 1.0 ? 1.0 : r ) * 255.0 );
	}
}

/*
The reviewed sky-box FAMILIES of the procedural profile (part B; v11.md has the per-family verdict and the skipped
boxes with their reasons). Keyed on the skyParms outer box AS PARSED. Values are the family's REST state; a storm
(R_HZM_Storm sunFade) raises cover toward overcast and dims the sun term. A box not listed gets no clouds.
  cover    share of the sky covered (the coverage threshold is 1 - cover)
  sigma    extinction per dome unit at full density (africanight's look: 0.05)
  sun      sun/moon light on the cloud, x the box's brightest 1x1 mip luminance
  amb      sky light, x the box zenith (top) / horizon (bottom) 1x1 mips
  tile     the coverage tile in dome units (a cloud is ~1/8 of it); wind heading (deg, world yaw) and speed (du/s)
  eros     top erosion (the base gets 0.55 of it)        lmax  the highlight knee, x the brightest box luminance
*/
typedef struct hzmCloudFamily_s {
	const char	*box;
	const char	*label;
	float		cover, sigma, sun, amb, tile, windDeg, windSpeed, eros, lmax;
	float		ramp, nfreq;	// coverage ramp width above the threshold; erosion-noise repeats per coverage tile
	float		detail, dfreq;	// v2: outline billows (coverage lowered by up to `detail` by a field `dfreq` per tile)
	float		basesharp, base1, topk;	// v2: base crispness 0..1, the thin-cloud base height, the tower height
} hzmCloudFamily_t;

static const hzmCloudFamily_t hzmCloudFamilies[] = {
	// cumulus fields (twin v7: billowy outline, flat crisp base, sun-lit) - and one stratus deck (twin v9, m5l2's rain)
	//  box                     label       cover  sigma  sun   amb    tile     wdeg  wspd  eros   lmax  ramp   nfreq det   dfreq bsharp base1  topk
	{ "env/mohday1",       "cumulus",  0.45f, 0.070f, 2.5f, 0.75f, 2400.0f,  60.0f, 3.0f, 0.85f, 1.6f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ "env/mohday2",       "cumulus",  0.45f, 0.070f, 2.5f, 0.75f, 2400.0f,  60.0f, 3.0f, 0.85f, 1.6f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ "env/nordawn",       "cumulus",  0.45f, 0.070f, 2.5f, 0.75f, 2400.0f,  30.0f, 2.5f, 0.85f, 1.6f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ "env/m5l2",          "stratus",  0.72f, 0.050f, 1.6f, 0.80f, 1600.0f,  80.0f, 6.0f, 0.85f, 1.1f, 0.30f, 12.0f, 0.15f, 16.0f, 0.8f, 0.08f, 0.35f },
	{ "env/lighthousesky", "cumulus",  0.40f, 0.070f, 3.0f, 0.75f, 2048.0f,  45.0f, 2.0f, 0.85f, 3.0f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ "env/mohnightfog",   "cumulus",  0.40f, 0.070f, 3.0f, 0.75f, 2048.0f,  45.0f, 2.0f, 0.85f, 3.0f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ "env/t1l3",          "cumulus",  0.55f, 0.070f, 2.0f, 0.80f, 2048.0f, 100.0f, 3.0f, 0.85f, 2.0f, 0.30f, 12.0f, 0.30f, 20.0f, 0.8f, 0.08f, 0.70f },
	{ 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }
};

static const hzmCloudFamily_t *R_HZM_VclFamily( const char *box ) {
	int i;

	if ( !box || !box[0] ) {
		return 0;
	}
	for ( i = 0; hzmCloudFamilies[i].box; i++ ) {
		if ( R_HZM_VclStrIEq( box, hzmCloudFamilies[i].box ) ) {
			return &hzmCloudFamilies[i];
		}
	}
	return 0;
}

// the deny list (plan 2.3), checked before anything else; renderer-local on purpose. NULL = not denied.
// Omaha (never touch): the four maps, their _sml twins and every box / shader naming an Omaha sky;
// the user's own exclusions m3l2 and m3l3 (2026-09-26).
static const char *R_HZM_VclDeny( const char *mapName, const char *skyShader, const char *skyBox ) {
	static const char *const omahaMaps[] = { "m3l1a", "m3l1b", "e3l1", "e3l1_sml", "e3l2", "e3l2_sml", 0 };
	static const char *const omahaNames[] = { "dday2", "e3l1sky", "e3l2sky", "omaha", "inteldim", 0 };
	int i;

	for ( i = 0; omahaMaps[i]; i++ ) {
		if ( R_HZM_VclStrIEq( mapName, omahaMaps[i] ) ) {
			return "deny:omaha";
		}
	}
	if ( R_HZM_VclStrIEq( mapName, "m3l2" ) ) {
		return "deny:user-m3l2";
	}
	if ( R_HZM_VclStrIEq( mapName, "m3l3" ) ) {
		return "deny:user-m3l3";
	}
	for ( i = 0; omahaNames[i]; i++ ) {
		if ( ( skyShader && R_HZM_VclStrIHas( skyShader, omahaNames[i] ) )
			|| ( skyBox && R_HZM_VclStrIHas( skyBox, omahaNames[i] ) ) ) {
			return "deny:omaha";
		}
	}
	return 0;
}

#endif
