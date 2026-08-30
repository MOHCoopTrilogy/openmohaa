/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl.input.c  -- builds an intended movement command to send to the server

#include "client.h"
#include "cl_ui.h"

unsigned	frame_msec;
int			old_com_frameTime;

/*
===============================================================================

KEY BUTTONS

Continuous button event tracking is complicated by the fact that two different
input sources (say, mouse button 1 and the control key) can both press the
same button, but the button should only be released when both of the
pressing key have been released.

When a key event issues a button command (+forward, +attackprimary, etc), it appends
its key number as argv(1) so it can be matched up with the release.

argv(2) will be set to the time the event happened, which allows exact
control even at low framerates when the down and up events may both get qued
at the same time.

===============================================================================
*/


kbutton_t	in_left, in_right, in_forward, in_back;
kbutton_t	in_lookup, in_lookdown, in_moveleft, in_moveright;
kbutton_t	in_strafe, in_speed;

qboolean	in_guimouse;

kbutton_t	in_up, in_down;

#ifdef USE_VOIP
kbutton_t	in_voiprecord;
#endif

kbutton_t	in_buttons[16];

qboolean	in_mlooking;

void IN_ToggleMouse( void ) {
	if( in_guimouse )
	{
		IN_MouseOff();
	}
	else
	{
		IN_MouseOn();
	}
}

void IN_MouseOn( void ) {
	if( !in_guimouse )
	{
		for( int k = 0; k <= K_MWHEELUP; k++ )
		{
			if( keys[ k ].down )
			{
				CL_KeyEvent( k, qfalse, 0 );
			}
		}
	}

	in_guimouse = qtrue;
}

void IN_MouseOff( void ) {
	in_guimouse = qfalse;
}

void IN_MLookDown( void ) {
	in_mlooking = qtrue;
}

void IN_MLookUp( void ) {
	in_mlooking = qfalse;
	if ( !cl_freelook->integer ) {
		IN_CenterView ();
	}
}

void IN_KeyDown( kbutton_t *b ) {
	int		k;
	char	*c;

	c = Cmd_Argv(1);
	if ( c[0] ) {
		k = atoi(c);
	} else {
		k = -1;		// typed manually at the console for continuous down
	}

	if ( k == b->down[0] || k == b->down[1] ) {
		return;		// repeating key
	}

	if ( !b->down[0] ) {
		b->down[0] = k;
	} else if ( !b->down[1] ) {
		b->down[1] = k;
	} else {
		Com_Printf ("Three keys down for a button!\n");
		return;
	}

	if ( b->active ) {
		return;		// still down
	}

	// save timestamp for partial frame summing
	c = Cmd_Argv(2);
	b->downtime = atoi(c);

	b->active = qtrue;
	b->wasPressed = qtrue;
}

void IN_KeyUp( kbutton_t *b ) {
	int		k;
	char	*c;
	unsigned	uptime;

	c = Cmd_Argv(1);
	if ( c[0] ) {
		k = atoi(c);
	} else {
		// typed manually at the console, assume for unsticking, so clear all
		b->down[0] = b->down[1] = 0;
		b->active = qfalse;
		return;
	}

	if ( b->down[0] == k ) {
		b->down[0] = 0;
	} else if ( b->down[1] == k ) {
		b->down[1] = 0;
	} else {
		return;		// key up without coresponding down (menu pass through)
	}
	if ( b->down[0] || b->down[1] ) {
		return;		// some other key is still holding it down
	}

	b->active = qfalse;

	// save timestamp for partial frame summing
	c = Cmd_Argv(2);
	uptime = atoi(c);
	if ( uptime ) {
		b->msec += uptime - b->downtime;
	} else {
		b->msec += frame_msec / 2;
	}

	b->active = qfalse;
}



/*
===============
CL_KeyState

Returns the fraction of the frame that the key was down
===============
*/
float CL_KeyState( kbutton_t *key ) {
	float		val;
	int			msec;

	msec = key->msec;
	key->msec = 0;

	if ( key->active ) {
		// still down
		if ( !key->downtime ) {
			msec = com_frameTime;
		} else {
			msec += com_frameTime - key->downtime;
		}
		key->downtime = com_frameTime;
	}

#if 0
	if (msec) {
		Com_Printf ("%i ", msec);
	}
#endif

	val = (float)msec / frame_msec;
	if ( val < 0 ) {
		val = 0;
	}
	if ( val > 1 ) {
		val = 1;
	}

	return val;
}



void IN_UpDown(void) {IN_KeyDown(&in_up);}
void IN_UpUp(void) {IN_KeyUp(&in_up);}
void IN_DownDown(void) {IN_KeyDown(&in_down);}
void IN_DownUp(void) {IN_KeyUp(&in_down);}
void IN_LeftDown(void) {IN_KeyDown(&in_left);}
void IN_LeftUp(void) {IN_KeyUp(&in_left);}
void IN_RightDown(void) {IN_KeyDown(&in_right);}
void IN_RightUp(void) {IN_KeyUp(&in_right);}
void IN_ForwardDown(void) {IN_KeyDown(&in_forward);}
void IN_ForwardUp(void) {IN_KeyUp(&in_forward);}
void IN_BackDown(void) {IN_KeyDown(&in_back);}
void IN_BackUp(void) {IN_KeyUp(&in_back);}
void IN_LookupDown(void) {IN_KeyDown(&in_lookup);}
void IN_LookupUp(void) {IN_KeyUp(&in_lookup);}
void IN_LookdownDown(void) {IN_KeyDown(&in_lookdown);}
void IN_LookdownUp(void) {IN_KeyUp(&in_lookdown);}
void IN_MoveleftDown(void) {IN_KeyDown(&in_moveleft);}
void IN_MoveleftUp(void) {IN_KeyUp(&in_moveleft);}
void IN_MoverightDown(void) {IN_KeyDown(&in_moveright);}
void IN_MoverightUp(void) {IN_KeyUp(&in_moveright);}
void IN_AttackPrimaryDown(void) { IN_KeyDown(&in_buttons[0]);}
void IN_AttackPrimaryUp(void) { IN_KeyUp(&in_buttons[0]); }
void IN_AttackSecondaryDown(void) { IN_KeyDown(&in_buttons[1]); }
void IN_AttackSecondaryUp(void) { IN_KeyUp(&in_buttons[1]); }
void IN_UseDown(void) { IN_KeyDown(&in_buttons[3]); }
void IN_UseUp(void) { IN_KeyUp(&in_buttons[3]); }
void IN_LeanLeftDown(void) { IN_KeyDown(&in_buttons[4]); }
void IN_LeanLeftUp(void) { IN_KeyUp(&in_buttons[4]); }
void IN_LeanRightDown(void) { IN_KeyDown(&in_buttons[5]); }
void IN_LeanRightUp(void) { IN_KeyUp(&in_buttons[5]); }

void IN_SpeedDown(void) {IN_KeyDown(&in_speed);}
void IN_SpeedUp(void) {IN_KeyUp(&in_speed);}
void IN_StrafeDown(void) {IN_KeyDown(&in_strafe);}
void IN_StrafeUp(void) {IN_KeyUp(&in_strafe);}

#ifdef USE_VOIP
void IN_VoipRecordDown(void)
{
	IN_KeyDown(&in_voiprecord);
	Cvar_Set("cl_voipSend", "1");
}

void IN_VoipRecordUp(void)
{
	IN_KeyUp(&in_voiprecord);
	Cvar_Set("cl_voipSend", "0");
}
#endif

