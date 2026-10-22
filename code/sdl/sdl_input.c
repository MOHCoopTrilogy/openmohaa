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

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "../client/client.h"
#include "../sys/sys_local.h"

#if !SDL_VERSION_ATLEAST(2, 0, 17)
#define KMOD_SCROLL KMOD_RESERVED
#endif

static cvar_t *in_keyboardDebug     = NULL;

static SDL_GameController *gamepad = NULL;
static SDL_Joystick *stick = NULL;

static qboolean mouseAvailable = qfalse;
static qboolean mouseActive = qfalse;

static cvar_t *in_mouse             = NULL;
static cvar_t *in_nograb;

static cvar_t *in_joystick          = NULL;
static cvar_t *in_joystickThreshold = NULL;
static cvar_t *in_joystickNo        = NULL;
static cvar_t *in_joystickUseAnalog = NULL;

// ---- [HZM gamepad-next] -------------------------------------------------------------------------------------
// in_gamepad 1 (default) turns controller support on WITHOUT the old in_joystick/in_restart dance. The SDL
// joystick/gamecontroller subsystems are NEVER initialised on the boot path (bug-2606: SDL_Init(JOYSTICK) hung
// forever on a bad HID/Bluetooth device, black-screening the client). They are brought up on a WORKER thread,
// started only after ~1 s of rendered frames, and the main thread never waits on it: if it has not finished in
// 6 s the pad is declared HUNG and given up on, the game carries on with keyboard + mouse.
// Only SDL GameController-class devices are opened (raw HID joysticks are ignored).
static cvar_t *in_gamepad       = NULL;
static cvar_t *in_padUiSpeed    = NULL;   // UI cursor speed, screen widths per second at full stick
static cvar_t *in_padDebug      = NULL;   // 1 = log mapped pad keys as "^~^~^ PADKEY ..."
static cvar_t *in_padTestScript = NULL;   // CVAR_INIT (command line only): drive a virtual pad from a script file
static cvar_t *in_padActive     = NULL;   // 1 while the last input came from the pad, 0 for keyboard / mouse
static cvar_t *in_padCfgVer     = NULL;   // pad settings migration marker (archived)
static qboolean padAutoLatched  = qfalse; // in_padGame -1 (auto): set by the first pad button / stick use
static cvar_t *in_padGame       = NULL;   // 1 = the pad also plays the game (movement / look / fire); menus always work
static cvar_t *in_padUiAccel    = NULL;   // extra cursor speed while the stick is held fully over (0 = none)
static cvar_t *in_padUiDeadzone = NULL;   // menu stick deadzone, 0..0.9
static cvar_t *in_padTestGo     = NULL;   // set to 1 (console) to restart the test script clock
static cvar_t *in_padTestHang   = NULL;   // CVAR_INIT: seconds the init thread stalls (proves the hang guard)

enum { PAD_IDLE, PAD_INITING, PAD_READY, PAD_HUNG, PAD_FAILED };
static volatile int padState = PAD_IDLE;
static volatile int padThreadDone = 0;
static volatile int padThreadOk = 0;
static volatile int padThreadQuit = 0;
static SDL_Thread *padThread = NULL;
static int padInitStartMs = 0;
static int padFramesSeen = 0;
static qboolean padWantOpen = qfalse;
static qboolean padWasEnabled = qfalse;
static int padLastActive = -1;
static int padMode = 0;                    // 0 game, 1 ui (menu), 2 console
int   in_padPeek = 0;                      // [HZM padlean] client sets it: left stick = peek, so the stick keys / axes are held at zero here
float in_padStick[2];                      // [HZM padlean] left stick after the dead zone, -1..1
static byte padHeldKey[MAX_KEYS];
static void IN_PadSetStatus( const char *s );

static int vidRestartTime = 0;

static int in_eventTime = 0;

static SDL_Window *SDL_window = NULL;

#define CTRL(a) ((a)-'a'+1)

/*
===============
IN_PrintKey
===============
*/
static void IN_PrintKey( const SDL_Keysym *keysym, keyNum_t key, qboolean down )
{
	if( down )
		Com_Printf( "+ " );
	else
		Com_Printf( "  " );

	Com_Printf( "Scancode: 0x%02x(%s) Sym: 0x%02x(%s)",
			keysym->scancode, SDL_GetScancodeName( keysym->scancode ),
			keysym->sym, SDL_GetKeyName( keysym->sym ) );

	if( keysym->mod & KMOD_LSHIFT )   Com_Printf( " KMOD_LSHIFT" );
	if( keysym->mod & KMOD_RSHIFT )   Com_Printf( " KMOD_RSHIFT" );
	if( keysym->mod & KMOD_LCTRL )    Com_Printf( " KMOD_LCTRL" );
	if( keysym->mod & KMOD_RCTRL )    Com_Printf( " KMOD_RCTRL" );
	if( keysym->mod & KMOD_LALT )     Com_Printf( " KMOD_LALT" );
	if( keysym->mod & KMOD_RALT )     Com_Printf( " KMOD_RALT" );
	if( keysym->mod & KMOD_LGUI )     Com_Printf( " KMOD_LGUI" );
	if( keysym->mod & KMOD_RGUI )     Com_Printf( " KMOD_RGUI" );
	if( keysym->mod & KMOD_NUM )      Com_Printf( " KMOD_NUM" );
	if( keysym->mod & KMOD_CAPS )     Com_Printf( " KMOD_CAPS" );
	if( keysym->mod & KMOD_MODE )     Com_Printf( " KMOD_MODE" );
	if( keysym->mod & KMOD_SCROLL )   Com_Printf( " KMOD_SCROLL" );

	Com_Printf( " Q:0x%02x(%s)\n", key, Key_KeynumToString( key ) );
}

#define MAX_CONSOLE_KEYS 16

