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

/*****************************************************************************
 * name:		snd_mem.c
 *
 * desc:		sound caching
 *
 * $Archive: /MissionPack/code/client/snd_mem.c $
 *
 *****************************************************************************/

#include "snd_local.h"
#include "snd_codec.h"

#define DEF_COMSOUNDMEGS 16

/*
===============================================================================

memory management

===============================================================================
*/

static	sndBuffer	*buffer = NULL;
static	sndBuffer	*freelist = NULL;
static	int inUse = 0;
static	int totalInUse = 0;

short *sfxScratchBuffer = NULL;
sfx_t *sfxScratchPointer = NULL;
int	   sfxScratchIndex = 0;

void	SND_free(sndBuffer *v) {
	*(sndBuffer **)v = freelist;
	freelist = (sndBuffer*)v;
	inUse += sizeof(sndBuffer);
}

sndBuffer*	SND_malloc(void) {
	sndBuffer *v;
	int oldInUse = inUse;

    while (!freelist) {
        if (!S_FreeOldestSound()) {
            Com_Error(ERR_FATAL, "Out of SND allocations. Start the game with an higher com_soundMegs value and try again.");
            return NULL;
		}
	}

	inUse -= sizeof(sndBuffer);
	totalInUse += sizeof(sndBuffer);

	v = freelist;
	freelist = *(sndBuffer **)freelist;
	v->next = NULL;
	return v;
}

void SND_setup(void) {
	sndBuffer *p, *q;
	cvar_t	*cv;
	int scs;

	cv = Cvar_Get( "com_soundMegs", XSTRING(DEF_COMSOUNDMEGS), CVAR_LATCH | CVAR_ARCHIVE );

	scs = (cv->integer*1536);

	buffer = malloc(scs*sizeof(sndBuffer) );
	// allocate the stack based hunk allocator
	sfxScratchBuffer = malloc(SND_CHUNK_SIZE * sizeof(short) * 4);	//Hunk_Alloc(SND_CHUNK_SIZE * sizeof(short) * 4);
	sfxScratchPointer = NULL;

	inUse = scs*sizeof(sndBuffer);
	p = buffer;;
	q = p + scs;
	while (--q > p)
		*(sndBuffer **)q = q-1;
	
	*(sndBuffer **)q = NULL;
	freelist = p + scs - 1;

	Com_Printf("Sound memory manager started\n");
}

void SND_shutdown(void)
{
		free(sfxScratchBuffer);
		free(buffer);
}

/*
================
ResampleSfx

resample / decimate to the current source rate
================
*/
static int ResampleSfx( sfx_t *sfx, int channels, int inrate, int inwidth, int samples, byte *data, qboolean compressed ) {
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i, j;
	int		sample, samplefrac, fracstep;
	int			part;
	int		idx, maxsrc;	// HZM: source-index clamp (overrun guard)
	sndBuffer	*chunk;

	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = samples / stepscale;
	maxsrc = samples * channels;	// HZM: number of valid source samples; reads must stay below this

	srcsample = 0;
	samplefrac = 0;
	fracstep = stepscale * 256 * channels;
	chunk = sfx->soundData;

	for (i=0 ; i<outcount ; i++)
	{
		srcsample += samplefrac >> 8;
		samplefrac &= 255;
		samplefrac += fracstep;
		for (j=0 ; j<channels ; j++)
		{
			// HZM: clamp the source index so the resampler never reads past the end of the
			// input sample buffer. srcsample is advanced by fracstep and on the final
			// iterations can reach the buffer end and overrun by up to 'channels' samples ->
			// AV (root-caused on mono WAVs that need resampling: the m1l3c/m3l1a dog-model
			// transitions AND e3l4's SubPen_Generator_Run.wav, both surfaced here as
			// 0xC0000005). Repeating the last valid sample is inaudible vs. crashing.
			idx = srcsample + j;
			if (idx >= maxsrc) { idx = maxsrc - 1; }
			if (idx < 0) { idx = 0; }
			if( inwidth == 2 ) {
				sample = ( ((short *)data)[idx] );
			} else {
				sample = (unsigned int)( (unsigned char)(data[idx]) - 128) << 8;
			}
			part = (i*channels+j)&(SND_CHUNK_SIZE-1);
			if (part == 0) {
				sndBuffer	*newchunk;
				newchunk = SND_malloc();
				if (chunk == NULL) {
					sfx->soundData = newchunk;
				} else {
					chunk->next = newchunk;
				}
				chunk = newchunk;
			}

			chunk->sndChunk[part] = sample;
		}
	}

	return outcount;
}