void IN_Button0Down(void) {IN_KeyDown(&in_buttons[0]);}
void IN_Button0Up(void) {IN_KeyUp(&in_buttons[0]);}
void IN_Button1Down(void) {IN_KeyDown(&in_buttons[1]);}
void IN_Button1Up(void) {IN_KeyUp(&in_buttons[1]);}
void IN_Button2Down(void) {IN_KeyDown(&in_buttons[2]);}
void IN_Button2Up(void) {IN_KeyUp(&in_buttons[2]);}
void IN_Button3Down(void) {IN_KeyDown(&in_buttons[3]);}
void IN_Button3Up(void) {IN_KeyUp(&in_buttons[3]);}
void IN_Button4Down(void) {IN_KeyDown(&in_buttons[4]);}
void IN_Button4Up(void) {IN_KeyUp(&in_buttons[4]);}
void IN_Button5Down(void) {IN_KeyDown(&in_buttons[5]);}
void IN_Button5Up(void) {IN_KeyUp(&in_buttons[5]);}
void IN_Button6Down(void) {IN_KeyDown(&in_buttons[6]);}
void IN_Button6Up(void) {IN_KeyUp(&in_buttons[6]);}
void IN_Button7Down(void) {IN_KeyDown(&in_buttons[7]);}
void IN_Button7Up(void) {IN_KeyUp(&in_buttons[7]);}
void IN_Button8Down(void) {IN_KeyDown(&in_buttons[8]);}
void IN_Button8Up(void) {IN_KeyUp(&in_buttons[8]);}
void IN_Button9Down(void) {IN_KeyDown(&in_buttons[9]);}
void IN_Button9Up(void) {IN_KeyUp(&in_buttons[9]);}
void IN_Button10Down(void) {IN_KeyDown(&in_buttons[10]);}
void IN_Button10Up(void) {IN_KeyUp(&in_buttons[10]);}
void IN_Button11Down(void) {IN_KeyDown(&in_buttons[11]);}
void IN_Button11Up(void) {IN_KeyUp(&in_buttons[11]);}
void IN_Button12Down(void) {IN_KeyDown(&in_buttons[12]);}
void IN_Button12Up(void) {IN_KeyUp(&in_buttons[12]);}
void IN_Button13Down(void) {IN_KeyDown(&in_buttons[13]);}
void IN_Button13Up(void) {IN_KeyUp(&in_buttons[13]);}
void IN_Button14Down(void) {IN_KeyDown(&in_buttons[14]);}
void IN_Button14Up(void) {IN_KeyUp(&in_buttons[14]);}
void IN_Button15Down(void) {IN_KeyDown(&in_buttons[15]);}
void IN_Button15Up(void) {IN_KeyUp(&in_buttons[15]);}

void IN_ButtonDown (void) {
	IN_KeyDown(&in_buttons[1]);}
void IN_ButtonUp (void) {
	IN_KeyUp(&in_buttons[1]);}

void IN_CenterView (void) {
	cl.viewangles[PITCH] = -SHORT2ANGLE(cl.snap.ps.delta_angles[PITCH]);
}


//==========================================================================

cvar_t	*cl_upspeed;
cvar_t	*cl_forwardspeed;
cvar_t	*cl_sidespeed;

cvar_t	*cl_yawspeed;
cvar_t	*cl_pitchspeed;

cvar_t	*cl_run;

cvar_t	*cl_anglespeedkey;


/*
================
CL_AdjustAngles

Moves the local angle positions
================
*/
void CL_AdjustAngles( void ) {
	float	speed;

	if ( in_speed.active ) {
		speed = 0.001 * cls.frametime * cl_anglespeedkey->value;
	} else {
		speed = 0.001 * cls.frametime;
	}

	if ( !in_strafe.active ) {
		cl.viewangles[YAW] -= speed*cl_yawspeed->value*CL_KeyState (&in_right);
		cl.viewangles[YAW] += speed*cl_yawspeed->value*CL_KeyState (&in_left);
	}

	cl.viewangles[PITCH] -= speed*cl_pitchspeed->value * CL_KeyState (&in_lookup);
	cl.viewangles[PITCH] += speed*cl_pitchspeed->value * CL_KeyState (&in_lookdown);
}

/*
================
CL_KeyMove

Sets the usercmd_t based on key states
================
*/
void CL_KeyMove( usercmd_t *cmd ) {
	int		movespeed;
	int		forward, side, up;

	forward = 0;
	side = 0;
	up = 0;
	movespeed = 127;
	if ( in_strafe.active ) {
		side += movespeed * CL_KeyState (&in_right);
		side -= movespeed * CL_KeyState (&in_left);
	}

	side += movespeed * CL_KeyState (&in_moveright);
	side -= movespeed * CL_KeyState (&in_moveleft);


	up += movespeed * CL_KeyState (&in_up);
	up -= movespeed * CL_KeyState (&in_down);

	forward += movespeed * CL_KeyState (&in_forward);
	forward -= movespeed * CL_KeyState (&in_back);

	cmd->forwardmove = ClampChar( forward );
	cmd->rightmove = ClampChar( side );
	cmd->upmove = ClampChar( up );
}

/*
=================
CL_GetMouseState
=================
*/
void CL_GetMouseState( int *x, int *y, unsigned int *buttons )
{
	if( x )
		*x = cl.mousex;

	if( y )
		*y = cl.mousey;

	if( buttons )
	{
		if( in_guimouse ) {
			*buttons = cl.mouseButtons;
		} else {
			*buttons = 0;
		}
	}
}

/*
=================
CL_SetMousePos
=================
*/
void CL_SetMousePos( int x, int y )
{
	cl.mousex = x;
	cl.mousey = y;
}

/*
=================
CL_MouseEvent
=================
*/
void CL_MouseEvent( int dx, int dy, int time ) {
	if( in_guimouse )
	{
		cl.mousex += dx;
		cl.mousey += dy;

		if( cl.mousex < 0 )
			cl.mousex = 0;

		if( cl.mousex > cls.glconfig.vidWidth )
			cl.mousex = cls.glconfig.vidWidth;

		if( cl.mousey < 0 )
			cl.mousey = 0;

		if( cl.mousey > cls.glconfig.vidHeight )
			cl.mousey = cls.glconfig.vidHeight;
	}
	else if ( !paused->integer )
	{
		cl.mouseDx[cl.mouseIndex] += dx;
		cl.mouseDy[cl.mouseIndex] += dy;
	}
}

/*
=================
CL_JoystickEvent

Joystick values stay set until changed
=================
*/
void CL_JoystickEvent( int axis, int value, int time ) {
	if ( axis < 0 || axis >= MAX_JOYSTICK_AXIS ) {
		Com_Error( ERR_DROP, "CL_JoystickEvent: bad axis %i", axis );
	}
	cl.joystickAxis[axis] = value;
}

/*
=================
CL_UpdateMouse

Added in OPM
Update mouse position with absolute position (relative to the window)
when the UI catcher is active.
=================
*/
void CL_UpdateMouse() {
    if (!(Key_GetCatcher() & KEYCATCH_UI)) {
        return;
    }

    if (IN_IsCursorActive()) {
        // If it's grabbed, don't use absolute position
        return;
    }

    if (com_unfocused->integer) {
        // Ignore updates when unfocused
        return;
    }

    IN_GetMousePosition(&cl.mousex, &cl.mousey);
}

