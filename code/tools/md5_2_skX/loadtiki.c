/*
===========================================================================
Copyright (C) 2012 su44

This file is part of md5_2_skX source code.

md5_2_skX source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

md5_2_skX source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with md5_2_skX source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// loadtiki.c - loading MoHAA .tik, .skc and .skd files.

#include "md5_2_skX.h"

int getBoneIndex(tModel_t *m, const char *s) {
	int i;

	if(!Q_stricmp(s,"worldbone")) {
		return -1;
	}
	for(i = 0; i < m->numBones; i++) {
		if(!Q_stricmp(m->bones[i].name,s)) {
			return i;
		}
	}
	T_Printf("Warning: cant find bone %s in %s\n",s,m->fname);
	return -1;
}
tModel_t *readSKD(const char *fname, float scale) {
	int len;
	skdHeader_t *h;
	skdSurface_t *sf;
	skdBone_t *b;
	tBone_t *ob;
	tModel_t *out;
	tSurf_t *os;
	int i, j, k;

	T_Printf("Loading MoHAA skd model file %s...\n",fname);

	len = F_LoadBuf(fname,(byte**)&h);

	if(len == -1) {
		T_Printf("readSKD: Cannot open %s\n",fname);
		return 0;
	}

	out = T_Malloc(sizeof(tModel_t));

	strcpy(out->fname,fname);

	out->numSurfaces = h->numSurfaces;
	out->numBones = h->numBones;
	if(out->numBones > 512) {
		T_Error("readSKD: too many bones (%i) in %s\n",out->numBones,fname);
	}
	out->surfs = T_Malloc(sizeof(tSurf_t)*out->numSurfaces);
	out->bones = T_Malloc(sizeof(tBone_t)*out->numBones);

	// load bones
	// HZM coop 2026-07-06: all joint types are read now (human rigs use
	// ROTATION/IKSHOULDER/IKELBOW/IKWRIST/HOSEROT/AVROT besides POSROT).
	// Per-type base data + bone refs are kept on tBone_t; appendSKC derives
	// bind poses / anim channels from them. See skx_format.h for the layouts.

	b = (skdBone_t *) ( (byte *)h + h->ofsBones );
	ob = out->bones;
	for ( i = 0; i < h->numBones; i++, ob++) {
		int numRefs, n;
		const char *refName;

		strcpy(ob->name,b->name);
		if(b->jointType < 0 || b->jointType >= JT_NUMJOINTTYPES) {
			T_Error("readSKD: unknown joint type %i on bone %s (skd file %s)\n",
				b->jointType,b->name,fname);
		}
		ob->jointType = b->jointType;

		// raw base data floats (layout depends on type - see skx_format.h)
		n = (b->ofsChannels - b->ofsValues) / 4;
		if(n < 0) n = 0;
		if(n > SKX_MAX_BONE_BASEDATA) n = SKX_MAX_BONE_BASEDATA;
		ob->numBaseData = n;
		memcpy(ob->baseData,(byte *)b + b->ofsValues,n * sizeof(float));

		// hoserot variant flag - an int at ofsBaseData+36
		// (the engine reads it via fileData->parent[fileData->ofsBaseData + 4])
		ob->hoseRotType = HRTYPE_PLAIN;
		if(ob->jointType == JT_HOSEROT && (b->ofsChannels - b->ofsValues) >= 40) {
			ob->hoseRotType = *(int *)((byte *)b + b->ofsValues + 36);
		}

		// bone reference names (resolved to indexes in the second pass)
		switch(ob->jointType) {
		case JT_AVROT:
			numRefs = 2;
			break;
		case JT_IKELBOW:
		case JT_IKWRIST:
		case JT_HOSEROT:
			numRefs = 1;
			break;
		default:
			numRefs = 0;
			break;
		}
		ob->numRefs = numRefs;
		ob->refIndex[0] = ob->refIndex[1] = -1;
		refName = (const char *)b + b->ofsRefs;
		for(j = 0; j < numRefs; j++) {
			if(refName >= (const char *)b + b->ofsEnd) {
				T_Printf("Warning: bone %s in %s is missing ref name %i\n",ob->name,fname,j);
				ob->refName[j][0] = 0;
				continue;
			}
			Q_strncpyz(ob->refName[j],refName,sizeof(ob->refName[j]));
			refName += strlen(refName) + 1;
		}

		b = (skdBone_t *)( (byte *)b + b->ofsEnd );
	}

	b = (skdBone_t *) ( (byte *)h + h->ofsBones );
	ob = out->bones;
	for ( i = 0; i < h->numBones; i++, ob++) {
		ob->parent = getBoneIndex(out,b->parent);
		if(ob->parent >= i && ob->parent != -1) {
			// md5AnimateBones and the md5 format itself assume parents-first
			// ordering; every vanilla skd satisfies this.
			T_Printf("Warning: bone %s (%i) has forward parent %s (%i) in %s\n",
				ob->name,i,b->parent,ob->parent,fname);
		}
		// IK goal bones (wrist/foot): their skc channels hold the MODEL-SPACE
		// IK target, not a parent-relative transform. Reparent them to
		// worldbone so their md5 "local" data == model space. This keeps the
		// decompiled animation correct in Blender AND lets the type-blind
		// recompile path write channels the engine interprets correctly
		// (the shipped skd keeps the original IKWRIST hierarchy - only the
		// md5 working copy is flattened).
		if(ob->jointType == JT_IKWRIST && ob->parent != -1) {
			T_Printf("note: IK goal bone %s reparented from %s to worldbone for md5\n",
				ob->name,b->parent);
			ob->parent = -1;
		}
		for(j = 0; j < ob->numRefs; j++) {
			if(ob->refName[j][0]) {
				ob->refIndex[j] = getBoneIndex(out,ob->refName[j]);
			}
		}
		b = (skdBone_t *)( (byte *)b + b->ofsEnd );
	}

	// load surfaces
	sf = (skdSurface_t *) ( (byte *)h + h->ofsSurfaces );
	os = out->surfs;
	for ( i = 0; i < h->numSurfaces; i++,os++) {
		skdVertex_t *v;
		skdTriangle_t *t;
		tVert_t *ov;

		strcpy(os->name,sf->name);
		os->numTris = sf->numTriangles;
		os->numVerts = sf->numVerts;
		os->tris = T_Malloc(sizeof(tTri_t)*sf->numTriangles);
		os->verts = T_Malloc(sizeof(tVert_t)*sf->numVerts);

		// copy triangles
		t = (skdTriangle_t *) ( (byte *)sf + sf->ofsTriangles );
		assert(sizeof(skdTriangle_t) == sizeof(tTri_t));
		memcpy(os->tris,t,sizeof(skdTriangle_t)*sf->numTriangles);

		// copy and scale vertices data
		v = (skdVertex_t *) ( (byte *)sf + sf->ofsVerts );
		ov = os->verts;
		for ( j = 0; j < sf->numVerts; j++,ov++) {
			skdWeight_t *w;
			tWeight_t *ow;
	
			ov->numWeights = v->numWeights;
			VectorCopy(v->normal,ov->normal);
			ov->texCoords[0] = v->texCoords[0];
			ov->texCoords[1] = v->texCoords[1];
			ov->weights = T_Malloc(sizeof(tWeight_t)*ov->numWeights);

			w = (skdWeight_t *) ( (byte *)v + sizeof(skdVertex_t)+(sizeof(skdMorph_t)*v->numMorphs));
			ow = ov->weights;
			for ( k = 0; k < v->numWeights; k++,ow++) {
				VectorScale(w->offset,scale,ow->offset);
				if(w->boneWeight<0 || w->boneWeight>1) {
					T_Error("invalid bone weight %f in %s \n",w->boneWeight,out->fname);
				}
				ow->boneWeight = w->boneWeight;
				ow->boneNum = w->boneIndex;
				w = (skdWeight_t *) ( (byte *)w + sizeof(skdWeight_t));
			}
			v = (skdVertex_t *) ( (byte *)v + sizeof(skdVertex_t) + v->numWeights*sizeof(skdWeight_t) + v->numMorphs*sizeof(skdMorph_t) );
		}
		sf = (skdSurface_t *)( (byte *)sf + sf->ofsEnd );
	}

	F_FreeBuf((byte*)h);

	T_Printf("Succesfully loaded MoHAA model %s\n",fname);

	return out;
}
int getChannelIndexInternal(skcHeader_t *h, const char *channelName) {
	const char *c;
	int i;

	c = ( (byte *)h + h->ofsChannels );
	for(i = 0; i < h->numChannels; i++) {
		if(!Q_stricmp(c,channelName)) 
			return i;
		c += SKC_MAX_CHANNEL_CHARS;
	}

	return -1;
}
static vec4_t vecBuf[512];
static int vecIndex = 0;
float *getChannelValue(skcHeader_t *h, const char *name, int frameNum) {

	int channelIndex;
	skcFrame_t *f;
	float *values;

	channelIndex = getChannelIndexInternal(h,name);

	if(channelIndex == -1)
		return 0;

	f = (skcFrame_t *)( (byte *)h + sizeof(*h) + sizeof(*f) * frameNum );

	values = (float*)((byte *)h+f->ofsValues);

	values += (4 * channelIndex);

	vecIndex++;
	if(vecIndex >= sizeof(vecBuf) / sizeof(vecBuf[0])) {
		vecIndex = 0;
	}

	QuatCopy(values, vecBuf[vecIndex]);
	return vecBuf[vecIndex];
}
float *findPosChannel(skcHeader_t *h, const char *name, int frameNum) {
	char channelName[32];
	strcpy(channelName,name);
	strcat(channelName," pos");
	return getChannelValue(h,channelName,frameNum);
}
float *findRotChannel_raw(skcHeader_t *h, const char *name, int frameNum) {
	char channelName[32];
	strcpy(channelName,name);
	strcat(channelName," rot");
	return getChannelValue(h,channelName,frameNum);
}
// HZM coop 2026-07-06: generalized so the IK bones can read their baked-FK
// helper channels ("<name> rotFK") through the exact same processing path.
// Returns 0 when the channel does not exist in the skc.
float *findRotChannelSuffix(skcHeader_t *h, const char *name, const char *suffix, int frameNum) {
static int i = 0;
	static quat_t qs[1024];
	float *q;
	char channelName[40];
	float *f;
	float len;

	strcpy(channelName,name);
	strcat(channelName,suffix);
	f = getChannelValue(h,channelName,frameNum);
	if(f == 0) {
		return 0;
	}

	i++;
	i %= 1024;
	q = qs[i];

	QuatCopy(f,q);
	len = QuatNormalize(q);
	if(abs(len-1.f) > 0.1) {
		T_Error("Non-normalized quat in skc file (%f)\n",len);
	}
	FixQuatForMD5_P(q);
	return q;
}
float *findRotChannel(skcHeader_t *h, const char *name, int frameNum, int parentIndex) {
static int i = 0;
	static quat_t qs[1024];
	float *q;
	float *f;

	f = findRotChannelSuffix(h,name," rot",frameNum);
	if(f) {
		return f;
	}
	// missing channel: identity (matches the engine's SKELBONE_ZERO fallback)
	i++;
	i %= 1024;
	q = qs[i];
	QuatSet(q,0,0,0,-1);
	FixQuatForMD5_P(q);
	return q;
}

/*
====================================================================
HZM coop 2026-07-06 - joint-type aware channel/bind-pose helpers.

Which skc channels drive which bone type (ground truth:
skeletorbones.cpp GetDirtyTransform per class + GetNumChannels):

  JT_POSROT     "<n> pos" + "<n> rot"  - local, as before
  JT_ROTATION   "<n> rot"              - local; POSITION is static skd base data
  JT_IKWRIST    "<n> pos" + "<n> rot"  - MODEL-SPACE IK goal (bone is reparented
                                         to worldbone by readSKD, so treating the
                                         channels as local stays correct)
  JT_IKSHOULDER "<n> rotFK"            - baked local FK rot (engine ignores it,
                                         the IK solver recomputes it at runtime);
                                         position static base[4..6]
  JT_IKELBOW    "<n> rotFK"            - baked local FK rot; position static base[0..2]
  JT_HOSEROT/JT_AVROT/JT_ZERO  none    - fully procedural; static approximation

Verified vs vanilla data: composing Pelvis->Thigh(rotFK)->Calf(rotFK) with the
static base offsets reproduces the "Foot pos" goal channel to ~1e-5 on every
frame of alert_sprint.skc for both legs.
====================================================================
*/