/*
================
ResampleSfx

resample / decimate to the current source rate
================
*/
static int ResampleSfxRaw( short *sfx, int channels, int inrate, int inwidth, int samples, byte *data ) {
	int			outcount;
	int			srcsample;
	float		stepscale;
	int			i, j;
	int			sample, samplefrac, fracstep;
	int			idx, maxsrc;	// HZM: source-index clamp (overrun guard)

	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = samples / stepscale;
	maxsrc = samples * channels;	// HZM: number of valid source samples

	srcsample = 0;
	samplefrac = 0;
	fracstep = stepscale * 256 * channels;

	for (i=0 ; i<outcount ; i++)
	{
		srcsample += samplefrac >> 8;
		samplefrac &= 255;
		samplefrac += fracstep;
		for (j=0 ; j<channels ; j++)
		{
			idx = srcsample + j;	// HZM: clamp source index (same overrun guard as ResampleSfx)
			if (idx >= maxsrc) { idx = maxsrc - 1; }
			if (idx < 0) { idx = 0; }
			if( inwidth == 2 ) {
				sample = LittleShort ( ((short *)data)[idx] );
			} else {
				sample = (int)( (unsigned char)(data[idx]) - 128) << 8;
			}
			sfx[i*channels+j] = sample;
		}
	}
	return outcount;
}

//=============================================================================

/*
==============
S_LoadSound

The filename may be different than sfx->name in the case
of a forced fallback of a player specific sound
==============
*/
qboolean S_LoadSound( sfx_t *sfx )
{
	byte	*data;
	short	*samples;
	snd_info_t	info;
//	int		size;

	// load it in
	data = S_CodecLoad(sfx->soundName, &info);
	if(!data)
		return qfalse;

	if ( info.width == 1 ) {
		Com_DPrintf(S_COLOR_YELLOW "WARNING: %s is a 8 bit audio file\n", sfx->soundName);
	}

	//if ( info.rate != 22050 ) {
	//	Com_DPrintf(S_COLOR_YELLOW "WARNING: %s is not a 22kHz audio file\n", sfx->soundName);
	//}

	samples = Hunk_AllocateTempMemory(info.channels * info.samples * sizeof(short) * 2);

	sfx->lastTimeUsed = Com_Milliseconds()+1;

	// each of these compression schemes works just fine
	// but the 16bit quality is much nicer and with a local
	// install assured we can rely upon the sound memory
	// manager to do the right thing for us and page
	// sound in as needed

	if( info.channels == 1 && sfx->soundCompressed == qtrue) {
		sfx->soundCompressionMethod = 1;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, data + info.dataofs );
		S_AdpcmEncodeSound(sfx, samples);
#if 0
	} else if (info.channels == 1 && info.samples>(SND_CHUNK_SIZE*16) && info.width >1) {
		sfx->soundCompressionMethod = 3;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, (data + info.dataofs) );
		encodeMuLaw( sfx, samples);
	} else if (info.channels == 1 && info.samples>(SND_CHUNK_SIZE*6400) && info.width >1) {
		sfx->soundCompressionMethod = 2;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfxRaw( samples, info.channels, info.rate, info.width, info.samples, (data + info.dataofs) );
		encodeWavelet( sfx, samples);
#endif
	} else {
		sfx->soundCompressionMethod = 0;
		sfx->soundData = NULL;
		sfx->soundLength = ResampleSfx( sfx, info.channels, info.rate, info.width, info.samples, data + info.dataofs, qfalse );
	}

	sfx->soundChannels = info.channels;
	
	Hunk_FreeTempMemory(samples);
	Hunk_FreeTempMemory(data);

	return qtrue;
}

void S_DisplayFreeMemory(void) {
	Com_Printf("%d bytes free sound buffer memory, %d total used\n", inUse, totalInUse);
}
