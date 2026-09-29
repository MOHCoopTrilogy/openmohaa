// HZM coop [2026-09-27] realistic lightning - the sky pass (tr_hzm_lightning.c RB_HzmLt_SkyPass).
// A full-screen quad in NDC drawn at depth range (1,1): it lands only on pixels no opaque geometry covered (the sky and
// the clear colour of caulk-sky maps). The world view ray per corner is built the way shadowmask_vp does it.
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;

varying vec3   var_ViewDir;

void main()
{
	gl_Position = attr_Position;
	vec2 screenCoords = gl_Position.xy / gl_Position.w;
	var_ViewDir = u_ViewForward + u_ViewLeft * -screenCoords.x + u_ViewUp * screenCoords.y;
}
