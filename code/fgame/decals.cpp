/*
===========================================================================
Copyright (C) 2015 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// decals.cpp: Decal entities

#include "decals.h"
#include "level.h"

CLASS_DECLARATION( Entity, Decal, NULL )
	{
		{ NULL, NULL }
	};

Decal::Decal
   (
   )

   {
   edict->s.eType = ET_DECAL;
   edict->s.modelindex  = 1;	      // must be non-zero
   PostEvent( EV_Remove, FRAMETIME );
   }

void Decal::setDirection
   (
   Vector dir
   )

   {
   edict->s.surfaces[0] = DirToByte( dir );
   }

void Decal::setShader
   (
   str decal_shader
   )

   {
   str temp_shader;

   shader = decal_shader;
   edict->s.tag_num = gi.imageindex( shader.c_str() );

	temp_shader = shader + ".spr";
	CacheResource( temp_shader );
   }

void Decal::setOrientation
   (
   str deg
   )

   {
   Vector ang;

   if ( !deg.icmp( "random" ) )
      ang[2] = random() * 360;
   else
      ang[2] = atof( deg );   

   setAngles( ang );
   }

void Decal::setRadius
   (
   float rad
   )

   {
   edict->s.scale = rad;
   }

// HZM coop - tint the decal. The client (CG_EntityEffects) unpacks s.constantLight as r=byte0, g=byte1,
// b=byte2, intensity=byte3, and modulates the mark by that color (MIN with the white default). Without this
// the mark renders with the default WHITE modulate, so a grayscale splat (bloodsplat.spr) shows up white -
// which is exactly the "white squares" the player blood trail left. Intensity byte = 0 so it casts NO light.
void Decal::setColor
   (
   float r,
   float g,
   float b
   )

   {
   int ir, ig, ib;

   if ( r < 0.0f ) r = 0.0f; else if ( r > 1.0f ) r = 1.0f;
   if ( g < 0.0f ) g = 0.0f; else if ( g > 1.0f ) g = 1.0f;
   if ( b < 0.0f ) b = 0.0f; else if ( b > 1.0f ) b = 1.0f;

   ir = (int)( r * 255.0f );
   ig = (int)( g * 255.0f );
   ib = (int)( b * 255.0f );
   if ( ir < 1 ) ir = 1; // keep constantLight non-zero (0 would read as black) and != 0xffffff

   edict->s.constantLight = ir | ( ig << 8 ) | ( ib << 16 ); // high byte (intensity) = 0 -> no dynamic light
   }

