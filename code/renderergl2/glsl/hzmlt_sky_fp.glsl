// HZM coop [2026-09-27] realistic lightning - the sky pass. Additive (ONE, ONE) onto sky pixels only.
//   u_HzmLtWorld  = (unit direction to the lit cloud region, world space; sky energy)
//   u_HzmLtBolt   = (unit direction to the bolt centre, world space; bolt energy, 0 = no bolt)
//   u_HzmLtParams = (tan of the bolt half-width, tan of the bolt half-height, sin of the lowest bolt elevation, 0)
// The light comes from the cloud region around the strike (a 22-degree lobe, a 35-degree halo and a faint whole-deck
// term) and fades out over the lowest ~10 degrees, so there is no flat band along the horizon (diagnosis.md 3). All
// zero = nothing is added, but the pass does not even run then (RB_HzmLt_SkyPass).
uniform sampler2D u_DiffuseMap;
uniform vec4      u_HzmLtWorld;
uniform vec4      u_HzmLtBolt;
uniform vec4      u_HzmLtParams;

varying vec3      var_ViewDir;

void main()
{
	vec3  r      = normalize(var_ViewDir);
	float elFade = smoothstep(0.01745, 0.19199, r.z);
	float ang    = acos(clamp(dot(r, u_HzmLtWorld.xyz), -1.0, 1.0));
	float lobe   = exp(-0.5 * ang * ang / (0.384 * 0.384));
	float halo   = exp(-0.5 * ang * ang / (0.611 * 0.611));
	float k      = u_HzmLtWorld.w * (0.95 * lobe * elFade + 0.35 * halo * (1.0 - 0.5 * elFade) + 0.06 * elFade);
	vec3  col    = vec3(0.86, 0.90, 1.0) * k;

	if (u_HzmLtBolt.w > 0.0)
	{
		vec3  b = u_HzmLtBolt.xyz;
		float d = dot(r, b);

		if (d > 0.0 && r.z > u_HzmLtParams.z)
		{
			vec3 bRight = normalize(cross(b, vec3(0.0, 0.0, 1.0)));
			vec3 bUp    = cross(bRight, b);
			vec2 p      = vec2(dot(r, bRight), dot(r, bUp)) / d;
			vec2 uv     = vec2(0.5 + 0.5 * p.x / u_HzmLtParams.x, 0.5 - 0.5 * p.y / u_HzmLtParams.y);

			if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
			{
				col += vec3(0.90, 0.93, 1.0) * (texture2D(u_DiffuseMap, uv).r * u_HzmLtBolt.w);
			}
		}
	}

	gl_FragColor = vec4(col, 1.0);
}
