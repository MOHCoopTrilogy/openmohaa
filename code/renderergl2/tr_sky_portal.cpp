/*
===========================================================================
Copyright (C) 2023 the OpenMoHAA team

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

// tr_sky_portal.cpp: sky portal

#include "tr_local.h"

/*
=================
R_Sky_Init

=================
*/
void R_Sky_Init()
{
    tr.viewParms.isPortalSky = qfalse;
    tr.portalsky.numSurfs    = 0;
}

/*
=================
R_Sky_Reset

=================
*/
void R_Sky_Reset()
{
    if (tr.viewParms.isPortalSky) {
        return;
    }

    tr.portalsky.mins[0]  = 8192.0;
    tr.portalsky.mins[1]  = 8192.0;
    tr.portalsky.mins[2]  = 8192.0;
    tr.portalsky.maxs[0]  = -8192.0;
    tr.portalsky.maxs[1]  = -8192.0;
    tr.portalsky.maxs[2]  = -8192.0;
    tr.portalsky.cntNode  = 0;
    tr.portalsky.numSurfs = 0;
}

/*
=================
R_Sky_AddSurf

=================
*/
void R_Sky_AddSurf(msurface_t *surf)
{
    if (tr.viewParms.isPortalSky) {
        static int last_sky_warning = 0;

        if (tr.refdef.time - 1000 > last_sky_warning) {
            ri.Printf(3, "WARNING: sky being drawn in a sky portal!  Bad!  Bad!\n");
            last_sky_warning = tr.refdef.time;
        }

        return;
    }

    // HZM [user 08-02]: REVERTED an SF_FACE-only filter that briefly lived here.
    // It was added as belt-and-braces while chasing the t3l1/t3l2 crash, on the theory that a
    // wrongly-typed surface was reaching SurfIsOffscreen2. That theory was WRONG - the real cause
    // was a 16-vs-32-bit index type mismatch in SurfIsOffscreen2 itself (renderergl2 typedefs
    // glIndex_t as unsigned short; the loop read it through an `unsigned*`). That is fixed at the
    // source in tr_main.c, and the bounds guard there remains as the safety net.
    // The filter was not merely redundant, it was HARMFUL: SF_GRID and SF_TRIANGLES are perfectly
    // valid sky surfaces (a curved or mesh sky brush), and dropping them left holes in the 3D
    // skybox - reported as the sky looking pixelated/broken. gl1, which renders this correctly,
    // does no type filtering here at all. Collect everything, exactly like gl1.
    if (tr.portalsky.numSurfs < 32) {
        tr.portalsky.skySurfs[tr.portalsky.numSurfs++] = surf;
    }

    if (tr.portalsky.cntNode) {
        int i;

        for (i = 0; i < 3; i++) {
            if (tr.portalsky.mins[i] >= tr.portalsky.cntNode->mins[i]) {
                tr.portalsky.mins[i] = tr.portalsky.cntNode->mins[i];
            }

            if (tr.portalsky.maxs[i] >= tr.portalsky.cntNode->maxs[i]) {
                tr.portalsky.maxs[i] = tr.portalsky.cntNode->maxs[i];
            }
        }
    }
}

