/*
===========================================================================
HZM coop - server-side demo recorder ("record everything").

A client demo only holds what the server sent that client: the entities in its PVS / area, the
sounds and effects near it. Anything out of view was never written, so the demo editor's free
camera finds an empty map there. This recorder sits on the server and writes, every server
frame, a snapshot that holds EVERY networked entity on the map - no PVS, no area, no farplane or
fov culling - plus every configstring change, the reliable server commands, all sounds and all
cgame effect messages.

It writes the standard client demo format (demos/<name>.dm_<protocol>: gamestate + snapshot
messages, each prefixed by sequence and length), so the stock client and its demo editor play it
back unchanged. It is not a client: it takes no slot in svs.clients, has no gentity, no team, no
scoreboard line, and the AI can never see it. It only reads server state - it never sets
SVF_SENT, lastNetTime or snapshotCounter, so the game behaves exactly as if it were not there.

The stored point of view is the listen-server host, else the first active player. The free
camera does not care; it only decides what plain `demo` playback shows.

Prior art: ioq3 forks / OpenJK / ET:Legacy have server demos, but those are their own record
formats replayed by the server. Writing the client format instead means no new playback code.

Commands (server owners only - players never need them):
  sv_record [name]     start (continuous across map changes)
  sv_stoprecord        stop
  sv_recordstatus      frames, cost, size
Cvars:
  sv_autoRecord        1 = dedicated servers record every map automatically (2 = listen servers too)
  sv_autoRecordMax     keep at most this many auto demos (oldest deleted)
  sv_autoRecordMaxMB   ... and at most this many MB of them
The client's `record` on a listen-server host (F9) is routed here - see CL_Record_f.
===========================================================================
*/

#include "server.h"
#include "../qcommon/bg_compat.h"
#include "../qcommon/hzm_diag.h"

#define SVR_MAX_PENDING		512
#define SVR_AUTO_PREFIX		"auto_"

typedef struct {
	qboolean		active;
	qboolean		autoMode;		// started by sv_autoRecord: one file per map, rotated
	fileHandle_t	file;
	char			path[MAX_OSPATH];

	int				serverId;		// map the current gamestate was written for (-1 = none yet)
	int				fileServerId;	// map the file was opened on: auto mode starts a new file per map
	int				povNum;			// stored point of view (client index), -1 = none
	int				messageSequence;
	int				reliableSequence;
	int				lastTime;		// svs.time of the last snapshot written

	// reliable commands queued since the last message
	char			*pending;		// SVR_MAX_PENDING * MAX_STRING_CHARS
	int				numPending;
	int				droppedCmds;

	// previous snapshot, for delta compression
	playerState_t	ps;
	entityState_t	*ents[2];
	int				numEnts[2];
	int				cur;
	qboolean		deltaValid;

	server_sound_t	sounds[MAX_SERVER_SOUNDS];
	int				numSounds;

	char			print[MAX_STRING_CHARS];
	qboolean		locprint;
	int				printX, printY;

	// stats
	int				frames;
	int				maxEnts;
	unsigned long long	totalUsec;
	unsigned long long	maxUsec;
	int				bytes;
	int				maxMsg;
	int				gamestates;
} svrec_t;

static svrec_t	svr;

cvar_t	*sv_autoRecord;
cvar_t	*sv_autoRecordMax;
cvar_t	*sv_autoRecordMaxMB;

static void SVR_Close( void );

/*
===============
SVR_IsRecording
===============
*/
qboolean SVR_IsRecording( void ) {
	return svr.active;
}

int SVR_PovClient( void ) {
	return svr.active ? svr.povNum : -1;
}

/*
===============
SVR_Ready

True once a gamestate for the running map is in the file - only then are commands, sounds and
effects collected (before it, the gamestate itself carries the state).
===============
*/
static qboolean SVR_Ready( void ) {
	return svr.active && svr.file && svr.serverId == sv.serverId;
}

/*
===============
SVR_WriteMessage
===============
*/
static void SVR_WriteMessage( msg_t *msg ) {
	int		len;

	len = LittleLong( svr.messageSequence );
	FS_Write( &len, 4, svr.file );
	len = LittleLong( msg->cursize );
	FS_Write( &len, 4, svr.file );
	FS_Write( msg->data, msg->cursize, svr.file );
	svr.messageSequence++;
	svr.bytes += msg->cursize + 8;
	if ( msg->cursize > svr.maxMsg ) {
		svr.maxMsg = msg->cursize;
	}
}