// type-aware "does this bone have a pos channel, and what is its value"
static float *bonePosChannel(tModel_t *m, int boneNum, skcHeader_t *h, int frameNum) {
	tBone_t *b = m->bones + boneNum;
	switch(b->jointType) {
	case JT_POSROT:
	case JT_IKWRIST:
		return findPosChannel(h,b->name,frameNum);
	default:
		return 0;
	}
}

// type-aware rot channel; returns 0 when the bone's rotation is not animated
static float *boneRotChannel(tModel_t *m, int boneNum, skcHeader_t *h, int frameNum) {
	tBone_t *b = m->bones + boneNum;
	switch(b->jointType) {
	case JT_POSROT:
	case JT_ROTATION:
	case JT_IKWRIST:
		return findRotChannelSuffix(h,b->name," rot",frameNum);
	case JT_IKSHOULDER:
	case JT_IKELBOW:
		// baked FK helper channels written by the original exporter
		return findRotChannelSuffix(h,b->name," rotFK",frameNum);
	default:
		return 0;
	}
}

// static (non-animated) local transform of a bone, derived from its skd base
// data. Used for every piece a bone has no channel for. Rotations are in the
// same convention findRotChannelSuffix returns (normalized, W >= 0).
static void boneStaticLocal(tModel_t *m, int boneNum, float scale, bone_t *out) {
	tBone_t *b = m->bones + boneNum;

	VectorSet(out->p,0,0,0);
	QuatSet(out->q,0,0,0,1);

	switch(b->jointType) {
	case JT_ROTATION:
		// base data [0..2] = offset from parent (rotation comes from channel)
		if(b->numBaseData >= 3) {
			VectorScale(b->baseData,scale,out->p);
		}
		break;
	case JT_IKSHOULDER:
		// base data [0..3] = bind orientation quat, [4..6] = offset from parent
		if(b->numBaseData >= 7) {
			VectorScale(b->baseData + 4,scale,out->p);
			QuatCopy(b->baseData,out->q);
			QuatNormalize(out->q);
			FixQuatForMD5_P(out->q);
		}
		break;
	case JT_IKELBOW:
	case JT_IKWRIST:
		// base data [0..2] = bind offset from parent, no bind rotation
		if(b->numBaseData >= 3) {
			VectorScale(b->baseData,scale,out->p);
		}
		break;
	case JT_HOSEROT:
		// base data [3..5] = offset. The 180y variants evaluate against the
		// parent frame with X/Z axes inverted, which for a small bind bend is
		// a plain 180 deg rotation about local Y (and the loader's basePos
		// x/z negation cancels against it - file offset applies as-is).
		if(b->numBaseData >= 6) {
			VectorScale(b->baseData + 3,scale,out->p);
		}
		if(b->hoseRotType != HRTYPE_PLAIN) {
			QuatSet(out->q,0,1,0,0);
		}
		break;
	case JT_AVROT:
		// base data [0] = slerp weight, [1..3] = offset from parent.
		// Rotation is refined from the refs by buildFrame0Locals.
		if(b->numBaseData >= 4) {
			VectorScale(b->baseData + 1,scale,out->p);
		}
		break;
	default:
		// JT_POSROT with missing channels / JT_ZERO: identity at parent -
		// same as the engine's SKELBONE_ZERO behavior.
		break;
	}
}