/*
=================
CL_JoystickMove
=================
*/
void CL_JoystickMove( usercmd_t *cmd ) {
	float	anglespeed;

	float yaw     = j_yaw->value     * cl.joystickAxis[j_yaw_axis->integer];
	float right   = j_side->value    * cl.joystickAxis[j_side_axis->integer];
	float forward = j_forward->value * cl.joystickAxis[j_forward_axis->integer];
	float pitch   = j_pitch->value   * cl.joystickAxis[j_pitch_axis->integer];
	float up      = j_up->value      * cl.joystickAxis[j_up_axis->integer];

	if ( in_speed.active ^ cl_run->integer ) {
		cmd->buttons |= BUTTON_RUN;
	}

	if ( in_speed.active ) {
		anglespeed = 0.001 * cls.frametime * cl_anglespeedkey->value;
	} else {
		anglespeed = 0.001 * cls.frametime;
	}

	if ( !in_strafe.active ) {
		cl.viewangles[YAW] += anglespeed * yaw;
		cmd->rightmove = ClampChar( cmd->rightmove + (int)right );
	} else {
		cl.viewangles[YAW] += anglespeed * right;
		cmd->rightmove = ClampChar( cmd->rightmove + (int)yaw );
	}

	if ( in_mlooking ) {
		cl.viewangles[PITCH] += anglespeed * forward;
		cmd->forwardmove = ClampChar( cmd->forwardmove + (int)pitch );
	} else {
		cl.viewangles[PITCH] += anglespeed * pitch;
		cmd->forwardmove = ClampChar( cmd->forwardmove + (int)forward );
	}

	cmd->upmove = ClampChar( cmd->upmove + (int)up );
}

// HZM coop - THIRD-PERSON FREE CAM mouse routing. These are the (previously vestigial - nothing in the
// engine ever wrote them) client camera-look globals from cl_main.cpp that the cgame already reads every
// frame through cgi.get_camera_offset() in CG_OffsetThirdPersonView. While the cgame flags the free cam
// as owning the mouse (cg_freecamCapture, written once per frame by CG_UpdateFreecam in cg_view.c), mouse
// deltas accumulate HERE instead of turning cl.viewangles - so the character keeps its facing (usercmd
// angles freeze) while the chase camera orbits. DEPLOY NOTE: this exe change ships as a PAIR with the
// matching cgame.dll (the cgame applies/eases the orbit and owns the capture cvar).
extern qboolean camera_active;
extern vec3_t   camera_offset;