/*
===============
SVR_AddCommand
===============
*/
static void SVR_AddCommand( const char *cmd ) {
	if ( svr.numPending >= SVR_MAX_PENDING ) {
		svr.droppedCmds++;
		return;
	}
	Q_strncpyz( svr.pending + svr.numPending * MAX_STRING_CHARS, cmd, MAX_STRING_CHARS );
	svr.numPending++;
}

/*
===============
SVR_ServerCommand

Every reliable command added to a client. The stored point of view's commands are mirrored, so
the demo carries exactly what that player's own demo would (prints, chat, objectives, stufftext).
Configstrings come straight from SVR_Configstring instead, so they never depend on the POV.
===============
*/
void SVR_ServerCommand( client_t *client, const char *cmd ) {
	if ( !SVR_Ready() || client - svs.clients != svr.povNum ) {
		return;
	}
	if ( !strncmp( cmd, "cs ", 3 ) || !strncmp( cmd, "bcs", 3 ) ) {
		return;
	}
	SVR_AddCommand( cmd );
}

/*
===============
SVR_Configstring

Same command forms as SV_SendConfigstring.
===============
*/
void SVR_Configstring( int index ) {
	int		maxChunkSize = MAX_STRING_CHARS - 24;
	int		denormalized;
	size_t	len;
	char	buf[MAX_STRING_CHARS];
	char	cmd[MAX_STRING_CHARS];

	if ( !SVR_Ready() ) {
		return;
	}

	denormalized = (int)CPT_DenormalizeConfigstring( index );
	len = strlen( sv.configstrings[index] );

	if ( len >= (size_t)maxChunkSize ) {
		int		sent = 0;
		size_t	remaining = len;
		const char *op;

		while ( remaining > 0 ) {
			if ( sent == 0 ) {
				op = "bcs0";
			} else if ( remaining < (size_t)maxChunkSize ) {
				op = "bcs2";
			} else {
				op = "bcs1";
			}
			Q_strncpyz( buf, &sv.configstrings[index][sent], maxChunkSize );
			Com_sprintf( cmd, sizeof( cmd ), "%s %i \"%s\"\n", op, denormalized, buf );
			SVR_AddCommand( cmd );

			sent += ( maxChunkSize - 1 );
			if ( remaining > (size_t)( maxChunkSize - 1 ) ) {
				remaining -= ( maxChunkSize - 1 );
			} else {
				remaining = 0;
			}
		}
	} else {
		Com_sprintf( cmd, sizeof( cmd ), "cs %i \"%s\"\n", denormalized, sv.configstrings[index] );
		SVR_AddCommand( cmd );
	}
}

/*
===============
SVR_Sound / SVR_StopSound

Every sound the game plays, wherever it is on the map.
===============
*/
void SVR_Sound( vec3_t *org, int entnum, int channel, int sound_index, float volume, float mindist, float pitch, float maxdist, qboolean streamed ) {
	server_sound_t *s;

	if ( !SVR_Ready() || svr.numSounds >= MAX_SERVER_SOUNDS ) {
		return;
	}
	s = &svr.sounds[svr.numSounds++];
	Com_Memset( s, 0, sizeof( *s ) );
	s->entity_number = entnum;
	if ( org ) {
		VectorCopy( *org, s->origin );
	}
	s->channel = channel;
	s->volume = volume;
	s->min_dist = mindist;
	s->pitch = pitch;
	s->maxDist = maxdist;
	s->sound_index = sound_index;
	s->streamed = streamed;
}

void SVR_StopSound( int entnum, int channel ) {
	server_sound_t *s;

	if ( !SVR_Ready() || svr.numSounds >= MAX_SERVER_SOUNDS ) {
		return;
	}
	s = &svr.sounds[svr.numSounds++];
	Com_Memset( s, 0, sizeof( *s ) );
	s->stop_flag = qtrue;
	s->entity_number = entnum;
	s->channel = channel;
}

