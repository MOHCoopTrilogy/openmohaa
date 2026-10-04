/*
===========================================================================
HZM coop - the coop start screen's "Dedicated" option (dedicated_launch 2026-10-04).

Ticking Dedicated used to do nothing: coop_mod/start_server.cfg pins `ui_dedicated 0; dedicated 0`, because
UI_StartDMMap_f turns ui_dedicated into `set dedicated 1` IN THIS PROCESS, which shuts the client and renderer
down (and crashed a player's GPU driver, bug-2671). This file runs a REAL dedicated server instead, as a separate
process, and joins it as an ordinary client. The client itself never touches `dedicated`.

  coop_dedStart [campaign]  the Dedicated variants of APPLY / BEGIN FULL CAMPAIGN (ui/coop_start.urc). Launches
                            <the folder of this exe>\omohaaded.exe (fixed name, no shell, no search path) with the
                            same fs_basepath / fs_homepath / com_target_game as this client, the map in ui_dmmap and
                            the host settings on the screen, each one validated. Then CL_DedicatedFrame polls the
                            server with `getinfo` until it answers (or exits, or times out) and runs `connect`.
  coop_dedStop              stops the server this game started (rcon quit with a per-launch random password, then
                            TerminateProcess after 3 s).

Leaving: a user `disconnect` from that server, or quitting the game, stops it unless coop_dedKeep is 1 (the
"Keep Running" box). The process sits in a job object with KILL_ON_JOB_CLOSE while coop_dedKeep is 0, so a crash of
this client cannot orphan it either. Its PID is printed at launch and at exit.

Shared homepath, so the server's XP/unlock saves are the host's own. Two things are kept apart: it writes
configs/hzm_dedicated.cfg (never this client's omconfig.cfg), and logfile is 0 (qconsole.log is open in this
process - a second writer would truncate it). coop_dedLog 1 gives the server the log (+developer 1) only when this
client's own logfile is 0.

Servers can never start or stop it: cmd_filter.c drops any server statement that mentions coop_ded* - which covers
the commands, the cvars and the campaign cfg (coop_mod/cfg/coop_dedcampaign.cfg), so `globalwidgetcommand <button>
stuffcommand "exec ..."` cannot plant a launch behind a button either.
===========================================================================
*/

#include "client.h"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
#	include <windows.h>
#	include <bcrypt.h>
#	pragma comment( lib, "bcrypt.lib" )
#	pragma comment( lib, "ws2_32.lib" )
#endif

static cvar_t *coop_dedKeep;
static cvar_t *coop_dedStatus;
static cvar_t *coop_dedLog;
static cvar_t *coop_dedTimeout;

#ifdef _WIN32

#	define DED_EXE_NAME		"omohaaded.exe"
#	define DED_PORT_BASE	12203
#	define DED_PORT_SPAN	10
#	define DED_POLL_MS		1000
#	define DED_STOP_MS		3000
#	define DED_CMDLINE_MAX	1900 // sys_main.c rebuilds argv into a MAX_STRING_CHARS (2048) buffer
#	define DED_MAX_PLUS		30	 // common.c MAX_CONSOLE_LINES 32; line 0 is the text before the first '+'
#	define DED_AWAY_MS		30000 // RUNNING, Keep Running off, this client not in its game this long -> stop it

typedef enum { DED_IDLE, DED_STARTING, DED_RUNNING, DED_STOPPING } dedState_t;

static dedState_t s_state = DED_IDLE;
static HANDLE     s_proc;
static HANDLE     s_job;
static DWORD      s_pid;
static int        s_port;
static netadr_t   s_adr;
static char       s_connect[64];
static char       s_rcon[32];
static char       s_map[64];
static int        s_startMs;
static int        s_nextPollMs;
static int        s_stopDeadline;
static int        s_awaySince;
static qboolean   s_connectPending;

