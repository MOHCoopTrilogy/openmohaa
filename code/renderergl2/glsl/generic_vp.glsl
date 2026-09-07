attribute vec3 attr_Position;
attribute vec3 attr_Normal;

#if defined(USE_VERTEX_ANIMATION)
attribute vec3 attr_Position2;
attribute vec3 attr_Normal2;
#elif defined(USE_BONE_ANIMATION)
attribute vec4 attr_BoneIndexes;
attribute vec4 attr_BoneWeights;
#endif

attribute vec4 attr_Color;
attribute vec4 attr_TexCoord0;

// HZM gl2 re-port (bug-gl2-nextbundle2): always declared - the second texture
// bundle can source lightmap coords in any generic permutation
attribute vec4 attr_TexCoord1;

#if defined(USE_TCMOD)
uniform vec4   u_DiffuseTexMatrix0;
uniform vec4   u_DiffuseTexMatrix1;
uniform vec4   u_DiffuseTexMatrix2;
uniform vec4   u_DiffuseTexMatrix3;
uniform vec4   u_DiffuseTexMatrix4;
uniform vec4   u_DiffuseTexMatrix5;
uniform vec4   u_DiffuseTexMatrix6;
uniform vec4   u_DiffuseTexMatrix7;
#endif

#if defined(USE_TCGEN) || defined(USE_RGBAGEN)
uniform vec3   u_LocalViewOrigin;
#endif

#if defined(USE_TCGEN)
uniform int    u_TCGen0;
uniform vec3   u_TCGen0Vector0;
uniform vec3   u_TCGen0Vector1;
#endif

#if defined(USE_FOG)
uniform vec4   u_FogDistance;
uniform vec4   u_FogDepth;
uniform float  u_FogEyeT;
uniform vec4   u_FogColorMask;
#endif

#if defined(USE_DEFORM_VERTEXES)
uniform int    u_DeformGen;
uniform float  u_DeformParams[5];
uniform float  u_Time;
#endif

uniform mat4   u_ModelViewProjectionMatrix;
uniform vec4   u_BaseColor;
uniform vec4   u_VertColor;

// HZM gl2 re-port (bug-gl2-nextbundle2): second texture bundle (MOHAA
// 'nextbundle'). Unconditional so no new permutations are needed; when the
// fragment side has u_Texture1Env == 0 the varying is simply unused.
uniform int    u_Texture1TCGen;
uniform vec4   u_Texture1Matrix0;
uniform vec4   u_Texture1Matrix1;
uniform vec4   u_Texture1Matrix2;
uniform vec4   u_Texture1Matrix3;
uniform vec4   u_Texture1Matrix4;
uniform vec4   u_Texture1Matrix5;
uniform vec4   u_Texture1Matrix6;
uniform vec4   u_Texture1Matrix7;

#if defined(USE_RGBAGEN)
uniform int    u_ColorGen;
uniform int    u_AlphaGen;
uniform vec4   u_AlphaGenParams;   // HZM: (alphaMin, alphaMax, loClamp, hiClamp)
uniform vec3   u_AmbientLight;
uniform vec3   u_DirectedLight;
uniform vec3   u_ModelLightDir;
uniform float  u_PortalRange;
// HZM coop (bug-2508): light for alphaGen lightingSpecular, u_LightOrigin convention
// (w == 0: xyz is a direction toward the light; w == 1: xyz is a point). Model space.
uniform vec4   u_HzmSpecLight;
#endif

#if defined(USE_VERTEX_ANIMATION)
uniform float  u_VertexLerp;
#elif defined(USE_BONE_ANIMATION)
uniform mat4 u_BoneMatrix[MAX_GLSL_BONES];
#endif

varying vec2   var_DiffuseTex;
varying vec2   var_Tex2; // HZM gl2 re-port (bug-gl2-nextbundle2)
varying vec4   var_Color;

