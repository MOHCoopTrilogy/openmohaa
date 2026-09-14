// AMD FidelityFX Super Resolution 1.0 - EASU (Edge-Adaptive Spatial Upsampling), GLSL port.
// Ported from FidelityFX-FSR-1.0 ffx_fsr1.h (the FSR_EASU_F 32-bit float path), MIT License,
// Copyright (c) 2021 Advanced Micro Devices, Inc. Full notice:
// code/thirdparty/FidelityFX-FSR-1.0/license.txt
// HZM: constants u_FsrCon0..3 are passed as plain floats by the CPU (no uint bit-reinterpret);
// the bit-hack rcp/rsqrt approximations are replaced by exact GLSL on the 32-bit float path.
// textureGather needs GLSL 4.00 or GL_ARB_gpu_shader5 (enabled via extradefines).
// No double quotes and no backslash line-continuations (the stringify build step forbids them).

uniform sampler2D u_TextureMap;   // scene colour at render-scale size, display-referred
uniform vec4 u_FsrCon0;
uniform vec4 u_FsrCon1;
uniform vec4 u_FsrCon2;
uniform vec4 u_FsrCon3;

float easuRcp(float x){ return 1.0 / x; }
float easuSat(float x){ return clamp(x, 0.0, 1.0); }
vec3  easuMax3v(vec3 a, vec3 b, vec3 c){ return max(a, max(b, c)); }
vec3  easuMin3v(vec3 a, vec3 b, vec3 c){ return min(a, min(b, c)); }

vec4 FsrEasuRF(vec2 p){ return textureGather(u_TextureMap, p, 0); }
vec4 FsrEasuGF(vec2 p){ return textureGather(u_TextureMap, p, 1); }
vec4 FsrEasuBF(vec2 p){ return textureGather(u_TextureMap, p, 2); }

void FsrEasuSetF(inout vec2 dir, inout float len, vec2 pp,
                 bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE){
  float w = 0.0;
  if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
  if (biT) w =        pp.x  * (1.0 - pp.y);
  if (biU) w = (1.0 - pp.x) *        pp.y ;
  if (biV) w =        pp.x  *        pp.y ;
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = easuRcp(lenX);
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = easuSat(abs(dirX) * lenX);
  lenX *= lenX;
  len += lenX * w;
  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = easuRcp(lenY);
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = easuSat(abs(dirY) * lenY);
  lenY *= lenY;
  len += lenY * w;
}

void FsrEasuTapF(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
                 float lob, float clp, vec3 c){
  vec2 v;
  v.x = (off.x * ( dir.x)) + (off.y * dir.y);
  v.y = (off.x * (-dir.y)) + (off.y * dir.x);
  v *= len;
  float d2 = v.x * v.x + v.y * v.y;
  d2 = min(d2, clp);
  float wB = (2.0 / 5.0) * d2 + (-1.0);
  float wA = lob * d2 + (-1.0);
  wB *= wB;
  wA *= wA;
  wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));
  float w = wB * wA;
  aC += c * w;
  aW += w;
}

void main(){
  // Integer output pixel from the destination viewport (full native frame).
  vec2 ip = floor(gl_FragCoord.xy);

  vec2 pp = ip * u_FsrCon0.xy + u_FsrCon0.zw;
  vec2 fp = floor(pp);
  pp -= fp;

  vec2 p0 = fp * u_FsrCon1.xy + u_FsrCon1.zw;
  vec2 p1 = p0 + u_FsrCon2.xy;
  vec2 p2 = p0 + u_FsrCon2.zw;
  vec2 p3 = p0 + u_FsrCon3.xy;

  vec4 bczzR = FsrEasuRF(p0);
  vec4 bczzG = FsrEasuGF(p0);
  vec4 bczzB = FsrEasuBF(p0);
  vec4 ijfeR = FsrEasuRF(p1);
  vec4 ijfeG = FsrEasuGF(p1);
  vec4 ijfeB = FsrEasuBF(p1);
  vec4 klhgR = FsrEasuRF(p2);
  vec4 klhgG = FsrEasuGF(p2);
  vec4 klhgB = FsrEasuBF(p2);
  vec4 zzonR = FsrEasuRF(p3);
  vec4 zzonG = FsrEasuGF(p3);
  vec4 zzonB = FsrEasuBF(p3);

  vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
  vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
  vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
  vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);

  float bL = bczzL.x;
  float cL = bczzL.y;
  float iL = ijfeL.x;
  float jL = ijfeL.y;
  float fL = ijfeL.z;
  float eL = ijfeL.w;
  float kL = klhgL.x;
  float lL = klhgL.y;
  float hL = klhgL.z;
  float gL = klhgL.w;
  float oL = zzonL.z;
  float nL = zzonL.w;

  vec2 dir = vec2(0.0);
  float len = 0.0;
  FsrEasuSetF(dir, len, pp, true , false, false, false, bL, eL, fL, gL, jL);
  FsrEasuSetF(dir, len, pp, false, true , false, false, cL, fL, gL, hL, kL);
  FsrEasuSetF(dir, len, pp, false, false, true , false, fL, iL, jL, kL, nL);
  FsrEasuSetF(dir, len, pp, false, false, false, true , gL, jL, kL, lL, oL);

  vec2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < (1.0 / 32768.0);
  dirR = inversesqrt(dirR);
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= vec2(dirR);
  len = len * 0.5;
  len *= len;
  float stretch = (dir.x * dir.x + dir.y * dir.y) * easuRcp(max(abs(dir.x), abs(dir.y)));
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  float clp = easuRcp(lob);

  vec3 min4 = min(easuMin3v(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w), vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                  vec3(klhgR.x, klhgG.x, klhgB.x));
  vec3 max4 = max(easuMax3v(vec3(ijfeR.z, ijfeG.z, ijfeB.z), vec3(klhgR.w, klhgG.w, klhgB.w), vec3(ijfeR.y, ijfeG.y, ijfeB.y)),
                  vec3(klhgR.x, klhgG.x, klhgB.x));

  vec3 aC = vec3(0.0);
  float aW = 0.0;
  FsrEasuTapF(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));
  FsrEasuTapF(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));
  FsrEasuTapF(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));
  FsrEasuTapF(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.y, ijfeG.y, ijfeB.y));
  FsrEasuTapF(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.z, ijfeG.z, ijfeB.z));
  FsrEasuTapF(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));
  FsrEasuTapF(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, vec3(klhgR.x, klhgG.x, klhgB.x));
  FsrEasuTapF(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));
  FsrEasuTapF(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));
  FsrEasuTapF(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, vec3(klhgR.w, klhgG.w, klhgB.w));
  FsrEasuTapF(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));
  FsrEasuTapF(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));

  vec3 pix = min(max4, max(min4, aC * vec3(easuRcp(aW))));
  gl_FragColor = vec4(pix, 1.0);
}