/*
===============
SVR_CenterPrint
===============
*/
void SVR_CenterPrint( int clientNum, const char *msg, qboolean locprint, int x, int y ) {
	if ( !SVR_Ready() || clientNum != svr.povNum ) {
		return;
	}
	Q_strncpyz( svr.print, msg, sizeof( svr.print ) );
	svr.locprint = locprint;
	svr.printX = x;
	svr.printY = y;
}

/*
===============
SVR_PickPov

The listen-server host (loopback), else the first active player.
===============
*/
static int SVR_PickPov( void ) {
	int			i;
	client_t	*cl;
	int			first = -1;

	for ( i = 0, cl = svs.clients; i < svs.iNumClients; i++, cl++ ) {
		if ( cl->state != CS_ACTIVE || !cl->gentity ) {
			continue;
		}
		if ( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
			return i;
		}
		if ( first < 0 ) {
			first = i;
		}
	}
	return first;
}

/*
===============
SVR_WriteGamestate

The same message SV_SendClientGameState builds, written for the stored point of view.
===============
*/
static void SVR_WriteGamestate( void ) {
	static byte		buf[MAX_MSGLEN];
	msg_t			msg;
	int				i;
	entityState_t	nullstate;
	entityState_t	*base;

	MSG_Init( &msg, buf, sizeof( buf ) );
	MSG_WriteLong( &msg, 0 );		// reliable acknowledge

	MSG_WriteSVC( &msg, svc_gamestate );
	MSG_WriteLong( &msg, svr.reliableSequence );

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( sv.configstrings[i][0] ) {
			MSG_WriteSVC( &msg, svc_configstring );
			MSG_WriteShort( &msg, (int)CPT_DenormalizeConfigstring( i ) );
			MSG_WriteScrambledBigString( &msg, sv.configstrings[i] );
		}
	}

	MSG_GetNullEntityState( &nullstate );
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		base = &sv.svEntities[i].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteSVC( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue, sv.frameTime );
	}

	MSG_WriteByte( &msg, svc_EOF );
	MSG_WriteLong( &msg, svr.povNum );
	MSG_WriteLong( &msg, sv.checksumFeed );
	MSG_WriteServerFrameTime( &msg, sv.frameTime );
	MSG_WriteByte( &msg, svc_EOF );

	if ( msg.overflowed ) {
		Com_Printf( "^1sv_record: gamestate overflowed - recording stopped\n" );
		SVR_Close();
		return;
	}

	SVR_WriteMessage( &msg );

	svr.serverId = sv.serverId;
	Com_Printf( "^~^~^ SVREC gamestate file=%s map=%s pov=%d\n", svr.path, sv_mapname->string, svr.povNum );
	svr.deltaValid = qfalse;
	svr.numPending = 0;
	svr.numSounds = 0;
	svr.print[0] = 0;
	svr.gamestates++;

	// effects queued before the gamestate belong to the previous map
	SV_ResetCGMSlot( SVR_CGM_SLOT );
}

/*
===============
SVR_IsSkyEntity

The engine marks entities it reached through the sky portal pass with RF_SKYENTITY. Without a
viewpoint there is no such pass, so: an entity is in the sky box when its area is the sky
origin's (the sky box is sealed off from the playable map).
===============
*/
static qboolean SVR_IsSkyEntity( svEntity_t *svEnt, int skyArea ) {
	if ( skyArea < 0 ) {
		return qfalse;
	}
	return svEnt->areanum == skyArea || svEnt->areanum2 == skyArea;
}