/*
===============
IN_IsConsoleKey

TODO: If the SDL_Scancode situation improves, use it instead of
      both of these methods
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if( cl_consoleKeys->modified )
	{
		char *text_p, *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if( key == character )
		key = 0;

	for( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch( c->type )
		{
			case QUAKE_KEY:
				if( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}

/*
===============
IN_TranslateSDLToQ3Key
===============
*/
static keyNum_t IN_TranslateSDLToQ3Key( SDL_Keysym *keysym, qboolean down )
{
	keyNum_t key = 0;

	if( keysym->scancode >= SDL_SCANCODE_1 && keysym->scancode <= SDL_SCANCODE_0 )
	{
		// Always map the number keys as such even if they actually map
		// to other characters (eg, "1" is "&" on an AZERTY keyboard).
		// This is required for SDL before 2.0.6, except on Windows
		// which already had this behavior.
		if( keysym->scancode == SDL_SCANCODE_0 )
			key = '0';
		else
			key = '1' + keysym->scancode - SDL_SCANCODE_1;
	}
	else if( keysym->sym >= SDLK_SPACE && keysym->sym < SDLK_DELETE )
	{
		// These happen to match the ASCII chars
		key = (int)keysym->sym;
	}
	else if (keysym->scancode == SDL_SCANCODE_GRAVE)
	{
		// Added in OPM
		// so the console can be toggled properly
		key = '`';
	}
	else
	{
		switch( keysym->sym )
		{
			case SDLK_PAGEUP:       key = K_PGUP;          break;
			case SDLK_KP_9:         key = K_KP_PGUP;       break;
			case SDLK_PAGEDOWN:     key = K_PGDN;          break;
			case SDLK_KP_3:         key = K_KP_PGDN;       break;
			case SDLK_KP_7:         key = K_KP_HOME;       break;
			case SDLK_HOME:         key = K_HOME;          break;
			case SDLK_KP_1:         key = K_KP_END;        break;
			case SDLK_END:          key = K_END;           break;
			case SDLK_KP_4:         key = K_KP_LEFTARROW;  break;
			case SDLK_LEFT:         key = K_LEFTARROW;     break;
			case SDLK_KP_6:         key = K_KP_RIGHTARROW; break;
			case SDLK_RIGHT:        key = K_RIGHTARROW;    break;
			case SDLK_KP_2:         key = K_KP_DOWNARROW;  break;
			case SDLK_DOWN:         key = K_DOWNARROW;     break;
			case SDLK_KP_8:         key = K_KP_UPARROW;    break;
			case SDLK_UP:           key = K_UPARROW;       break;
			case SDLK_ESCAPE:       key = K_ESCAPE;        break;
			case SDLK_KP_ENTER:     key = K_KP_ENTER;      break;
			case SDLK_RETURN:       key = K_ENTER;         break;
			case SDLK_TAB:          key = K_TAB;           break;
			case SDLK_F1:           key = K_F1;            break;
			case SDLK_F2:           key = K_F2;            break;
			case SDLK_F3:           key = K_F3;            break;
			case SDLK_F4:           key = K_F4;            break;
			case SDLK_F5:           key = K_F5;            break;
			case SDLK_F6:           key = K_F6;            break;
			case SDLK_F7:           key = K_F7;            break;
			case SDLK_F8:           key = K_F8;            break;
			case SDLK_F9:           key = K_F9;            break;
			case SDLK_F10:          key = K_F10;           break;
			case SDLK_F11:          key = K_F11;           break;
			case SDLK_F12:          key = K_F12;           break;
			case SDLK_F13:          key = K_F13;           break;
			case SDLK_F14:          key = K_F14;           break;
			case SDLK_F15:          key = K_F15;           break;

			case SDLK_BACKSPACE:    key = K_BACKSPACE;     break;
			case SDLK_KP_PERIOD:    key = K_KP_DEL;        break;
			case SDLK_DELETE:       key = K_DEL;           break;
			case SDLK_PAUSE:        key = K_PAUSE;         break;

			case SDLK_LSHIFT:
			case SDLK_RSHIFT:       key = K_SHIFT;         break;

			case SDLK_LCTRL:
			case SDLK_RCTRL:        key = K_CTRL;          break;

#ifdef __APPLE__
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_COMMAND;       break;
#else
			case SDLK_RGUI:
			case SDLK_LGUI:         key = K_SUPER;         break;
#endif

			case SDLK_RALT:
			case SDLK_LALT:         key = K_ALT;           break;

			case SDLK_KP_5:         key = K_KP_5;          break;
			case SDLK_INSERT:       key = K_INS;           break;
			case SDLK_KP_0:         key = K_KP_INS;        break;
			case SDLK_KP_MULTIPLY:  key = K_KP_STAR;       break;
			case SDLK_KP_PLUS:      key = K_KP_PLUS;       break;
			case SDLK_KP_MINUS:     key = K_KP_MINUS;      break;
			case SDLK_KP_DIVIDE:    key = K_KP_SLASH;      break;

			case SDLK_MODE:         key = K_MODE;          break;
			case SDLK_HELP:         key = K_HELP;          break;
			case SDLK_PRINTSCREEN:  key = K_PRINT;         break;
			case SDLK_SYSREQ:       key = K_SYSREQ;        break;
			case SDLK_MENU:         key = K_MENU;          break;
			case SDLK_APPLICATION:	key = K_MENU;          break;
			case SDLK_POWER:        key = K_POWER;         break;
			case SDLK_UNDO:         key = K_UNDO;          break;
			case SDLK_SCROLLLOCK:   key = K_SCROLLOCK;     break;
			case SDLK_NUMLOCKCLEAR: key = K_KP_NUMLOCK;    break;
			case SDLK_CAPSLOCK:     key = K_CAPSLOCK;      break;

			default:
				if( !( keysym->sym & SDLK_SCANCODE_MASK ) && keysym->scancode <= 95 )
				{
					// Map Unicode characters to 95 world keys using the key's scan code.
					// FIXME: There aren't enough world keys to cover all the scancodes.
					// Maybe create a map of scancode to quake key at start up and on
					// key map change; allocate world key numbers as needed similar
					// to SDL 1.2.
					key = K_WORLD_0 + (int)keysym->scancode;
				}
				break;
		}
	}

	if( in_keyboardDebug->integer )
		IN_PrintKey( keysym, key, down );

	if( IN_IsConsoleKey( key, 0 ) )
	{
		// Console keys can't be bound or generate characters
		key = K_CONSOLE;
	}

	return key;
}

/*
===============
IN_GobbleMotionEvents
===============
*/
static void IN_GobbleMotionEvents( void )
{
	SDL_Event dummy[ 1 ];
	int val = 0;

	// Gobble any mouse motion events
	SDL_PumpEvents( );
	while( ( val = SDL_PeepEvents( dummy, 1, SDL_GETEVENT,
		SDL_MOUSEMOTION, SDL_MOUSEMOTION ) ) > 0 ) { }

	if ( val < 0 )
		Com_Printf( "IN_GobbleMotionEvents failed: %s\n", SDL_GetError( ) );
}

/*
===============
IN_ActivateMouse
===============
*/
static void IN_ActivateMouse( qboolean isFullscreen )
{
	if (!mouseAvailable || !SDL_WasInit( SDL_INIT_VIDEO ) )
		return;

	if( !mouseActive )
	{
		SDL_SetRelativeMouseMode( SDL_TRUE );
		SDL_SetWindowGrab( SDL_window, SDL_TRUE );

		IN_GobbleMotionEvents( );
	}

	// in_nograb makes no sense in fullscreen mode
	if( !isFullscreen )
	{
		if( in_nograb->modified || !mouseActive )
		{
			if( in_nograb->integer ) {
				SDL_SetRelativeMouseMode( SDL_FALSE );
				SDL_SetWindowGrab( SDL_window, SDL_FALSE );
			} else {
				SDL_SetRelativeMouseMode( SDL_TRUE );
				SDL_SetWindowGrab( SDL_window, SDL_TRUE );
			}

			in_nograb->modified = qfalse;
		}
	}

	mouseActive = qtrue;
}

/*
===============
IN_DeactivateMouse
===============
*/
static void IN_DeactivateMouse( qboolean isFullscreen )
{
	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
		return;

	// Always show the cursor when the mouse is disabled,
	// but not when fullscreen
	if( !isFullscreen )
		SDL_ShowCursor( SDL_TRUE );

	if( !mouseAvailable )
		return;

	if( mouseActive )
	{
		IN_GobbleMotionEvents( );

		SDL_SetWindowGrab( SDL_window, SDL_FALSE );
		SDL_SetRelativeMouseMode( SDL_FALSE );

		// Don't warp the mouse unless the cursor is within the window
		if( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_MOUSE_FOCUS )
			SDL_WarpMouseInWindow( SDL_window, cls.glconfig.vidWidth / 2, cls.glconfig.vidHeight / 2 );

		mouseActive = qfalse;
	}
}

// We translate axes movement into keypresses
static int joy_keys[16] = {
	K_LEFTARROW, K_RIGHTARROW,
	K_UPARROW, K_DOWNARROW,
	K_JOY17, K_JOY18,
	K_JOY19, K_JOY20,
	K_JOY21, K_JOY22,
	K_JOY23, K_JOY24,
	K_JOY25, K_JOY26,
	K_JOY27, K_JOY28
};

// translate hat events into keypresses
// the 4 highest buttons are used for the first hat ...
static int hat_keys[16] = {
	K_JOY29, K_JOY30,
	K_JOY31, K_JOY32,
	K_JOY25, K_JOY26,
	K_JOY27, K_JOY28,
	K_JOY21, K_JOY22,
	K_JOY23, K_JOY24,
	K_JOY17, K_JOY18,
	K_JOY19, K_JOY20
};


struct
{
	qboolean buttons[SDL_CONTROLLER_BUTTON_MAX + 1]; // +1 because old max was 16, current SDL_CONTROLLER_BUTTON_MAX is 15
	unsigned int oldaxes;
	int oldaaxes[MAX_JOYSTICK_AXIS];
	unsigned int oldhats;
	qboolean suppress[SDL_CONTROLLER_BUTTON_MAX + 1]; // held across a menu->game switch: ignore until released
} stick_state;


/*
===============
IN_PadNoteActive / IN_PadNoteKbm

in_padActive tells cgame and the menus which kind of key prompt to show.
===============
*/
static void IN_PadSetActive( int v )
{
	if ( v == padLastActive ) {
		return;
	}
	padLastActive = v;
	Cvar_Set( "in_padActive", v ? "1" : "0" );
}

static void IN_PadNoteKbm( void )
{
	if ( padLastActive != 0 ) {
		IN_PadSetActive( 0 );
	}
}

/*
===============
IN_PadKey

The one place a pad key reaches the game. Tracks what is held so a menu switch / unplug can release it.
===============
*/
static void IN_PadKey( int key, qboolean down )
{
	Com_QueueEvent( in_eventTime, SE_KEY, key, down, 0, NULL );
	if ( key >= 0 && key < MAX_KEYS ) {
		padHeldKey[key] = down ? 1 : 0;
	}
	IN_PadSetActive( 1 );
	if ( in_padDebug && in_padDebug->integer ) {
		Com_Printf( "^~^~^ PADKEY %s %s mode=%d\n", Key_KeynumToString( key ), down ? "down" : "up", padMode );
	}
}

static qboolean IN_PadEnabled( void )
{
	return ( in_gamepad && in_gamepad->integer ) || ( in_joystick && in_joystick->integer );
}

static void IN_PadSetStatus( const char *s )
{
	static char last[32];

	if ( strcmp( last, s ) ) {
		Q_strncpyz( last, s, sizeof( last ) );
		Cvar_Set( "in_padStatus", s );
	}
}