// the coop host settings forwarded as they are (each must be an integer; anything else is left out)
static const char *s_hostInts[] = {
	"coop_health", "coop_lmsLives",
	// HOST RULES (ui/coop_hostrules.urc)
	"coop_bloodTrail", "coop_chalPopup", "coop_coverAuto", "coop_hardcore", "coop_tinnitus", "coop_xpKillPopup",
};

static void Ded_Status( const char *fmt, ... ) {
	char    buf[256];
	va_list ap;

	va_start( ap, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	buf[sizeof( buf ) - 1] = 0;
	Cvar_Set( "coop_dedStatus", buf );
	Com_Printf( "coop dedicated: %s\n", buf );
}

// server name / password: printable, and nothing the command line or the console tokenizer gives meaning to
// (" + ; / \ and control characters: '+' splits Com_ParseCommandLine, ';' splits Cbuf, '//' starts a comment
// in an unquoted token, '"' and '\' change the Windows argv quoting)
static qboolean Ded_TextCharOk( char c ) {
	if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) ) {
		return qtrue;
	}
	return ( c && strchr( " _-.!#()[]:^@*=~,?&'", c ) ) ? qtrue : qfalse;
}

static qboolean Ded_IsInt( const char *s ) {
	int n = 0;

	if ( *s == '-' ) {
		s++;
	}
	for ( ; *s; s++, n++ ) {
		if ( *s < '0' || *s > '9' || n >= 7 ) {
			return qfalse;
		}
	}
	return n > 0 ? qtrue : qfalse;
}

static qboolean Ded_MapNameOk( const char *m ) {
	size_t n = strlen( m ), i;

	if ( n < 1 || n > 63 || m[0] == '/' || m[0] == '-' ) { // '-': sys_main.c stops at an argv "--uri"
		return qfalse;
	}
	for ( i = 0; i < n; i++ ) {
		char c = m[i];
		if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_'
				|| c == '-' || c == '/' ) ) {
			return qfalse;
		}
		if ( c == '/' && m[i + 1] == '/' ) {
			return qfalse;
		}
	}
	return qtrue;
}