#if defined(USE_DEFORM_VERTEXES)
vec3 DeformPosition(const vec3 pos, const vec3 normal, const vec2 st)
{
	float base =      u_DeformParams[0];
	float amplitude = u_DeformParams[1];
	float phase =     u_DeformParams[2];
	float frequency = u_DeformParams[3];
	float spread =    u_DeformParams[4];

	if (u_DeformGen == DGEN_BULGE)
	{
		phase *= st.x;
	}
	else // if (u_DeformGen <= DGEN_WAVE_INVERSE_SAWTOOTH)
	{
		phase += dot(pos.xyz, vec3(spread));
	}

	float value = phase + (u_Time * frequency);
	float func;

	if (u_DeformGen == DGEN_WAVE_SIN)
	{
		func = sin(value * 2.0 * M_PI);
	}
	else if (u_DeformGen == DGEN_WAVE_SQUARE)
	{
		func = sign(fract(0.5 - value));
	}
	else if (u_DeformGen == DGEN_WAVE_TRIANGLE)
	{
		func = abs(fract(value + 0.75) - 0.5) * 4.0 - 1.0;
	}
	else if (u_DeformGen == DGEN_WAVE_SAWTOOTH)
	{
		func = fract(value);
	}
	else if (u_DeformGen == DGEN_WAVE_INVERSE_SAWTOOTH)
	{
		func = (1.0 - fract(value));
	}
	else // if (u_DeformGen == DGEN_BULGE)
	{
		func = sin(value);
	}

	return pos + normal * (base + func * amplitude);
}
#endif

#if defined(USE_TCGEN)
vec2 GenTexCoords(int TCGen, vec3 position, vec3 normal, vec3 TCGenVector0, vec3 TCGenVector1)
{
	vec2 tex = attr_TexCoord0.st;

	if (TCGen == TCGEN_LIGHTMAP)
	{
		tex = attr_TexCoord1.st;
	}
	else if (TCGen == TCGEN_ENVIRONMENT_MAPPED)
	{
		vec3 viewer = normalize(u_LocalViewOrigin - position);
		vec2 ref = reflect(viewer, normal).yz;
		tex.s = ref.x * -0.5 + 0.5;
		tex.t = ref.y *  0.5 + 0.5;
	}
	else if (TCGen == TCGEN_ENVIRONMENT_MAPPED2)
	{
		// HZM gl2 re-port: MOHAA 'texgen environmentmodel' (gl1 RB_CalcEnvironmentTexCoords2):
		// front-facing verts (d>0) use the viewer direction, back-facing reflect it.
		vec3 viewer = normalize(u_LocalViewOrigin - position);
		float d = dot(normal, viewer);
		vec3 reflected = (d > 0.0) ? viewer : (viewer - 2.0 * d * normal);
		tex.s = 0.5 + reflected.y * 0.5;
		tex.t = 0.5 - reflected.z * 0.5;
	}
	else if (TCGen == TCGEN_VECTOR)
	{
		tex = vec2(dot(position, TCGenVector0), dot(position, TCGenVector1));
	}
	
	return tex;
}
#endif

// HZM gl2 re-port (bug-gl2-nextbundle2): ModTexCoords is now defined
// unconditionally (was #if defined(USE_TCMOD)) so the second-bundle path can
// use it in every permutation.
vec2 ModTexCoords(vec2 st, vec3 position, vec4 texMatrix[8])
{
	vec2 st2 = st;
	vec2 offsetPos = vec2(position.x + position.z, position.y);

	st2 = vec2(st2.x * texMatrix[0].x + st2.y * texMatrix[0].y + texMatrix[0].z,
	           st2.x * texMatrix[1].x + st2.y * texMatrix[1].y + texMatrix[1].z);
	st2 += texMatrix[0].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[1].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[2].x + st2.y * texMatrix[2].y + texMatrix[2].z,
	           st2.x * texMatrix[3].x + st2.y * texMatrix[3].y + texMatrix[3].z);
	st2 += texMatrix[2].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[3].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[4].x + st2.y * texMatrix[4].y + texMatrix[4].z,
	           st2.x * texMatrix[5].x + st2.y * texMatrix[5].y + texMatrix[5].z);
	st2 += texMatrix[4].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[5].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[6].x + st2.y * texMatrix[6].y + texMatrix[6].z,
	           st2.x * texMatrix[7].x + st2.y * texMatrix[7].y + texMatrix[7].z);
	st2 += texMatrix[6].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[7].w * 2.0 * M_PI));

	return st2;
}