/*
=================
R_Sky_Render

=================
*/
void R_Sky_Render()
{
    int         i;
    viewParms_t newParms, oldParms;
    mnode_t    *leaf;

    if (!tr.portalsky.numSurfs) {
        return;
    }

    if (!tr.refdef.sky_portal) {
        return;
    }

    if (tr.skyRendered) {
        // already rendered
        return;
    }

    if (tr.viewParms.isPortalSky) {
        return;
    }

    for (i = 0; i < tr.portalsky.numSurfs; i++) {
        if (!SurfIsOffscreen2(
                (const srfBspSurface_t*)tr.portalsky.skySurfs[i]->data,
                tr.portalsky.skySurfs[i]->shader,
                ENTITYNUM_WORLD
            )) {
            break;
        }
    }

    if (i == tr.portalsky.numSurfs) {
        return;
    }

    oldParms = tr.viewParms;
    newParms = tr.viewParms;

    if (r_skyportal->integer) {
        if (sscanf(
                r_skyportal_origin->string,
                "%f %f %f",
                &newParms.or.origin[0],
                &newParms.or.origin[1],
                &newParms.or.origin[2]
            )
            != 3) {
            ri.Printf(PRINT_WARNING, "WARNING: Invalid sky portal origin: %s\n", r_skyportal_origin->string);
            return;
        }
    } else {
        VectorCopy(tr.refdef.sky_origin, newParms.or.origin);
        MatrixMultiply(newParms.or.axis, tr.refdef.sky_axis, newParms.or.axis);
    }

    VectorCopy(newParms.or.origin, newParms.pvsOrigin);
    newParms.isPortalSky       = qtrue;
    newParms.farplane_distance = tr.refdef.skybox_farplane;
    newParms.renderTerrain     = tr.refdef.render_terrain;

    if (oldParms.farplane_bias == 0.0 || oldParms.farplane_distance == 0.0) {
        newParms.farplane_bias = 0.0;
    } else {
        newParms.farplane_bias = newParms.farplane_distance / oldParms.farplane_distance * oldParms.farplane_bias;
    }

    leaf = R_PointInLeaf(newParms.pvsOrigin);
    if (leaf) {
        R_RenderView(&newParms);
    }

    tr.viewParms = oldParms;

    tr.portalsky.numSurfs = 0;
    tr.skyRendered        = qtrue;

    // HZM gl2 re-port (bug-gl2-portalsky): re-derive the outer view's rotation,
    // projection and frustum after the nested portal-sky R_RenderView trashed
    // them, the way gl2's own R_RenderView establishes them. gl1 calls its
    // no-arg R_SetupFrustum() here, but gl2's real R_SetupFrustum takes
    // parameters - the stale no-arg call this replaced would have linked
    // against it via C linkage and passed garbage arguments once this code
    // became reachable.
    R_RotateForViewer();
    R_SetupProjection(&tr.viewParms, r_zproj->value, tr.viewParms.zFar, qtrue);
}

/*
=================
R_Sky_ChangeFrustum

=================
*/
void R_Sky_ChangeFrustum()
{
    cplane_t *frust;
    int       i;
    vec3_t    bounds[2];
    vec3_t    origin;
    cplane_t  abouts[4];

    VectorCopy(tr.portalsky.mins, bounds[0]);
    VectorCopy(tr.portalsky.maxs, bounds[1]);
    VectorCopy(tr.viewParms.or.origin, origin);

    VectorCopy(tr.viewParms.or.axis[2], abouts[0].normal);
    VectorNegate(tr.viewParms.or.axis[2], abouts[1].normal);
    VectorCopy(tr.viewParms.or.axis[1], abouts[2].normal);
    VectorNegate(tr.viewParms.or.axis[1], abouts[3].normal);

    for (i = 0; i < 4; i++) {
        cplane_t out;
        int      i1;
        float    leastfov = 99999;

        frust = &tr.viewParms.frustum[i];

        abouts[i].dist = DotProduct(tr.viewParms.or.origin, abouts[i].normal);
        CrossProduct(frust->normal, abouts[i].normal, out.normal);

        for (i1 = 0; i1 < 8; i1++) {
            vec3_t point;
            vec_t  distFromFrustum;
            float  angle;
            int    i2;

            for (i2 = 0; i2 < 3; i2++) {
                point[i2] = bounds[i1 % 3][i2];
            }

            VectorSubtract(point, origin, point);
            distFromFrustum = DotProduct(point, frust->normal);
            if (distFromFrustum <= 0) {
                break;
            }

            angle = atan2(DotProduct(point, out.normal), distFromFrustum);
            if (angle >= 0) {
                angle = RAD2DEG(angle);
                if (leastfov > angle) {
                    leastfov = angle;
                }
            }
        }

        if (leastfov > 0 && leastfov < 180) {
            RotatePointAroundVector(frust->normal, abouts[i].normal, frust->normal, leastfov);
            VectorNormalize(frust->normal);

            frust->type = PLANE_NON_AXIAL;
            frust->dist = DotProduct(origin, frust->normal);
            SetPlaneSignbits(frust);
        }
    }
}