/*
===============
IN_PadReleaseAll

Key-up for every pad key still held and zero for every analog axis, so nothing sticks when a menu opens, the
pad is unplugged or support is switched off.
===============
*/
static void IN_PadReleaseAll( void )
{
	int i;

	for ( i = 0; i < MAX_KEYS; i++ ) {
		if ( padHeldKey[i] ) {
			IN_PadKey( i, qfalse );
		}
	}
	if ( in_joystickUseAnalog && in_joystickUseAnalog->integer ) {
		for ( i = 0; i < MAX_JOYSTICK_AXIS; i++ ) {
			Com_QueueEvent( in_eventTime, SE_JOYSTICK_AXIS, i, 0, 0, NULL );
		}
	}
	memset( &stick_state, '\0', sizeof( stick_state ) );
}

static void IN_PadClose( void )
{
	if ( gamepad ) {
		IN_PadReleaseAll();
		Com_Printf( "Gamepad closed.\n" );
		SDL_GameControllerClose( gamepad );
		gamepad = NULL;
	}
}

/*
===============
IN_PadOpenFirst

Opens the in_joystickNo-th GameController-class device (a virtual pad first when a test script is running).
===============
*/
static void IN_PadOpenFirst( void )
{
	int  i, total, n = 0, want, cand = -1, first = -1, virt = -1;
	char buf[1024] = "";

	if ( gamepad || padState != PAD_READY ) {
		return;
	}
	total = SDL_NumJoysticks();
	want  = in_joystickNo ? in_joystickNo->integer : 0;
	for ( i = 0; i < total; i++ ) {
		if ( !SDL_IsGameController( i ) ) {
			continue;
		}
		Q_strcat( buf, sizeof( buf ), SDL_JoystickNameForIndex( i ) );
		Q_strcat( buf, sizeof( buf ), "\n" );
		if ( first < 0 ) {
			first = i;
		}
		if ( SDL_JoystickIsVirtual( i ) && virt < 0 ) {
			virt = i;
		}
		if ( n == want ) {
			cand = i;
		}
		n++;
	}
	Cvar_Set( "in_availableJoysticks", buf );
	if ( in_padTestScript && in_padTestScript->string[0] && virt >= 0 ) {
		cand = virt;
	} else if ( cand < 0 ) {
		cand = first;
	}
	if ( cand < 0 ) {
		IN_PadSetStatus( "no pad" );
		return;
	}
	gamepad = SDL_GameControllerOpen( cand );
	if ( !gamepad ) {
		Com_Printf( "Gamepad open failed: %s\n", SDL_GetError() );
		IN_PadSetStatus( "open failed" );
		return;
	}
	memset( &stick_state, '\0', sizeof( stick_state ) );
	Com_Printf( "Gamepad opened: %s\n", SDL_GameControllerName( gamepad ) );
	IN_PadSetStatus( "connected" );
}

/*
===============
IN_PadInitThread

SDL_InitSubSystem can block forever on a bad HID device (bug-2606), so it runs here, off the main thread. The
thread parks afterwards so any thread-affine driver state (COM, raw input) stays alive until shutdown.
===============
*/
static int SDLCALL IN_PadInitThread( void *unused )
{
	(void)unused;
	if ( in_padTestHang && in_padTestHang->integer > 0 ) {
		SDL_Delay( in_padTestHang->integer * 1000 );   // test only: simulate SDL_Init hanging on a bad HID device
	}
	padThreadOk   = ( SDL_InitSubSystem( SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER ) == 0 );
	padThreadDone = 1;
	while ( !padThreadQuit ) {
		SDL_Delay( 100 );
	}
	return 0;
}

/*
===============
IN_InitJoystick

Called from IN_Init / IN_Restart. Touches NO SDL joystick API: it only arms the deferred bring-up.
===============
*/
static void IN_InitJoystick( void )
{
	IN_PadClose();
	memset( &stick_state, '\0', sizeof( stick_state ) );
	Cvar_Get( "in_availableJoysticks", "", CVAR_ROM );
	Cvar_Get( "in_padStatus", "off", CVAR_TEMP );
	padWantOpen = IN_PadEnabled();
}

/*
===============
IN_ShutdownJoystick
===============
*/
static void IN_ShutdownJoystick( void )
{
	IN_PadClose();
	if ( stick ) {
		SDL_JoystickClose( stick );
		stick = NULL;
	}
	// keep the subsystem + parked thread alive across in_restart; only a real exit tears it down
}

static qboolean KeyToAxisAndSign(int keynum, int *outAxis, int *outSign)
{
	char *bind;

	if (!keynum)
		return qfalse;

	bind = Key_GetBinding(keynum);

	if (!bind || *bind != '+')
		return qfalse;

	*outSign = 0;

	if (Q_stricmp(bind, "+forward") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+back") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveleft") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveright") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+lookup") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+lookdown") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+left") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+right") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveup") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+movedown") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? -1 : 1;
	}

	return *outSign != 0;
}

/*
===============
Test driver: in_padTestScript <file> (command line only) attaches an SDL VIRTUAL joystick and plays a script on
it, so movement / look / fire / menu navigation can be proven without a physical pad. Script lines:
    @attach <ms>                                   plug the virtual pad in
    @detach <ms>                                   unplug it (hot-plug test)
    <ms> lx ly rx ry lt rt buttons                 sticks -32768..32767, triggers 0..32767, buttons = bit mask
                                                   (SDL order: A0 B1 X2 Y3 BACK4 GUIDE5 START6 LS7 RS8 LB9 RB10
                                                   DUP11 DDOWN12 DLEFT13 DRIGHT14), decimal or 0x hex
===============
*/
typedef struct {
	int  t;
	int  kind;   // 0 state, 1 attach, 2 detach, 3 console command
	int  ax[6];
	int  btn;
	char cmd[64];
} padStep_t;

#define PAD_MAX_STEPS 2048
static padStep_t padSteps[PAD_MAX_STEPS];
static int padNumSteps = -1;
static int padNextStep = 0;
static int padT0 = 0;
static int padHold = 0;     // "@hold" in the script: the clock does not start until in_padTestGo is set

static void IN_PadTestLoad( void )
{
	FILE *f;
	char  line[256];

	padNumSteps = 0;
	f = fopen( in_padTestScript->string, "r" );
	if ( !f ) {
		Com_Printf( "^~^~^ PADTEST cannot open %s\n", in_padTestScript->string );
		return;
	}
	while ( fgets( line, sizeof( line ), f ) && padNumSteps < PAD_MAX_STEPS ) {
		padStep_t *st = &padSteps[padNumSteps];

		memset( st, 0, sizeof( *st ) );
		if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' ) {
			continue;
		}
		if ( !strncmp( line, "@hold", 5 ) ) {
			padHold = 1;
			continue;
		}
		if ( !strncmp( line, "@attach", 7 ) ) {
			st->kind = 1;
			if ( sscanf( line + 7, "%d", &st->t ) == 1 ) padNumSteps++;
		} else if ( !strncmp( line, "@cursor", 7 ) ) {
			st->kind = 4;      // test only: put the UI cursor somewhere known
			if ( sscanf( line + 7, "%d %d %d", &st->t, &st->ax[0], &st->ax[1] ) == 3 ) padNumSteps++;
		} else if ( !strncmp( line, "@kbm", 4 ) ) {
			st->kind = 5;      // test only: act as if the keyboard / mouse was just used (key prompts switch to key names)
			if ( sscanf( line + 4, "%d", &st->t ) == 1 ) padNumSteps++;
		} else if ( !strncmp( line, "@cmd", 4 ) ) {
			char *c = line + 4;

			st->kind = 3;
			if ( sscanf( c, "%d", &st->t ) == 1 ) {
				while ( *c == ' ' ) c++;
				while ( *c && *c != ' ' ) c++;      // skip the time token
				while ( *c == ' ' ) c++;
				Q_strncpyz( st->cmd, c, sizeof( st->cmd ) );
				st->cmd[strcspn( st->cmd, "\r\n" )] = 0;
				padNumSteps++;
			}
		} else if ( !strncmp( line, "@detach", 7 ) ) {
			st->kind = 2;
			if ( sscanf( line + 7, "%d", &st->t ) == 1 ) padNumSteps++;
		} else if ( sscanf( line, "%d %d %d %d %d %d %d %i", &st->t, &st->ax[0], &st->ax[1], &st->ax[2], &st->ax[3],
		                    &st->ax[4], &st->ax[5], &st->btn ) == 8 ) {
			padNumSteps++;
		}
	}
	fclose( f );
	Com_Printf( "^~^~^ PADTEST loaded %d steps from %s\n", padNumSteps, in_padTestScript->string );
}