/*
===============
SVR_BuildEntities

Every entity a client could ever be sent, from anywhere: the PVS / area / farplane / visibility
tests of SV_AddEntitiesVisibleFromPoint are skipped, the per-client and "nothing to draw" ones kept.
===============
*/
static int SVR_BuildEntities( entityState_t *out ) {
	int			e, n = 0;
	gentity_t	*ent, *parentEnt;
	svEntity_t	*svEnt;
	int			skyArea = -1;

	if ( sv.skyportal ) {
		for ( e = 0; e < sv.num_entities; e++ ) {
			ent = SV_GentityNum( e );
			if ( ent->inuse && ent->r.linked && ( ent->s.renderfx & RF_SKYORIGIN ) ) {
				skyArea = CM_LeafArea( CM_PointLeafnum( ent->s.origin ) );
				break;
			}
		}
	}

	for ( e = 0; e < sv.num_entities && n < MAX_GENTITIES; e++ ) {
		ent = SV_GentityNum( e );

		if ( !ent->inuse || !ent->r.linked ) {
			continue;
		}
		if ( ent->r.svFlags & SVF_NOCLIENT ) {
			continue;
		}
		if ( ( ent->r.svFlags & SVF_SINGLECLIENT ) && ent->r.singleClient != svr.povNum ) {
			continue;
		}
		if ( ( ent->r.svFlags & SVF_NOTSINGLECLIENT ) && ent->r.singleClient == svr.povNum ) {
			continue;
		}

		parentEnt = NULL;
		if ( ent->s.parent != ENTITYNUM_NONE ) {
			parentEnt = SV_GentityNum( ent->s.parent );
			if ( parentEnt && ( parentEnt->r.svFlags & SVF_NOCLIENT ) ) {
				continue;
			}
		}

		if ( !( ent->s.renderfx & RF_SKYORIGIN )
			&& !( ent->r.svFlags & ( SVF_BROADCAST | SVF_SENDONCE | SVF_SENDPVS ) )
			&& !parentEnt && !ent->s.modelindex && !ent->s.loopSound ) {
			continue;	// nothing to draw or hear
		}

		svEnt = SV_SvEntityForGentity( parentEnt ? parentEnt : ent );

		out[n] = ent->s;
		// these are per-viewer marks the snapshot builder leaves on the shared state
		out[n].renderfx &= ~( RF_SHADOW_PLANE | RF_WRAP_FRAMES | RF_SKYENTITY );
		if ( !( ent->s.renderfx & RF_SKYORIGIN ) && SVR_IsSkyEntity( svEnt, skyArea ) ) {
			out[n].renderfx |= RF_SKYENTITY;
		}
		n++;
	}
	return n;	// entity numbers ascend, as delta compression needs
}

/*
===============
SVR_EmitEntities

SV_EmitPacketEntities over the recorder's own two frames.
===============
*/
static void SVR_EmitEntities( entityState_t *from, int numFrom, entityState_t *to, int numTo, msg_t *msg ) {
	int		oldindex = 0, newindex = 0;
	int		oldnum, newnum;

	while ( newindex < numTo || oldindex < numFrom ) {
		newnum = newindex >= numTo ? 99999 : to[newindex].number;
		oldnum = oldindex >= numFrom ? 99999 : from[oldindex].number;

		if ( newnum == oldnum ) {
			MSG_WriteDeltaEntity( msg, &from[oldindex], &to[newindex], qfalse, sv.frameTime );
			oldindex++;
			newindex++;
		} else if ( newnum < oldnum ) {
			MSG_WriteDeltaEntity( msg, &sv.svEntities[newnum].baseline, &to[newindex], qtrue, sv.frameTime );
			newindex++;
		} else {
			MSG_WriteDeltaEntity( msg, &from[oldindex], NULL, qtrue, sv.frameTime );
			oldindex++;
		}
	}
	MSG_WriteEntityNum( msg, MAX_GENTITIES - 1 );
}