#if defined(USE_RGBAGEN)
vec4 CalcColor(vec3 position, vec3 normal)
{
	vec4 color = u_VertColor * attr_Color + u_BaseColor;
	
	if (u_ColorGen == CGEN_LIGHTING_DIFFUSE)
	{
		float incoming = clamp(dot(normal, u_ModelLightDir), 0.0, 1.0);

		color.rgb = clamp(u_DirectedLight * incoming + u_AmbientLight, 0.0, 1.0);
	}
	
	vec3 viewer = u_LocalViewOrigin - position;

	if (u_AlphaGen == AGEN_LIGHTING_SPECULAR)
	{
		// HZM coop (bug-2508): this used to be the ioquake3 placeholder point
		// vec3(-960.0, 1980.0, 96.0) in model space - on a world sheet that is a lamp 2,700u
		// inland, up the bluff. tr_shade.c now uploads u_HzmSpecLight:
		//   w == 0 : xyz is a DIRECTION toward the light - the worldspawn sun bridged by
		//            tr_bsp.c, rotated into model space - used when the stage authored no origin
		//   w == 1 : xyz is the POINT the stage authored (alphaGen lightingSpecular a x y z),
		//            model space, exactly what gl1 RB_CalcSpecularAlpha subtracts
		// The pow-4 falloff is unchanged and alphaMax stays ignored (gl1 ignores it too).
		vec3 lightDir = normalize(u_HzmSpecLight.xyz - position * u_HzmSpecLight.w);
		vec3 reflected = -reflect(lightDir, normal);
		
		color.a = clamp(dot(reflected, normalize(viewer)), 0.0, 1.0);
		color.a *= color.a;
		color.a *= color.a;
	}
	else if (u_AlphaGen == AGEN_PORTAL)
	{
		color.a = clamp(length(viewer) / u_PortalRange, 0.0, 1.0);
	}
	else if (u_AlphaGen == AGEN_SCOORD || u_AlphaGen == AGEN_TCOORD)
	{
		// HZM gl2 parity: MOHAA fades a stage across the surface by ramping alpha along one texture
		// axis. Port of RB_CalcAlphaFromTexCoords in renderergl1 tr_shade_calc.c - same order of
		// operations, done per-vertex here instead of per-vertex on the CPU. Note gl1 reads the RAW
		// attr_TexCoord0, before any tcMod, so this must too.
		float coord = (u_AlphaGen == AGEN_SCOORD) ? attr_TexCoord0.s : attr_TexCoord0.t;
		float f = (u_AlphaGenParams.y - u_AlphaGenParams.x) * coord + u_AlphaGenParams.x;
		color.a = clamp(f, u_AlphaGenParams.z, u_AlphaGenParams.w);
	}
	else if (u_AlphaGen == AGEN_DOT || u_AlphaGen == AGEN_ONE_MINUS_DOT)
	{
		// HZM coop (bug-2508): MOHAA's Fresnel-style 'alphaGen dot min max' and
		// 'alphaGen oneMinusDot min max'. Port of RB_CalcAlphaFromDot / RB_CalcAlphaFromOneMinusDot
		// (renderergl1 tr_shade_calc.c:1075,1115):
		//   f = (N . V)^2     (oneMinusDot: 1 - that)
		//   a = (max - min) * f + min, clamped to [0,1]
		// u_AlphaGenParams.xy carries (alphaMin, alphaMax); .zw is NOT used here - it holds the
		// sCoord clamp pair, which collapses to (0,0) when the stage has no 4th parameter.
		// PARITY NOTE: gl1 writes this value into RGB (colors[0..2]) and leaves alpha alone;
		// gl2 writes ALPHA, which is what the directive name and every retail author expected.
		// gl1 is deliberately untouched - A/B the same shader on cl_renderer opengl1 vs opengl2.
		float d = dot(normalize(normal), normalize(viewer));
		float f = d * d;
		if (u_AlphaGen == AGEN_ONE_MINUS_DOT)
		{
			f = 1.0 - f;
		}
		color.a = clamp((u_AlphaGenParams.y - u_AlphaGenParams.x) * f + u_AlphaGenParams.x, 0.0, 1.0);
	}
	
	return color;
}
#endif