/*
=================
CL_MouseMove
=================
*/
void CL_MouseMove( usercmd_t *cmd ) {
	float	mx, my;
	float	cgameSensitivity;

	// allow mouse smoothing
	if (m_filter->integer)
	{
		mx = (cl.mouseDx[0] + cl.mouseDx[1]) * 0.5f;
		my = (cl.mouseDy[0] + cl.mouseDy[1]) * 0.5f;
	}
	else
	{
		mx = cl.mouseDx[cl.mouseIndex];
		my = cl.mouseDy[cl.mouseIndex];
	}

	cl.mouseIndex ^= 1;
	cl.mouseDx[cl.mouseIndex] = 0;
	cl.mouseDy[cl.mouseIndex] = 0;

    if (mx == 0.0f && my == 0.0f)
        return;

	if (cl_mouseAccel->value != 0.0f)
	{
		if(cl_mouseAccelStyle->integer == 0)
		{
			float accelSensitivity;
			float rate;
			
			rate = sqrt(mx * mx + my * my) / (float) frame_msec;

			accelSensitivity = cl_sensitivity->value + rate * cl_mouseAccel->value;
			mx *= accelSensitivity;
			my *= accelSensitivity;
			
			if(cl_showMouseRate->integer)
				Com_Printf("rate: %f, accelSensitivity: %f\n", rate, accelSensitivity);
		}
		else
		{
			float rate[2];
			float power[2];

			// sensitivity remains pretty much unchanged at low speeds
			// cl_mouseAccel is a power value to how the acceleration is shaped
			// cl_mouseAccelOffset is the rate for which the acceleration will have doubled the non accelerated amplification
			// NOTE: decouple the config cvars for independent acceleration setup along X and Y?

			rate[0] = fabs(mx) / (float) frame_msec;
			rate[1] = fabs(my) / (float) frame_msec;
			power[0] = powf(rate[0] / cl_mouseAccelOffset->value, cl_mouseAccel->value);
			power[1] = powf(rate[1] / cl_mouseAccelOffset->value, cl_mouseAccel->value);

			mx = cl_sensitivity->value * (mx + ((mx < 0) ? -power[0] : power[0]) * cl_mouseAccelOffset->value);
			my = cl_sensitivity->value * (my + ((my < 0) ? -power[1] : power[1]) * cl_mouseAccelOffset->value);

			if(cl_showMouseRate->integer)
				Com_Printf("ratex: %f, ratey: %f, powx: %f, powy: %f\n", rate[0], rate[1], power[0], power[1]);
		}
	}
	else
	{
		mx *= cl_sensitivity->value;
		my *= cl_sensitivity->value;
	}

	cgameSensitivity = 1.0f;
	if( cge && !UI_MenuActive() )
	{
		if( cge->CG_SensitivityScale() >= 0.0 ) {
			cgameSensitivity = cge->CG_SensitivityScale();
		}
	}

	mx *= cgameSensitivity;
	my *= cgameSensitivity;

	cmd->buttons |= BUTTON_ANY;

	// HZM coop - THIRD-PERSON FREE CAM: when the cgame owns the mouse (free orbit around the character),
	// route the deltas into the camera_offset orbit accumulator and leave cl.viewangles untouched. The
	// full sensitivity chain (accel / cl_sensitivity / cgame scale) is already folded into mx/my above,
	// and m_yaw/m_pitch below match the normal look feel exactly (including pitch invert). The total
	// orbit pitch (frozen aim pitch + orbit offset) is clamped to +/-85 so the camera never flips over
	// the poles; yaw is free 360 (kept normalized so the cgame ease-out always takes the short way home).
	{
		static cvar_t  *cl_freecamCapture  = NULL;
		static cvar_t  *cl_freecamFold    = NULL;
		static qboolean bWasFreecamCapture = qfalse;
		if ( !cl_freecamCapture ) {
			cl_freecamCapture = Cvar_Get( "cg_freecamCapture", "0", 0 );
			cl_freecamFold    = Cvar_Get( "cg_freecamFold", "0", 0 ); // published by the cgame while IN COVER
		}
		if ( cge && cl_freecamCapture->integer ) {
			float aimPitch;

			// HZM coop [226] - capture RE-ENGAGED while covered (peek just released): zero the
			// orbit so the camera resumes exactly AT the aim held while peeked ("camera should
			// always go back to where you were aiming when you let go" - user).
			if ( !bWasFreecamCapture && cl_freecamFold->integer ) {
				camera_offset[YAW]   = 0;
				camera_offset[PITCH] = 0;
			}
			bWasFreecamCapture = qtrue;

			// HZM coop [237] - EFFECTIVE aim pitch including the server's delta_angles: reload /
			// respawn / script view nudges land in delta_angles while cl.viewangles stays frozen
			// under capture, so a clamp computed from cl.viewangles alone drifted its +/-85 window
			// off the real pitch and could wedge with zero mouse headroom ("after you reload in
			// free cam your camera gets stuck at an upwards angle" - user).
			aimPitch = cl.viewangles[PITCH];
			if ( cl.snap.valid ) {
				aimPitch += SHORT2ANGLE( cl.snap.ps.delta_angles[PITCH] );
			}
			aimPitch = AngleNormalize180( aimPitch );

			camera_offset[YAW]   -= m_yaw->value * mx;
			camera_offset[YAW]    = AngleNormalize180( camera_offset[YAW] );
			camera_offset[PITCH] += m_pitch->value * my;
			if ( aimPitch + camera_offset[PITCH] > 85.0f ) {
				camera_offset[PITCH] = 85.0f - aimPitch;
			} else if ( aimPitch + camera_offset[PITCH] < -85.0f ) {
				camera_offset[PITCH] = -85.0f - aimPitch;
			}

			// HZM coop [user 07-12] - FREE-AIM PITCH TRACKING: the orbit used to hold ALL of the pitch,
			// so the character never aimed up/down in free cam ("the gun should still move as well
			// unless im looking behind me"). While the camera is within 100 deg of the body facing,
			// MIGRATE the orbit pitch into the real viewangles each frame: the camera's TOTAL pitch
			// (aimPitch + offset) is unchanged - no visual jump - but the server now sees it, so the
			// character raises/lowers the gun with the camera. Orbited BEHIND, pitch stays in the
			// orbit and the aim simply holds; swinging back forward folds it in smoothly. The total
			// was already clamped to +/-85 above, so the migrated aim stays inside the server clamp.
			{
				static cvar_t *cl_freecamAimPitch = NULL;
				if ( !cl_freecamAimPitch ) {
					cl_freecamAimPitch = Cvar_Get( "cl_freecamAimPitch", "1", CVAR_ARCHIVE );
				}
				if ( cl_freecamAimPitch->integer && !cl_freecamFold->integer
					&& fabs( camera_offset[YAW] ) < 100.0f && camera_offset[PITCH] != 0.0f ) {
					cl.viewangles[PITCH] += camera_offset[PITCH];
					camera_offset[PITCH]  = 0;
				}
			}

			camera_active = qtrue;
			return; // the player's viewangles stay frozen while the free cam owns the mouse
		}
		// HZM coop [226] - capture DROPPED while covered (peek just started): FOLD the orbit into
		// the real viewangles so the shoulder aim opens exactly where the camera was looking
		// ("default to where you were just aiming before you went to shoulder view" - user).
		// [228] fold-GRACE: when cover ENDS, the fold flag and the capture can clear in the SAME
		// frame - the un-folded orbit was discarded and the view snapped to the stale pre-cover
		// angles. Remember the fold flag for a short grace window so that drop still folds.
		{
			static int iLastFoldMs = -10000;
			if ( cl_freecamFold->integer ) {
				iLastFoldMs = cls.realtime;
			}
			if ( bWasFreecamCapture && !cl_freecamFold->integer
				&& ( cls.realtime - iLastFoldMs ) < 600 ) {
				bWasFreecamCapture = qfalse;
				cl.viewangles[YAW]    = AngleNormalize180( cl.viewangles[YAW] + camera_offset[YAW] );
				cl.viewangles[PITCH] += camera_offset[PITCH];
				if ( cl.viewangles[PITCH] > 85.0f ) {
					cl.viewangles[PITCH] = 85.0f;
				} else if ( cl.viewangles[PITCH] < -85.0f ) {
					cl.viewangles[PITCH] = -85.0f;
				}
				camera_offset[YAW]   = 0;
				camera_offset[PITCH] = 0;
			}
		}
		// HZM coop [user 2026-08-27] THE ORBIT MUST SURVIVE THE ADS HANDOFF. cg_freecamFold has
		// NO publisher anywhere in the tree (grep: this file is its only reader), so the fold
		// below was dead code and every capture drop silently THREW THE ORBIT AWAY, restoring
		// the frozen pre-orbit aim: "when I move the camera behind me and then hold right mouse
		// to go over shoulder it just forces my crosshair back in front of my player where he was
		// aiming before". Folding on EVERY drop is the [226] intent stated in the comments above
		// ("the shoulder aim opens exactly where the camera was looking") and is what makes the
		// prone roll-onto-your-back reachable from free cam at all: orbit behind, hold ADS, and
		// the aim - now really pointing behind you - trips the supine latch.
		if ( bWasFreecamCapture ) {
			static cvar_t *cl_freecamFoldAds = NULL;
			if ( !cl_freecamFoldAds ) {
				cl_freecamFoldAds = Cvar_Get( "cl_freecamFoldAds", "1", CVAR_ARCHIVE );
			}
			bWasFreecamCapture = qfalse;
			if ( cl_freecamFold->integer || cl_freecamFoldAds->integer ) {
				cl.viewangles[YAW]    = AngleNormalize180( cl.viewangles[YAW] + camera_offset[YAW] );
				cl.viewangles[PITCH] += camera_offset[PITCH];
				if ( cl.viewangles[PITCH] > 85.0f ) {
					cl.viewangles[PITCH] = 85.0f;
				} else if ( cl.viewangles[PITCH] < -85.0f ) {
					cl.viewangles[PITCH] = -85.0f;
				}
				camera_offset[YAW]   = 0;
				camera_offset[PITCH] = 0;
			}
		}
		camera_active = qfalse;
	}

	// add mouse X/Y movement to cmd
	if(in_strafe.active)
		cmd->rightmove = ClampChar(cmd->rightmove + m_side->value * mx);
	else
		cl.viewangles[YAW] -= m_yaw->value * mx;

	if ((in_mlooking || cl_freelook->integer) && !in_strafe.active)
		cl.viewangles[PITCH] += m_pitch->value * my;
	else
		cmd->forwardmove = ClampChar(cmd->forwardmove - m_forward->value * my);

	// HZM coop [user 2026-08-27] MOUNTED AIM ARC - you pivot ON the rest.
	//
	// [bug-2133] PLACED AFTER the mouse deltas, and comparing in the EFFECTIVE aim frame.
	// Two bugs lived in the first cut. It ran BEFORE this frame's delta was added, so the angle
	// actually packed into the usercmd was the clamped angle plus a whole frame of mouse movement -
	// a fast sweep parked the aim well outside the cone and the view snapped back the moment the
	// mouse stopped. And it compared cl.viewangles against a centre published from ps.viewangles;
	// those frames differ by delta_angles (spawn facing, script nudges, and recoil, which rewrites
	// pitch on every shot), so mounting yanked the aim by that offset. The server now publishes the
	// command-frame yaw, and pitch is compared with delta_angles folded in the way the free-cam
	// clamp above already does.
	//
	// This is the trade that makes mounting a MODE rather than a free buff: the gun is steadier, but
	// your field of fire is a cone and someone coming round your flank is a real problem.
	{
		static cvar_t *cl_braceView = NULL;
		static cvar_t *cl_braceYaw  = NULL;
		static cvar_t *cl_braceArc  = NULL;
		static cvar_t *cl_braceUp   = NULL;
		static cvar_t *cl_braceDn   = NULL;
		if ( !cl_braceView ) {
			cl_braceView = Cvar_Get( "coop_braceView", "0", 0 );
			cl_braceYaw  = Cvar_Get( "coop_braceYaw", "0", 0 );
			cl_braceArc  = Cvar_Get( "coop_braceArc", "40", CVAR_ARCHIVE );
			cl_braceUp   = Cvar_Get( "coop_braceArcUp", "25", CVAR_ARCHIVE );
			cl_braceDn   = Cvar_Get( "coop_braceArcDown", "20", CVAR_ARCHIVE );
		}
		if ( cl_braceView->integer > 50 && cl_braceArc->value > 1.0f && cl.snap.valid ) {
			float off = AngleNormalize180( cl.viewangles[YAW] - (float)cl_braceYaw->integer );
			float arc = cl_braceArc->value;
			float pit = cl.viewangles[PITCH] + SHORT2ANGLE( cl.snap.ps.delta_angles[PITCH] );
			pit = AngleNormalize180( pit );
			if ( off > arc ) {
				cl.viewangles[YAW] -= ( off - arc );
			} else if ( off < -arc ) {
				cl.viewangles[YAW] -= ( off + arc );
			}
			if ( pit < -cl_braceUp->value ) {
				cl.viewangles[PITCH] -= ( pit + cl_braceUp->value );
			} else if ( pit > cl_braceDn->value ) {
				cl.viewangles[PITCH] -= ( pit - cl_braceDn->value );
			}
		}
	}

	if (!isfinite(cl.viewangles[PITCH]) || !isfinite(cl.viewangles[YAW])) {
		Com_DPrintf("Invalid client viewangles encountered (view pitch: %f, view yaw: %f)!\n", cl.viewangles[PITCH], cl.viewangles[YAW]);
		Com_DPrintf("cgameSensitivity: %f | mx: %f | my: %f | m_pitch: %f | m_yaw: %f\n", cgameSensitivity, mx, my, m_pitch->value, m_yaw->value);
		Com_DPrintf("Resetting client viewangles\n");
		cl.viewangles[PITCH] = 0;
		cl.viewangles[YAW] = 0;
	}
}

