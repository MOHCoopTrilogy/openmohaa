// HZM water pass (r_hzmWater, tr_hzm_water.c; docs/proposals/water_wetness_2026-09-27 plan W3).
// Draws over an allowlisted water surface AFTER its retail stages, from the same tess and with the same deform - the
// fogpass_vp.glsl recipe: DeformPosition below is fogpass_vp's, fed by the same ComputeDeformValues(), so a GPU-deformed
// wave lands where the retail stages put it (a CPU deform arrives already applied, with u_DeformGen 0). The pass
// depth-tests LEQUAL against the surface it sits on, exactly as the fog pass does.
attribute vec3  attr_Position;
attribute vec3  attr_Normal;
attribute vec4  attr_TexCoord0;
attribute vec4  attr_TexCoord1;

#if defined(USE_DEFORM_VERTEXES)
uniform int     u_DeformGen;
uniform float   u_DeformParams[5];
uniform float   u_Time;
#endif

uniform mat4    u_ModelViewProjectionMatrix;

varying vec3    var_Position;
varying vec2    var_LightTex;
varying float   var_NormalZ;     // the BSP face's own (wound) normal: a cull-none underside reads < 0

#if defined(USE_DEFORM_VERTEXES)
vec3 DeformPosition(const vec3 pos, const vec3 normal, const vec2 st)
{
	if (u_DeformGen == 0)
	{
		return pos;
	}

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
		func = sign(0.5 - fract(value));
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

void main()
{
	vec3 position = attr_Position;

#if defined(USE_DEFORM_VERTEXES)
	position = DeformPosition(position, attr_Normal, attr_TexCoord0.st);
#endif

	gl_Position  = u_ModelViewProjectionMatrix * vec4(position, 1.0);
	var_Position = position;
	var_LightTex = attr_TexCoord1.xy;
	var_NormalZ  = attr_Normal.z;
}
