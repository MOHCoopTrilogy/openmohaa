// HZM gl2 volumetric clouds (plan 2.4 step 3): the composite's vertex shader. It draws today's cloud-dome vertices
// (R_BuildCloudData -> FillCloudBox) with today's texture-coordinate path: ModTexCoords below is a verbatim copy of
// generic_vp.glsl's, fed the same ComputeTexMods matrices, so the gauze texel is exactly today's layer texel.
// No double quotes anywhere in this file (tools/stringify).

attribute vec3 attr_Position;
attribute vec4 attr_TexCoord0;

uniform mat4   u_ModelViewProjectionMatrix;
uniform vec3   u_ViewOrigin;

uniform vec4   u_DiffuseTexMatrix0;
uniform vec4   u_DiffuseTexMatrix1;
uniform vec4   u_DiffuseTexMatrix2;
uniform vec4   u_DiffuseTexMatrix3;
uniform vec4   u_DiffuseTexMatrix4;
uniform vec4   u_DiffuseTexMatrix5;
uniform vec4   u_DiffuseTexMatrix6;
uniform vec4   u_DiffuseTexMatrix7;

varying vec2   var_DiffuseTex;
varying vec3   var_Dir;

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

void main()
{
	vec3 position = attr_Position;
	gl_Position = u_ModelViewProjectionMatrix * vec4(position, 1.0);

	vec4 diffuseTexMatrix[8];
	diffuseTexMatrix[0] = u_DiffuseTexMatrix0;
	diffuseTexMatrix[1] = u_DiffuseTexMatrix1;
	diffuseTexMatrix[2] = u_DiffuseTexMatrix2;
	diffuseTexMatrix[3] = u_DiffuseTexMatrix3;
	diffuseTexMatrix[4] = u_DiffuseTexMatrix4;
	diffuseTexMatrix[5] = u_DiffuseTexMatrix5;
	diffuseTexMatrix[6] = u_DiffuseTexMatrix6;
	diffuseTexMatrix[7] = u_DiffuseTexMatrix7;
	var_DiffuseTex = ModTexCoords(attr_TexCoord0.st, position, diffuseTexMatrix);

	var_Dir = position - u_ViewOrigin;
}