#if defined(USE_FOG)
float CalcFog(vec3 position)
{
	float s = dot(vec4(position, 1.0), u_FogDistance) * 8.0;
	float t = dot(vec4(position, 1.0), u_FogDepth);

	float eyeOutside = float(u_FogEyeT < 0.0);
	float fogged = float(t >= eyeOutside);

	t += 1e-6;
	t *= fogged / (t - u_FogEyeT * eyeOutside);

	return s * t;
}
#endif

void main()
{
#if defined(USE_VERTEX_ANIMATION)
	vec3 position  = mix(attr_Position, attr_Position2, u_VertexLerp);
	vec3 normal    = mix(attr_Normal,   attr_Normal2,   u_VertexLerp);
#elif defined(USE_BONE_ANIMATION)
	mat4 vtxMat  = u_BoneMatrix[int(attr_BoneIndexes.x)] * attr_BoneWeights.x;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.y)] * attr_BoneWeights.y;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.z)] * attr_BoneWeights.z;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.w)] * attr_BoneWeights.w;
	mat3 nrmMat = mat3(cross(vtxMat[1].xyz, vtxMat[2].xyz), cross(vtxMat[2].xyz, vtxMat[0].xyz), cross(vtxMat[0].xyz, vtxMat[1].xyz));

	vec3 position  = vec3(vtxMat * vec4(attr_Position, 1.0));
	vec3 normal    = normalize(nrmMat * attr_Normal);
#else
	vec3 position  = attr_Position;
	vec3 normal    = attr_Normal;
#endif

#if defined(USE_DEFORM_VERTEXES)
	position = DeformPosition(position, normal, attr_TexCoord0.st);
#endif

	gl_Position = u_ModelViewProjectionMatrix * vec4(position, 1.0);

#if defined(USE_TCGEN)
	vec2 tex = GenTexCoords(u_TCGen0, position, normal, u_TCGen0Vector0, u_TCGen0Vector1);
#else
	vec2 tex = attr_TexCoord0.st;
#endif

#if defined(USE_TCMOD)
	vec4 diffuseTexMatrix[8];
	diffuseTexMatrix[0] = u_DiffuseTexMatrix0;
	diffuseTexMatrix[1] = u_DiffuseTexMatrix1;
	diffuseTexMatrix[2] = u_DiffuseTexMatrix2;
	diffuseTexMatrix[3] = u_DiffuseTexMatrix3;
	diffuseTexMatrix[4] = u_DiffuseTexMatrix4;
	diffuseTexMatrix[5] = u_DiffuseTexMatrix5;
	diffuseTexMatrix[6] = u_DiffuseTexMatrix6;
	diffuseTexMatrix[7] = u_DiffuseTexMatrix7;
	var_DiffuseTex = ModTexCoords(tex, position, diffuseTexMatrix);
#else
    var_DiffuseTex = tex;
#endif

	// HZM gl2 re-port (bug-gl2-nextbundle2): second texture bundle texcoords.
	// TCGEN_LIGHTMAP sources the lightmap channel (attr_TexCoord1); everything
	// else uses the base channel (env-mapped 2nd bundles fall back to base
	// coords - documented residual, no retail user found). tcMods of bundle[1]
	// were composed into u_Texture1Matrix* on the CPU (same slot layout as the
	// diffuse matrix set).
	{
		vec2 tex2 = (u_Texture1TCGen == TCGEN_LIGHTMAP) ? attr_TexCoord1.st : attr_TexCoord0.st;
		vec4 tex1Matrix[8];
		tex1Matrix[0] = u_Texture1Matrix0;
		tex1Matrix[1] = u_Texture1Matrix1;
		tex1Matrix[2] = u_Texture1Matrix2;
		tex1Matrix[3] = u_Texture1Matrix3;
		tex1Matrix[4] = u_Texture1Matrix4;
		tex1Matrix[5] = u_Texture1Matrix5;
		tex1Matrix[6] = u_Texture1Matrix6;
		tex1Matrix[7] = u_Texture1Matrix7;
		var_Tex2 = ModTexCoords(tex2, position, tex1Matrix);
	}

#if defined(USE_RGBAGEN)
	var_Color = CalcColor(position, normal);
#else
	var_Color = u_VertColor * attr_Color + u_BaseColor;
#endif

#if defined(USE_FOG)
	var_Color *= vec4(1.0) - u_FogColorMask * sqrt(clamp(CalcFog(position), 0.0, 1.0));
#endif
}
