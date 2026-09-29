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
 * name:		cl_cin.c
 *
 * desc:		video and cinematic playback
 *
 * $Archive: /MissionPack/code/client/cl_cin.c $
 *
 * cl_glconfig.hwtype trtypes 3dfx/ragepro need 256x256
 *
 *****************************************************************************/

#include "client.h"
#include "../uilib/ui_public.h"
#include "snd_local.h"

#include "cl_ui.h"

#define MAXSIZE				8
#define MINSIZE				4

#define DEFAULT_CIN_WIDTH	512
#define DEFAULT_CIN_HEIGHT	512

#define ROQ_QUAD			0x1000
#define ROQ_QUAD_INFO		0x1001
#define ROQ_CODEBOOK		0x1002
#define ROQ_QUAD_VQ			0x1011
#define ROQ_QUAD_JPEG		0x1012
#define ROQ_QUAD_HANG		0x1013
#define ROQ_PACKET			0x1030
#define ZA_SOUND_MONO		0x1020
#define ZA_SOUND_STEREO		0x1021

#define MAX_VIDEO_HANDLES	16


static void RoQ_init( void );
static void CL_FinishedCinematic();

/******************************************************************************
*
* Class:		trFMV
*
* Description:	RoQ/RnR manipulation routines
*				not entirely complete for first run
*
******************************************************************************/

static	long				ROQ_YY_tab[256];
static	long				ROQ_UB_tab[256];
static	long				ROQ_UG_tab[256];
static	long				ROQ_VG_tab[256];
static	long				ROQ_VR_tab[256];
static	unsigned short		vq2[256*16*4];
static	unsigned short		vq4[256*64*4];
static	unsigned short		vq8[256*256*4];


typedef struct {
	byte				linbuf[DEFAULT_CIN_WIDTH*DEFAULT_CIN_HEIGHT*4*2];
	byte				file[65536];
	short				sqrTable[256];

	int		mcomp[256];
	byte				*qStatus[2][32768];

	long				oldXOff, oldYOff, oldysize, oldxsize;

	int					currentHandle;
} cinematics_t;

typedef struct {
	char				fileName[MAX_OSPATH];
	int					CIN_WIDTH, CIN_HEIGHT;
	int					xpos, ypos, width, height;
	qboolean			looping, holdAtEnd, dirty, alterGameState, silent, shader;
	fileHandle_t		iFile;
	e_status			status;
	int					startTime;
	int					lastTime;
	long				tfps;
	long				RoQPlayed;
	long				ROQSize;
	unsigned int		RoQFrameSize;
	long				onQuad;
	long				numQuads;
	long				samplesPerLine;
	unsigned int		roq_id;
	long				screenDelta;

	void ( *VQ0)(byte *status, void *qdata );
	void ( *VQ1)(byte *status, void *qdata );
	void ( *VQNormal)(byte *status, void *qdata );
	void ( *VQBuffer)(byte *status, void *qdata );

	long				samplesPerPixel;				// defaults to 2
	byte*				gray;
	unsigned int		xsize, ysize, maxsize, minsize;

	qboolean			half, smootheddouble, inMemory;
	long				normalBuffer0;
	long				roq_flags;
	long				roqF0;
	long				roqF1;
	long				t[2];
	long				roqFPS;
	int					playonwalls;
	byte*				buf;
	long				drawX, drawY;

	long realTime;
	long currTime;
	long soundTime;
	qboolean coopOverlay; // HZM coop bug-3234: the networked briefing overlay owns this slot's teardown
} cin_cache;

static cinematics_t		cin;
static cin_cache		cinTable[MAX_VIDEO_HANDLES];
static int				currentHandle = -1;
static int				CL_handle = -1;

static connstate_t		oldClientState;

static int				audioStartTime = 0;

extern "C" int				s_soundtime;		// sample PAIRS


void CIN_CloseAllVideos(void) {
	int		i;

	for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
		if (cinTable[i].fileName[0] != 0 ) {
			CIN_StopCinematic(i);
		}
	}
}


static int CIN_HandleForVideo(void) {
	int		i;

	for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
		if ( cinTable[i].fileName[0] == 0 ) {
			cinTable[i].coopOverlay = qfalse; // HZM coop bug-3234: the overlay flag never survives into a new use
			return i;
		}
	}
	Com_Error( ERR_DROP, "CIN_HandleForVideo: none free" );
	return -1;
}


extern "C" int Com_Milliseconds(void);

//-----------------------------------------------------------------------------
// RllSetupTable
//
// Allocates and initializes the square table.
//
// Parameters:	None
//
// Returns:		Nothing
//-----------------------------------------------------------------------------
static void RllSetupTable( void )
{
	int z;

	for (z=0;z<128;z++) {
		cin.sqrTable[z] = (short)(z*z);
		cin.sqrTable[z+128] = (short)(-cin.sqrTable[z]);
	}
}



//-----------------------------------------------------------------------------
// RllDecodeMonoToMono
//
// Decode mono source data into a mono buffer.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= # of shorts of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
long RllDecodeMonoToMono(unsigned char *from,short *to,unsigned int size,char signedOutput ,unsigned short flag)
{
	unsigned int z;
	int prev;
	
	if (signedOutput)	
		prev =  flag - 0x8000;
	else 
		prev = flag;

	for (z=0;z<size;z++) {
		prev = to[z] = (short)(prev + cin.sqrTable[from[z]]); 
	}
	return size;	//*sizeof(short));
}


//-----------------------------------------------------------------------------
// RllDecodeMonoToStereo
//
// Decode mono source data into a stereo buffer. Output is 4 times the number
// of bytes in the input.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= 1/4 # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
long RllDecodeMonoToStereo(unsigned char *from,short *to,unsigned int size,char signedOutput,unsigned short flag)
{
	unsigned int z;
	int prev;
	
	if (signedOutput)	
		prev =  flag - 0x8000;
	else 
		prev = flag;

	for (z = 0; z < size; z++) {
		prev = (short)(prev + cin.sqrTable[from[z]]);
		to[z*2+0] = to[z*2+1] = (short)(prev);
	}
	
	return size;	// * 2 * sizeof(short));
}


//-----------------------------------------------------------------------------
// RllDecodeStereoToStereo
//
// Decode stereo source data into a stereo buffer.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= 1/2 # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
long RllDecodeStereoToStereo(unsigned char *from,short *to,unsigned int size,char signedOutput, unsigned short flag)
{
	unsigned int z;
	unsigned char *zz = from;
	int	prevL, prevR;

	if (signedOutput) {
		prevL = (flag & 0xff00) - 0x8000;
		prevR = ((flag & 0x00ff) << 8) - 0x8000;
	} else {
		prevL = flag & 0xff00;
		prevR = (flag & 0x00ff) << 8;
	}

	for (z=0;z<size;z+=2) {
                prevL = (short)(prevL + cin.sqrTable[*zz++]); 
                prevR = (short)(prevR + cin.sqrTable[*zz++]);
                to[z+0] = (short)(prevL);
                to[z+1] = (short)(prevR);
	}
	
	return (size>>1);	//*sizeof(short));
}