// a filesystem path for the command line: copied without trailing slashes (a '\' before the closing quote
// would escape it), refused if it holds a character the command line cannot carry
static qboolean Ded_PathOk( const char *in, char *out, size_t outSize ) {
	size_t n;

	Q_strncpyz( out, in, outSize );
	n = strlen( out );
	while ( n > 0 && ( out[n - 1] == '\\' || out[n - 1] == '/' ) ) {
		out[--n] = 0;
	}
	if ( n && out[n - 1] == ':' ) {
		Q_strcat( out, (int)outSize, "\\." ); // a drive root: "C:\." (a bare "C:" is that drive's current folder)
		n = strlen( out );
	}
	if ( !n || n >= outSize - 1 ) {
		return qfalse;
	}
	for ( const char *p = out; *p; p++ ) {
		unsigned char c = (unsigned char)*p;
		if ( c < 0x20 || c == '"' || c == '+' || c == ';' || c == '%' ) {
			return qfalse;
		}
		if ( c == '/' && ( p[1] == '/' || p[1] == '*' ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean Ded_PortFree( struct in_addr ip, int port ) {
	SOCKET      s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	BOOL        ex = TRUE;
	sockaddr_in a;
	int         r;

	if ( s == INVALID_SOCKET ) {
		return qfalse;
	}
	setsockopt( s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&ex, sizeof( ex ) );
	memset( &a, 0, sizeof( a ) );
	a.sin_family = AF_INET;
	a.sin_addr   = ip;
	a.sin_port   = htons( (u_short)port );
	r            = bind( s, (sockaddr *)&a, sizeof( a ) );
	closesocket( s );
	return r == 0 ? qtrue : qfalse;
}

static void Ded_ApplyKeep( void ) {
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;

	if ( !s_job ) {
		return;
	}
	memset( &li, 0, sizeof( li ) );
	li.BasicLimitInformation.LimitFlags = ( coop_dedKeep && coop_dedKeep->integer ) ? 0 : JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SetInformationJobObject( s_job, JobObjectExtendedLimitInformation, &li, sizeof( li ) );
}

// forget the process (the job handle closes too: with KILL_ON_JOB_CLOSE set that ends a still-running server)
static void Ded_Release( void ) {
	if ( s_proc ) {
		CloseHandle( s_proc );
	}
	if ( s_job ) {
		CloseHandle( s_job );
	}
	s_proc  = NULL;
	s_job   = NULL;
	s_pid   = 0;
	s_state = DED_IDLE;
	SecureZeroMemory( s_rcon, sizeof( s_rcon ) );
}

static qboolean Ded_Alive( void ) {
	return ( s_proc && WaitForSingleObject( s_proc, 0 ) == WAIT_TIMEOUT ) ? qtrue : qfalse;
}

static void Ded_SendQuit( void ) {
	char msg[96];

	if ( !s_rcon[0] ) {
		return;
	}
	msg[0] = msg[1] = msg[2] = msg[3] = (char)-1;
	msg[4] = 2; // direction byte, as CL_Rcon_f
	msg[5] = 0;
	Q_strcat( msg, sizeof( msg ), "rcon " );
	Q_strcat( msg, sizeof( msg ), s_rcon );
	Q_strcat( msg, sizeof( msg ), " quit" );
	NET_SendPacket( NS_CLIENT, strlen( msg ) + 1, msg, s_adr ); // the 0xff header is non-zero, as CL_Rcon_f
	NET_SendPacket( NS_CLIENT, strlen( msg ) + 1, msg, s_adr ); // UDP: twice (the second may be rate-limited)
	SecureZeroMemory( msg, sizeof( msg ) );
}

// stop the server this game started: rcon quit, then TerminateProcess. `wait` blocks (quit / a new launch).
static void Ded_Stop( qboolean wait ) {
	if ( !s_proc ) {
		return;
	}
	if ( !Ded_Alive() ) {
		Ded_Release();
		return;
	}
	Com_Printf( "coop dedicated: stopping the server (pid %lu)\n", (unsigned long)s_pid );
	if ( s_rcon[0] ) {
		Ded_SendQuit();
	} else {
		TerminateProcess( s_proc, 1 );
	}
	if ( wait ) {
		if ( WaitForSingleObject( s_proc, DED_STOP_MS ) != WAIT_OBJECT_0 ) {
			Com_Printf( "coop dedicated: the server did not quit in time - ending it\n" );
			TerminateProcess( s_proc, 1 );
			WaitForSingleObject( s_proc, 2000 );
		}
		Ded_Release();
		return;
	}
	s_state        = DED_STOPPING;
	s_stopDeadline = Sys_Milliseconds() + DED_STOP_MS;
}

static void Ded_Fail( const char *fmt, ... ) {
	char    buf[256];
	va_list ap;

	va_start( ap, fmt );
	Q_vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	buf[sizeof( buf ) - 1] = 0;
	Ded_Status( "%s", buf );
}

typedef struct {
	char   s[2400];
	size_t n;
	qboolean overflow;
} dedCmd_t;

static void Ded_Add( dedCmd_t *c, const char *fmt, ... ) {
	va_list ap;
	int     w;

	if ( c->overflow ) {
		return;
	}
	va_start( ap, fmt );
	w = Q_vsnprintf( c->s + c->n, sizeof( c->s ) - c->n, fmt, ap );
	va_end( ap );
	if ( w < 0 || (size_t)w >= sizeof( c->s ) - c->n ) {
		c->overflow = qtrue;
		c->s[c->n]  = 0;
		return;
	}
	c->n += (size_t)w;
}

static void CL_DedStart_f( void ) {
	char                exePath[MAX_PATH], exeDir[MAX_PATH], basePath[MAX_OSPATH], homePath[MAX_OSPATH];
	char                hostName[32], ipStr[32], map[64];
	const char         *s;
	qboolean            campaign = ( Cmd_Argc() > 1 && !Q_stricmp( Cmd_Argv( 1 ), "campaign" ) ) ? qtrue : qfalse;
	int                 maxClients, port, i, logToServer;
	struct in_addr      ip;
	dedCmd_t           *cmd;
	STARTUPINFOA        si;
	PROCESS_INFORMATION pi;
	DWORD               n;

	// a server we started earlier is replaced, never doubled
	if ( s_proc ) {
		Ded_Stop( qtrue );
	}
	if ( clc.state != CA_DISCONNECTED ) {
		Cbuf_ExecuteText( EXEC_NOW, "disconnect\n" );
		if ( s_proc ) {
			Ded_Stop( qtrue );
		}
	}

	// --- the fixed executable: <folder of this exe>\omohaaded.exe -----------------------------------------
	n = GetModuleFileNameA( NULL, exePath, sizeof( exePath ) );
	if ( !n || n >= sizeof( exePath ) - 1 ) {
		Ded_Fail( "could not find the game folder" );
		return;
	}
	Q_strncpyz( exeDir, exePath, sizeof( exeDir ) );
	{
		char *slash = strrchr( exeDir, '\\' );
		if ( !slash ) {
			Ded_Fail( "could not find the game folder" );
			return;
		}
		*slash = 0;
	}
	if ( strlen( exeDir ) + 1 + strlen( DED_EXE_NAME ) >= sizeof( exePath ) ) {
		Ded_Fail( "the game folder path is too long" );
		return;
	}
	Com_sprintf( exePath, sizeof( exePath ), "%s\\%s", exeDir, DED_EXE_NAME );
	{
		DWORD attr = GetFileAttributesA( exePath );
		if ( attr == INVALID_FILE_ATTRIBUTES || ( attr & FILE_ATTRIBUTE_DIRECTORY ) ) {
			Ded_Fail( "%s is missing from the game folder - untick Dedicated to host normally", DED_EXE_NAME );
			return;
		}
	}

	// --- validate every value that goes on the command line ---------------------------------------------
	Q_strncpyz( map, Cvar_VariableString( "ui_dmmap" ), sizeof( map ) );
	if ( !Ded_MapNameOk( map ) ) {
		Ded_Fail( "select a map first" );
		return;
	}
	if ( FS_ReadFile( va( "maps/%s.bsp", map ), NULL ) <= 0 ) {
		Ded_Fail( "map %s was not found", map );
		return;
	}
	if ( !Ded_PathOk( Cvar_VariableString( "fs_basepath" ), basePath, sizeof( basePath ) )
		|| !Ded_PathOk( Cvar_VariableString( "fs_homepath" ), homePath, sizeof( homePath ) ) ) {
		Ded_Fail( "the game or home folder path has a character a server command line cannot carry (\" + ; %%)" );
		return;
	}

	// server name: as the UI does for ui_hostname, bad characters become '_'
	Q_strncpyz( hostName, Cvar_VariableString( "sv_hostname" ), sizeof( hostName ) );
	for ( i = 0; hostName[i]; i++ ) {
		if ( !Ded_TextCharOk( hostName[i] ) || ( i == 0 && hostName[i] == '-' ) ) {
			hostName[i] = '_';
		}
	}
	while ( i > 0 && hostName[i - 1] == ' ' ) {
		hostName[--i] = 0;
	}
	if ( !hostName[0] || hostName[0] == ' ' ) {
		Q_strncpyz( hostName, "HZM Coop Server", sizeof( hostName ) );
	}

	// password: refused rather than altered, so it always matches what the joining players type
	s = Cvar_VariableString( "password" );
	if ( strlen( s ) > 63 || s[0] == '-' ) {
		Ded_Fail( "the password is too long" );
		return;
	}
	for ( const char *p = s; *p; p++ ) {
		if ( !Ded_TextCharOk( *p ) ) {
			Ded_Fail( "the password can only use letters, digits, spaces and _-.!#()[]:^@*=~,?&'" );
			return;
		}
	}

	// sv_maxclients is LATCHED in this process: what the Max Players field (or coop_dedcampaign.cfg) set is the
	// latched value, and ->string still holds the one this client's last listen server ran with (test run 1: 1, not 6)
	{
		cvar_t *mc = Cvar_Get( "sv_maxclients", "8", 0 );
		s          = ( mc->latchedString && mc->latchedString[0] ) ? mc->latchedString : mc->string;
	}
	maxClients = Ded_IsInt( s ) ? atoi( s ) : 8;
	if ( maxClients < 1 ) {
		maxClients = 1;
	} else if ( maxClients > MAX_CLIENTS ) {
		maxClients = MAX_CLIENTS;
	}

	// the address the server binds: this client's own net_ip (0.0.0.0 = every interface, as a listen host)
	s = Cvar_VariableString( "net_ip" );
	if ( !s[0] ) {
		s = "0.0.0.0";
	} else if ( !Q_stricmp( s, "localhost" ) ) {
		s = "127.0.0.1";
	}
	if ( inet_pton( AF_INET, s, &ip ) != 1 ) {
		Ded_Fail( "net_ip must be an IPv4 address (or localhost) for a dedicated server" );
		return;
	}
	inet_ntop( AF_INET, &ip, ipStr, sizeof( ipStr ) );

	// --- a free UDP port, 12203 up (this client usually holds 12203 itself) -------------------------------
	port = 0;
	for ( i = 0; i < DED_PORT_SPAN; i++ ) {
		if ( Ded_PortFree( ip, DED_PORT_BASE + i ) ) {
			port = DED_PORT_BASE + i;
			break;
		}
	}
	if ( !port ) {
		Ded_Fail( "no free port in %d-%d - close the other server, or untick Dedicated", DED_PORT_BASE,
			DED_PORT_BASE + DED_PORT_SPAN - 1 );
		return;
	}

	// --- a per-launch rcon password: how coop_dedStop / quit ask the server to shut down cleanly ----------
	s_rcon[0] = 0;
	{
		static const char alnum[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		unsigned char     rnd[24];
		if ( BCryptGenRandom( NULL, rnd, sizeof( rnd ), BCRYPT_USE_SYSTEM_PREFERRED_RNG ) == 0 ) {
			for ( i = 0; i < 24; i++ ) {
				s_rcon[i] = alnum[rnd[i] % ( sizeof( alnum ) - 1 )];
			}
			s_rcon[24] = 0;
		}
		SecureZeroMemory( rnd, sizeof( rnd ) );
	}

	logToServer = ( coop_dedLog->integer > 0 && Cvar_VariableIntegerValue( "logfile" ) == 0 ) ? 1 : 0;
	if ( coop_dedLog->integer > 0 && !logToServer ) {
		Com_Printf( "coop dedicated: coop_dedLog needs this game's own logfile at 0 (they share qconsole.log) - not logging the server\n" );
	}

	// --- the command line: every value quoted, every value validated above ----------------------------------
	cmd = (dedCmd_t *)Z_Malloc( sizeof( dedCmd_t ) );
	memset( cmd, 0, sizeof( *cmd ) );
	Ded_Add( cmd, "\"%s\"", exePath );
	Ded_Add( cmd, " +set dedicated 1 +set com_target_game %d", Cvar_VariableIntegerValue( "com_target_game" ) );
	Ded_Add( cmd, " +set fs_basepath \"%s\" +set fs_homepath \"%s\"", basePath, homePath );
	s = Cvar_VariableString( "fs_game" );
	if ( s[0] ) {
		for ( const char *p = s; *p; p++ ) {
			if ( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) || ( *p >= '0' && *p <= '9' ) || *p == '_'
					|| *p == '-' ) ) {
				Z_Free( cmd );
				Ded_Fail( "fs_game has a character a server command line cannot carry" );
				return;
			}
		}
		Ded_Add( cmd, " +set fs_game \"%s\"", s );
	}
	Ded_Add( cmd, " +set config \"hzm_dedicated.cfg\" +set logfile %d", logToServer ? 2 : 0 );
	if ( logToServer ) {
		Ded_Add( cmd, " +set developer 1" );
	}
	Ded_Add( cmd, " +set net_enabled 1 +set net_ip \"%s\" +set net_port %d", ipStr, port );
	Ded_Add( cmd, " +set sv_maxclients %d +set g_gametype 2 +set sv_gamespy %d", maxClients,
		Cvar_VariableIntegerValue( "ui_gamespy" ) ? 1 : 0 );
	Ded_Add( cmd, " +set sv_hostname \"%s\" +set password \"%s\"", hostName, Cvar_VariableString( "password" ) );
	if ( s_rcon[0] ) {
		Ded_Add( cmd, " +set rconPassword \"%s\"", s_rcon );
	}
	Ded_Add( cmd, " +set coop_campaign %d", campaign ? 1 : 0 );
	for ( i = 0; i < (int)ARRAY_LEN( s_hostInts ); i++ ) {
		s = Cvar_VariableString( s_hostInts[i] );
		if ( Ded_IsInt( s ) ) {
			Ded_Add( cmd, " +set %s \"%s\"", s_hostInts[i], s );
		}
	}
	// the server rules the listen path runs from start_server.cfg, then the map (`map`, never ui_startdmmap)
	Ded_Add( cmd, " +exec coop_mod/cfg/dedicated_server.cfg +map %s", map );

	{
		// Com_ParseCommandLine keeps MAX_CONSOLE_LINES (32) lines and silently drops the rest - +exec and +map are last
		int plus = 0;
		for ( const char *p = cmd->s; *p; p++ ) {
			plus += ( p[0] == ' ' && p[1] == '+' ) ? 1 : 0;
		}
		if ( plus > DED_MAX_PLUS ) {
			cmd->overflow = qtrue;
		}
	}
	if ( cmd->overflow || cmd->n - strlen( exePath ) > DED_CMDLINE_MAX ) {
		SecureZeroMemory( cmd, sizeof( *cmd ) );
		Z_Free( cmd );
		Ded_Fail( "the server command line is too long (install path?)" );
		return;
	}

	// --- launch: suspended, into a job (KILL_ON_JOB_CLOSE unless Keep Running), no window, no handles -------
	memset( &si, 0, sizeof( si ) );
	si.cb = sizeof( si );
	memset( &pi, 0, sizeof( pi ) );
	if ( !CreateProcessA( exePath, cmd->s, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, exeDir, &si,
			 &pi ) ) {
		DWORD err = GetLastError();
		SecureZeroMemory( cmd, sizeof( *cmd ) );
		Z_Free( cmd );
		SecureZeroMemory( s_rcon, sizeof( s_rcon ) );
		Ded_Fail( "could not start %s (error %lu) - untick Dedicated to host normally", DED_EXE_NAME, (unsigned long)err );
		return;
	}
	SecureZeroMemory( cmd, sizeof( *cmd ) );
	Z_Free( cmd );

	s_job = CreateJobObjectA( NULL, NULL );
	if ( s_job ) {
		Ded_ApplyKeep();
		if ( !AssignProcessToJobObject( s_job, pi.hProcess ) ) {
			Com_Printf( "coop dedicated: could not tie the server to this game (error %lu) - it is stopped on quit only\n",
				(unsigned long)GetLastError() );
			CloseHandle( s_job );
			s_job = NULL;
		}
	}
	ResumeThread( pi.hThread );
	CloseHandle( pi.hThread );

	s_proc  = pi.hProcess;
	s_pid   = pi.dwProcessId;
	s_port  = port;
	s_state = DED_STARTING;
	Q_strncpyz( s_map, map, sizeof( s_map ) );
	Com_sprintf( s_connect, sizeof( s_connect ), "%s:%d", ip.s_addr == htonl( INADDR_ANY ) ? "127.0.0.1" : ipStr, port );
	NET_StringToAdr( s_connect, &s_adr, NA_IP );
	s_awaySince  = 0;
	s_connectPending = qfalse;
	s_startMs    = Sys_Milliseconds();
	s_nextPollMs = s_startMs + 500;
	coop_dedKeep->modified = qfalse;

	Com_Printf( "coop dedicated: started %s pid %lu - map %s%s, port %d on %s, %d players, keep running %s\n", exePath,
		(unsigned long)s_pid, map, campaign ? " (campaign)" : "", port, ipStr, maxClients,
		coop_dedKeep->integer ? "yes" : "no" );
	Ded_Status( "starting the dedicated server (%s, port %d)...", map, port );
}

static void CL_DedStop_f( void ) {
	if ( !s_proc ) {
		Com_Printf( "coop dedicated: no server started from this game is running\n" );
		return;
	}
	Ded_Stop( qfalse );
	Ded_Status( "stopping the dedicated server" );
}

/*
CL_DedicatedInfoResponse - from CL_ConnectionlessPacket: the server we started answered `getinfo`, so its map is
up. Join it.
*/
void CL_DedicatedInfoResponse( netadr_t from ) {
	if ( s_state != DED_STARTING || !NET_CompareAdr( from, s_adr ) ) {
		return;
	}
	s_state = DED_RUNNING;
	if ( clc.state != CA_DISCONNECTED ) {
		Ded_Status( "the server is up on port %d - you are in another game; type connect %s to join", s_port, s_connect );
		return;
	}
	Ded_Status( "the server is up on port %d - joining", s_port );
	// joined from CL_DedicatedFrame, not appended to the command buffer: anything already queued there (a `wait`
	// chain from a cfg, say) would hold an appended connect back indefinitely (test run 1), and running it here, in
	// the middle of packet parsing, would disconnect/reconnect under CL_PacketEvent's feet
	s_connectPending = qtrue;
}

/*
CL_DedicatedFrame - every client frame: watch the process, poll while it starts, finish a pending stop
*/
void CL_DedicatedFrame( void ) {
	DWORD code = 0;
	int   now;

	if ( s_state == DED_IDLE || !s_proc ) {
		return;
	}
	if ( s_connectPending ) {
		s_connectPending = qfalse;
		if ( s_state == DED_RUNNING && clc.state == CA_DISCONNECTED ) {
			Cbuf_ExecuteText( EXEC_NOW, va( "connect %s\n", s_connect ) );
		}
	}
	if ( coop_dedKeep->modified ) {
		coop_dedKeep->modified = qfalse;
		Ded_ApplyKeep();
	}
	now = Sys_Milliseconds();
	if ( !Ded_Alive() ) {
		GetExitCodeProcess( s_proc, &code );
		if ( s_state == DED_STARTING ) {
			Ded_Status( "the server stopped while starting (exit code %lu) - untick Dedicated to host normally",
				(unsigned long)code );
		} else if ( s_state == DED_STOPPING ) {
			Ded_Status( "the dedicated server stopped" );
		} else {
			Ded_Status( "the dedicated server (pid %lu) stopped (exit code %lu)", (unsigned long)s_pid, (unsigned long)code );
		}
		Ded_Release();
		return;
	}
	if ( s_state == DED_STARTING ) {
		int timeoutMs = coop_dedTimeout->integer;
		if ( timeoutMs < 10 ) {
			timeoutMs = 10;
		} else if ( timeoutMs > 600 ) {
			timeoutMs = 600;
		}
		timeoutMs *= 1000;
		if ( now - s_startMs > timeoutMs ) {
			Ded_Stop( qtrue );
			Ded_Status( "the server did not answer within %d s, so it was stopped - untick Dedicated to host normally",
				timeoutMs / 1000 );
			return;
		}
		if ( now - s_nextPollMs >= 0 ) {
			CL_NET_OutOfBandPrint( s_adr, "getinfo xxx" );
			s_nextPollMs = now + DED_POLL_MS;
		}
	} else if ( s_state == DED_RUNNING && !coop_dedKeep->integer ) {
		// joined another server, was dropped, or the join failed: a server nobody asked to keep is not left behind
		if ( clc.state != CA_DISCONNECTED && NET_CompareAdr( clc.serverAddress, s_adr ) ) {
			s_awaySince = 0;
		} else if ( !s_awaySince ) {
			s_awaySince = now ? now : 1;
		} else if ( now - s_awaySince > DED_AWAY_MS ) {
			Ded_Stop( qfalse );
			Ded_Status( "you are no longer in your dedicated server's game, so it is stopping" );
		}
	} else if ( s_state == DED_STOPPING && now - s_stopDeadline >= 0 ) {
		Com_Printf( "coop dedicated: the server did not quit in time - ending it\n" );
		TerminateProcess( s_proc, 1 );
		WaitForSingleObject( s_proc, 2000 );
		Ded_Status( "the dedicated server stopped" );
		Ded_Release();
	}
}

/*
CL_DedicatedUserDisconnect - end of CL_Disconnect_f (the user's own `disconnect`; a remote server cannot send one,
cmd_filter.c whitelists it for a LOCAL server only). `was` = the address we were connected to.
*/
void CL_DedicatedUserDisconnect( netadr_t was ) {
	if ( !s_proc || s_state == DED_STOPPING || !NET_CompareAdr( was, s_adr ) ) {
		return;
	}
	if ( coop_dedKeep->integer ) {
		Ded_Status( "your dedicated server is still running (port %d, pid %lu) - coop_dedStop stops it", s_port,
			(unsigned long)s_pid );
		return;
	}
	Ded_Stop( qfalse );
	Ded_Status( "you left, so your dedicated server is stopping" );
}

/*
CL_DedicatedShutdown - CL_Shutdown on quit: stop the server, or leave it running when Keep Running is ticked
*/
void CL_DedicatedShutdown( void ) {
	if ( !s_proc ) {
		return;
	}
	if ( !Ded_Alive() ) {
		Ded_Release();
		return;
	}
	if ( coop_dedKeep->integer ) {
		Ded_ApplyKeep(); // KILL_ON_JOB_CLOSE off, so closing the job handle does not end it
		Com_Printf( "coop dedicated: leaving the server running - %s pid %lu, port %d (end it in Task Manager)\n",
			DED_EXE_NAME, (unsigned long)s_pid, s_port );
		if ( s_proc ) {
			CloseHandle( s_proc );
		}
		if ( s_job ) {
			CloseHandle( s_job );
		}
		s_proc = s_job = NULL;
		s_state        = DED_IDLE;
		SecureZeroMemory( s_rcon, sizeof( s_rcon ) );
		return;
	}
	Ded_Stop( qtrue );
	Com_Printf( "coop dedicated: server stopped\n" );
}

#else // !_WIN32

static void CL_DedStart_f( void ) {
	Cvar_Set( "coop_dedStatus", "the Dedicated option is Windows-only - run omohaaded yourself" );
}

static void CL_DedStop_f( void ) {}

void CL_DedicatedInfoResponse( netadr_t from ) {}
void CL_DedicatedFrame( void ) {}
void CL_DedicatedUserDisconnect( netadr_t was ) {}
void CL_DedicatedShutdown( void ) {}

#endif

/*
CL_DedicatedInit - from CL_Init
*/
void CL_DedicatedInit( void ) {
	coop_dedKeep    = Cvar_Get( "coop_dedKeep", "0", CVAR_ARCHIVE );
	coop_dedStatus  = Cvar_Get( "coop_dedStatus", "Dedicated: your game starts a separate server, then joins it", 0 );
	coop_dedLog     = Cvar_Get( "coop_dedLog", "0", 0 );
	coop_dedTimeout = Cvar_Get( "coop_dedTimeout", "120", 0 );
	Cmd_AddCommand( "coop_dedStart", CL_DedStart_f );
	Cmd_AddCommand( "coop_dedStop", CL_DedStop_f );
}