static void IN_PadTestDrive( void )
{
	int elapsed, i;

	if ( !in_padTestScript || !in_padTestScript->string[0] || padState != PAD_READY ) {
		return;
	}
	if ( padNumSteps < 0 ) {
		IN_PadTestLoad();
		padT0       = Sys_Milliseconds();
		padNextStep = 0;
	}
	if ( padHold && !( in_padTestGo && in_padTestGo->integer ) ) {
		return;
	}
	padHold = 0;
	if ( in_padTestGo && in_padTestGo->integer ) {
		Cvar_Set( "in_padTestGo", "0" );
		padNextStep = 0;
		padT0       = Sys_Milliseconds();
		Com_Printf( "^~^~^ PADTEST restart\n" );
	}
	elapsed = Sys_Milliseconds() - padT0;
	while ( padNextStep < padNumSteps && padSteps[padNextStep].t <= elapsed ) {
		padStep_t *st = &padSteps[padNextStep];

		if ( st->kind == 1 ) {
			SDL_VirtualJoystickDesc d;
			int                     nj = SDL_NumJoysticks(), haveVirt = 0;

			for ( i = 0; i < nj; i++ ) {
				if ( SDL_JoystickIsVirtual( i ) ) haveVirt = 1;
			}
			if ( haveVirt ) {
				padNextStep++;
				continue;
			}
			memset( &d, 0, sizeof( d ) );
			d.version  = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
			d.type     = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
			d.naxes    = 6;
			d.nbuttons = 15;
			d.name     = "HZM Virtual Pad";
			i          = SDL_JoystickAttachVirtualEx( &d );
			Com_Printf( "^~^~^ PADTEST attach -> %d\n", i );
			padWantOpen = qtrue;
		} else if ( st->kind == 5 ) {
			IN_PadNoteKbm();
		} else if ( st->kind == 4 ) {
			CL_SetMousePos( st->ax[0], st->ax[1] );
		} else if ( st->kind == 3 ) {
			Cbuf_ExecuteText( EXEC_APPEND, va( "%s\n", st->cmd ) );
		} else if ( st->kind == 2 ) {
			int n = SDL_NumJoysticks();

			for ( i = 0; i < n; i++ ) {
				if ( SDL_JoystickIsVirtual( i ) ) {
					SDL_JoystickDetachVirtual( i );
					break;
				}
			}
			Com_Printf( "^~^~^ PADTEST detach\n" );
		} else if ( gamepad ) {
			SDL_Joystick *j = SDL_GameControllerGetJoystick( gamepad );

			for ( i = 0; i < 4; i++ ) {
				SDL_JoystickSetVirtualAxis( j, i, (Sint16)st->ax[i] );
			}
			for ( i = 4; i < 6; i++ ) {
				int raw = st->ax[i] * 2 - 32768;
				if ( raw > 32767 ) raw = 32767;
				if ( raw < -32768 ) raw = -32768;
				SDL_JoystickSetVirtualAxis( j, i, (Sint16)raw );
			}
			for ( i = 0; i < 15; i++ ) {
				SDL_JoystickSetVirtualButton( j, i, ( st->btn >> i ) & 1 );
			}
			Com_Printf( "^~^~^ PADTEST step t=%d btn=0x%x\n", st->t, st->btn );
		}
		padNextStep++;
	}
}

/*
===============
IN_PadService

Per client frame: deferred, off-thread SDL bring-up (see the comment at the top), open / re-open of the pad
after hot-plug, and the optional test driver.
===============
*/
/*
IN_PadMirrors

Friendly text for the Options > Controls > AIM / SENSITIVITY value labels (a label shows a cvar's raw string, "1.000000").
in_padV_<name> = percent for the sensitivities and deadzones, seconds / multiplier for the acceleration.
*/
static void IN_PadMirrors( void )
{
	static const struct {
		const char *cv, *mirror, *fmt;
	} m[] = {
		{"joy_lookX", "in_padV_lookX", "pct"},         {"joy_lookY", "in_padV_lookY", "pct"},
		{"joy_adsScale", "in_padV_adsScale", "pct"},   {"joy_threshold", "in_padV_inner", "pct"},
		{"joy_outerThreshold", "in_padV_outer", "pct"}, {"joy_response", "in_padV_response", "x1"},
		{"joy_accelTime", "in_padV_accelTime", "sec"}, {"joy_accelMax", "in_padV_accelMax", "x1"},
	};
	int i;

	for ( i = 0; i < (int)( sizeof( m ) / sizeof( m[0] ) ); i++ ) {
		float v = Cvar_VariableValue( m[i].cv );
		char  buf[24];

		if ( !strcmp( m[i].fmt, "pct" ) ) {
			Com_sprintf( buf, sizeof( buf ), "%d%%", (int)( v * 100.0f + 0.5f ) );
		} else if ( !strcmp( m[i].fmt, "sec" ) ) {
			Com_sprintf( buf, sizeof( buf ), "%.1f s", v );
		} else {
			Com_sprintf( buf, sizeof( buf ), "%.1fx", v );
		}
		Cvar_Get( m[i].mirror, "", CVAR_TEMP );
		if ( strcmp( Cvar_VariableString( m[i].mirror ), buf ) ) {
			Cvar_Set( m[i].mirror, buf );
		}
	}
}

