// HZM render-scale supersampling downsample (r_renderScale > 1.0): a 4-tap bilinear tent that
// resamples the super-sized scene down to the native frame. Not FSR - a plain quality box/tent, the
// standard SSAA resolve. Runs on display-referred values (post-tone), like every SSAA resolve in
// this class. u_FsrCon0.xy = tap offset in source UV (0.5 * S/D - 0.25 source texels).
// No double quotes and no backslash line-continuations (the stringify build step forbids them).

uniform sampler2D u_TextureMap;
uniform vec4 u_FsrCon0;

varying vec2 var_TexCoords;

void main(){
  vec2 o = u_FsrCon0.xy;
  vec3 c = texture2D(u_TextureMap, var_TexCoords + vec2(-o.x, -o.y)).rgb
         + texture2D(u_TextureMap, var_TexCoords + vec2( o.x, -o.y)).rgb
         + texture2D(u_TextureMap, var_TexCoords + vec2(-o.x,  o.y)).rgb
         + texture2D(u_TextureMap, var_TexCoords + vec2( o.x,  o.y)).rgb;
  gl_FragColor = vec4(c * 0.25, 1.0);
}