// Builds the frame-0 LOCAL pose for every bone (channels where available,
// static base data otherwise) and refines JT_AVROT rotations to
// slerp(ref1, ref2, weight) so their bind orientation matches the engine.
static void buildFrame0Locals(tModel_t *m, skcHeader_t *h, float scale, bone_t *f0) {
	static bone_t w[512];
	int i;

	for(i = 0; i < m->numBones; i++) {
		float *p, *q;

		boneStaticLocal(m,i,scale,&f0[i]);

		p = bonePosChannel(m,i,h,0);
		if(p) {
			VectorScale(p,scale,f0[i].p);
		}
		q = boneRotChannel(m,i,h,0);
		if(q) {
			QuatCopy(q,f0[i].q);
		}
	}

	// world transforms with the identity-rotation avrot placeholders
	memcpy(w,f0,sizeof(bone_t)*m->numBones);
	md5AnimateBones(m,w);

	// refine avrot bones: world rotation = slerp of the two referenced bones'
	// world rotations. Recovered as a local rotation by inverting the exact
	// composition md5AnimateBones performs:
	//   world.q = QuaternionMultiply(inv(local.q), parentWorld.q)
	//   => local.q = inv( QuaternionMultiply(world.q's inverse-side...) )
	// i.e. local.q = inv(world.q) (x) parentWorld.q
	for(i = 0; i < m->numBones; i++) {
		tBone_t *b = m->bones + i;
		quat_t wq, invw;
		float weight;

		if(b->jointType != JT_AVROT) {
			continue;
		}
		if(b->refIndex[0] < 0 || b->refIndex[1] < 0 ||
		   b->refIndex[0] >= m->numBones || b->refIndex[1] >= m->numBones) {
			T_Printf("Warning: avrot bone %s has unresolved refs, keeping identity\n",b->name);
			continue;
		}
		weight = (b->numBaseData >= 1) ? b->baseData[0] : 0.5f;

		QuatSlerp(w[b->refIndex[0]].q,w[b->refIndex[1]].q,weight,wq);
		QuatNormalize(wq);
		// keep the refined world rot available for later avrot bones/children
		QuatCopy(wq,w[i].q);

		QuatCopy(wq,invw);
		QuatInverse(invw);
		if(b->parent == -1) {
			// root: world.q = inv(local.q)
			QuatCopy(invw,f0[i].q);
		} else {
			// local.q = inv(world.q) (x) parentWorld.q
			// (QuaternionMultiply(out,first,second) computes second (x) first)
			QuaternionMultiply(f0[i].q,w[b->parent].q,invw);
		}
		QuatNormalize(f0[i].q);
		FixQuatForMD5_P(f0[i].q);
	}
}
tAnim_t *appendSKC(tModel_t *m, const char *fname, float scale) {
	int len;
	skcHeader_t *h;
	skcFrame_t *f; //, *firstFrame;
	tAnim_t *out;
	tFrame_t *of;
//	const char *c;
	int i, j;
	int cFlags[512];
	bone_t baseFrame[512];
	static bone_t f0locals[512];
	int numAnimatedComponents;

	T_Printf("Loading MoHAA skc animation file %s...\n",fname);

	len = F_LoadBuf(fname,(byte**)&h);

	if(len == -1) {
		T_Printf("Cannot open %s\n",fname);
		return 0;
	}

	memset(cFlags,0,sizeof(cFlags));

	// HZM coop 2026-07-06: frame-0 local pose for every bone, including the
	// static/procedural joint types that have no channels in the skc.
	buildFrame0Locals(m,h,scale,f0locals);

	out = T_Malloc(sizeof(tAnim_t));
	out->frameRate = 1.f / h->frameTime;
	out->numBones = m->numBones;
	out->numFrames = h->numFrames;
	out->frames = T_Malloc(sizeof(tFrame_t)*h->numFrames);
	out->boneData = T_Malloc(sizeof(tAnimBone_t)*m->numBones);

	// copy frame bounding boxes
	f = (skcFrame_t *)( (byte *)h + sizeof(*h) );
	of = out->frames;
	for(i = 0; i < h->numFrames; i++,of++,f++) {
		//anim->frames[i].radius = f->radius;
		VectorCopy(f->bounds[1],of->maxs);
		VectorCopy(f->bounds[0],of->mins);
	}

	// detect which components changes
	// HZM coop 2026-07-06: channel lookups are joint-type aware now; bones
	// whose type has no pos/rot channel fall back to their static local pose.
	for(j = 0; j < m->numBones; j++) {
		float *baseRot, *testRot;
		float *basePos, *testPos;

		basePos = bonePosChannel(m,j,h,0);
		if(basePos == 0) {
			VectorCopy(f0locals[j].p,baseFrame[j].p);
		} else {
			VectorScale(basePos,scale,basePos);
			VectorCopy(basePos,baseFrame[j].p);
			for(i = 1; i < h->numFrames; i++) {
				testPos = bonePosChannel(m,j,h,i);
				VectorScale(testPos,scale,testPos);
				// detect X change
				if(testPos[0] != basePos[0]) {
					cFlags[j] |= COMPONENT_BIT_TX;
				}
				// detect Y change
				if(testPos[1] != basePos[1]) {
					cFlags[j] |= COMPONENT_BIT_TY;
				}
				// detect Z change
				if(testPos[2] != basePos[2]) {
					cFlags[j] |= COMPONENT_BIT_TZ;
				}
			}
		}

		baseRot = boneRotChannel(m,j,h,0);
		if(baseRot == 0) {
			QuatCopy(f0locals[j].q,baseFrame[j].q);
		} else {
			QuatCopy(baseRot,baseFrame[j].q);
			for(i = 1; i < h->numFrames; i++) {
				testRot = boneRotChannel(m,j,h,i);
				if(testRot == 0) {
					continue;
				}
				// detect X change
				if(testRot[0] != baseRot[0]) {
					cFlags[j] |= COMPONENT_BIT_QX;
				}
				// detect Y change
				if(testRot[1] != baseRot[1]) {
					cFlags[j] |= COMPONENT_BIT_QY;
				}
				// detect Z change
				if(testRot[2] != baseRot[2]) {
					cFlags[j] |= COMPONENT_BIT_QZ;
				}
				// NOTE: quaternion W component is not stored at all in md5 files
			}
		}
	}

	// count the number of animated components and copy some bone data
	numAnimatedComponents = 0;
	for(j = 0; j < m->numBones; j++) {
		//int c;

		out->boneData[j].firstComponent = numAnimatedComponents;
		//c = 0;

		for(i = 0; i < 6; i++) {
			if(cFlags[j] & (1 << i)) {
				numAnimatedComponents++;
			//	c++;
			}
		}

		//out->boneData[j].numAnimatedComponents = c;
		out->boneData[j].componentBits = cFlags[j];
		strcpy(out->boneData[j].name,m->bones[j].name);
		out->boneData[j].parent = m->bones[j].parent;
	}

	// copy results out
	out->baseFrame = T_Malloc(sizeof(bone_t)*m->numBones);
	memcpy(out->baseFrame,baseFrame,sizeof(bone_t)*m->numBones);
	out->numAnimatedComponents = numAnimatedComponents;
	of = out->frames;
	for(i = 0; i < h->numFrames; i++,of++) {
		int c;
		float *cp;

		cp = of->components = T_Malloc(numAnimatedComponents*sizeof(float));

		//c = 0;
		for(j = 0; j < m->numBones; j++) {
			float *pos, *rot;

			pos = bonePosChannel(m,j,h,i);
			if(pos) {
				VectorScale(pos,scale,pos);
				// write X change
				if(cFlags[j] & COMPONENT_BIT_TX) {
					*cp = pos[0];
					cp++;
				}
				// write Y change
				if(cFlags[j] & COMPONENT_BIT_TY) {
					*cp = pos[1];
					cp++;
				}
				// write Z change
				if(cFlags[j] & COMPONENT_BIT_TZ) {
					*cp = pos[2];
					cp++;
				}
			}

			rot = boneRotChannel(m,j,h,i);
			if(rot) {
				// write X change
				if(cFlags[j] & COMPONENT_BIT_QX) {
					*cp = rot[0];
					cp++;
				}
				// write Y change
				if(cFlags[j] & COMPONENT_BIT_QY) {
					*cp = rot[1];
					cp++;
				}
				// write Z change
				if(cFlags[j] & COMPONENT_BIT_QZ) {
					*cp = rot[2];
					cp++;
				}	
			}
		}

		c = cp - of->components;
		assert(c == numAnimatedComponents);
	}

#if 0
	// validate generated tAnim_t components
	for(i = 0; i < out->numFrames; i++) {
		bone_t *b = setupMD5AnimBones(out,0); 
		for(j = 0; j < m->numBones; j++, b++) {
			float *o;
			o = findRotChannel_raw(h,m->bones[j].name,i,m->bones[j].parent);
			if(o) {
				T_Printf("Generated: %f %f %f %f, original %f %f %f %f\n",b->q[0],b->q[1],b->q[2],b->q[3],
					o[0],o[1],o[2],o[3]);
			} else {
				T_Printf("Generated: %f %f %f %f, original <none>\n",b->q[0],b->q[1],b->q[2],b->q[3]);
			}

			
		}
	}

#endif

	// generate baseFrame, but only once,
	// from the first appended SKC
	if(m->baseFrame == 0) {
		// HZM coop 2026-07-06: frame-0 locals already carry the joint-type
		// aware static poses (and the avrot slerp refinement), so the mesh
		// bind pose is just their hierarchical composition.
		bone_t b[512];
		memcpy(b,f0locals,sizeof(bone_t)*m->numBones);
		md5AnimateBones(m,b);
		m->baseFrame = T_Malloc(m->numBones*sizeof(bone_t));
		memcpy(m->baseFrame,b,m->numBones*sizeof(bone_t));
	}

	F_FreeBuf((byte*)h);

	T_Printf("Succesfully loaded MoHAA animation %s\n",fname);


	return out;
}