static void IN_PadService( void )
{
	static int nextOpenMs = 0;
	int        now = Sys_Milliseconds();
	qboolean   en  = IN_PadEnabled();

	IN_PadMirrors();

	padFramesSeen++;
	if ( en != padWasEnabled ) {
		padWasEnabled = en;
		if ( en ) {
			padWantOpen = qtrue;
		} else {
			IN_PadClose();
			IN_PadSetStatus( "off" );
		}
	}
	if ( !en ) {
		return;
	}
	if ( padState == PAD_HUNG || padState == PAD_FAILED ) {
		return;
	}

	if ( padState == PAD_IDLE ) {
		if ( padFramesSeen < 60 || now < 1500 ) {
			return;                       // let the first frames and the main menu come up untouched
		}
		SDL_SetHint( SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" );
		SDL_SetHint( SDL_HINT_JOYSTICK_THREAD, "1" );
		padThread = SDL_CreateThread( IN_PadInitThread, "padinit", NULL );
		if ( !padThread ) {
			padState = PAD_FAILED;
			IN_PadSetStatus( "failed" );
			return;
		}
		padState       = PAD_INITING;
		padInitStartMs = now;
		IN_PadSetStatus( "starting" );
		return;
	}

	if ( padState == PAD_INITING ) {
		if ( padThreadDone ) {
			if ( padThreadOk ) {
				padState = PAD_READY;
				Com_Printf( "Gamepad support ready (%d ms after start).\n", now - padInitStartMs );
				IN_PadSetStatus( "no pad" );
			} else {
				padState = PAD_FAILED;
				Com_Printf( "Gamepad init failed: %s\n", SDL_GetError() );
				IN_PadSetStatus( "failed" );
			}
		} else if ( now - padInitStartMs > 6000 ) {
			padState = PAD_HUNG;
			Com_Printf( "^1Gamepad init did not finish in 6 s (bad HID device?) - gamepad disabled, keyboard and mouse unaffected.\n" );
			IN_PadSetStatus( "hung" );
		}
		return;
	}

	{   // the Controls page shows this word next to the Auto / Off / On buttons
		const char *lbl = ( in_padGame && in_padGame->integer == 1 ) ? "On" : ( ( in_padGame && in_padGame->integer == 0 ) ? "Off" : "Auto" );

		Cvar_Get( "in_padGameLabel", "Auto", CVAR_TEMP );
		if ( strcmp( Cvar_VariableString( "in_padGameLabel" ), lbl ) ) {
			Cvar_Set( "in_padGameLabel", lbl );
		}
		{   // same for the response-curve choice
			int         cv  = Cvar_VariableIntegerValue( "joy_curve" );
			const char *cl  = cv == 0 ? "Linear" : ( cv == 2 ? "Dynamic" : "Exponential" );

			Cvar_Get( "in_padCurveLabel", "Exponential", CVAR_TEMP );
			if ( strcmp( Cvar_VariableString( "in_padCurveLabel" ), cl ) ) {
				Cvar_Set( "in_padCurveLabel", cl );
			}
			{   // [HZM padlean] and the stick-peek mode word
				int         pk = Cvar_VariableIntegerValue( "joy_stickPeek" );
				const char *pl = pk == 0 ? "Off" : ( pk == 2 ? "While aiming (Airborne)" : "Hold peek button" );

				Cvar_Get( "in_padPeekLabel", "Hold peek button", CVAR_TEMP );
				if ( strcmp( Cvar_VariableString( "in_padPeekLabel" ), pl ) ) {
					Cvar_Set( "in_padPeekLabel", pl );
				}
			}
		}
	}
	IN_PadTestDrive();
	if ( !gamepad && padWantOpen && now >= nextOpenMs ) {
		nextOpenMs = now + 400;
		IN_PadOpenFirst();
		if ( gamepad ) {
			padWantOpen = qfalse;
		}
	}
}

/*
===============
IN_PadMenuKey

What a pad button does while a menu is up. Returns 0 for "nothing".
===============
*/
static int IN_PadMenuKey( int button )
{
	switch ( button ) {
	case SDL_CONTROLLER_BUTTON_A:          return K_MOUSE1;
	case SDL_CONTROLLER_BUTTON_B:          return K_ESCAPE;
	case SDL_CONTROLLER_BUTTON_START:      return K_ESCAPE;
	case SDL_CONTROLLER_BUTTON_DPAD_UP:    return K_PAD0_DPAD_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return K_PAD0_DPAD_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return K_PAD0_DPAD_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_PAD0_DPAD_RIGHT;
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return K_PAD0_LEFTSHOULDER;     // tabs (UIWindowManager::PadTab)
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_PAD0_RIGHTSHOULDER;
	}
	return 0;
}

static float IN_PadRamp( int raw )
{
	float f = (float)abs( raw ) / 32767.0f;
	float t = in_joystickThreshold ? in_joystickThreshold->value : 0.15f;

	f = ( f - t ) / ( 1.0f - t );
	if ( f < 0.0f ) f = 0.0f;
	if ( f > 1.0f ) f = 1.0f;
	return raw < 0 ? -f : f;
}

/*
===============
IN_GamepadUI

Menu mode (works in every menu, with in_padGame 0 or 1):
  left stick   UI cursor, response curve + a ramp-up while held fully over (in_padUiSpeed / in_padUiAccel /
               in_padUiDeadzone)
  right stick  scrolls the list under the cursor (mouse-wheel ticks, rate follows the deflection)
  D-pad        moves focus to the next widget in that direction (UIWindowManager::PadNavigate); auto-repeats
  A            left click        B / Start   back / close (Escape)
  LB / RB      previous / next tab (UIWindowManager::PadTab)
  Y            Windows on-screen keyboard
===============
*/
static void IN_GamepadUI( void )
{
	static int   lastMs = 0;
	static float accX = 0, accY = 0, held = 0, wheelAcc = 0;
	static int   repeatAt[4];
	int          now = Sys_Milliseconds();
	float        dt = ( now - lastMs ) * 0.001f, lx, ly, mag, dz, curve, accel, rs, speed;
	int          i, dx, dy;

	if ( dt > 0.1f || dt < 0.0f ) dt = 0.1f;
	lastMs = now;

	dz = in_padUiDeadzone ? in_padUiDeadzone->value : 0.2f;
	if ( dz < 0.0f ) dz = 0.0f;
	if ( dz > 0.9f ) dz = 0.9f;

	// ---- left stick: cursor. Radial deadzone, then (m^2) shaping so small pushes are precise.
	lx  = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTX ) / 32767.0f;
	ly  = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTY ) / 32767.0f;
	mag = sqrtf( lx * lx + ly * ly );
	if ( mag > dz ) {
		float m = ( mag - dz ) / ( 1.0f - dz );

		if ( m > 1.0f ) m = 1.0f;
		curve = m * m / mag;                     // scale the unit direction by the shaped magnitude
		lx *= curve;
		ly *= curve;
		held += ( m > 0.85f ) ? dt : -dt * 3.0f;  // ramp-up only for a sustained full push
		if ( held < 0.0f ) held = 0.0f;
		if ( held > 0.8f ) held = 0.8f;
	} else {
		lx = ly = 0.0f;
		held = 0.0f;
	}
	accel = 1.0f + ( in_padUiAccel ? in_padUiAccel->value : 1.0f ) * ( held / 0.8f );
	speed = cls.glconfig.vidWidth * ( in_padUiSpeed ? in_padUiSpeed->value : 0.55f ) * accel;
	accX += lx * speed * dt;
	accY += ly * speed * dt;
	dx = (int)accX;
	dy = (int)accY;
	accX -= dx;
	accY -= dy;
	if ( dx || dy ) {
		CL_PadMoveCursor( dx, dy );
		IN_PadSetActive( 1 );
		if ( in_padDebug && in_padDebug->integer ) {
			static int nextLogMs;

			if ( now >= nextLogMs ) {
				nextLogMs = now + 300;
				Com_Printf( "^~^~^ PADUI cursor %d %d accel %.2f\n", cl.mousex, cl.mousey, accel );
			}
		}
	}

	// ---- right stick: scroll the list under the cursor (wheel ticks, up to 14 per second)
	rs = SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_RIGHTY ) / 32767.0f;
	if ( fabsf( rs ) > dz ) {
		rs = ( fabsf( rs ) - dz ) / ( 1.0f - dz ) * ( rs < 0 ? -1.0f : 1.0f );
		wheelAcc += rs * 14.0f * dt;
		while ( wheelAcc >= 1.0f ) {          // stick down = content moves up = wheel down
			IN_PadKey( K_MWHEELDOWN, qtrue );
			IN_PadKey( K_MWHEELDOWN, qfalse );
			wheelAcc -= 1.0f;
		}
		while ( wheelAcc <= -1.0f ) {
			IN_PadKey( K_MWHEELUP, qtrue );
			IN_PadKey( K_MWHEELUP, qfalse );
			wheelAcc += 1.0f;
		}
	} else {
		wheelAcc = 0.0f;
	}

	for ( i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++ ) {
		qboolean pressed = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A + i );
		int      key     = IN_PadMenuKey( SDL_CONTROLLER_BUTTON_A + i );
		int      dpad    = i - SDL_CONTROLLER_BUTTON_DPAD_UP;

		if ( pressed != stick_state.buttons[i] ) {
			stick_state.buttons[i] = pressed;
			if ( key ) {
				IN_PadKey( key, pressed );
			}
			if ( pressed && i == SDL_CONTROLLER_BUTTON_Y ) {
#ifdef _WIN32
				Sys_OpenOnScreenKeyboard();
#endif
			}
			if ( dpad >= 0 && dpad < 4 && pressed ) {
				repeatAt[dpad] = now + 380;
			}
		} else if ( pressed && dpad >= 0 && dpad < 4 && now >= repeatAt[dpad] ) {
			// held D-pad auto-repeats, so a long list can be walked
			repeatAt[dpad] = now + 140;
			IN_PadKey( key, qfalse );
			IN_PadKey( key, qtrue );
		}
	}
}

