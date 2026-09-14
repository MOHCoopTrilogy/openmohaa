// AMD FidelityFX Super Resolution 1.0 - RCAS (Robust Contrast-Adaptive Sharpening), GLSL port.
// Ported from FidelityFX-FSR-1.0 ffx_fsr1.h (the FSR_RCAS_F 32-bit float path, FSR_RCAS_DENOISE on),
// MIT License, Copyright (c) 2021 Advanced Micro Devices, Inc. Full notice:
// code/thirdparty/FidelityFX-FSR-1.0/license.txt
// HZM: u_FsrCon0.x is the sharpness scalar exp2(-stops), set by the CPU (no uint bit-reinterpret).
// Runs 1:1 at native size; texelFetch needs GLSL 1.30+ (core here).
// No double quotes and no backslash line-continuations (the stringify build step forbids them).

uniform sampler2D u_TextureMap;   // display-referred image at native size
uniform vec4 u_FsrCon0;

float rcasRcp(float x){ return 1.0 / x; }
float rcasSat(float x){ return clamp(x, 0.0, 1.0); }
float rcasMax3(float a, float b, float c){ return max(a, max(b, c)); }
float rcasMin3(float a, float b, float c){ return min(a, min(b, c)); }

void main(){
  ivec2 sp = ivec2(gl_FragCoord.xy);
  vec3 b = texelFetch(u_TextureMap, sp + ivec2( 0, -1), 0).rgb;
  vec3 d = texelFetch(u_TextureMap, sp + ivec2(-1,  0), 0).rgb;
  vec3 e = texelFetch(u_TextureMap, sp, 0).rgb;
  vec3 f = texelFetch(u_TextureMap, sp + ivec2( 1,  0), 0).rgb;
  vec3 h = texelFetch(u_TextureMap, sp + ivec2( 0,  1), 0).rgb;

  float bR = b.r, bG = b.g, bB = b.b;
  float dR = d.r, dG = d.g, dB = d.b;
  float eR = e.r, eG = e.g, eB = e.b;
  float fR = f.r, fG = f.g, fB = f.b;
  float hR = h.r, hG = h.g, hB = h.b;

  float bL = bB * 0.5 + (bR * 0.5 + bG);
  float dL = dB * 0.5 + (dR * 0.5 + dG);
  float eL = eB * 0.5 + (eR * 0.5 + eG);
  float fL = fB * 0.5 + (fR * 0.5 + fG);
  float hL = hB * 0.5 + (hR * 0.5 + hG);

  float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
  nz = rcasSat(abs(nz) * rcasRcp(rcasMax3(rcasMax3(bL, dL, eL), fL, hL) - rcasMin3(rcasMin3(bL, dL, eL), fL, hL)));
  nz = -0.5 * nz + 1.0;

  float mn4R = min(rcasMin3(bR, dR, fR), hR);
  float mn4G = min(rcasMin3(bG, dG, fG), hG);
  float mn4B = min(rcasMin3(bB, dB, fB), hB);
  float mx4R = max(rcasMax3(bR, dR, fR), hR);
  float mx4G = max(rcasMax3(bG, dG, fG), hG);
  float mx4B = max(rcasMax3(bB, dB, fB), hB);

  vec2 peakC = vec2(1.0, -1.0 * 4.0);
  float hitMinR = min(mn4R, eR) * rcasRcp(4.0 * mx4R);
  float hitMinG = min(mn4G, eG) * rcasRcp(4.0 * mx4G);
  float hitMinB = min(mn4B, eB) * rcasRcp(4.0 * mx4B);
  float hitMaxR = (peakC.x - max(mx4R, eR)) * rcasRcp(4.0 * mn4R + peakC.y);
  float hitMaxG = (peakC.x - max(mx4G, eG)) * rcasRcp(4.0 * mn4G + peakC.y);
  float hitMaxB = (peakC.x - max(mx4B, eB)) * rcasRcp(4.0 * mn4B + peakC.y);
  float lobeR = max(-hitMinR, hitMaxR);
  float lobeG = max(-hitMinG, hitMaxG);
  float lobeB = max(-hitMinB, hitMaxB);
  float limit = (0.25 - (1.0 / 16.0));
  float lobe = max(-limit, min(rcasMax3(lobeR, lobeG, lobeB), 0.0)) * u_FsrCon0.x;
  lobe *= nz;

  float rcpL = rcasRcp(4.0 * lobe + 1.0);
  vec3 pix;
  pix.r = (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL;
  pix.g = (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL;
  pix.b = (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL;
  gl_FragColor = vec4(pix, 1.0);
}
