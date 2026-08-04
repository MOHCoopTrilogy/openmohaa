// HZM gl2 NEW POST-FX [user 07-29]: CAMERA MOTION BLUR. Not a gl1 port - rend2 has no motion blur
// at all, so this is net-new. Rotation-only: it smears along the screen-space path the view centre
// travelled since the previous frame, which is what a real shutter would capture while the player
// turns. Translation is deliberately NOT included - per-object velocity would need a velocity
// G-buffer and an extra geometry pass, and in a corridor shooter almost all of the felt motion is
// the mouse anyway.
//
// SYMMETRIC sampling, from -0.5 to +0.5 of the motion vector, is a deliberate choice rather than a
// trailing smear: it makes the result independent of the SIGN of the vector. The engine axis
// convention here is Quake3's (viewaxis[1] is LEFT, not right), so a one-sided smear would trail
// the wrong way if that convention were misread. Averaging along the axis cannot get it wrong, and
// for a subtle effect the two look near-identical in motion.
//
// u_Color.x = strength   u_Color.y/.z = screen-space motion vector (UV units)   u_Color.w unused
//
// NOTE: no double quotes anywhere in this file - stringify emits it as a C string literal.

uniform sampler2D u_TextureMap;
uniform vec4      u_Color;

varying vec2      var_TexCoords;

void main()
{
	vec2 v = vec2(u_Color.y, u_Color.z) * u_Color.x;

	// Below roughly a third of a texel the taps all land in the same pixel: the average is just the
	// source colour, so skip the work and return it exactly rather than paying for 8 fetches.
	if (dot(v, v) < 0.0000001)
	{
		gl_FragColor = texture2D(u_TextureMap, var_TexCoords);
		return;
	}

	vec4 sum = vec4(0.0);

	// 8 taps, evenly spread across the shutter interval and centred on the fragment.
	for (int i = 0; i < 8; i++)
	{
		float t  = (float(i) / 7.0) - 0.5;
		vec2  uv = clamp(var_TexCoords + v * t, vec2(0.0), vec2(1.0));
		sum += texture2D(u_TextureMap, uv);
	}

	gl_FragColor = sum * 0.125;
}