/*
===============
SVR_WriteSnapshot
===============
*/
static void SVR_WriteSnapshot( void ) {
	static byte		buf[MAX_MSGLEN];
	msg_t			msg;
	playerState_t	ps, *gps;
	client_t		*pov;
	int				i, next, areabytes;
	byte			areabits[MAX_MAP_AREA_BYTES];
	char			*cmd;

	pov = &svs.clients[svr.povNum];
	gps = SV_GameClientNum( svr.povNum );
	ps = *gps;
	ps.net_pm_flags = CPT_DenormalizePlayerStateFlags( gps->pm_flags );
	ps.iNetViewModelAnim = CPT_DenormalizeViewModelAnim( gps->iViewModelAnim );
	ps.radarInfo = pov->radarInfo;

	next = svr.cur ^ 1;
	svr.numEnts[next] = SVR_BuildEntities( svr.ents[next] );
	if ( svr.numEnts[next] > svr.maxEnts ) {
		svr.maxEnts = svr.numEnts[next];
	}

	MSG_Init( &msg, buf, sizeof( buf ) );
	MSG_WriteLong( &msg, 0 );		// reliable acknowledge

	// reliable commands, numbered on from the gamestate's sequence
	for ( i = 0; i < svr.numPending; i++ ) {
		cmd = svr.pending + i * MAX_STRING_CHARS;
		svr.reliableSequence++;
		MSG_WriteSVC( &msg, svc_serverCommand );
		MSG_WriteLong( &msg, svr.reliableSequence );
		MSG_WriteScrambledString( &msg, cmd );
	}
	svr.numPending = 0;

	MSG_WriteSVC( &msg, svc_snapshot );
	MSG_WriteLong( &msg, svs.time );
	MSG_WriteByte( &msg, sv.timeResidual > 254 ? 255 : sv.timeResidual );
	MSG_WriteByte( &msg, svr.deltaValid ? 1 : 0 );	// delta from the previous message
	MSG_WriteByte( &msg, svs.snapFlagServerBit );

	// every area visible: an all-zero mask (the mask marks areas that are NOT seen)
	areabytes = CM_WriteAreaBits( areabits, 0 );
	if ( areabytes > 255 ) {
		areabytes = 0;
	}
	Com_Memset( areabits, 0, sizeof( areabits ) );
	MSG_WriteByte( &msg, areabytes );
	MSG_WriteData( &msg, areabits, areabytes );

	if ( svr.deltaValid ) {
		MSG_WriteDeltaPlayerstate( &msg, &svr.ps, &ps, sv.frameTime );
		SVR_EmitEntities( svr.ents[svr.cur], svr.numEnts[svr.cur], svr.ents[next], svr.numEnts[next], &msg );
	} else {
		MSG_WriteDeltaPlayerstate( &msg, NULL, &ps, sv.frameTime );
		SVR_EmitEntities( NULL, 0, svr.ents[next], svr.numEnts[next], &msg );
	}

	MSG_WriteSounds( &msg, svr.sounds, svr.numSounds );
	svr.numSounds = 0;

	if ( svr.print[0] ) {
		if ( svr.locprint ) {
			MSG_WriteSVC( &msg, svc_locprint );
			MSG_WriteShort( &msg, svr.printX );
			MSG_WriteShort( &msg, svr.printY );
			MSG_WriteScrambledString( &msg, svr.print );
		} else {
			MSG_WriteSVC( &msg, svc_centerprint );
			MSG_WriteScrambledString( &msg, svr.print );
		}
		svr.print[0] = 0;
	}

	SV_WriteCGMSlot( SVR_CGM_SLOT, &msg );

	MSG_WriteByte( &msg, svc_EOF );

	if ( msg.overflowed ) {
		// cannot happen with MAX_MSGLEN at 256 KB; if it ever does, resync with a full snapshot
		Com_Printf( "^3sv_record: snapshot overflowed - skipped\n" );
		svr.deltaValid = qfalse;
		return;
	}

	SVR_WriteMessage( &msg );
	svr.ps = ps;
	svr.cur = next;
	svr.deltaValid = qtrue;
	svr.frames++;
}

/*
===============
SVR_Rotate

Keep the auto demos under sv_autoRecordMax files and sv_autoRecordMaxMB megabytes: oldest go first.
Names start with the date, so name order is age order.
===============
*/
static int QDECL SVR_StrCmp( const void *a, const void *b ) {
	return strcmp( *(const char **)a, *(const char **)b );
}