/*
===============
IN_GamepadMove
===============
*/
static void IN_GamepadMove( void )
{
	int i;
	int translatedAxes[MAX_JOYSTICK_AXIS];
	qboolean translatedAxesSet[MAX_JOYSTICK_AXIS];
	int catcher, mode;

	SDL_GameControllerUpdate();

	// 0 = playing, 1 = a menu has the keyboard (pad drives the cursor), 2 = console (pad is dead)
	catcher = Key_GetCatcher();
	mode    = ( catcher & KEYCATCH_CONSOLE ) ? 2 : ( ( catcher & KEYCATCH_UI ) ? 1 : 0 );
	if ( mode != padMode ) {
		IN_PadReleaseAll();                       // nothing carries over from play into a menu or back
		for ( i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++ ) {
			// buttons already down at the switch are ignored until let go (A on "Apply" must not jump)
			stick_state.buttons[i]  = qfalse;
			stick_state.suppress[i] = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A + i ) ? qtrue : qfalse;
		}
		if ( in_padDebug && in_padDebug->integer ) {
			Com_Printf( "^~^~^ PADMODE %d -> %d\n", padMode, mode );
		}
		padMode = mode;
	}
	if ( mode == 2 ) {
		return;
	}
	if ( mode == 1 ) {
		for ( i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++ ) {
			if ( stick_state.suppress[i] && !SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A + i ) ) {
				stick_state.suppress[i] = qfalse;
			}
		}
		IN_GamepadUI();
		return;
	}

	// any pad button or stick use latches "auto" on (in_padGame -1); keyboard and mouse keep working alongside
	if ( !padAutoLatched ) {
		for ( i = 0; i < SDL_CONTROLLER_BUTTON_MAX && !padAutoLatched; i++ ) {
			if ( SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_A + i ) ) {
				padAutoLatched = qtrue;
			}
		}
		for ( i = 0; i < SDL_CONTROLLER_AXIS_MAX && !padAutoLatched; i++ ) {
			float f = fabsf( (float)SDL_GameControllerGetAxis( gamepad, SDL_CONTROLLER_AXIS_LEFTX + i ) / 32767.0f );

			if ( f > ( in_joystickThreshold ? in_joystickThreshold->value : 0.15f ) + 0.1f ) {
				padAutoLatched = qtrue;
			}
		}
	}

	// in-game play with the pad: in_padGame 1 = on, 0 = off, -1 = auto (on once a pad control has been used).
	// Start still opens the menu when it is off.
	if ( !( ( in_padGame && in_padGame->integer == 1 ) || ( in_padGame && in_padGame->integer < 0 && padAutoLatched ) ) ) {
		qboolean start = SDL_GameControllerGetButton( gamepad, SDL_CONTROLLER_BUTTON_START ) ? qtrue : qfalse;

		if ( start != stick_state.buttons[SDL_CONTROLLER_BUTTON_START] ) {
			stick_state.buttons[SDL_CONTROLLER_BUTTON_START] = start;
			IN_PadKey( K_ESCAPE, start );
		}
		return;
	}

	// check buttons
	for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		qboolean pressed = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_A + i);

		if ( stick_state.suppress[i] ) {
			if ( !pressed ) {
				stick_state.suppress[i] = qfalse;
			}
			pressed = qfalse;
		}
		if (pressed != stick_state.buttons[i])
		{
#if SDL_VERSION_ATLEAST( 2, 0, 14 )
			if ( i >= SDL_CONTROLLER_BUTTON_MISC1 ) {
				IN_PadKey(K_PAD0_MISC1 + i - SDL_CONTROLLER_BUTTON_MISC1, pressed);
			} else
#endif
			{
				IN_PadKey(K_PAD0_A + i, pressed);
			}
			stick_state.buttons[i] = pressed;
		}
	}

	// must defer translated axes until all real axes are processed
	// must be done this way to prevent a later mapped axis from zeroing out a previous one
	if (in_joystickUseAnalog->integer)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			translatedAxes[i] = 0;
			translatedAxesSet[i] = qfalse;
		}
	}

	// check axes
	for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
	{
		int axis = SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX + i);
		int oldAxis = stick_state.oldaaxes[i];

		// Smoothly ramp from dead zone to maximum value
		float f = ((float)abs(axis) / 32767.0f - in_joystickThreshold->value) / (1.0f - in_joystickThreshold->value);

		if (f < 0.0f)
			f = 0.0f;

		axis = (int)(32767 * ((axis < 0) ? -f : f));

		if (i < 2) {
			in_padStick[i] = axis / 32767.0f;
			if (in_padPeek) {
				axis = 0;       // [HZM padlean] ADS + peek: the stick does not walk (keyups go out below; it walks again when ADS is released)
			}
		}

		if (axis != oldAxis)
		{
			const int negMap[SDL_CONTROLLER_AXIS_MAX] = { K_PAD0_LEFTSTICK_LEFT,  K_PAD0_LEFTSTICK_UP,   K_PAD0_RIGHTSTICK_LEFT,  K_PAD0_RIGHTSTICK_UP, 0, 0 };
			const int posMap[SDL_CONTROLLER_AXIS_MAX] = { K_PAD0_LEFTSTICK_RIGHT, K_PAD0_LEFTSTICK_DOWN, K_PAD0_RIGHTSTICK_RIGHT, K_PAD0_RIGHTSTICK_DOWN, K_PAD0_LEFTTRIGGER, K_PAD0_RIGHTTRIGGER };

			qboolean posAnalog = qfalse, negAnalog = qfalse;
			int negKey = negMap[i];
			int posKey = posMap[i];

			if (in_joystickUseAnalog->integer)
			{
				int posAxis = 0, posSign = 0, negAxis = 0, negSign = 0;

				// get axes and axes signs for keys if available
				posAnalog = KeyToAxisAndSign(posKey, &posAxis, &posSign);
				negAnalog = KeyToAxisAndSign(negKey, &negAxis, &negSign);

				// positive to negative/neutral -> keyup if axis hasn't yet been set
				if (posAnalog && !translatedAxesSet[posAxis] && oldAxis > 0 && axis <= 0)
				{
					translatedAxes[posAxis] = 0;
					translatedAxesSet[posAxis] = qtrue;
				}

				// negative to positive/neutral -> keyup if axis hasn't yet been set
				if (negAnalog && !translatedAxesSet[negAxis] && oldAxis < 0 && axis >= 0)
				{
					translatedAxes[negAxis] = 0;
					translatedAxesSet[negAxis] = qtrue;
				}

				// negative/neutral to positive -> keydown
				if (posAnalog && axis > 0)
				{
					translatedAxes[posAxis] = axis * posSign;
					translatedAxesSet[posAxis] = qtrue;
				}

				// positive/neutral to negative -> keydown
				if (negAnalog && axis < 0)
				{
					translatedAxes[negAxis] = -axis * negSign;
					translatedAxesSet[negAxis] = qtrue;
				}
			}

			// keyups first so they get overridden by keydowns later

			// positive to negative/neutral -> keyup
			if (!posAnalog && posKey && oldAxis > 0 && axis <= 0)
				IN_PadKey(posKey, qfalse);

			// negative to positive/neutral -> keyup
			if (!negAnalog && negKey && oldAxis < 0 && axis >= 0)
				IN_PadKey(negKey, qfalse);

			// negative/neutral to positive -> keydown
			if (!posAnalog && posKey && oldAxis <= 0 && axis > 0)
				IN_PadKey(posKey, qtrue);

			// positive/neutral to negative -> keydown
			if (!negAnalog && negKey && oldAxis >= 0 && axis < 0)
				IN_PadKey(negKey, qtrue);

			stick_state.oldaaxes[i] = axis;
		}
	}

	// set translated axes
	if (in_joystickUseAnalog->integer)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			if (translatedAxesSet[i])
				Com_QueueEvent(in_eventTime, SE_JOYSTICK_AXIS, i, translatedAxes[i], 0, NULL);
		}
	}
}