/*
==============
CL_ClearButtons
==============
*/
void CL_ClearButtons( void ) {
	memset( in_buttons, 0, sizeof( in_buttons ) );
}

/*
==============
CL_CmdButtons
==============
*/
void CL_CmdButtons( usercmd_t *cmd ) {
	int		i;
	
	//
	// figure button bits
	// send a button bit even if the key was pressed and released in
	// less than a frame
	//
	for (i = 0 ; i < 15 ; i++) {
		if ( in_buttons[i].active || in_buttons[i].wasPressed ) {
			cmd->buttons |= 1 << i;
		}
		in_buttons[i].wasPressed = qfalse;
	}

	if (UI_MenuActive() || UI_ConsoleIsOpen()) {
		cmd->buttons |= BUTTON_TALK;
	}
	
	if ( in_speed.active ^ cl_run->integer ) {
		cmd->buttons |= BUTTON_RUN;
	}

	if (cge) {
		// send weapon commands from cg
		cmd->buttons |= cge->CG_WeaponCommandButtonBits();
	}

	// allow the game to know if any key at all is
	// currently pressed, even if it isn't bound to anything
	if ( anykeydown && Key_GetCatcher() == 0) {
		cmd->buttons |= BUTTON_MOUSE;
	}

	if (anykeydown) {
		cmd->buttons |= BUTTON_ANY;
	}
}


/*
==============
CL_FinishMove
==============
*/
void CL_FinishMove( usercmd_t *cmd ) {
	int		i;

	// send the current server time so the amount of movement
	// can be determined without allowing cheating
	cmd->serverTime = cl.serverTime;

	for (i=0 ; i<3 ; i++) {
		cmd->angles[i] = ANGLE2SHORT(cl.viewangles[i]);
	}

	// HZM coop [226] - IN-COVER free-look AIMS THE SERVER: while covered (cg_freecamFold,
	// published by the cgame) and the free orbit owns the mouse, send the COMPOSITED camera
	// direction (frozen viewangles + orbit offset) as the usercmd angles. cl.viewangles stay
	// frozen (they are the fold base for the peek handoff above); the server's viewangles now
	// track the camera, so LOW-cover blindfire fires where the camera points and the torso
	// aim-twist leans the upper body toward it. Wall-corner blindfire keeps its anchored steer.
	{
		static cvar_t *fm_freecamFold    = NULL;
		static cvar_t *fm_freecamCapture = NULL;
		if ( !fm_freecamFold ) {
			fm_freecamFold    = Cvar_Get( "cg_freecamFold", "0", 0 );
			fm_freecamCapture = Cvar_Get( "cg_freecamCapture", "0", 0 );
		}
		if ( cge && fm_freecamFold->integer && fm_freecamCapture->integer ) {
			float fAimPitch = cl.viewangles[PITCH] + camera_offset[PITCH];
			if ( fAimPitch > 85.0f ) {
				fAimPitch = 85.0f;
			} else if ( fAimPitch < -85.0f ) {
				fAimPitch = -85.0f;
			}
			cmd->angles[YAW]   = ANGLE2SHORT( AngleNormalize180( cl.viewangles[YAW] + camera_offset[YAW] ) );
			cmd->angles[PITCH] = ANGLE2SHORT( fAimPitch );
		}
	}
}


/*
=================
CL_CreateCmd
=================
*/
usercmd_t CL_CreateCmd( void ) {
	usercmd_t	cmd;
	vec3_t		oldAngles;

	VectorCopy( cl.viewangles, oldAngles );

	// keyboard angle adjustment
	CL_AdjustAngles ();

	Com_Memset( &cmd, 0, sizeof( cmd ) );

	CL_CmdButtons( &cmd );

	// get basic movement from keyboard
	CL_KeyMove( &cmd );

	// get basic movement from mouse
	CL_MouseMove( &cmd );

	// get basic movement from joystick
	CL_JoystickMove( &cmd );

	// check to make sure the angles haven't wrapped
	if ( cl.viewangles[PITCH] - oldAngles[PITCH] > 90 ) {
		cl.viewangles[PITCH] = oldAngles[PITCH] + 90;
	} else if ( oldAngles[PITCH] - cl.viewangles[PITCH] > 90 ) {
		cl.viewangles[PITCH] = oldAngles[PITCH] - 90;
	}

	// HZM coop - FREE CAM v2 "modern third person" movement: while the free orbit owns the mouse
	// AND the player is giving movement input, steer the CHARACTER toward the camera-relative
	// movement direction (hold W = run where the camera looks, A/D = camera-relative strafes,
	// the classic GTA/Souls scheme) and counter-rotate the orbit by the same amount so the CAMERA
	// itself does not move. The facing eases at cl_freecamTurnRate deg/s (turn-to-face, not a snap)
	// and the move vector is re-expressed against the NEW facing each frame, so the world-space
	// run direction is exactly what was asked from frame one even while the body is still turning.
	// No movement input = no turning (pure orbit-and-inspect is untouched). ADS still drops the
	// capture and instantly restores precise aim. cl_freecamAutoFace 0 = old character-relative WASD.
	{
		static cvar_t *fcCap = NULL, *fcAuto = NULL, *fcRate = NULL;
		if ( !fcCap )  { fcCap  = Cvar_Get( "cg_freecamCapture", "0", 0 ); }
		if ( !fcAuto ) { fcAuto = Cvar_Get( "cl_freecamAutoFace", "1", CVAR_ARCHIVE ); } // [user 07-11] default ON - free cam IS the modern-TPS scheme: idle = free orbit, moving = body turns to face the camera and runs that way (steer the character with the camera). Set 0 for the old orbit-only (WASD relative to frozen facing).
		if ( !fcRate ) { fcRate = Cvar_Get( "cl_freecamTurnRate", "480", CVAR_ARCHIVE ); }
		if ( fcCap->integer && fcAuto->integer && ( cmd.forwardmove || cmd.rightmove ) ) {
			float fm      = (float)cmd.forwardmove;
			float rm      = (float)cmd.rightmove;
			float mag     = sqrtf( fm * fm + rm * rm );
			float moveAng = RAD2DEG( atan2f( -rm, fm ) );
			float heading = cl.viewangles[YAW] + camera_offset[YAW] + moveAng;
			float diff    = AngleNormalize180( heading - cl.viewangles[YAW] );
			float step    = fcRate->value * 0.001f * cls.frametime;
			float rel;
			if ( step < 0 ) { step = 0; }
			if ( fabsf( diff ) <= step ) { step = fabsf( diff ); }
			if ( diff < 0 ) { step = -step; }
			cl.viewangles[YAW] = AngleNormalize180( cl.viewangles[YAW] + step );
			camera_offset[YAW] = AngleNormalize180( camera_offset[YAW] - step );
			rel = (float)DEG2RAD( AngleNormalize180( heading - cl.viewangles[YAW] ) );
			if ( mag > 127.0f ) { mag = 127.0f; }
			cmd.forwardmove = ClampChar( (int)( cosf( rel ) * mag ) );
			cmd.rightmove   = ClampChar( (int)( -sinf( rel ) * mag ) );
		}
	}

	// store out the final values
	CL_FinishMove( &cmd );

	// draw debug graphs of turning for mouse testing
	if ( cl_debugMove->integer ) {
		if ( cl_debugMove->integer == 1 ) {
			SCR_DebugGraph( fabs(cl.viewangles[YAW] - oldAngles[YAW]) );
		}
		if ( cl_debugMove->integer == 2 ) {
			SCR_DebugGraph( fabs(cl.viewangles[PITCH] - oldAngles[PITCH]) );
		}
	}

	return cmd;
}