static void SVR_Rotate( void ) {
	char	**list;
	char	*names[1024];
	long	sizes[1024];
	int		num, n = 0, i;
	char	ext[16];
	char	path[MAX_OSPATH];
	long	total = 0;
	int		maxFiles = sv_autoRecordMax->integer;
	long	maxBytes = sv_autoRecordMaxMB->integer * 1024L * 1024L;

	Com_sprintf( ext, sizeof( ext ), ".dm_%d", com_protocol->integer );
	list = FS_ListFiles( "demos", ext, qfalse, &num );
	for ( i = 0; i < num && n < 1024; i++ ) {
		if ( Q_stricmpn( list[i], SVR_AUTO_PREFIX, strlen( SVR_AUTO_PREFIX ) ) ) {
			continue;
		}
		Com_sprintf( path, sizeof( path ), "demos/%s", list[i] );
		if ( !Q_stricmp( path, svr.path ) ) {
			continue;	// the one being written
		}
		names[n] = list[i];
		sizes[n] = FS_ReadFileEx( path, NULL, qtrue );
		if ( sizes[n] < 0 ) {
			sizes[n] = 0;
		}
		total += sizes[n];
		n++;
	}
	qsort( names, n, sizeof( names[0] ), SVR_StrCmp );
	// re-read sizes in sorted order is unnecessary: recompute per name below
	for ( i = 0; i < n; i++ ) {
		long sz;

		// the file being written counts as one of the kept ones
		if ( ( maxFiles <= 0 || n - i + 1 <= maxFiles ) && ( maxBytes <= 0 || total <= maxBytes ) ) {
			break;
		}
		Com_sprintf( path, sizeof( path ), "demos/%s", names[i] );
		sz = FS_ReadFileEx( path, NULL, qtrue );
		FS_HomeRemove( path );
		Com_Printf( "sv_record: rotated out %s\n", path );
		total -= sz > 0 ? sz : 0;
	}
	FS_FreeFileList( list );
}

/*
===============
SVR_Open
===============
*/
static qboolean SVR_Open( const char *name ) {
	Com_sprintf( svr.path, sizeof( svr.path ), "demos/%s.dm_%d", name, com_protocol->integer );
	svr.file = FS_FOpenFileWrite( svr.path );
	if ( !svr.file ) {
		Com_Printf( "^1sv_record: couldn't open %s\n", svr.path );
		svr.path[0] = 0;
		return qfalse;
	}
	Com_Printf( "^2sv_record: recording everything on the map to %s\n", svr.path );
	svr.serverId = -1;
	svr.fileServerId = sv.serverId;
	svr.messageSequence = 1;
	svr.reliableSequence = 0;
	svr.deltaValid = qfalse;
	svr.frames = 0;
	svr.maxEnts = 0;
	svr.totalUsec = 0;
	svr.maxUsec = 0;
	svr.bytes = 0;
	svr.maxMsg = 0;
	svr.gamestates = 0;
	svr.droppedCmds = 0;
	return qtrue;
}

/*
===============
SVR_Close
===============
*/
static void SVR_Close( void ) {
	int		len = -1;

	if ( svr.file ) {
		FS_Write( &len, 4, svr.file );
		FS_Write( &len, 4, svr.file );
		FS_FCloseFile( svr.file );
		svr.file = 0;
		if ( !svr.gamestates ) {
			FS_HomeRemove( svr.path );
			Com_Printf( "sv_record: nobody entered the map - removed the empty %s\n", svr.path );
			svr.serverId = -1;
			return;
		}
		Com_Printf( "sv_record: stopped %s - %d snapshots, %d map(s), %.1f MB, avg %.3f ms max %.3f ms per frame, peak %d entities\n",
			svr.path, svr.frames, svr.gamestates, svr.bytes / ( 1024.0 * 1024.0 ),
			svr.frames ? ( svr.totalUsec / (double)svr.frames ) / 1000.0 : 0.0,
			svr.maxUsec / 1000.0, svr.maxEnts );
		Com_Printf( "^~^~^ SVREC stop file=%s frames=%d maps=%d bytes=%d avg_us=%d max_us=%d peak_ents=%d max_msg=%d dropped_cmds=%d\n",
			svr.path, svr.frames, svr.gamestates, svr.bytes,
			svr.frames ? (int)( svr.totalUsec / svr.frames ) : 0, (int)svr.maxUsec, svr.maxEnts, svr.maxMsg, svr.droppedCmds );
	}
	svr.serverId = -1;
}

static void SVR_AutoName( char *out, int size ) {
	qtime_t	t;
	char	map[MAX_QPATH];
	char	*s;

	Com_RealTime( &t );
	Q_strncpyz( map, sv_mapname->string, sizeof( map ) );
	for ( s = map; *s; s++ ) {
		if ( *s == '/' || *s == '\\' || *s == ':' || *s == '$' ) {
			*s = '_';
		}
	}
	Com_sprintf( out, size, SVR_AUTO_PREFIX "%04d-%02d-%02d_%02d-%02d-%02d_%s",
		t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, map );
}