/*
===============
IN_JoyMove
===============
*/
static void IN_JoyMove( void )
{
	unsigned int axes = 0;
	unsigned int hats = 0;
	int total = 0;
	int i = 0;

	if (gamepad)
	{
		IN_GamepadMove();
		return;
	}

	if (!stick)
		return;

	SDL_JoystickUpdate();

	// update the ball state.
	total = SDL_JoystickNumBalls(stick);
	if (total > 0)
	{
		int balldx = 0;
		int balldy = 0;
		for (i = 0; i < total; i++)
		{
			int dx = 0;
			int dy = 0;
			SDL_JoystickGetBall(stick, i, &dx, &dy);
			balldx += dx;
			balldy += dy;
		}
		if (balldx || balldy)
		{
			// !!! FIXME: is this good for stick balls, or just mice?
			// Scale like the mouse input...
			if (abs(balldx) > 1)
				balldx *= 2;
			if (abs(balldy) > 1)
				balldy *= 2;
			Com_QueueEvent( in_eventTime, SE_MOUSE, balldx, balldy, 0, NULL );
		}
	}

	// now query the stick buttons...
	total = SDL_JoystickNumButtons(stick);
	if (total > 0)
	{
		if (total > ARRAY_LEN(stick_state.buttons))
			total = ARRAY_LEN(stick_state.buttons);
		for (i = 0; i < total; i++)
		{
			qboolean pressed = (SDL_JoystickGetButton(stick, i) != 0);
			if (pressed != stick_state.buttons[i])
			{
				Com_QueueEvent( in_eventTime, SE_KEY, K_JOY1 + i, pressed, 0, NULL );
				stick_state.buttons[i] = pressed;
			}
		}
	}

	// look at the hats...
	total = SDL_JoystickNumHats(stick);
	if (total > 0)
	{
		if (total > 4) total = 4;
		for (i = 0; i < total; i++)
		{
			((Uint8 *)&hats)[i] = SDL_JoystickGetHat(stick, i);
		}
	}

	// update hat state
	if (hats != stick_state.oldhats)
	{
		for( i = 0; i < 4; i++ ) {
			if( ((Uint8 *)&hats)[i] != ((Uint8 *)&stick_state.oldhats)[i] ) {
				// release event
				switch( ((Uint8 *)&stick_state.oldhats)[i] ) {
					case SDL_HAT_UP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qfalse, 0, NULL );
						break;
					case SDL_HAT_RIGHT:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qfalse, 0, NULL );
						break;
					case SDL_HAT_DOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qfalse, 0, NULL );
						break;
					case SDL_HAT_LEFT:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qfalse, 0, NULL );
						break;
					case SDL_HAT_RIGHTUP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qfalse, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qfalse, 0, NULL );
						break;
					case SDL_HAT_RIGHTDOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qfalse, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qfalse, 0, NULL );
						break;
					case SDL_HAT_LEFTUP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qfalse, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qfalse, 0, NULL );
						break;
					case SDL_HAT_LEFTDOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qfalse, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qfalse, 0, NULL );
						break;
					default:
						break;
				}
				// press event
				switch( ((Uint8 *)&hats)[i] ) {
					case SDL_HAT_UP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qtrue, 0, NULL );
						break;
					case SDL_HAT_RIGHT:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qtrue, 0, NULL );
						break;
					case SDL_HAT_DOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qtrue, 0, NULL );
						break;
					case SDL_HAT_LEFT:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qtrue, 0, NULL );
						break;
					case SDL_HAT_RIGHTUP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qtrue, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qtrue, 0, NULL );
						break;
					case SDL_HAT_RIGHTDOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qtrue, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 1], qtrue, 0, NULL );
						break;
					case SDL_HAT_LEFTUP:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 0], qtrue, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qtrue, 0, NULL );
						break;
					case SDL_HAT_LEFTDOWN:
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 2], qtrue, 0, NULL );
						Com_QueueEvent( in_eventTime, SE_KEY, hat_keys[4*i + 3], qtrue, 0, NULL );
						break;
					default:
						break;
				}
			}
		}
	}

	// save hat state
	stick_state.oldhats = hats;

	// finally, look at the axes...
	total = SDL_JoystickNumAxes(stick);
	if (total > 0)
	{
		if (in_joystickUseAnalog->integer)
		{
			if (total > MAX_JOYSTICK_AXIS) total = MAX_JOYSTICK_AXIS;
			for (i = 0; i < total; i++)
			{
				Sint16 axis = SDL_JoystickGetAxis(stick, i);
				float f = ( (float) abs(axis) ) / 32767.0f;
				
				if( f < in_joystickThreshold->value ) axis = 0;

				if ( axis != stick_state.oldaaxes[i] )
				{
					Com_QueueEvent( in_eventTime, SE_JOYSTICK_AXIS, i, axis, 0, NULL );
					stick_state.oldaaxes[i] = axis;
				}
			}
		}
		else
		{
			if (total > 16) total = 16;
			for (i = 0; i < total; i++)
			{
				Sint16 axis = SDL_JoystickGetAxis(stick, i);
				float f = ( (float) axis ) / 32767.0f;
				if( f < -in_joystickThreshold->value ) {
					axes |= ( 1 << ( i * 2 ) );
				} else if( f > in_joystickThreshold->value ) {
					axes |= ( 1 << ( ( i * 2 ) + 1 ) );
				}
			}
		}
	}

	/* Time to update axes state based on old vs. new. */
	if (axes != stick_state.oldaxes)
	{
		for( i = 0; i < 16; i++ ) {
			if( ( axes & ( 1 << i ) ) && !( stick_state.oldaxes & ( 1 << i ) ) ) {
				Com_QueueEvent( in_eventTime, SE_KEY, joy_keys[i], qtrue, 0, NULL );
			}

			if( !( axes & ( 1 << i ) ) && ( stick_state.oldaxes & ( 1 << i ) ) ) {
				Com_QueueEvent( in_eventTime, SE_KEY, joy_keys[i], qfalse, 0, NULL );
			}
		}
	}

	/* Save for future generations. */
	stick_state.oldaxes = axes;
}

/*
===============
IN_TakeLoadingNavKeys

HZM coop [user 2026-09-27] loading-screen hint arrows (client/cl_ui.cpp UI_LoadHints_PollNav). While a map load
blocks, no client frame runs: Sys_PumpMessageLoop dispatches the window messages into the SDL queue, and every key
waits there until the load ends. This takes the LEFT/RIGHT arrow KEY-DOWNS out of the queue and counts the real
presses. Auto-repeats are taken too, but not counted: they belong to a press already counted, or to a key that was
held before the load, whose key-up must still reach the game. Key-ups, every other key and the keypad arrows (bound
to dev tools in autoexec.cfg) stay queued exactly as before.
===============
*/
static int IN_LoadingNavFilter( void *userdata, SDL_Event *e )
{
	int *counts = (int *)userdata;

	if( e->type == SDL_KEYDOWN && ( e->key.keysym.sym == SDLK_LEFT || e->key.keysym.sym == SDLK_RIGHT ) )
	{
		if( !e->key.repeat )
			counts[ e->key.keysym.sym == SDLK_RIGHT ? 1 : 0 ]++;
		return 0; // drop it from the queue
	}
	return 1;
}

void IN_TakeLoadingNavKeys( int *left, int *right )
{
	int counts[ 2 ] = { 0, 0 };

	if( SDL_WasInit( SDL_INIT_VIDEO ) )
		SDL_FilterEvents( IN_LoadingNavFilter, counts );

	*left  = counts[ 0 ];
	*right = counts[ 1 ];
}

/*
===============
IN_ProcessEvents
===============
*/
static void IN_ProcessEvents( void )
{
	SDL_Event e;
	keyNum_t key = 0;
	static keyNum_t lastKeyDown = 0;

	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
			return;

	while( SDL_PollEvent( &e ) )
	{
		switch( e.type )
		{
			case SDL_KEYDOWN:
				IN_PadNoteKbm();
				if ( e.key.repeat && Key_GetCatcher( ) == 0 )
					break;

				if( ( key = IN_TranslateSDLToQ3Key( &e.key.keysym, qtrue ) ) )
					Com_QueueEvent( in_eventTime, SE_KEY, key, qtrue, 0, NULL );

				if( key == K_BACKSPACE )
					Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
				else if (key == K_ENTER || key == K_TAB || key == K_ESCAPE) // Added in OPM: mostly used by UINotepad
					Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL);
				else if( keys[K_CTRL].down && key >= 'a' && key <= 'z' )
					Com_QueueEvent( in_eventTime, SE_CHAR, CTRL(key), 0, 0, NULL );

				lastKeyDown = key;
				break;

			case SDL_KEYUP:
				if( ( key = IN_TranslateSDLToQ3Key( &e.key.keysym, qfalse ) ) )
					Com_QueueEvent( in_eventTime, SE_KEY, key, qfalse, 0, NULL );

				lastKeyDown = 0;
				break;

			case SDL_TEXTINPUT:
				if( lastKeyDown != K_CONSOLE )
				{
					char *c = e.text.text;

					// Quick and dirty UTF-8 to UTF-32 conversion
					while( *c )
					{
						int utf32 = 0;

						if( ( *c & 0x80 ) == 0 )
							utf32 = *c++;
						else if( ( *c & 0xE0 ) == 0xC0 ) // 110x xxxx
						{
							utf32 |= ( *c++ & 0x1F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF0 ) == 0xE0 ) // 1110 xxxx
						{
							utf32 |= ( *c++ & 0x0F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else if( ( *c & 0xF8 ) == 0xF0 ) // 1111 0xxx
						{
							utf32 |= ( *c++ & 0x07 ) << 18;
							utf32 |= ( *c++ & 0x3F ) << 12;
							utf32 |= ( *c++ & 0x3F ) << 6;
							utf32 |= ( *c++ & 0x3F );
						}
						else
						{
							Com_DPrintf( "Unrecognised UTF-8 lead byte: 0x%x\n", (unsigned int)*c );
							c++;
						}

						if( utf32 != 0 )
						{
							if( IN_IsConsoleKey( 0, utf32 ) )
							{
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
								Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
							}
							else
								Com_QueueEvent( in_eventTime, SE_CHAR, utf32, 0, 0, NULL );
						}
          }
        }
				break;

			case SDL_MOUSEMOTION:
				if( mouseActive )
				{
					if( !e.motion.xrel && !e.motion.yrel )
						break;
					if( abs( e.motion.xrel ) + abs( e.motion.yrel ) >= 3 )
						IN_PadNoteKbm();
					Com_QueueEvent( in_eventTime, SE_MOUSE, e.motion.xrel, e.motion.yrel, 0, NULL );
				}
				break;

			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				{
					int b;
					if( e.type == SDL_MOUSEBUTTONDOWN )
						IN_PadNoteKbm();
					switch( e.button.button )
					{
						case SDL_BUTTON_LEFT:   b = K_MOUSE1;     break;
						case SDL_BUTTON_MIDDLE: b = K_MOUSE3;     break;
						case SDL_BUTTON_RIGHT:  b = K_MOUSE2;     break;
						case SDL_BUTTON_X1:     b = K_MOUSE4;     break;
						case SDL_BUTTON_X2:     b = K_MOUSE5;     break;
						default:                b = K_AUX1 + ( e.button.button - SDL_BUTTON_X2 + 1 ) % 16; break;
					}
					Com_QueueEvent( in_eventTime, SE_KEY, b,
						( e.type == SDL_MOUSEBUTTONDOWN ? qtrue : qfalse ), 0, NULL );
				}
				break;

			case SDL_MOUSEWHEEL:
				if( e.wheel.y > 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qfalse, 0, NULL );
				}
				else if( e.wheel.y < 0 )
				{
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qtrue, 0, NULL );
					Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qfalse, 0, NULL );
				}
				break;

			case SDL_CONTROLLERDEVICEADDED:
				// hot-plug: IN_PadService opens it on the next frame (rate limited)
				if ( padState == PAD_READY && IN_PadEnabled() ) {
					padWantOpen = qtrue;
					Com_Printf( "Gamepad plugged in.\n" );
				}
				break;

			case SDL_CONTROLLERDEVICEREMOVED:
				if ( gamepad && e.cdevice.which == SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( gamepad ) ) ) {
					IN_PadClose();
					IN_PadSetStatus( "unplugged" );
					padWantOpen = qtrue;     // another pad may still be there
					Com_Printf( "Gamepad unplugged.\n" );
				}
				break;

			case SDL_QUIT:
				Cbuf_ExecuteText(EXEC_NOW, "quit Closed window\n");
				break;

			case SDL_WINDOWEVENT:
				switch( e.window.event )
				{
					case SDL_WINDOWEVENT_RESIZED:
						{
							int width, height;

							width = e.window.data1;
							height = e.window.data2;

							// ignore this event on fullscreen
							if( cls.glconfig.isFullscreen )
							{
								break;
							}

							// check if size actually changed
							if( cls.glconfig.vidWidth == width && cls.glconfig.vidHeight == height )
							{
								break;
							}

							Cvar_SetValue( "r_customwidth", width );
							Cvar_SetValue( "r_customheight", height );
							Cvar_Set( "r_mode", "-1" );

							// Wait until user stops dragging for 1 second, so
							// we aren't constantly recreating the GL context while
							// he tries to drag...
							vidRestartTime = Sys_Milliseconds( ) + 1000;
						}
						break;

					case SDL_WINDOWEVENT_MINIMIZED:    Cvar_SetValue( "com_minimized", 1 ); break;
					case SDL_WINDOWEVENT_RESTORED:
					case SDL_WINDOWEVENT_MAXIMIZED:    Cvar_SetValue( "com_minimized", 0 ); break;
					case SDL_WINDOWEVENT_FOCUS_LOST:   Cvar_SetValue( "com_unfocused", 1 ); break;
					case SDL_WINDOWEVENT_FOCUS_GAINED: Cvar_SetValue( "com_unfocused", 0 ); break;
				}
				break;