/*
=================
CL_CreateNewCommands

Create a new usercmd_t structure for this frame
=================
*/
void CL_CreateNewCommands( void ) {
	usercmd_t	*cmd;
	int			cmdNum;

	// no need to create usercmds until we have a gamestate
	if ( clc.state < CA_PRIMED ) {
		return;
	}

	frame_msec = com_frameTime - old_com_frameTime;

	// if running over 1000fps, act as if each frame is 1ms
	// prevents divisions by zero
	if ( frame_msec < 1 ) {
		frame_msec = 1;
	}

	// if running less than 5fps, truncate the extra time to prevent
	// unexpected moves after a hitch
	if ( frame_msec > 200 ) {
		frame_msec = 200;
	}
	old_com_frameTime = com_frameTime;


	// generate a command for this frame
	cl.cmdNumber++;
	cmdNum = cl.cmdNumber & CMD_MASK;
	cl.cmds[cmdNum] = CL_CreateCmd ();
	cmd = &cl.cmds[cmdNum];
}

/*
=================
CL_EyeInfo

Gather eyes data from client game module
=================
*/
void CL_EyeInfo(usereyes_t* info) {
	int i;
	vec3_t vAngles, vOfs;

	if (cge) {
		// copy the eye offset
		cge->CG_EyeOffset(&vOfs);

		for (i = 0; i < 3; i++) {
			vOfs[i] += 0.5;
			if (vOfs[i] < -127.0) {
				vOfs[i] = -127.0;
			} else if (vOfs[i] > 128.0) {
				vOfs[i] = 128.0;
			}

			info->ofs[i] = vOfs[i];
		}

		cge->CG_EyeAngles(&vAngles);
		info->angles[0] = vAngles[0];
		info->angles[1] = vAngles[1];
	} else {
		// clear the eye offset
		for (i = 0; i < 3; i++) {
			info->ofs[i] = 0;
		}

		info->angles[0] = info->angles[1] = 0.0;
	}
}

/*
=================
CL_ReadyToSendPacket

Returns qfalse if we are over the maxpackets limit
and should choke back the bandwidth a bit by not sending
a packet this frame.  All the commands will still get
delivered in the next packet, but saving a header and
getting more delta compression will reduce total bandwidth.
=================
*/
qboolean CL_ReadyToSendPacket( void ) {
	int		oldPacketNum;
	int		delta;

	// don't send anything if playing back a demo
	if ( clc.demoplaying || clc.state == CA_CINEMATIC ) {
		return qfalse;
	}

	// If we are downloading, we send no less than 50ms between packets
	if ( *clc.downloadTempName &&
		cls.realtime - clc.lastPacketSentTime < 50 ) {
		return qfalse;
	}

	// if we don't have a valid gamestate yet, only send
	// one packet a second
	if ( clc.state != CA_ACTIVE &&
		clc.state != CA_PRIMED &&
		!*clc.downloadTempName &&
		cls.realtime - clc.lastPacketSentTime < 1000 ) {
		return qfalse;
	}

	// send every frame for loopbacks
	if ( clc.netchan.remoteAddress.type == NA_LOOPBACK ) {
		return qtrue;
	}

	// send every frame for LAN
	if ( cl_lanForcePackets->integer && Sys_IsLANAddress( clc.netchan.remoteAddress ) ) {
		return qtrue;
	}

	// check for exceeding cl_maxpackets
	if ( cl_maxpackets->integer < 30 ) {
		Cvar_Set( "cl_maxpackets", "30" );
	} else if ( cl_maxpackets->integer > 125 ) {
		Cvar_Set( "cl_maxpackets", "125" );
	}
	oldPacketNum = (clc.netchan.outgoingSequence - 1) & PACKET_MASK;
	delta = cls.realtime -  cl.outPackets[ oldPacketNum ].p_realtime;
	if ( delta < 1000 / cl_maxpackets->integer ) {
		// the accumulated commands will go out in the next packet
		return qfalse;
	}

	return qtrue;
}