/*
===============
SVR_Start
===============
*/
static qboolean SVR_Start( const char *name, qboolean autoMode ) {
	if ( svr.active ) {
		SVR_Close();
		svr.active = qfalse;
	}
	if ( !svr.pending ) {
		svr.pending = (char *)Z_Malloc( SVR_MAX_PENDING * MAX_STRING_CHARS );
		svr.ents[0] = (entityState_t *)Z_Malloc( MAX_GENTITIES * sizeof( entityState_t ) );
		svr.ents[1] = (entityState_t *)Z_Malloc( MAX_GENTITIES * sizeof( entityState_t ) );
	}
	if ( !SVR_Open( name ) ) {
		return qfalse;
	}
	svr.active = qtrue;
	svr.autoMode = autoMode;
	svr.povNum = -1;
	svr.numPending = 0;
	svr.numSounds = 0;
	svr.print[0] = 0;
	if ( autoMode ) {
		SVR_Rotate();
	}
	return qtrue;
}

/*
===============
SVR_Stop
===============
*/
void SVR_Stop( void ) {
	if ( !svr.active ) {
		return;
	}
	SVR_Close();
	svr.active = qfalse;
	svr.povNum = -1;
	SV_ResetCGMSlot( SVR_CGM_SLOT );
}

/*
===============
SVR_Frame

After SV_SendClientMessages, once per server frame that ran the game.
===============
*/
void SVR_Frame( void ) {
	unsigned long long	t0, dt;
	int					pov;
	if ( sv.state != SS_GAME ) {
		return;
	}

	// dedicated servers record every map on their own
	if ( !svr.active && sv_autoRecord->integer
		&& ( com_dedicated->integer || sv_autoRecord->integer >= 2 )
		&& SVR_PickPov() >= 0 ) {
		char name[MAX_QPATH];

		SVR_AutoName( name, sizeof( name ) );
		SVR_Start( name, qtrue );
	}

	if ( !svr.active ) {
		return;
	}
	if ( svs.time == svr.lastTime && SVR_Ready() ) {
		return;		// no game frame ran
	}

	t0 = HzmDiag_Micros();

	// a new map: auto mode starts a new file, a manual recording carries on in the same file
	if ( svr.autoMode && svr.fileServerId != sv.serverId ) {
		char name[MAX_QPATH];

		SVR_Close();
		SVR_AutoName( name, sizeof( name ) );
		if ( !SVR_Open( name ) ) {
			svr.active = qfalse;
			return;
		}
		SVR_Rotate();
	}

	pov = svr.povNum;
	if ( pov < 0 || pov >= svs.iNumClients || svs.clients[pov].state != CS_ACTIVE || !svs.clients[pov].gentity ) {
		pov = SVR_PickPov();
		if ( pov < 0 ) {
			return;		// nobody in the game yet: nothing to frame the snapshot on
		}
	}

	if ( pov != svr.povNum || svr.serverId != sv.serverId ) {
		// a new point of view needs a gamestate naming it (clientNum is in the gamestate)
		svr.povNum = pov;
		SVR_WriteGamestate();
		if ( !svr.active || !svr.file ) {
			return;
		}
	}

	if ( !g_CGMessagesReady( SVR_CGM_SLOT ) ) {
		SV_InitCGMessage( SVR_CGM_SLOT );
	}

	SVR_WriteSnapshot();
	svr.lastTime = svs.time;

	dt = HzmDiag_Micros() - t0;
	svr.totalUsec += dt;
	if ( dt > svr.maxUsec ) {
		svr.maxUsec = dt;
	}
}

/*
===============
SVR_Shutdown

Server going down (quit, disconnect, error): finish the file so it plays.
===============
*/
void SVR_Shutdown( void ) {
	SVR_Stop();
}