//-----------------------------------------------------------------------------
// RllDecodeStereoToMono
//
// Decode stereo source data into a mono buffer.
//
// Parameters:	from -> buffer holding encoded data
//				to ->	buffer to hold decoded data
//				size =	number of bytes of input (= # of bytes of output)
//				signedOutput = 0 for unsigned output, non-zero for signed output
//				flag = flags from asset header
//
// Returns:		Number of samples placed in output buffer
//-----------------------------------------------------------------------------
long RllDecodeStereoToMono(unsigned char *from,short *to,unsigned int size,char signedOutput, unsigned short flag)
{
	unsigned int z;
	int prevL,prevR;
	
	if (signedOutput) {
		prevL = (flag & 0xff00) - 0x8000;
		prevR = ((flag & 0x00ff) << 8) -0x8000;
	} else {
		prevL = flag & 0xff00;
		prevR = (flag & 0x00ff) << 8;
	}

	for (z=0;z<size;z+=1) {
		prevL= prevL + cin.sqrTable[from[z*2]];
		prevR = prevR + cin.sqrTable[from[z*2+1]];
		to[z] = (short)((prevL + prevR)/2);
	}

	return size;
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void move8_32( byte *src, byte *dst, int spl )
{
	int i;

	for(i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void move4_32( byte *src, byte *dst, int spl  )
{
	int i;

	for(i = 0; i < 4; ++i)
	{
		memcpy(dst, src, 16);
		src += spl;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void blit8_32( byte *src, byte *dst, int spl  )
{
	int i;

	for(i = 0; i < 8; ++i)
	{
		memcpy(dst, src, 32);
		src += 32;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/
static void blit4_32( byte *src, byte *dst, int spl  )
{
	int i;

	for(i = 0; i < 4; ++i)
	{
		memmove(dst, src, 16);
		src += 16;
		dst += spl;
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void blit2_32( byte *src, byte *dst, int spl  )
{
	memcpy(dst, src, 8);
	memcpy(dst+spl, src+8, 8);
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void blitVQQuad32fs( byte **status, unsigned char *data )
{
unsigned short	newd, celdata, code;
unsigned int	index, i;
int		spl;

	newd	= 0;
	celdata = 0;
	index	= 0;
	
        spl = cinTable[currentHandle].samplesPerLine;
        
	do {
		if (!newd) { 
			newd = 7;
			celdata = data[0] + data[1]*256;
			data += 2;
		} else {
			newd--;
		}

		code = (unsigned short)(celdata&0xc000); 
		celdata <<= 2;
		
		switch (code) {
			case	0x8000:													// vq code
				blit8_32( (byte *)&vq8[(*data)*128], status[index], spl );
				data++;
				index += 5;
				break;
			case	0xc000:													// drop
				index++;													// skip 8x8
				for(i=0;i<4;i++) {
					if (!newd) { 
						newd = 7;
						celdata = data[0] + data[1]*256;
						data += 2;
					} else {
						newd--;
					}
						
					code = (unsigned short)(celdata&0xc000); celdata <<= 2; 

					switch (code) {											// code in top two bits of code
						case	0x8000:										// 4x4 vq code
							blit4_32( (byte *)&vq4[(*data)*32], status[index], spl );
							data++;
							break;
						case	0xc000:										// 2x2 vq code
							blit2_32( (byte *)&vq2[(*data)*8], status[index], spl );
							data++;
							blit2_32( (byte *)&vq2[(*data)*8], status[index]+8, spl );
							data++;
							blit2_32( (byte *)&vq2[(*data)*8], status[index]+spl*2, spl );
							data++;
							blit2_32( (byte *)&vq2[(*data)*8], status[index]+spl*2+8, spl );
							data++;
							break;
						case	0x4000:										// motion compensation
							move4_32( status[index] + cin.mcomp[(*data)], status[index], spl );
							data++;
							break;
					}
					index++;
				}
				break;
			case	0x4000:													// motion compensation
				move8_32( status[index] + cin.mcomp[(*data)], status[index], spl );
				data++;
				index += 5;
				break;
			case	0x0000:
				index += 5;
				break;
		}
	} while ( status[index] != NULL );
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void ROQ_GenYUVTables( void )
{
	float t_ub,t_vr,t_ug,t_vg;
	long i;

	t_ub = (1.77200f/2.0f) * (float)(1<<6) + 0.5f;
	t_vr = (1.40200f/2.0f) * (float)(1<<6) + 0.5f;
	t_ug = (0.34414f/2.0f) * (float)(1<<6) + 0.5f;
	t_vg = (0.71414f/2.0f) * (float)(1<<6) + 0.5f;
	for(i=0;i<256;i++) {
		float x = (float)(2 * i - 255);
	
		ROQ_UB_tab[i] = (long)( ( t_ub * x) + (1<<5));
		ROQ_VR_tab[i] = (long)( ( t_vr * x) + (1<<5));
		ROQ_UG_tab[i] = (long)( (-t_ug * x)		 );
		ROQ_VG_tab[i] = (long)( (-t_vg * x) + (1<<5));
		ROQ_YY_tab[i] = (long)( (i << 6) | (i >> 2) );
	}
}

#define VQ2TO4(a,b,c,d) { \
    	*c++ = a[0];	\
	*d++ = a[0];	\
	*d++ = a[0];	\
	*c++ = a[1];	\
	*d++ = a[1];	\
	*d++ = a[1];	\
	*c++ = b[0];	\
	*d++ = b[0];	\
	*d++ = b[0];	\
	*c++ = b[1];	\
	*d++ = b[1];	\
	*d++ = b[1];	\
	*d++ = a[0];	\
	*d++ = a[0];	\
	*d++ = a[1];	\
	*d++ = a[1];	\
	*d++ = b[0];	\
	*d++ = b[0];	\
	*d++ = b[1];	\
	*d++ = b[1];	\
	a += 2; b += 2; }
 
#define VQ2TO2(a,b,c,d) { \
	*c++ = *a;	\
	*d++ = *a;	\
	*d++ = *a;	\
	*c++ = *b;	\
	*d++ = *b;	\
	*d++ = *b;	\
	*d++ = *a;	\
	*d++ = *a;	\
	*d++ = *b;	\
	*d++ = *b;	\
	a++; b++; }

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static unsigned short yuv_to_rgb( long y, long u, long v )
{ 
	long r,g,b,YY = (long)(ROQ_YY_tab[(y)]);

	r = (YY + ROQ_VR_tab[v]) >> 9;
	g = (YY + ROQ_UG_tab[u] + ROQ_VG_tab[v]) >> 8;
	b = (YY + ROQ_UB_tab[u]) >> 9;
	
	if (r<0) r = 0; if (g<0) g = 0; if (b<0) b = 0;
	if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;

	return (unsigned short)((r<<11)+(g<<5)+(b));
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/
static unsigned int yuv_to_rgb24( long y, long u, long v )
{ 
	long r,g,b,YY = (long)(ROQ_YY_tab[(y)]);

	r = (YY + ROQ_VR_tab[v]) >> 6;
	g = (YY + ROQ_UG_tab[u] + ROQ_VG_tab[v]) >> 6;
	b = (YY + ROQ_UB_tab[u]) >> 6;
	
	if (r<0) r = 0; if (g<0) g = 0; if (b<0) b = 0;
	if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
	
	return LittleLong ((r)|(g<<8)|(b<<16)|(255<<24));
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void decodeCodeBook( byte *input, unsigned short roq_flags )
{
	long	i, j, two, four;
	unsigned short	*aptr, *bptr, *cptr, *dptr;
	long	y0,y1,y2,y3,cr,cb;
	byte	*bbptr, *baptr, *bcptr, *bdptr;
	union {
		unsigned int *i;
		unsigned short *s;
	} iaptr, ibptr, icptr, idptr;

	if (!roq_flags) {
		two = four = 256;
	} else {
		two  = roq_flags>>8;
		if (!two) two = 256;
		four = roq_flags&0xff;
	}

	four *= 2;

	bptr = (unsigned short *)vq2;

	if (!cinTable[currentHandle].half) {
		if (!cinTable[currentHandle].smootheddouble) {
//
// normal height
//
			if (cinTable[currentHandle].samplesPerPixel==2) {
				for(i=0;i<two;i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = yuv_to_rgb( y0, cr, cb );
					*bptr++ = yuv_to_rgb( y1, cr, cb );
					*bptr++ = yuv_to_rgb( y2, cr, cb );
					*bptr++ = yuv_to_rgb( y3, cr, cb );
				}

				cptr = (unsigned short *)vq4;
				dptr = (unsigned short *)vq8;
		
				for(i=0;i<four;i++) {
					aptr = (unsigned short *)vq2 + (*input++)*4;
					bptr = (unsigned short *)vq2 + (*input++)*4;
					for(j=0;j<2;j++)
						VQ2TO4(aptr,bptr,cptr,dptr);
				}
			} else if (cinTable[currentHandle].samplesPerPixel==4) {
				ibptr.s = bptr;
				for(i=0;i<two;i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = yuv_to_rgb24( y0, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y1, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y2, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y3, cr, cb );
				}

				icptr.s = vq4;
				idptr.s = vq8;
	
				for(i=0;i<four;i++) {
					iaptr.s = vq2;
					iaptr.i += (*input++)*4;
					ibptr.s = vq2;
					ibptr.i += (*input++)*4;
					for(j=0;j<2;j++) 
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
				}
			} else if (cinTable[currentHandle].samplesPerPixel==1) {
				bbptr = (byte *)bptr;
				for(i=0;i<two;i++) {
					*bbptr++ = cinTable[currentHandle].gray[*input++];
					*bbptr++ = cinTable[currentHandle].gray[*input++];
					*bbptr++ = cinTable[currentHandle].gray[*input++];
					*bbptr++ = cinTable[currentHandle].gray[*input]; input +=3;
				}

				bcptr = (byte *)vq4;
				bdptr = (byte *)vq8;
	
				for(i=0;i<four;i++) {
					baptr = (byte *)vq2 + (*input++)*4;
					bbptr = (byte *)vq2 + (*input++)*4;
					for(j=0;j<2;j++) 
						VQ2TO4(baptr,bbptr,bcptr,bdptr);
				}
			}
		} else {
//
// double height, smoothed
//
			if (cinTable[currentHandle].samplesPerPixel==2) {
				for(i=0;i<two;i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*bptr++ = yuv_to_rgb( y0, cr, cb );
					*bptr++ = yuv_to_rgb( y1, cr, cb );
					*bptr++ = yuv_to_rgb( ((y0*3)+y2)/4, cr, cb );
					*bptr++ = yuv_to_rgb( ((y1*3)+y3)/4, cr, cb );
					*bptr++ = yuv_to_rgb( (y0+(y2*3))/4, cr, cb );
					*bptr++ = yuv_to_rgb( (y1+(y3*3))/4, cr, cb );
					*bptr++ = yuv_to_rgb( y2, cr, cb );
					*bptr++ = yuv_to_rgb( y3, cr, cb );
				}

				cptr = (unsigned short *)vq4;
				dptr = (unsigned short *)vq8;
		
				for(i=0;i<four;i++) {
					aptr = (unsigned short *)vq2 + (*input++)*8;
					bptr = (unsigned short *)vq2 + (*input++)*8;
					for(j=0;j<2;j++) {
						VQ2TO4(aptr,bptr,cptr,dptr);
						VQ2TO4(aptr,bptr,cptr,dptr);
					}
				}
			} else if (cinTable[currentHandle].samplesPerPixel==4) {
				ibptr.s = bptr;
				for(i=0;i<two;i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input++;
					cr = (long)*input++;
					cb = (long)*input++;
					*ibptr.i++ = yuv_to_rgb24( y0, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y1, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( ((y0*3)+y2)/4, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( ((y1*3)+y3)/4, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( (y0+(y2*3))/4, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( (y1+(y3*3))/4, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y2, cr, cb );
					*ibptr.i++ = yuv_to_rgb24( y3, cr, cb );
				}

				icptr.s = vq4;
				idptr.s = vq8;
	
				for(i=0;i<four;i++) {
					iaptr.s = vq2;
					iaptr.i += (*input++)*8;
					ibptr.s = vq2;
					ibptr.i += (*input++)*8;
					for(j=0;j<2;j++) {
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
						VQ2TO4(iaptr.i, ibptr.i, icptr.i, idptr.i);
					}
				}
			} else if (cinTable[currentHandle].samplesPerPixel==1) {
				bbptr = (byte *)bptr;
				for(i=0;i<two;i++) {
					y0 = (long)*input++;
					y1 = (long)*input++;
					y2 = (long)*input++;
					y3 = (long)*input; input+= 3;
					*bbptr++ = cinTable[currentHandle].gray[y0];
					*bbptr++ = cinTable[currentHandle].gray[y1];
					*bbptr++ = cinTable[currentHandle].gray[((y0*3)+y2)/4];
					*bbptr++ = cinTable[currentHandle].gray[((y1*3)+y3)/4];
					*bbptr++ = cinTable[currentHandle].gray[(y0+(y2*3))/4];
					*bbptr++ = cinTable[currentHandle].gray[(y1+(y3*3))/4];						
					*bbptr++ = cinTable[currentHandle].gray[y2];
					*bbptr++ = cinTable[currentHandle].gray[y3];
				}

				bcptr = (byte *)vq4;
				bdptr = (byte *)vq8;
	
				for(i=0;i<four;i++) {
					baptr = (byte *)vq2 + (*input++)*8;
					bbptr = (byte *)vq2 + (*input++)*8;
					for(j=0;j<2;j++) {
						VQ2TO4(baptr,bbptr,bcptr,bdptr);
						VQ2TO4(baptr,bbptr,bcptr,bdptr);
					}
				}
			}			
		}
	} else {
//
// 1/4 screen
//
		if (cinTable[currentHandle].samplesPerPixel==2) {
			for(i=0;i<two;i++) {
				y0 = (long)*input; input+=2;
				y2 = (long)*input; input+=2;
				cr = (long)*input++;
				cb = (long)*input++;
				*bptr++ = yuv_to_rgb( y0, cr, cb );
				*bptr++ = yuv_to_rgb( y2, cr, cb );
			}

			cptr = (unsigned short *)vq4;
			dptr = (unsigned short *)vq8;
	
			for(i=0;i<four;i++) {
				aptr = (unsigned short *)vq2 + (*input++)*2;
				bptr = (unsigned short *)vq2 + (*input++)*2;
				for(j=0;j<2;j++) { 
					VQ2TO2(aptr,bptr,cptr,dptr);
				}
			}
		} else if (cinTable[currentHandle].samplesPerPixel == 1) {
			bbptr = (byte *)bptr;
				
			for(i=0;i<two;i++) {
				*bbptr++ = cinTable[currentHandle].gray[*input]; input+=2;
				*bbptr++ = cinTable[currentHandle].gray[*input]; input+=4;
			}

			bcptr = (byte *)vq4;
			bdptr = (byte *)vq8;
	
			for(i=0;i<four;i++) {
				baptr = (byte *)vq2 + (*input++)*2;
				bbptr = (byte *)vq2 + (*input++)*2;
				for(j=0;j<2;j++) { 
					VQ2TO2(baptr,bbptr,bcptr,bdptr);
				}
			}			
		} else if (cinTable[currentHandle].samplesPerPixel == 4) {
			ibptr.s = bptr;
			for(i=0;i<two;i++) {
				y0 = (long)*input; input+=2;
				y2 = (long)*input; input+=2;
				cr = (long)*input++;
				cb = (long)*input++;
				*ibptr.i++ = yuv_to_rgb24( y0, cr, cb );
				*ibptr.i++ = yuv_to_rgb24( y2, cr, cb );
			}

			icptr.s = vq4;
			idptr.s = vq8;
	
			for(i=0;i<four;i++) {
				iaptr.s = vq2;
				iaptr.i += (*input++)*2;
				ibptr.s = vq2 + (*input++)*2;
				ibptr.i += (*input++)*2;
				for(j=0;j<2;j++) { 
					VQ2TO2(iaptr.i,ibptr.i,icptr.i,idptr.i);
				}
			}
		}
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void recurseQuad( long startX, long startY, long quadSize, long xOff, long yOff )
{
	byte *scroff;
	long bigx, bigy, lowx, lowy, useY;
	long offset;

	offset = cinTable[currentHandle].screenDelta;
	
	lowx = lowy = 0;
	bigx = cinTable[currentHandle].xsize;
	bigy = cinTable[currentHandle].ysize;

	if (bigx > cinTable[currentHandle].CIN_WIDTH) bigx = cinTable[currentHandle].CIN_WIDTH;
	if (bigy > cinTable[currentHandle].CIN_HEIGHT) bigy = cinTable[currentHandle].CIN_HEIGHT;

	if ( (startX >= lowx) && (startX+quadSize) <= (bigx) && (startY+quadSize) <= (bigy) && (startY >= lowy) && quadSize <= MAXSIZE) {
		useY = startY;
		scroff = cin.linbuf + (useY+((cinTable[currentHandle].CIN_HEIGHT-bigy)>>1)+yOff)*(cinTable[currentHandle].samplesPerLine) + (((startX+xOff))*cinTable[currentHandle].samplesPerPixel);

		cin.qStatus[0][cinTable[currentHandle].onQuad  ] = scroff;
		cin.qStatus[1][cinTable[currentHandle].onQuad++] = scroff+offset;
	}

	if ( quadSize != MINSIZE ) {
		quadSize >>= 1;
		recurseQuad( startX,		  startY		  , quadSize, xOff, yOff );
		recurseQuad( startX+quadSize, startY		  , quadSize, xOff, yOff );
		recurseQuad( startX,		  startY+quadSize , quadSize, xOff, yOff );
		recurseQuad( startX+quadSize, startY+quadSize , quadSize, xOff, yOff );
	}
}


/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void setupQuad( long xOff, long yOff )
{
	long numQuadCels, i,x,y;
	byte *temp;

	if (xOff == cin.oldXOff && yOff == cin.oldYOff && cinTable[currentHandle].ysize == cin.oldysize && cinTable[currentHandle].xsize == cin.oldxsize) {
		return;
	}

	cin.oldXOff = xOff;
	cin.oldYOff = yOff;
	cin.oldysize = cinTable[currentHandle].ysize;
	cin.oldxsize = cinTable[currentHandle].xsize;

	numQuadCels  = (cinTable[currentHandle].xsize*cinTable[currentHandle].ysize) / (16);
	numQuadCels += numQuadCels/4;
	numQuadCels += 64;							  // for overflow

	cinTable[currentHandle].onQuad = 0;

	for(y=0;y<(long)cinTable[currentHandle].ysize;y+=16) 
		for(x=0;x<(long)cinTable[currentHandle].xsize;x+=16) 
			recurseQuad( x, y, 16, xOff, yOff );

	temp = NULL;

	for(i=(numQuadCels-64);i<numQuadCels;i++) {
		cin.qStatus[0][i] = temp;			  // eoq
		cin.qStatus[1][i] = temp;			  // eoq
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void readQuadInfo( byte *qData )
{
	if (currentHandle < 0) return;

	cinTable[currentHandle].xsize    = qData[0]+qData[1]*256;
	cinTable[currentHandle].ysize    = qData[2]+qData[3]*256;
	cinTable[currentHandle].maxsize  = qData[4]+qData[5]*256;
	cinTable[currentHandle].minsize  = qData[6]+qData[7]*256;
	
	cinTable[currentHandle].CIN_HEIGHT = cinTable[currentHandle].ysize;
	cinTable[currentHandle].CIN_WIDTH  = cinTable[currentHandle].xsize;

	cinTable[currentHandle].samplesPerLine = cinTable[currentHandle].CIN_WIDTH*cinTable[currentHandle].samplesPerPixel;
	cinTable[currentHandle].screenDelta = cinTable[currentHandle].CIN_HEIGHT*cinTable[currentHandle].samplesPerLine;

	cinTable[currentHandle].half = qfalse;
	cinTable[currentHandle].smootheddouble = qfalse;
	
	cinTable[currentHandle].VQ0 = cinTable[currentHandle].VQNormal;
	cinTable[currentHandle].VQ1 = cinTable[currentHandle].VQBuffer;

	cinTable[currentHandle].t[0] = cinTable[currentHandle].screenDelta;
	cinTable[currentHandle].t[1] = -cinTable[currentHandle].screenDelta;

        cinTable[currentHandle].drawX = cinTable[currentHandle].CIN_WIDTH;
        cinTable[currentHandle].drawY = cinTable[currentHandle].CIN_HEIGHT;
        
	// rage pro is very slow at 512 wide textures, voodoo can't do it at all
	if ( cls.glconfig.hardwareType == GLHW_RAGEPRO || cls.glconfig.maxTextureSize <= 256) {
                if (cinTable[currentHandle].drawX>256) {
                        cinTable[currentHandle].drawX = 256;
                }
                if (cinTable[currentHandle].drawY>256) {
                        cinTable[currentHandle].drawY = 256;
                }
		if (cinTable[currentHandle].CIN_WIDTH != 256 || cinTable[currentHandle].CIN_HEIGHT != 256) {
			Com_Printf("HACK: approxmimating cinematic for Rage Pro or Voodoo\n");
		}
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void RoQPrepMcomp( long xoff, long yoff ) 
{
	long i, j, x, y, temp, temp2;

	i=cinTable[currentHandle].samplesPerLine; j=cinTable[currentHandle].samplesPerPixel;
	if ( cinTable[currentHandle].xsize == (cinTable[currentHandle].ysize*4) && !cinTable[currentHandle].half ) { j = j+j; i = i+i; }
	
	for(y=0;y<16;y++) {
		temp2 = (y+yoff-8)*i;
		for(x=0;x<16;x++) {
			temp = (x+xoff-8)*j;
			cin.mcomp[(x*16)+y] = cinTable[currentHandle].normalBuffer0-(temp2+temp);
		}
	}
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void initRoQ( void ) 
{
	if (currentHandle < 0) return;

	cinTable[currentHandle].VQNormal = (void (*)(byte *, void *))blitVQQuad32fs;
	cinTable[currentHandle].VQBuffer = (void (*)(byte *, void *))blitVQQuad32fs;
	cinTable[currentHandle].samplesPerPixel = 4;
	ROQ_GenYUVTables();
	RllSetupTable();
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/
/*
static byte* RoQFetchInterlaced( byte *source ) {
	int x, *src, *dst;

	if (currentHandle < 0) return NULL;

	src = (int *)source;
	dst = (int *)cinTable[currentHandle].buf2;

	for(x=0;x<256*256;x++) {
		*dst = *src;
		dst++; src += 2;
	}
	return cinTable[currentHandle].buf2;
}
*/
static void RoQReset( void ) {
	
	if (currentHandle < 0) return;

	FS_FCloseFile( cinTable[currentHandle].iFile );
	FS_FOpenFileRead (cinTable[currentHandle].fileName, &cinTable[currentHandle].iFile, qtrue, qtrue);
	// let the background thread start reading ahead
	FS_Read (cin.file, 16, cinTable[currentHandle].iFile);
	RoQ_init();
	cinTable[currentHandle].status = FMV_LOOPED;
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void RoQInterrupt(void)
{
	byte				*framedata;
        short		sbuf[32768];
        int		ssize;
        
	if (currentHandle < 0) return;

	FS_Read( cin.file, cinTable[currentHandle].RoQFrameSize+8, cinTable[currentHandle].iFile );
	if ( cinTable[currentHandle].RoQPlayed >= cinTable[currentHandle].ROQSize ) { 
		if (cinTable[currentHandle].holdAtEnd==qfalse) {
			if (cinTable[currentHandle].looping) {
				RoQReset();
			} else {
				cinTable[currentHandle].status = FMV_EOF;
			}
		} else {
			cinTable[currentHandle].status = FMV_IDLE;
		}
		return; 
	}

	framedata = cin.file;
//
// new frame is ready
//
redump:
	switch(cinTable[currentHandle].roq_id) 
	{
		case	ROQ_QUAD_VQ:
			if ((cinTable[currentHandle].numQuads&1)) {
				cinTable[currentHandle].normalBuffer0 = cinTable[currentHandle].t[1];
				RoQPrepMcomp( cinTable[currentHandle].roqF0, cinTable[currentHandle].roqF1 );
				cinTable[currentHandle].VQ1( (byte *)cin.qStatus[1], framedata);
				cinTable[currentHandle].buf = 	cin.linbuf + cinTable[currentHandle].screenDelta;
			} else {
				cinTable[currentHandle].normalBuffer0 = cinTable[currentHandle].t[0];
				RoQPrepMcomp( cinTable[currentHandle].roqF0, cinTable[currentHandle].roqF1 );
				cinTable[currentHandle].VQ0( (byte *)cin.qStatus[0], framedata );
				cinTable[currentHandle].buf = 	cin.linbuf;
			}
			if (cinTable[currentHandle].numQuads == 0) {		// first frame
				Com_Memcpy(cin.linbuf+cinTable[currentHandle].screenDelta, cin.linbuf, cinTable[currentHandle].samplesPerLine*cinTable[currentHandle].ysize);
			}
			cinTable[currentHandle].numQuads++;
			cinTable[currentHandle].dirty = qtrue;
			break;
		case	ROQ_CODEBOOK:
			decodeCodeBook( framedata, (unsigned short)cinTable[currentHandle].roq_flags );
			break;
		case	ZA_SOUND_MONO:
			if (!cinTable[currentHandle].silent) {
				ssize = RllDecodeMonoToStereo( framedata, sbuf, cinTable[currentHandle].RoQFrameSize, 0, (unsigned short)cinTable[currentHandle].roq_flags);
                //                S_RawSamples(0, ssize, 22050, 2, 1, (byte *)sbuf, 1.0f, -1);
			}
			break;
		case	ZA_SOUND_STEREO:
			if (!cinTable[currentHandle].silent) {
				ssize = RllDecodeStereoToStereo( framedata, sbuf, cinTable[currentHandle].RoQFrameSize, 0, (unsigned short)cinTable[currentHandle].roq_flags);
                //                S_RawSamples(0, ssize, 22050, 2, 2, (byte *)sbuf, 1.0f, -1);
			}
			break;
		case	ROQ_QUAD_INFO:
			if (cinTable[currentHandle].numQuads == -1) {
				readQuadInfo( framedata );
				setupQuad( 0, 0 );
				cinTable[currentHandle].startTime = cinTable[currentHandle].lastTime = Com_Milliseconds();
			}
			if (cinTable[currentHandle].numQuads != 1) cinTable[currentHandle].numQuads = 0;
			break;
		case	ROQ_PACKET:
			cinTable[currentHandle].inMemory = cinTable[currentHandle].roq_flags;
			cinTable[currentHandle].RoQFrameSize = 0;           // for header
			break;
		case	ROQ_QUAD_HANG:
			cinTable[currentHandle].RoQFrameSize = 0;
			break;
		case	ROQ_QUAD_JPEG:
			break;
		default:
			cinTable[currentHandle].status = FMV_EOF;
			break;
	}	
//
// read in next frame data
//
	if ( cinTable[currentHandle].RoQPlayed >= cinTable[currentHandle].ROQSize ) { 
		if (cinTable[currentHandle].holdAtEnd==qfalse) {
			if (cinTable[currentHandle].looping) {
				RoQReset();
			} else {
				cinTable[currentHandle].status = FMV_EOF;
			}
		} else {
			cinTable[currentHandle].status = FMV_IDLE;
		}
		return; 
	}
	
	framedata		 += cinTable[currentHandle].RoQFrameSize;
	cinTable[currentHandle].roq_id		 = framedata[0] + framedata[1]*256;
	cinTable[currentHandle].RoQFrameSize = framedata[2] + framedata[3]*256 + framedata[4]*65536;
	cinTable[currentHandle].roq_flags	 = framedata[6] + framedata[7]*256;
	cinTable[currentHandle].roqF0		 = (signed char)framedata[7];
	cinTable[currentHandle].roqF1		 = (signed char)framedata[6];

	if (cinTable[currentHandle].RoQFrameSize>65536||cinTable[currentHandle].roq_id==0x1084) {
		Com_DPrintf("roq_size>65536||roq_id==0x1084\n");
		cinTable[currentHandle].status = FMV_EOF;
		if (cinTable[currentHandle].looping) {
			RoQReset();
		}
		return;
	}
	if (cinTable[currentHandle].inMemory && (cinTable[currentHandle].status != FMV_EOF)) { cinTable[currentHandle].inMemory--; framedata += 8; goto redump; }
//
// one more frame hits the dust
//
//	assert(cinTable[currentHandle].RoQFrameSize <= 65536);
//	r = FS_Read( cin.file, cinTable[currentHandle].RoQFrameSize+8, cinTable[currentHandle].iFile );
	cinTable[currentHandle].RoQPlayed	+= cinTable[currentHandle].RoQFrameSize+8;
}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void RoQ_init( void )
{
	cinTable[currentHandle].startTime = cinTable[currentHandle].lastTime = Com_Milliseconds();
	cinTable[currentHandle].realTime = Com_Milliseconds();
	cinTable[currentHandle].currTime = 0;
	cinTable[currentHandle].soundTime = 0;

	cinTable[currentHandle].RoQPlayed = 24;

/*	get frame rate */	
	cinTable[currentHandle].roqFPS	 = cin.file[ 6] + cin.file[ 7]*256;
	
	if (!cinTable[currentHandle].roqFPS) cinTable[currentHandle].roqFPS = 30;

	cinTable[currentHandle].numQuads = -1;

	cinTable[currentHandle].roq_id		= cin.file[ 8] + cin.file[ 9]*256;
	cinTable[currentHandle].RoQFrameSize	= cin.file[10] + cin.file[11]*256 + cin.file[12]*65536;
	cinTable[currentHandle].roq_flags	= cin.file[14] + cin.file[15]*256;

	if (cinTable[currentHandle].RoQFrameSize > 65536 || !cinTable[currentHandle].RoQFrameSize) { 
		return;
	}

}

/******************************************************************************
*
* Function:		
*
* Description:	
*
******************************************************************************/

static void RoQShutdown( void ) {
	const char *s;

	if (!cinTable[currentHandle].buf) {
		return;
	}

	if ( cinTable[currentHandle].status == FMV_IDLE ) {
		return;
	}
	Com_DPrintf("finished cinematic\n");
	cinTable[currentHandle].status = FMV_IDLE;

	if (cinTable[currentHandle].iFile) {
		FS_FCloseFile( cinTable[currentHandle].iFile );
		cinTable[currentHandle].iFile = 0;
	}

	// HZM coop bug-3234: an overlay slot is released here WITHOUT the fullscreen tail below (the shared movie
	// audio, the clc.state restore, CL_FinishedCinematic's nextmap exec); CL_CoopBV_Stop then sees it gone
	if (cinTable[currentHandle].coopOverlay) {
		cinTable[currentHandle].fileName[0] = 0;
		cinTable[currentHandle].coopOverlay = qfalse;
		currentHandle = -1;
		return;
	}

	S_StopMovieAudio();

	if (cinTable[currentHandle].alterGameState) {
		clc.state = oldClientState;
		// we can't just do a vstr nextmap, because
		// if we are aborting the intro cinematic with
		// a devmap command, nextmap would be valid by
		// the time it was referenced
		s = Cvar_VariableString( "nextmap" );
		if ( s[0] ) {
			Cbuf_ExecuteText( EXEC_APPEND, va("%s\n", s) );
			Cvar_Set( "nextmap", "" );
		}
		CL_handle = -1;
	}

	cinTable[currentHandle].fileName[0] = 0;
	currentHandle = -1;

	CL_FinishedCinematic();
}

static void CL_FinishedCinematic()
{
	const  char* s;

	if (!CL_FinishedIntro())
	{
		CL_FinishedStartStage();
		return;
	}

    s = Cvar_VariableString("nextmap");
    if (*s)
    {
        Cbuf_ExecuteText(EXEC_APPEND, va("%s\n", s));
        Cvar_Set("nextmap", "");
    }
}

/*
==================
CIN_StopCinematic
==================
*/
e_status CIN_StopCinematic(int handle) {
	
	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return FMV_EOF;
	currentHandle = handle;

	Com_DPrintf("trFMV::stop(), closing %s\n", cinTable[currentHandle].fileName);

	if (!cinTable[currentHandle].buf) {
		return FMV_EOF;
	}

	if (cinTable[currentHandle].alterGameState) {
		if ( clc.state != CA_CINEMATIC ) {
			return cinTable[currentHandle].status;
		}
	}
	cinTable[currentHandle].status = FMV_EOF;
	RoQShutdown();

	return FMV_EOF;
}

/*
==================
CIN_RunCinematic

Fetch and decompress the pending frame
==================
*/


e_status CIN_RunCinematic (int handle)
{
	int deltaTime;
	int frameTime;
	int soundTime;
	int newTime;
	long tfps;

	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return FMV_EOF;

	if (cin.currentHandle != handle) {
		currentHandle = handle;
		cin.currentHandle = currentHandle;
		cinTable[currentHandle].status = FMV_EOF;
		RoQReset();
	}

	if (cinTable[handle].playonwalls < -1)
	{
		return cinTable[handle].status;
	}

	currentHandle = handle;

	if (cinTable[currentHandle].alterGameState) {
		if ( clc.state != CA_CINEMATIC ) {
			return cinTable[currentHandle].status;
		}
	}

	if (cinTable[currentHandle].status == FMV_IDLE) {
		return cinTable[currentHandle].status;
	}

	frameTime = Com_Milliseconds();
	deltaTime = frameTime - cinTable[currentHandle].realTime;
	if (deltaTime > 100) {
		deltaTime = 100;
	}

	newTime = cinTable[currentHandle].currTime + deltaTime;
	if (cl_movieaudio->integer) {
		soundTime = S_CurrentMoviePosition();
		if (soundTime > cinTable[currentHandle].currTime || soundTime > cinTable[currentHandle].soundTime) {
			cinTable[currentHandle].currTime = newTime + (deltaTime * (soundTime - newTime)) / 100;
		}
		else {
			cinTable[currentHandle].currTime = newTime;
		}

        cinTable[currentHandle].soundTime = soundTime;
	} else {
		//
		// Fixed in OPM
		//
		cinTable[currentHandle].currTime = newTime;
	}

	cinTable[currentHandle].realTime = frameTime;

	tfps = (cinTable[currentHandle].currTime * 3) / 100;

	while(  (tfps != cinTable[currentHandle].numQuads)
		&& (cinTable[currentHandle].status == FMV_PLAY) ) 
	{
		RoQInterrupt();

		cinTable[currentHandle].realTime = Com_Milliseconds();
		if (cinTable[currentHandle].realTime > frameTime + 66) {
			break;
		}
	}

	cinTable[currentHandle].lastTime = frameTime;

	if (cinTable[currentHandle].status == FMV_LOOPED) {
		cinTable[currentHandle].status = FMV_PLAY;
	}

	if (cinTable[currentHandle].status == FMV_EOF) {
	  if (cinTable[currentHandle].looping) {
		RoQReset();
	  } else {
		RoQShutdown();
		// HZM coop bug-3234: RoQShutdown can leave currentHandle at -1 - never index cinTable[-1]
		return FMV_EOF;
	  }
	}

	return cinTable[currentHandle].status;
}

/*
==================
CIN_PlayCinematic
==================
*/
int CIN_PlayCinematic( const char *arg, int x, int y, int w, int h, int systemBits ) {
	unsigned short RoQID;
	char	name[MAX_OSPATH];
	int		i;

	if (strstr(arg, "/") == NULL && strstr(arg, "\\") == NULL) {
		Com_sprintf (name, sizeof(name), "video/%s", arg);
	} else {
		Com_sprintf (name, sizeof(name), "%s", arg);
	}

	if (!(systemBits & CIN_system)) {
		for ( i = 0 ; i < MAX_VIDEO_HANDLES ; i++ ) {
			if (!strcmp(cinTable[i].fileName, name) ) {
				return i;
			}
		}
	}

	Com_DPrintf("CIN_PlayCinematic( %s )\n", arg);

	Com_Memset(&cin, 0, sizeof(cinematics_t) );
	currentHandle = CIN_HandleForVideo();

	cin.currentHandle = currentHandle;

	Q_strncpyz(cinTable[currentHandle].fileName, name, sizeof(cinTable[currentHandle].fileName));

	cinTable[currentHandle].ROQSize = 0;
	cinTable[currentHandle].ROQSize = FS_FOpenFileRead (cinTable[currentHandle].fileName, &cinTable[currentHandle].iFile, qtrue, qtrue);

	if (cinTable[currentHandle].ROQSize<=0) {
		Com_DPrintf("play(%s), ROQSize<=0\n", arg);
		cinTable[currentHandle].fileName[0] = 0;
		return -1;
	}

	CIN_SetExtents(currentHandle, x, y, w, h);
	CIN_SetLooping(currentHandle, (systemBits & CIN_loop)!=0);

	cinTable[currentHandle].CIN_HEIGHT = DEFAULT_CIN_HEIGHT;
	cinTable[currentHandle].CIN_WIDTH  =  DEFAULT_CIN_WIDTH;
	cinTable[currentHandle].holdAtEnd = (systemBits & CIN_hold) != 0;
	cinTable[currentHandle].alterGameState = (systemBits & CIN_system) != 0;
	cinTable[currentHandle].playonwalls = 1;
	cinTable[currentHandle].silent = (systemBits & CIN_silent) != 0;
	cinTable[currentHandle].shader = (systemBits & CIN_shader) != 0;
	cinTable[currentHandle].coopOverlay = (systemBits & CIN_HZM_COOPOVERLAY) != 0; // HZM coop bug-3234

	if (cinTable[currentHandle].alterGameState) {
		// close the menu
		//uie.SetActiveMenu( UIMENU_NONE );
	} else {
		cinTable[currentHandle].playonwalls = cl_inGameVideo->integer;
	}

	initRoQ();
					
	FS_Read (cin.file, 16, cinTable[currentHandle].iFile);

	RoQID = (unsigned short)(cin.file[0]) + (unsigned short)(cin.file[1])*256;
	if (RoQID == 0x1084)
	{
		RoQ_init();
//		FS_Read (cin.file, cinTable[currentHandle].RoQFrameSize+8, cinTable[currentHandle].iFile);

		cinTable[currentHandle].status = FMV_PLAY;
		Com_DPrintf("trFMV::play(), playing %s\n", arg);

		if (cinTable[currentHandle].alterGameState) {
			clc.state = CA_CINEMATIC;
		}

		return currentHandle;
	}
	Com_DPrintf("trFMV::play(), invalid RoQ ID\n");

	// HZM coop bug-3234: a failed overlay start releases its slot in full here (RoQShutdown would skip it, or do
	// nothing at all before the first frame buffer exists) - never the fullscreen tail
	if (cinTable[currentHandle].coopOverlay) {
		if (cinTable[currentHandle].iFile) {
			FS_FCloseFile(cinTable[currentHandle].iFile);
			cinTable[currentHandle].iFile = 0;
		}
		cinTable[currentHandle].status      = FMV_EOF;
		cinTable[currentHandle].fileName[0] = 0;
		cinTable[currentHandle].coopOverlay = qfalse;
		currentHandle                       = -1;
		return -1;
	}

	RoQShutdown();
	return -1;
}

void CIN_SetExtents (int handle, int x, int y, int w, int h) {
	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;
	cinTable[handle].xpos = x;
	cinTable[handle].ypos = y;
	cinTable[handle].width = w;
	cinTable[handle].height = h;
	cinTable[handle].dirty = qtrue;
}

void CIN_SetLooping(int handle, qboolean loop) {
	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;
	cinTable[handle].looping = loop;
}

/*
==================
CIN_ResampleCinematic

Resample cinematic to 256x256 and store in buf2
==================
*/
void CIN_ResampleCinematic(int handle, int *buf2) {
	int ix, iy, *buf3, xm, ym, ll;
	byte	*buf;

	buf = cinTable[handle].buf;

	xm = cinTable[handle].CIN_WIDTH/256;
	ym = cinTable[handle].CIN_HEIGHT/256;
	ll = 8;
	if (cinTable[handle].CIN_WIDTH==512) {
		ll = 9;
	}

	buf3 = (int*)buf;
	if (xm==2 && ym==2) {
		byte *bc2, *bc3;
		int	ic, iiy;

		bc2 = (byte *)buf2;
		bc3 = (byte *)buf3;
		for (iy = 0; iy<256; iy++) {
			iiy = iy<<12;
			for (ix = 0; ix<2048; ix+=8) {
				for(ic = ix;ic<(ix+4);ic++) {
					*bc2=(bc3[iiy+ic]+bc3[iiy+4+ic]+bc3[iiy+2048+ic]+bc3[iiy+2048+4+ic])>>2;
					bc2++;
				}
			}
		}
	} else if (xm==2 && ym==1) {
		byte *bc2, *bc3;
		int	ic, iiy;

		bc2 = (byte *)buf2;
		bc3 = (byte *)buf3;
		for (iy = 0; iy<256; iy++) {
			iiy = iy<<11;
			for (ix = 0; ix<2048; ix+=8) {
				for(ic = ix;ic<(ix+4);ic++) {
					*bc2=(bc3[iiy+ic]+bc3[iiy+4+ic])>>1;
					bc2++;
				}
			}
		}
	} else {
		for (iy = 0; iy<256; iy++) {
			for (ix = 0; ix<256; ix++) {
					buf2[(iy<<8)+ix] = buf3[((iy*ym)<<ll) + (ix*xm)];
			}
		}
	}
}

/*
==================
CIN_DrawCinematic
==================
*/
void CIN_DrawCinematic (int handle) {
	float	x, y, w, h;
	byte	*buf;

	if (handle < 0 || handle>= MAX_VIDEO_HANDLES || cinTable[handle].status == FMV_EOF) return;

	if (!cinTable[handle].buf) {
		return;
	}

	x = cinTable[handle].xpos;
	y = cinTable[handle].ypos;
	w = cinTable[handle].width;
	h = cinTable[handle].height;
	buf = cinTable[handle].buf;
	SCR_AdjustFrom640( &x, &y, &w, &h );

	if (cinTable[handle].dirty && (cinTable[handle].CIN_WIDTH != cinTable[handle].drawX || cinTable[handle].CIN_HEIGHT != cinTable[handle].drawY)) {
		int *buf2;

		buf2 = ( int * )Hunk_AllocateTempMemory( 256*256*4 );

		CIN_ResampleCinematic(handle, buf2);

		re.DrawStretchRaw( x, y, w, h, 256, 256, 0, (byte *)buf2);
		cinTable[handle].dirty = qfalse;
		Hunk_FreeTempMemory(buf2);
		return;
	}

	re.DrawStretchRaw( x, y, w, h, cinTable[handle].drawX, cinTable[handle].drawY, 0, buf);
	cinTable[handle].dirty = qfalse;
}

static unsigned short CL_PlayRoQ(const char* name, const char* arg, const char* s) {
	unsigned short RoQID;

	Com_Memset(&cin, 0, sizeof(cinematics_t) );
	currentHandle = CIN_HandleForVideo();

	cin.currentHandle = currentHandle;

	Q_strncpyz(cinTable[currentHandle].fileName, name, sizeof(cinTable[currentHandle].fileName));

	cinTable[currentHandle].ROQSize = 0;
	cinTable[currentHandle].ROQSize = FS_FOpenFileRead (cinTable[currentHandle].fileName, &cinTable[currentHandle].iFile, qtrue, qtrue);

	if (cinTable[currentHandle].ROQSize<=0) {
		Com_DPrintf("Can not find RoQ cinematic '%s'\n", name);
		CL_FinishedCinematic();
		return -1;
	}

	if ((s && s[0] == '1') || Q_stricmp(arg,"demoend.roq")==0 || Q_stricmp(arg,"end.roq")==0) {
		cinTable[currentHandle].holdAtEnd = qtrue;
    }

    CIN_SetExtents(currentHandle, 0, 0, 640, 480);
    cinTable[currentHandle].alterGameState = qtrue;

    initRoQ();

	FS_Read (cin.file, 16, cinTable[currentHandle].iFile);

	RoQID = (unsigned short)(cin.file[0]) + (unsigned short)(cin.file[1])*256;
	if (RoQID == 0x1084)
	{
		RoQ_init();
//		FS_Read (cin.file, cinTable[currentHandle].RoQFrameSize+8, cinTable[currentHandle].iFile);

		cinTable[currentHandle].status = FMV_PLAY;
        Com_DPrintf("trFMV::play(), playing %s\n", arg);

        if (cl_movieaudio->integer) {
			audioStartTime = Com_Milliseconds();
            S_SetupMovieAudio(name);
        }

		oldClientState = clc.state;
        clc.state = CA_CINEMATIC;

		if (cls.glconfig.maxTextureSize <= 256 || s && *s == '2') {
			//cinTable[currentHandle].interlaced = qtrue;
		}

		CL_handle = currentHandle;

		do {
			SCR_RunCinematic();
		} while (!cinTable[currentHandle].buf && cinTable[currentHandle].status == FMV_PLAY);

		UI_CloseConsole();

		return currentHandle;
	}
	Com_DPrintf("trFMV::play(), invalid RoQ ID\n");

	RoQShutdown();
	return -1;
}

void CL_PlayCinematic_f(void) {
	char	*arg, *s;
	char	name[256];

	// HZM coop bug-3234: the networked briefing overlay already shows this film - do not also go fullscreen
	if (CL_CoopBriefVideoOwns(Cmd_Argv(1))) {
		Com_Printf("^~^~^ COOPBV ignored fullscreen cinematic %s (overlay running)\n", Cmd_Argv(1));
		return;
	}

	if (clc.state == CA_CINEMATIC) {
		SCR_StopCinematic();
	}

	arg = Cmd_Argv( 1 );
	Com_DPrintf("CL_PlayCinematic( %s )\n", arg);
	s = Cmd_Argv(2);

	CL_StartHunkUsers(qfalse);
    S_StopAllSounds2(qtrue);

	Com_sprintf(name, sizeof(name), "video/%s", arg);

	if (Q_stricmp(&name[strlen(name) - 4], ".roq")) {
		Com_Printf("Bad or missing cinematic extension.\n");
		return;
	}

    CL_PlayRoQ(name, arg, s);
}


void SCR_DrawCinematic (void) {
	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES) {
		CIN_DrawCinematic(CL_handle);
	}
}

void SCR_RunCinematic (void)
{
	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES) {
		CIN_RunCinematic(CL_handle);
	}
}

void SCR_StopCinematic(void) {
	if (CL_handle >= 0 && CL_handle < MAX_VIDEO_HANDLES) {
		CIN_StopCinematic(CL_handle);
	}
}


//=============================================================================================================
// HZM coop bug-3234 - NETWORKED BRIEFING VIDEO (overlay). See docs/proposals/briefing_joiners_2026-09-28.
// The server publishes serverinfo key coop_briefvideo = "<file>,<map>,<startLevelMs>" (mod: stuffsrv "sets ...").
// Every check below must pass or the key is ignored. Nothing here executes server text.
// Security review (2026-09-28) fixes folded in: movie audio via video/<file>; the overlay never runs RoQShutdown's
// fullscreen tail (cin_cache.coopOverlay); starts rate-limited; handle re-validated by file name; audio stopped
// only when the overlay owns it; 64-bit offset math.
//=============================================================================================================
static const char *s_cbvAllow[] = {
    "briefinge1.roq", "briefinge2.roq", "briefinge3.roq", "briefingt1.roq", "briefingt2.roq", "briefingt3.roq"
};
static char     s_cbvKey[96];          // last serverinfo value acted on ("" = none)
static char     s_cbvName[32];         // allowlisted file name now playing
static char     s_cbvPath[MAX_QPATH];  // "video/<name>", as cinTable stores it
static int      s_cbvHandle    = -1;   // cinTable handle of the overlay, -1 = none
static qboolean s_cbvAudio     = qfalse;
static int      s_cbvLastStart = -1000000;
static int      s_cbvStarts    = 0;    // starts since this client last left CA_ACTIVE

#define CBV_START_GAP_MS 3000
#define CBV_MAX_STARTS   8
#define CBV_MAX_OFFSET   (10 * 60 * 1000)

static void CL_CoopBV_Stop(const char *why)
{
    cin_cache *c;

    if (s_cbvHandle < 0) {
        return;
    }
    c = &cinTable[s_cbvHandle];
    Com_Printf("^~^~^ COOPBV stop %s reason=%s audioMs=%d\n", s_cbvName, why, s_cbvAudio ? S_CurrentMoviePosition() : -1);
    // only touch the slot while it is still ours (a restart of the file system can reuse slots/handles)
    if (c->coopOverlay && !Q_stricmp(c->fileName, s_cbvPath)) {
        // RoQShutdown minus its alterGameState / CL_FinishedCinematic ("nextmap") tail
        if (c->iFile) {
            FS_FCloseFile(c->iFile);
            c->iFile = 0;
        }
        c->status      = FMV_EOF;
        c->fileName[0] = 0;
        c->coopOverlay = qfalse;
        if (currentHandle == s_cbvHandle) {
            currentHandle = -1;
        }
        if (cin.currentHandle == s_cbvHandle) {
            cin.currentHandle = -1;
        }
    }
    // the movie channel is shared with the fullscreen player: stop it only if the overlay started it
    if (s_cbvAudio && clc.state != CA_CINEMATIC) {
        S_StopMovieAudio();
    }
    s_cbvAudio   = qfalse;
    s_cbvHandle  = -1;
    s_cbvName[0] = 0;
    s_cbvPath[0] = 0;
}

// allowlisted name for `s` (case-insensitive exact match), or NULL
static const char *CL_CoopBV_Allowed(const char *s)
{
    int i;

    for (i = 0; i < (int)ARRAY_LEN(s_cbvAllow); i++) {
        if (!Q_stricmp(s, s_cbvAllow[i])) {
            return s_cbvAllow[i];
        }
    }
    return NULL;
}

// Validate a coop_briefvideo value. Returns NULL and fills *name (the literal allowlist string) + *startMs when it
// is valid for THIS map, else a short reason. Pure: no side effects, so the fullscreen guard can use it too.
static const char *CL_CoopBV_Parse(const char *v, const char **name, int *startMs)
{
    char        buf[96];
    char       *f[3];
    char       *q;
    int         n, i;
    const char *info, *map;

    if (strlen(v) >= sizeof(buf)) {
        return "toolong";
    }
    Q_strncpyz(buf, v, sizeof(buf));

    n    = 0;
    f[0] = buf;
    for (q = buf; *q; q++) {
        if (*q == ',') {
            if (++n > 2) {
                break;
            }
            *q   = 0;
            f[n] = q + 1;
        }
    }
    if (n != 2) {
        return (!v[0] || !Q_stricmp(v, "off")) ? "cleared" : "malformed";
    }

    *name = CL_CoopBV_Allowed(f[0]);
    if (!*name) {
        return "notallowed";
    }

    if (!cl.gameState.stringOffsets[CS_SERVERINFO]) {
        return "noinfo";
    }
    info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
    map  = Info_ValueForKey(info, "mapname");
    if (!map[0] || Q_stricmp(map, f[1])) {
        return "othermap";
    }

    n = (int)strlen(f[2]);
    if (n < 1 || n > 9) {
        return "badtime";
    }
    for (i = 0; i < n; i++) {
        if (f[2][i] < '0' || f[2][i] > '9') {
            return "badtime";
        }
    }
    *startMs = atoi(f[2]);
    return NULL;
}

// returns qfalse only when a start is deferred by the rate limit (the caller retries on a later frame)
static qboolean CL_CoopBV_Apply(const char *v)
{
    int         startMs, now;
    long long   offset;
    const char *name, *why;
    char        path[MAX_QPATH];

    why = CL_CoopBV_Parse(v, &name, &startMs);
    if (why) {
        CL_CoopBV_Stop(why);
        return qtrue;
    }

    if (s_cbvHandle >= 0 && !Q_stricmp(s_cbvName, name)) {
        return qtrue; // same film already running (a re-published value) - never restart it
    }

    // rate limit: a server flipping the key cannot make this client restart/seek more than once per 3 s,
    // nor more than CBV_MAX_STARTS times per map
    now = Com_Milliseconds();
    if (s_cbvStarts >= CBV_MAX_STARTS) {
        CL_CoopBV_Stop("ratelimit");
        return qtrue;
    }
    if (now - s_cbvLastStart < CBV_START_GAP_MS) {
        return qfalse;
    }
    CL_CoopBV_Stop("replaced");

    Com_sprintf(path, sizeof(path), "video/%s", name);
    if (FS_FOpenFileRead(path, NULL, qfalse, qtrue) <= 0) {
        Com_Printf("^~^~^ COOPBV missing %s - not played\n", path);
        return qtrue;
    }

    offset = (long long)cl.snap.serverTime
           - (long long)atoi(cl.gameState.stringData + cl.gameState.stringOffsets[CS_LEVEL_START_TIME])
           - (long long)startMs;
    if (offset < 1500) {
        offset = 0; // everyone who was there when it started: from the top, together
    }
    if (offset > CBV_MAX_OFFSET) {
        Com_Printf("^~^~^ COOPBV stale %s offset=%lld - not played\n", name, offset);
        return qtrue;
    }

    s_cbvLastStart = now;
    s_cbvStarts++;
    // CIN_HZM_COOPOVERLAY marks the slot inside CIN_PlayCinematic, BEFORE the first RoQ read, so even a corrupt
    // file's failure path releases it without RoQShutdown's fullscreen tail
    s_cbvHandle = CIN_PlayCinematic(name, 0, 0, 640, 480, CIN_hold | CIN_HZM_COOPOVERLAY);
    if (s_cbvHandle < 0) {
        Com_Printf("^~^~^ COOPBV cannot open %s\n", path);
        return qtrue;
    }
    if (!cinTable[s_cbvHandle].coopOverlay) {
        // CIN_PlayCinematic handed back somebody else's slot already playing this file (its non-system de-dupe):
        // never adopt it
        Com_Printf("^~^~^ COOPBV %s busy in another player - not played\n", path);
        s_cbvHandle = -1;
        return qtrue;
    }
    Q_strncpyz(s_cbvName, name, sizeof(s_cbvName));
    Q_strncpyz(s_cbvPath, path, sizeof(s_cbvPath));
    cinTable[s_cbvHandle].currTime    = (long)offset;
    cinTable[s_cbvHandle].soundTime   = 0;

    if (cl_movieaudio->integer) {
        // the same audio lookup as the fullscreen player: video/<name>.mp3, else .wav
        S_SetupMovieAudio(path);
        s_cbvAudio = qtrue;
        if (offset > 0) {
            S_SeekMovieAudio((int)offset);
        }
    }
    Com_Printf(
        "^~^~^ COOPBV start %s offset=%lld start=%d audio=%d audioMs=%d\n",
        name,
        offset,
        startMs,
        cl_movieaudio->integer,
        s_cbvAudio ? S_CurrentMoviePosition() : -1
    );
    return qtrue;
}

/*
==================
CL_CoopBriefVideoFrame - once per client frame (CL_Frame)
==================
*/
void CL_CoopBriefVideoFrame(void)
{
    const char *v = "";
    e_status    st;

    if (clc.state == CA_ACTIVE && !clc.demoplaying && cl.gameState.stringOffsets[CS_SERVERINFO]) {
        v = Info_ValueForKey(cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "coop_briefvideo");
    } else if (clc.state != CA_ACTIVE) {
        s_cbvStarts = 0; // a new map (or connection) gets a fresh start budget
    }
    if (strlen(v) >= sizeof(s_cbvKey)) {
        v = "-"; // over-long: treated as invalid, and stored in a form that compares stably
    }

    // the slot must still be ours (file-system restarts, another player of the same slot)
    if (s_cbvHandle >= 0
        && (!cinTable[s_cbvHandle].coopOverlay || Q_stricmp(cinTable[s_cbvHandle].fileName, s_cbvPath))) {
        CL_CoopBV_Stop("lost");
    }

    if (strcmp(v, s_cbvKey)) {
        if (CL_CoopBV_Apply(v)) {
            Q_strncpyz(s_cbvKey, v, sizeof(s_cbvKey));
        }
    }

    if (s_cbvHandle >= 0) {
        st = CIN_RunCinematic(s_cbvHandle);
        if (st == FMV_IDLE || st == FMV_EOF) {
            CL_CoopBV_Stop("end");
        }
    }
}

/*
==================
CL_CoopBriefVideoDraw - View3D::Draw2D, before fades/letterbox/cgame 2D (screen pixels)
==================
*/
void CL_CoopBriefVideoDraw(float sw, float sh)
{
    static const vec4_t black = {0, 0, 0, 1};
    cin_cache          *c;
    float               x, y, w, h;

    if (s_cbvHandle < 0) {
        return;
    }
    c = &cinTable[s_cbvHandle];
    if (!c->coopOverlay || !c->buf || c->status == FMV_EOF || c->drawX <= 0 || c->drawY <= 0) {
        return;
    }

    // pillar/letter-box the 4:3 film over black
    h = sh;
    w = sh * 4.0f / 3.0f;
    if (w > sw) {
        w = sw;
        h = sw * 3.0f / 4.0f;
    }
    x = (sw - w) * 0.5f;
    y = (sh - h) * 0.5f;

    re.SetColor(black);
    re.DrawBox(0, 0, sw, sh);
    re.SetColor(NULL);

    if (c->dirty && (c->CIN_WIDTH != c->drawX || c->CIN_HEIGHT != c->drawY)) {
        int *buf2 = (int *)Hunk_AllocateTempMemory(256 * 256 * 4);

        CIN_ResampleCinematic(s_cbvHandle, buf2);
        re.DrawStretchRaw(x, y, w, h, 256, 256, 0, (byte *)buf2);
        c->dirty = qfalse;
        Hunk_FreeTempMemory(buf2);
        return;
    }
    re.DrawStretchRaw(x, y, w, h, c->drawX, c->drawY, 0, c->buf);
    c->dirty = qfalse;
}

/*
==================
CL_CoopBriefVideoOwns - true when the overlay shows `arg`, or the server's valid coop_briefvideo names it (the overlay
is then about to start: a listen host that was still loading can run the old-style `cinematic` in the same frame the
key first becomes visible). The old-style fullscreen command is then ignored.
==================
*/
qboolean CL_CoopBriefVideoOwns(const char *arg)
{
    const char *name;
    int         startMs;

    if (!arg) {
        return qfalse;
    }
    if (s_cbvHandle >= 0 && !Q_stricmp(arg, s_cbvName)) {
        return qtrue;
    }
    if (clc.demoplaying || !cl.gameState.stringOffsets[CS_SERVERINFO]) {
        return qfalse;
    }
    if (CL_CoopBV_Parse(
            Info_ValueForKey(cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO], "coop_briefvideo"),
            &name,
            &startMs
        )) {
        return qfalse;
    }
    if (Q_stricmp(arg, name)) {
        return qfalse;
    }
    // defer ONLY when the overlay is actually able to start - otherwise the fullscreen film must play, or the
    // player would see neither (security re-review LOW 1)
    if (s_cbvStarts >= CBV_MAX_STARTS) {
        return qfalse;
    }
    if (FS_FOpenFileRead(va("video/%s", name), NULL, qfalse, qtrue) <= 0) {
        return qfalse;
    }
    if ((long long)cl.snap.serverTime
            - (long long)atoi(cl.gameState.stringData + cl.gameState.stringOffsets[CS_LEVEL_START_TIME])
            - (long long)startMs
        > CBV_MAX_OFFSET) {
        return qfalse;
    }
    return qtrue;
}