/*
===================
CL_WritePacket

Create and send the command packet to the server
Including both the reliable commands and the usercmds

During normal gameplay, a client packet will contain something like:

4	sequence number
2	qport
4	serverid
4	acknowledged sequence number
4	clc.serverCommandSequence
<optional reliable commands>
1	clc_move or clc_moveNoDelta
1	command count
<count * usercmds>

===================
*/
void CL_WritePacket( void ) {
	msg_t		buf;
	byte		data[MAX_MSGLEN];
	int			i, j;
	usercmd_t	*cmd, *oldcmd;
	usercmd_t	nullcmd;
	int			packetNum;
	int			oldPacketNum;
	int			count, key;
	usereyes_t	eyeInfo;

	// don't send anything if playing back a demo
	if ( clc.demoplaying || clc.state == CA_CINEMATIC ) {
		return;
	}

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;		
		
	MSG_Init( &buf, data, sizeof(data) );

	MSG_Bitstream( &buf );
	// write the current serverId so the server
	// can tell if this is from the current gameState
	MSG_WriteLong( &buf, cl.serverId );

	// write the last message we received, which can
	// be used for delta compression, and is also used
	// to tell if we dropped a gamestate
	MSG_WriteLong( &buf, clc.serverMessageSequence );

	// write the last reliable message we received
	MSG_WriteLong( &buf, clc.serverCommandSequence );

	// write any unacknowledged clientCommands
	for ( i = clc.reliableAcknowledge + 1 ; i <= clc.reliableSequence ; i++ ) {
		MSG_WriteByte( &buf, clc_clientCommand );
		MSG_WriteLong( &buf, i );
		MSG_WriteScrambledString( &buf, clc.reliableCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
	}

	// we want to send all the usercmds that were generated in the last
	// few packet, so even if a couple packets are dropped in a row,
	// all the cmds will make it to the server
	if ( cl_packetdup->integer < 0 ) {
		Cvar_Set( "cl_packetdup", "0" );
	} else if ( cl_packetdup->integer > 5 ) {
		Cvar_Set( "cl_packetdup", "5" );
	}
	oldPacketNum = (clc.netchan.outgoingSequence - 1 - cl_packetdup->integer) & PACKET_MASK;
	count = cl.cmdNumber - cl.outPackets[ oldPacketNum ].p_cmdNumber;
	if ( count > MAX_PACKET_USERCMDS ) {
		count = MAX_PACKET_USERCMDS;
		Com_Printf("MAX_PACKET_USERCMDS\n");
	}

#ifdef USE_VOIP
	if (clc.voipOutgoingDataSize > 0)
	{
		if((clc.voipFlags & VOIP_SPATIAL) || Com_IsVoipTarget(clc.voipTargets, sizeof(clc.voipTargets), -1))
		{
			MSG_WriteByte (&buf, clc_voipOpus);
			MSG_WriteByte (&buf, clc.voipOutgoingGeneration);
			MSG_WriteLong (&buf, clc.voipOutgoingSequence);
			MSG_WriteByte (&buf, clc.voipOutgoingDataFrames);
			MSG_WriteData (&buf, clc.voipTargets, sizeof(clc.voipTargets));
			MSG_WriteByte(&buf, clc.voipFlags);
			MSG_WriteShort (&buf, clc.voipOutgoingDataSize);
			MSG_WriteData (&buf, clc.voipOutgoingData, clc.voipOutgoingDataSize);

			// If we're recording a demo, we have to fake a server packet with
			//  this VoIP data so it gets to disk; the server doesn't send it
			//  back to us, and we might as well eliminate concerns about dropped
			//  and misordered packets here.
			if(clc.demorecording && !clc.demowaiting)
			{
				const int voipSize = clc.voipOutgoingDataSize;
				msg_t fakemsg;
				byte fakedata[MAX_MSGLEN];
				MSG_Init (&fakemsg, fakedata, sizeof (fakedata));
				MSG_Bitstream (&fakemsg);
				MSG_WriteLong (&fakemsg, clc.reliableAcknowledge);
				MSG_WriteByte (&fakemsg, svc_voipOpus);
				MSG_WriteShort (&fakemsg, clc.clientNum);
				MSG_WriteByte (&fakemsg, clc.voipOutgoingGeneration);
				MSG_WriteLong (&fakemsg, clc.voipOutgoingSequence);
				MSG_WriteByte (&fakemsg, clc.voipOutgoingDataFrames);
				MSG_WriteShort (&fakemsg, clc.voipOutgoingDataSize );
				MSG_WriteBits (&fakemsg, clc.voipFlags, VOIP_FLAGCNT);
				MSG_WriteData (&fakemsg, clc.voipOutgoingData, voipSize);
				MSG_WriteByte (&fakemsg, svc_EOF);
				CL_WriteDemoMessage (&fakemsg, 0);
			}

			clc.voipOutgoingSequence += clc.voipOutgoingDataFrames;
			clc.voipOutgoingDataSize = 0;
			clc.voipOutgoingDataFrames = 0;
		}
		else
		{
			// We have data, but no targets. Silently discard all data
			clc.voipOutgoingDataSize = 0;
			clc.voipOutgoingDataFrames = 0;
		}
	}
#endif

	if ( count >= 1 ) {
		if ( cl_showSend->integer ) {
			Com_Printf( "(%i)", count );
		}

		// begin a client move command
		if ( cl_nodelta->integer || !cl.snap.valid || clc.demowaiting
			|| clc.serverMessageSequence != cl.snap.messageNum ) {
			MSG_WriteByte (&buf, clc_moveNoDelta);
		} else {
			MSG_WriteByte (&buf, clc_move);
		}

		// write the command count
		MSG_WriteByte( &buf, count );

		CL_EyeInfo( &eyeInfo );
		MSG_WriteDeltaEyeInfo( &buf, &cl.outPackets[ oldPacketNum ].p_eyeinfo, &eyeInfo );
		// use the checksum feed in the key
		key = clc.checksumFeed;
		// also use the message acknowledge
		key ^= clc.serverMessageSequence;
		// also use the last acknowledged server command in the key
		key ^= Com_HashKey(clc.serverCommands[ clc.serverCommandSequence & (MAX_RELIABLE_COMMANDS-1) ], 32);

		// write all the commands, including the predicted command
		for ( i = 0 ; i < count ; i++ ) {
			j = (cl.cmdNumber - count + i + 1) & CMD_MASK;
			cmd = &cl.cmds[j];
			MSG_WriteDeltaUsercmdKey (&buf, key, oldcmd, cmd);
			oldcmd = cmd;
		}
	}
	else {
        eyeInfo.ofs[0] = 0;
        eyeInfo.ofs[1] = 0;
        eyeInfo.ofs[2] = 0;
        eyeInfo.angles[0] = cl.viewangles[0];
        eyeInfo.angles[1] = cl.viewangles[1];
	}

	//
	// deliver the message
	//
	packetNum = clc.netchan.outgoingSequence & PACKET_MASK;
	cl.outPackets[ packetNum ].p_realtime = cls.realtime;
	cl.outPackets[ packetNum ].p_serverTime = oldcmd->serverTime;
	cl.outPackets[ packetNum ].p_cmdNumber = cl.cmdNumber;
	cl.outPackets[ packetNum ].p_eyeinfo = eyeInfo;
	clc.lastPacketSentTime = cls.realtime;

	if ( cl_showSend->integer ) {
		Com_Printf( "%zu ", buf.cursize );
	}

	CL_Netchan_Transmit (&clc.netchan, &buf);
}

/*
=================
CL_SendCmd

Called every frame to builds and sends a command packet to the server.
=================
*/
void CL_SendCmd( void ) {
	// don't send any message if not connected
	if ( clc.state < CA_CONNECTED ) {
		return;
	}

	// don't send commands if paused
	if ( com_sv_running->integer && paused->integer  ) {
		return;
	}

	// we create commands even if a demo is playing,
	CL_CreateNewCommands();

	// don't send a packet if the last packet was sent too recently
	if ( !CL_ReadyToSendPacket() ) {
		if ( cl_showSend->integer ) {
			Com_Printf( ". " );
		}
		return;
	}

	CL_WritePacket();
}

/*
============
CL_InitInput
============
*/
void CL_InitInput( void ) {
	Cmd_AddCommand("centerview", IN_CenterView);

	Cmd_AddCommand("+moveup", IN_UpDown);
	Cmd_AddCommand("-moveup", IN_UpUp);
	Cmd_AddCommand("+movedown", IN_DownDown);
	Cmd_AddCommand("-movedown", IN_DownUp);
	Cmd_AddCommand("+left", IN_LeftDown);
	Cmd_AddCommand("-left", IN_LeftUp);
	Cmd_AddCommand("+right", IN_RightDown);
	Cmd_AddCommand("-right", IN_RightUp);
	Cmd_AddCommand("+forward", IN_ForwardDown);
	Cmd_AddCommand("-forward", IN_ForwardUp);
	Cmd_AddCommand("+back", IN_BackDown);
	Cmd_AddCommand("-back", IN_BackUp);
	Cmd_AddCommand("+lookup", IN_LookupDown);
	Cmd_AddCommand("-lookup", IN_LookupUp);
	Cmd_AddCommand("+lookdown", IN_LookdownDown);
	Cmd_AddCommand("-lookdown", IN_LookdownUp);
	Cmd_AddCommand("+strafe", IN_StrafeDown);
	Cmd_AddCommand("-strafe", IN_StrafeUp);
	Cmd_AddCommand("+moveleft", IN_MoveleftDown);
	Cmd_AddCommand("-moveleft", IN_MoveleftUp);
	Cmd_AddCommand("+moveright", IN_MoverightDown);
	Cmd_AddCommand("-moveright", IN_MoverightUp);
	Cmd_AddCommand("+attack", IN_AttackPrimaryDown);
	Cmd_AddCommand("-attack", IN_AttackPrimaryUp);
	Cmd_AddCommand("+attackprimary", IN_AttackPrimaryDown);
	Cmd_AddCommand("-attackprimary", IN_AttackPrimaryUp);
	Cmd_AddCommand("+attacksecondary", IN_AttackSecondaryDown);
	Cmd_AddCommand("-attacksecondary", IN_AttackSecondaryUp);
	Cmd_AddCommand("+use", IN_Button3Down);
	Cmd_AddCommand("-use", IN_Button3Up);
	Cmd_AddCommand("+leanleft", IN_LeanLeftDown);
	Cmd_AddCommand("-leanleft", IN_LeanLeftUp);
	Cmd_AddCommand("+leanright", IN_LeanRightDown);
	Cmd_AddCommand("-leanright", IN_LeanRightUp);
	Cmd_AddCommand("+speed", IN_SpeedDown);
	Cmd_AddCommand("-speed", IN_SpeedUp);
	Cmd_AddCommand("+button0", IN_Button0Down);
	Cmd_AddCommand("-button0", IN_Button0Up);
	Cmd_AddCommand("+button1", IN_Button1Down);
	Cmd_AddCommand("-button1", IN_Button1Up);
	Cmd_AddCommand("+button2", IN_Button2Down);
	Cmd_AddCommand("-button2", IN_Button2Up);
	Cmd_AddCommand("+button3", IN_Button3Down);
	Cmd_AddCommand("-button3", IN_Button3Up);
	Cmd_AddCommand("+button4", IN_Button4Down);
	Cmd_AddCommand("-button4", IN_Button4Up);
	Cmd_AddCommand("+button5", IN_Button5Down);
	Cmd_AddCommand("-button5", IN_Button5Up);
	Cmd_AddCommand("+button6", IN_Button6Down);
	Cmd_AddCommand("-button6", IN_Button6Up);
	Cmd_AddCommand("+button7", IN_Button7Down);
	Cmd_AddCommand("-button7", IN_Button7Up);
	Cmd_AddCommand("+button8", IN_Button8Down);
	Cmd_AddCommand("-button8", IN_Button8Up);
	Cmd_AddCommand("+button9", IN_Button9Down);
	Cmd_AddCommand("-button9", IN_Button9Up);
	Cmd_AddCommand("+button10", IN_Button10Down);
	Cmd_AddCommand("-button10", IN_Button10Up);
	Cmd_AddCommand("+button11", IN_Button11Down);
	Cmd_AddCommand("-button11", IN_Button11Up);
	Cmd_AddCommand("+button12", IN_Button12Down);
	Cmd_AddCommand("-button12", IN_Button12Up);
	Cmd_AddCommand("+button13", IN_Button13Down);
	Cmd_AddCommand("-button13", IN_Button13Up);
	Cmd_AddCommand("+button14", IN_Button14Down);
	Cmd_AddCommand("-button14", IN_Button14Up);
	Cmd_AddCommand("+mlook", IN_MLookDown);
	Cmd_AddCommand("-mlook", IN_MLookUp);

#ifdef USE_VOIP
	Cmd_AddCommand ("+voiprecord", IN_VoipRecordDown);
	Cmd_AddCommand ("-voiprecord", IN_VoipRecordUp);
#endif

	cl_nodelta = Cvar_Get ("cl_nodelta", "0", 0);
	cl_debugMove = Cvar_Get ("cl_debugMove", "0", 0);
}

/*
============
CL_ShutdownInput
============
*/
void CL_ShutdownInput(void)
{
	Cmd_RemoveCommand("centerview");

	Cmd_RemoveCommand("+moveup");
	Cmd_RemoveCommand("-moveup");
	Cmd_RemoveCommand("+movedown");
	Cmd_RemoveCommand("-movedown");
	Cmd_RemoveCommand("+left");
	Cmd_RemoveCommand("-left");
	Cmd_RemoveCommand("+right");
	Cmd_RemoveCommand("-right");
	Cmd_RemoveCommand("+forward");
	Cmd_RemoveCommand("-forward");
	Cmd_RemoveCommand("+back");
	Cmd_RemoveCommand("-back");
	Cmd_RemoveCommand("+lookup");
	Cmd_RemoveCommand("-lookup");
	Cmd_RemoveCommand("+lookdown");
	Cmd_RemoveCommand("-lookdown");
	Cmd_RemoveCommand("+strafe");
	Cmd_RemoveCommand("-strafe");
	Cmd_RemoveCommand("+moveleft");
	Cmd_RemoveCommand("-moveleft");
	Cmd_RemoveCommand("+moveright");
	Cmd_RemoveCommand("-moveright");
	Cmd_RemoveCommand("+speed");
	Cmd_RemoveCommand("-speed");
	Cmd_RemoveCommand("+attack");
	Cmd_RemoveCommand("-attack");
	Cmd_RemoveCommand("+button0");
	Cmd_RemoveCommand("-button0");
	Cmd_RemoveCommand("+button1");
	Cmd_RemoveCommand("-button1");
	Cmd_RemoveCommand("+button2");
	Cmd_RemoveCommand("-button2");
	Cmd_RemoveCommand("+button3");
	Cmd_RemoveCommand("-button3");
	Cmd_RemoveCommand("+button4");
	Cmd_RemoveCommand("-button4");
	Cmd_RemoveCommand("+button5");
	Cmd_RemoveCommand("-button5");
	Cmd_RemoveCommand("+button6");
	Cmd_RemoveCommand("-button6");
	Cmd_RemoveCommand("+button7");
	Cmd_RemoveCommand("-button7");
	Cmd_RemoveCommand("+button8");
	Cmd_RemoveCommand("-button8");
	Cmd_RemoveCommand("+button9");
	Cmd_RemoveCommand("-button9");
	Cmd_RemoveCommand("+button10");
	Cmd_RemoveCommand("-button10");
	Cmd_RemoveCommand("+button11");
	Cmd_RemoveCommand("-button11");
	Cmd_RemoveCommand("+button12");
	Cmd_RemoveCommand("-button12");
	Cmd_RemoveCommand("+button13");
	Cmd_RemoveCommand("-button13");
	Cmd_RemoveCommand("+button14");
	Cmd_RemoveCommand("-button14");
	Cmd_RemoveCommand("+mlook");
	Cmd_RemoveCommand("-mlook");

#ifdef USE_VOIP
	Cmd_RemoveCommand("+voiprecord");
	Cmd_RemoveCommand("-voiprecord");
#endif
}