// TIKI loading.
// For the purpose of this exporter,
// we need to load setup and animation section.
// Include keywords are currently ignored.

const char *fixPath(const char *fname, const char *path, const char *tikiFilePath) {
	static char tmp[MAX_TOOLPATH];
	static char tmp2[MAX_TOOLPATH];
	const char *main, *p, *models;
	int l;

	if(F_Exists(fname)) {
		return fname;
	}
	strcpy(tmp,path);
	backSlashesToSlashes(tmp);
	if(tmp[strlen(tmp)-1] != '/') {
		strcat(tmp,"/");	
	}
	strcat(tmp,fname);
	if(F_Exists(fname)) {
		return tmp;
	}
	// try to extract path to MoHAA's main/mainta/maintt directory
	main = strstr(tikiFilePath,"main");
	if(main) {
		p = strchr(main,'/');
		l = (p-tikiFilePath)+1;
		memcpy(tmp2,tikiFilePath,l);
		tmp2[l] = 0;
		strcat(tmp2,tmp);
		if(F_Exists(tmp2)) {
			return tmp2;
		}
	}
	// if everything else fail, try to extract "models" path
	models = strstr(tikiFilePath,"models/");
	if(main) {
		l = (models-tikiFilePath)+strlen("models/");
		strncpy(tmp2,tikiFilePath,l);
		tmp2[l] = 0;
		p = strstr(tmp,"models/");
		if(p) {
			strcat(tmp2,p+strlen("models/"));
		} else {
			strcat(tmp2,tmp);
		}
		if(F_Exists(tmp2)) {
			return tmp2;
		}
	}
	return tmp;
}
void loadTIKI(const char *fname) {
	int len;
	char *txt;
	char *p;
	const char *fixedPath;
	const char *token;
	char path[MAX_TOOLPATH];
	float scale;

	len = F_LoadBuf(fname,(byte**)&txt);

	if(len == -1) {
		T_Error("loadTIKI: Cannot open %s\n",fname);
		return;
	}

	path[0] = 0;
	scale = 1.f;

	// NOTE: this will not open the "fname" file!
	COM_BeginParseSession(fname);

	p = txt;
	token = COM_ParseExt(&p, qtrue);
	while(token[0]) {
		if (!Q_stricmp(token, "path") || !Q_stricmp(token, "$path")) {
			token = COM_ParseExt(&p, qtrue);
			strcpy(path,token);
		} else if (!Q_stricmp(token, "scale")) {
			token = COM_ParseExt(&p, qtrue);
			scale = atof(token);
		} else if (!Q_stricmp(token, "skelmodel")) {
			token = COM_ParseExt(&p, qtrue);
			mainModel = readSKD(fixPath(token,path,fname),scale);
		} else if(strstr(token,".skc")) {
			tAnim_t *a;
			fixedPath = fixPath(token,path,fname);
			a = appendSKC(mainModel,fixedPath,scale);
			if(a) {
				strcpy(inAnimFNames[numAnims],fixedPath);
				anims[numAnims] = a;
				numAnims++;
			}
		}
		token = COM_ParseExt(&p, qtrue);
	}

	F_FreeBuf(txt);
}
