/*
===============
Commands
===============
*/
static void SVR_Record_f( void ) {
	char name[MAX_QPATH];

	if ( !com_sv_running->integer || sv.state != SS_GAME ) {
		Com_Printf( "sv_record: no map running\n" );
		return;
	}
	if ( Cmd_Argc() > 1 ) {
		Q_strncpyz( name, Cmd_Argv( 1 ), sizeof( name ) );
	} else {
		qtime_t t;

		Com_RealTime( &t );
		Com_sprintf( name, sizeof( name ), "server_%04d-%02d-%02d_%02d-%02d-%02d",
			t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec );
	}
	SVR_Start( name, qfalse );
}

static void SVR_StopRecord_f( void ) {
	if ( !svr.active ) {
		Com_Printf( "sv_record: not recording\n" );
		return;
	}
	SVR_Stop();
}

static void SVR_Status_f( void ) {
	if ( !svr.active ) {
		Com_Printf( "sv_record: not recording (sv_autoRecord %d)\n", sv_autoRecord->integer );
		return;
	}
	Com_Printf( "sv_record: %s%s - %d snapshots, %.1f MB, avg %.3f ms max %.3f ms, peak %d entities, pov client %d\n",
		svr.path, svr.autoMode ? " (auto)" : "", svr.frames, svr.bytes / ( 1024.0 * 1024.0 ),
		svr.frames ? ( svr.totalUsec / (double)svr.frames ) / 1000.0 : 0.0, svr.maxUsec / 1000.0,
		svr.maxEnts, svr.povNum );
	Com_Printf( "^~^~^ SVREC status file=%s frames=%d bytes=%d avg_us=%d max_us=%d peak_ents=%d max_msg=%d pov=%d\n",
		svr.path, svr.frames, svr.bytes, svr.frames ? (int)( svr.totalUsec / svr.frames ) : 0,
		(int)svr.maxUsec, svr.maxEnts, svr.maxMsg, svr.povNum );
}

/*
===============
SVR_Ents_f

sv_recordents [mindist] - what the recorder holds that the point of view's own demo would not: every
drawn entity at least mindist (default 1500) from it, with its model. For checking a recording.
===============
*/
static void SVR_Ents_f( void ) {
	int			e, n = 0, pov;
	float		minDist = Cmd_Argc() > 1 ? atof( Cmd_Argv( 1 ) ) : 1500.0f;
	gentity_t	*ent, *pe;
	vec3_t		d;
	const char	*model;

	if ( sv.state != SS_GAME ) {
		return;
	}
	pov = svr.povNum >= 0 ? svr.povNum : SVR_PickPov();
	if ( pov < 0 ) {
		Com_Printf( "sv_recordents: nobody in the game\n" );
		return;
	}
	pe = svs.clients[pov].gentity;
	for ( e = 0; e < sv.num_entities; e++ ) {
		ent = SV_GentityNum( e );
		if ( !ent->inuse || !ent->r.linked || ( ent->r.svFlags & SVF_NOCLIENT ) || !ent->s.modelindex ) {
			continue;
		}
		VectorSubtract( ent->s.origin, pe->s.origin, d );
		if ( VectorLength( d ) < minDist ) {
			continue;
		}
		model = sv.configstrings[CS_MODELS + ent->s.modelindex];
		Com_Printf( "^~^~^ SVRENT num=%d type=%d dist=%.0f org=%.0f %.0f %.0f model=%s\n", e, ent->s.eType,
			VectorLength( d ), ent->s.origin[0], ent->s.origin[1], ent->s.origin[2], model ? model : "" );
		n++;
	}
	Com_Printf( "sv_recordents: %d entities %.0f+ units from client %d\n", n, minDist, pov );
}

void SVR_Init( void ) {
	Cmd_AddCommand( "sv_recordents", SVR_Ents_f );
	sv_autoRecord = Cvar_Get( "sv_autoRecord", "1", CVAR_ARCHIVE );
	sv_autoRecordMax = Cvar_Get( "sv_autoRecordMax", "30", CVAR_ARCHIVE );
	sv_autoRecordMaxMB = Cvar_Get( "sv_autoRecordMaxMB", "2048", CVAR_ARCHIVE );
	Cmd_AddCommand( "sv_record", SVR_Record_f );
	Cmd_AddCommand( "sv_stoprecord", SVR_StopRecord_f );
	Cmd_AddCommand( "sv_recordstatus", SVR_Status_f );
	svr.serverId = -1;
	svr.povNum = -1;
}
