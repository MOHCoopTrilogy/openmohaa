/*
HZM coop - gl1 GLSL post-process layer (public interface).

Keeps gl1's fixed-function pipeline 100% intact: the 3D scene is rendered into an
offscreen FBO, optional GLSL post passes run, the result is composited to the
backbuffer, then gl1 draws the 2D HUD on top (crisp). All entry points are no-ops
unless the GL2/FBO procs loaded (glPostFxProcsLoaded) AND r_postProcess is on, so a
GPU without support (or the cvar off) renders exactly like vanilla gl1.
*/
#pragma once

void      R_InitPostFxGL1( void );                 // create FBO + shaders (call from R_Init)
void      R_ShutdownPostFxGL1( void );             // destroy GL resources (call from RE_Shutdown)
qboolean  R_PostFxActive( void );                  // procs loaded AND ready AND r_postProcess on
void      RB_PostFxApply( void );                  // Approach B: copy frame + run shader passes + draw back (no-op if inactive)
