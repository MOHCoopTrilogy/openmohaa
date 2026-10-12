// HZM gl2 volumetric clouds (plan 2.4 step 2): 3x3 tent [1 2 1]^2/16 of the trace target, clamped at the edges.
// The P0 twin's vc_temporal.tent3. GLSL >= 1.30 only. No double quotes anywhere in this file (tools/stringify).

uniform sampler2D u_VclCloud;     // the trace target (RGBA16F: premultiplied L, T)
uniform vec4      u_VclView;      // x W, y H

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	ivec2 hi = ivec2(int(u_VclView.x) - 1, int(u_VclView.y) - 1);
	vec4 acc = vec4(0.0);
	float ws = 0.0;
	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			ivec2 q = clamp(p + ivec2(i, j), ivec2(0), hi);
			float w = float((2 - abs(i)) * (2 - abs(j)));
			acc += texelFetch(u_VclCloud, q, 0) * w;
			ws += w;
		}
	}
	gl_FragColor = acc / ws;
}