#if defined(PROTOCOL_HANDLER) && defined(__APPLE__)
			case SDL_DROPFILE:
				{
					char *filename = e.drop.file;

					// Handle macOS open URL event. URL protocol scheme must be set in Info.plist.
					if( !Q_strncmp( filename, PROTOCOL_HANDLER ":", strlen( PROTOCOL_HANDLER ":" ) ) )
					{
						char *protocolCommand = Sys_ParseProtocolUri( filename );

						if( protocolCommand )
						{
							Cbuf_ExecuteText( EXEC_APPEND, va( "%s\n", protocolCommand ) );
							free( protocolCommand );
						}
					}

					SDL_free( filename );
				}
				break;
#endif

			default:
				break;
		}
	}
}

/*
===============
IN_Frame
===============
*/
void IN_Frame( void )
{
	qboolean loading;

	IN_PadService( );
	IN_JoyMove( );

	// If not DISCONNECTED (main menu) or ACTIVE (in game), we're loading
	loading = ( clc.state != CA_DISCONNECTED && clc.state != CA_ACTIVE );

	// update isFullscreen since it might of changed since the last vid_restart
	cls.glconfig.isFullscreen = Cvar_VariableIntegerValue( "r_fullscreen" ) != 0;

	if( !cls.glconfig.isFullscreen && ( Key_GetCatcher( ) & (KEYCATCH_CONSOLE|KEYCATCH_UI) ) )
	{
		// Console is down in windowed mode
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else if( !cls.glconfig.isFullscreen && loading )
	{
		// Loading in windowed mode
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else if( !( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_INPUT_FOCUS ) )
	{
		// Window not got focus
		IN_DeactivateMouse( cls.glconfig.isFullscreen );
	}
	else
		IN_ActivateMouse( cls.glconfig.isFullscreen );

	IN_ProcessEvents( );

	// Set event time for next frame to earliest possible time an event could happen
	in_eventTime = Sys_Milliseconds( );

	// In case we had to delay actual restart of video system
	if( ( vidRestartTime != 0 ) && ( vidRestartTime < Sys_Milliseconds( ) ) )
	{
		vidRestartTime = 0;
		Cbuf_AddText( "vid_restart\n" );
	}
}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
	int appState;

	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		Com_Error( ERR_FATAL, "IN_Init called before SDL_Init( SDL_INIT_VIDEO )" );
		return;
	}

	SDL_window = (SDL_Window *)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );

	in_keyboardDebug = Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );

	// mouse variables
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );

	in_joystick = Cvar_Get( "in_joystick", "0", CVAR_ARCHIVE|CVAR_LATCH );
	in_joystickThreshold = Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );
	in_joystickNo = Cvar_Get( "in_joystickNo", "0", CVAR_ARCHIVE );
	in_joystickUseAnalog = Cvar_Get( "in_joystickUseAnalog", "1", CVAR_ARCHIVE );
	in_gamepad = Cvar_Get( "in_gamepad", "1", CVAR_ARCHIVE );
	in_padUiSpeed = Cvar_Get( "in_padUiSpeed", "0.55", CVAR_ARCHIVE );
	in_padDebug = Cvar_Get( "in_padDebug", "0", 0 );
	in_padTestScript = Cvar_Get( "in_padTestScript", "", CVAR_INIT );
	in_padActive = Cvar_Get( "in_padActive", "0", CVAR_TEMP );
	in_padGame = Cvar_Get( "in_padGame", "-1", CVAR_ARCHIVE );
	// in_padGame: -1 auto (in-game pad play turns on as soon as a pad button or stick is used), 0 off, 1 on. Until
	// 2026-10-19 the default was 0, which nobody chose on purpose: a saved 0 is migrated to auto ONCE.
	in_padCfgVer = Cvar_Get( "in_padCfgVer", "0", CVAR_ARCHIVE );
	Cvar_Get( "in_padGameLabel", "Auto", CVAR_TEMP );
	if ( in_padCfgVer->integer < 1 ) {
		if ( in_padGame->integer == 0 ) {
			Cvar_Set( "in_padGame", "-1" );
		}
		Cvar_Set( "in_padCfgVer", "1" );
	}
	in_padUiAccel = Cvar_Get( "in_padUiAccel", "1.0", CVAR_ARCHIVE );
	in_padUiDeadzone = Cvar_Get( "in_padUiDeadzone", "0.2", CVAR_ARCHIVE );
	in_padTestGo = Cvar_Get( "in_padTestGo", "0", CVAR_TEMP );
	in_padTestHang = Cvar_Get( "in_padTestHang", "0", CVAR_INIT );

#if defined(PROTOCOL_HANDLER) && defined(__APPLE__)
	SDL_EventState( SDL_DROPFILE, SDL_ENABLE );
#endif

	SDL_StartTextInput( );

	mouseAvailable = ( in_mouse->value != 0 );
	IN_DeactivateMouse( Cvar_VariableIntegerValue( "r_fullscreen" ) != 0 );

	appState = SDL_GetWindowFlags( SDL_window );
	Cvar_SetValue( "com_unfocused",	!( appState & SDL_WINDOW_INPUT_FOCUS ) );
	Cvar_SetValue( "com_minimized", appState & SDL_WINDOW_MINIMIZED );

	IN_InitJoystick( );
	Com_DPrintf( "------------------------------------\n" );
}

/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	SDL_StopTextInput( );

	IN_DeactivateMouse( Cvar_VariableIntegerValue( "r_fullscreen" ) != 0 );
	mouseAvailable = qfalse;

	IN_ShutdownJoystick( );

	SDL_window = NULL;
}

/*
===============
IN_Restart
===============
*/
void IN_Restart( void )
{
	IN_ShutdownJoystick( );
	IN_Init( SDL_window );
}

/*
===============
HZM coop [user 2026-10-18] loading-screen cinematic (cl_loadcine.cpp): the swap interval is applied by the renderer only at
window creation (latched r_swapInterval), so a clip that must not wait for the display on every load tick sets it here,
on the main thread that owns the GL context, and puts it back when the load ends.
===============
*/
int IN_HZM_GetSwapInterval( void )
{
	return SDL_GL_GetSwapInterval( );
}

void IN_HZM_SetSwapInterval( int interval )
{
	SDL_GL_SetSwapInterval( interval );
}
