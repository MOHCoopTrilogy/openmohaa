/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team

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
// playerbot_movement.cpp: Manages bot movements

#include "playerbot.h"
#include "debuglines.h"

static int maxFallHeight = 400;

BotMovement::BotMovement()
{
    controlledEntity = NULL;

    m_pPath         = IPather::CreatePather();
    m_iLastMoveTime = 0;

    m_bPathing       = false;
    m_iTempAwayState = 0;
    m_fAttractTime   = 0;

    m_iCheckPathTime = 0;
    m_iTempAwayTime  = 0;
    m_iNumBlocks       = 0;
    m_iStuckPushTime   = 0; // [HZM bot wall-slide]
    m_iFeelerCommitTime = 0; // [HZM bot feelers]
    m_iFeelerSide       = 1;

    m_bAvoidCollision     = false;
    m_iCollisionCheckTime = 0;

    // [HZM Phase 2] sentinel far from any real map coord so the FIRST attract selection always rolls a scatter
    // goal; thereafter it is re-rolled only when the node origin actually moves (see MoveToBestAttractivePoint).
    m_vScatterAnchor = Vector(1.0e9f, 1.0e9f, 1.0e9f);
}

BotMovement::~BotMovement()
{
    delete m_pPath;
}

void BotMovement::SetControlledEntity(Player *newEntity)
{
    controlledEntity = newEntity;
}

// [HZM bot feelers] Clearance (trace fraction 0..1) of a whisker cast from the bot along dirAngles rotated by
// degOff degrees of yaw, out to len units, using the bot's own collision box. 1 = fully clear, near 0 = wall
// right in front. This is the "always aware of surroundings" sense the reactive block-recovery lacked (that
// one only looked 32u ahead every 250ms; these look ~112u every frame). Bot-only.
float BotMovement::WhiskerClear(const Vector& dirAngles, float degOff, float len)
{
    Vector a = dirAngles;
    a[1] += degOff; // yaw
    Vector f, r, u;
    AngleVectors(a, f, r, u);
    f.z = 0;
    if (f.lengthSquared() < 0.01f) {
        return 1.0f;
    }
    VectorNormalize2D(f);

    Vector mins = controlledEntity->mins;
    Vector maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;
    Vector base = controlledEntity->origin + Vector(0, 0, STEPSIZE);

    trace_t t = G_Trace(base, mins, maxs, base + f * len, controlledEntity, MASK_PLAYERSOLID, qtrue, "BotFeeler");

    // [HZM bot feelers] EXPECT GROUND (user): an opening with no floor under it is a pit, not a safe path - the
    // feeler must not steer the bot off a ledge. Drop a trace at the reached point; if no ground within a
    // survivable step-down (~STEPSIZE + 200), treat this direction as (almost) blocked so the steering avoids it.
    Vector  fwdPt = t.endpos;
    trace_t g     = G_Trace(
        fwdPt, mins, maxs, fwdPt - Vector(0, 0, STEPSIZE + 200.0f), controlledEntity, MASK_PLAYERSOLID, qtrue,
        "BotFeelerGround"
    );
    if (g.fraction >= 1.0f) {
        return Q_min(t.fraction, 0.15f);
    }
    return t.fraction;
}

void BotMovement::MoveThink(usercmd_t& botcmd)
{
    Vector vAngles;
    Vector vWishDir;
    Vector vDelta;

    botcmd.forwardmove = 0;
    botcmd.rightmove   = 0;

    CheckAttractiveNodes();

    if (!IsMoving()) {
        return;
    }

    if (m_pPath->GetNodeCount()) {
        m_vTargetPos = m_pPath->GetDestination();
    }

    if (m_pPath->IsQuerying()) {
        m_iLastMoveTime = level.inttime;
    }

    if (level.inttime >= m_iLastMoveTime + 5000 && m_vCurrentOrigin != controlledEntity->origin) {
        m_vCurrentOrigin = controlledEntity->origin;

        if (m_pPath->GetNodeCount() && !controlledEntity->GetLadder()) {
            // recalculate paths because of a new origin

            PathSearchParameter parameters;
            parameters.entity     = controlledEntity;
            parameters.fallHeight = maxFallHeight;
            m_pPath->FindPath(controlledEntity->origin, m_pPath->GetDestination(), parameters);
        }

        m_iLastMoveTime = level.inttime;
    }

    if (m_iTempAwayState == 2 && level.inttime >= m_iTempAwayTime + 750) {
        m_iTempAwayState = 0;

        PathSearchParameter parameters;
        parameters.entity     = controlledEntity;
        parameters.fallHeight = maxFallHeight;
        m_pPath->FindPath(controlledEntity->origin, m_vTargetPos, parameters);

        m_iLastMoveTime  = level.inttime;
        m_iCheckPathTime = level.inttime;
    }

    vDelta = m_pPath->GetCurrentDelta();
    vDelta = FixDeltaFromCollision(vDelta);

    if (m_pPath->GetNodeCount()) {
        m_pPath->UpdatePos(controlledEntity->origin);

        m_vCurrentGoal = controlledEntity->origin;
        VectorAdd2D(m_vCurrentGoal, vDelta, m_vCurrentGoal);

        if (MoveDone()) {
            // Clear the path
            m_pPath->Clear();
        }
    }

    if (ai_debugpath->integer) {
        G_DebugLine(controlledEntity->centroid, m_vCurrentGoal + Vector(0, 0, 36), 1, 1, 0, 1);
    }

    // Check if we're blocked
    if (level.inttime >= m_iCheckPathTime + 1000 && m_iTempAwayState != 2) {
        bool blocked = false;

        m_iCheckPathTime = level.inttime;

        if (m_iNumBlocks >= 5) {
            // Give up
            ClearMove();
        }

        if (!m_pPath->IsQuerying() && !controlledEntity->GetLadder()) {
            if (controlledEntity->GetMoveResult() >= MOVERESULT_BLOCKED
                || controlledEntity->velocity.lengthSquared() <= Square(8)) {
                blocked = true;
            } else if ((controlledEntity->origin - m_vLastCheckPos[0]).lengthSquared() <= Square(64)
                       && (controlledEntity->origin - m_vLastCheckPos[1]).lengthSquared() <= Square(64)) {
                blocked = true;
            }
        }

        if (!blocked) {
            m_iTempAwayState = 0;
            m_iNumBlocks     = 0;

            if (!m_pPath->GetNodeCount()) {
                m_vTargetPos   = controlledEntity->origin + Vector(G_CRandom(512), G_CRandom(512), G_CRandom(512));
                m_vCurrentGoal = m_vTargetPos;
            }
        } else if (m_iTempAwayState == 0) {
            m_iLastBlockTime = level.inttime;
            m_iTempAwayState = 1;
        }

        if (m_iTempAwayState && level.inttime >= m_iLastBlockTime + 1000) {
            Vector delta;
            Vector dir;

            m_iTempAwayState = 2;
            m_iTempAwayTime  = level.inttime;
            m_iNumBlocks++;

            // Try to backward a little
            if (m_pPath->GetNodeCount()) {
                delta = m_pPath->GetCurrentDelta();
            } else {
                delta = m_vTargetPos - controlledEntity->origin;
            }

            m_pPath->Clear();

            if (m_iNumBlocks < 2) {
                dir   = -delta;
                dir.z = 0;
                dir.normalize();

                if (dir.x < -0.5 || dir.x > 0.5) {
                    dir.x *= 4;
                    dir.y /= 4;
                } else if (dir.y < -0.5 || dir.y > 0.5) {
                    dir.x /= 4;
                    dir.y *= 4;
                } else {
                    dir.x = G_CRandom(2);
                    dir.y = G_CRandom(2);
                }

                m_vCurrentGoal = controlledEntity->origin + delta + dir * 128;
            } else {
                // [HZM Phase 4a] before the blind random jump (and the eventual give-up at m_iNumBlocks>=5),
                // aim for a REACHABLE point near the real goal, with the accept radius growing per block, so a
                // bot stuck pathing to an exact unreachable spot re-routes to solid ground instead of grinding
                // the wall. bot_nav_reroute 0 restores the stock random escape. Bot-only (coop instantiates no
                // BotMovement); no shared/RecastPather change.
                static cvar_t *s_botReroute = NULL;
                if (!s_botReroute) {
                    s_botReroute = gi.Cvar_Get("bot_nav_reroute", "1", CVAR_ARCHIVE);
                }
                if (s_botReroute->integer) {
                    MoveNear(m_vTargetPos, 128.0f + 96.0f * m_iNumBlocks);
                } else {
                    m_vCurrentGoal =
                        controlledEntity->origin + Vector(G_CRandom(512), G_CRandom(512), G_CRandom(512));
                }
            }
        }

        m_vLastCheckPos[1] = m_vLastCheckPos[0];
        m_vLastCheckPos[0] = controlledEntity->origin;
    }

    if (ai_debugpath->integer) {
        int i;
        int nodecount = m_pPath->GetNodeCount();

        for (i = 0; i < nodecount - 1; i++) {
            PathNav      node1  = m_pPath->GetNode(i);
            PathNav      node2  = m_pPath->GetNode(i + 1);
            const Vector vStart = node1.origin + Vector(0, 0, 32);
            const Vector vEnd   = node2.origin + Vector(0, 0, 32);

            G_DebugLine(vStart, vEnd, 1, 0, 0, 1);
        }
    }

    if (m_pPath->GetNodeCount() || m_iTempAwayState != 0) {
        if ((m_vTargetPos - controlledEntity->origin).lengthSquared() <= Square(16)) {
            ClearMove();
        }
    } else {
        //if ((m_vTargetPos - controlledEntity->origin).lengthXYSquared() <= Square(16)) {
        ClearMove();
        //}
    }

    // Rotate the dir
    if (m_pPath->GetNodeCount()) {
        m_vCurrentDir = CalculateDir(vDelta);
    } else {
        m_vCurrentDir = CalculateDir(m_vCurrentGoal - controlledEntity->origin);
    }

    vWishDir = CalculateRelativeWishDirection(m_vCurrentDir);

    // Forward to the specified direction
    float x = vWishDir.x * 127;
    float y = -vWishDir.y * 127;

    botcmd.forwardmove = (signed char)Q_clamp(x, -127, 127);
    botcmd.rightmove   = (signed char)Q_clamp(y, -127, 127);
    botcmd.upmove      = 0;

    // [HZM bot feelers] Proactive surroundings awareness (user: "always aware of surroundings", "whiskers long
    // enough they won't brush walls until they find the opening", "360 and up and down", "use their whiskers the
    // whole way towards the enemy spawn"). Every frame while the bot wants to move: feel FAR ahead along the goal
    // direction; if that is closing, feel the two forward diagonals and steer toward the clearer one BEFORE
    // touching the wall (arcing into the opening). If the whole forward arc is boxed, sweep a wide ring to find
    // ANY open heading and peel toward it. Vertical awareness (ledge up / drop) stays in CheckJump /
    // CheckJumpOverEdge below. This is the preventative layer; the reactive wall-slide after it only fires if a
    // bot is still fully pinned. bot_feelers 0 restores stock. Bot-only (coop instantiates no BotMovement).
    {
        // NOTE default 0: the always-on whisker steer over-fired on winding maps (constant re-steer => weaving,
        // slower, MORE stuck in the bot study). Kept for tuning; the targeted reactive wall-slide below is the
        // movement win. The dominant Push-bot problem is convergence (teams never reach LOS), fixed elsewhere.
        static cvar_t *s_botFeel = NULL;
        if (!s_botFeel) {
            s_botFeel = gi.Cvar_Get("bot_feelers", "0", CVAR_ARCHIVE);
        }
        bool bWants = (botcmd.forwardmove > 40 || botcmd.forwardmove < -40 || botcmd.rightmove > 40
                       || botcmd.rightmove < -40);
        // NOTE: this block always runs (when moving) so BOT-BOT SEPARATION below stays active regardless of
        // bot_feelers; only the whisker STEER is gated on bot_feelers (via c below).
        if (controlledEntity && bWants) {
            Vector gdir = m_vCurrentGoal - controlledEntity->origin;
            gdir.z = 0;
            if (gdir.lengthSquared() > 1.0f) {
                Vector ga = gdir.toAngles();

                // [HZM bot feelers] BOT-BOT SEPARATION (user: "if bots are stuck in each other the same whiskers
                // should slightly nudge them out of each other"). If a PLAYER (teammate/bot) is jammed right
                // ahead, peel sideways on a per-bot deterministic side (odd/even entnum -> opposite sides) so two
                // bots wedged into each other pick different ways and pop apart instead of both grinding the same
                // way. Takes priority over wall-steering for this frame.
                bool bSep = false;
                {
                    Vector fdir, fr, fu;
                    AngleVectors(ga, fdir, fr, fu);
                    fdir.z = 0;
                    if (fdir.lengthSquared() > 0.01f) {
                        VectorNormalize2D(fdir);
                        Vector smins = controlledEntity->mins;
                        Vector smaxs = controlledEntity->maxs;
                        smaxs.z -= STEPSIZE;
                        Vector  sbase = controlledEntity->origin + Vector(0, 0, STEPSIZE);
                        trace_t bt    = G_Trace(
                            sbase, smins, smaxs, sbase + fdir * 56.0f, controlledEntity, MASK_PLAYERSOLID, qtrue,
                            "BotSep"
                        );
                        if (bt.fraction < 1.0f && bt.ent && bt.ent->entity && bt.ent->entity != world
                            && bt.ent->entity->IsSubclassOfPlayer()) {
                            int side         = (controlledEntity->entnum & 1) ? 1 : -1;
                            botcmd.rightmove = (signed char)(side * 110);
                            if (botcmd.forwardmove > 60) {
                                botcmd.forwardmove = 60; // keep some push so they slide past, not just apart
                            }
                            bSep = true;
                        }
                    }
                }

                // feelers OFF (default) or separating this frame => c=1 so the whisker STEER below is skipped.
                // TIGHTER trigger (only when a wall is genuinely CLOSE, < ~0.45 of a 150u look) - the old 0.85
                // fired almost constantly on winding maps and caused weaving/slowdown.
                float c = (bSep || !s_botFeel->integer) ? 1.0f : WhiskerClear(ga, 0.0f, 150.0f);
                if (!bSep && c < 0.45f) {
                    // COMMITMENT (the anti-weave fix): only RE-decide the steer side when the previous commit has
                    // expired, then HOLD it ~450ms so the bot arcs smoothly around the obstruction instead of
                    // flip-flopping every frame (which slowed and stuck it before).
                    if (level.inttime >= m_iFeelerCommitTime) {
                        float l1 = WhiskerClear(ga, 45.0f, 130.0f);  // +yaw = left
                        float r1 = WhiskerClear(ga, -45.0f, 130.0f); // -yaw = right
                        if (l1 < 0.4f && r1 < 0.4f) {
                            // both diagonals also closing: probe wider for any opening and pick the better side
                            float lw      = WhiskerClear(ga, 80.0f, 110.0f);
                            float rw      = WhiskerClear(ga, -80.0f, 110.0f);
                            m_iFeelerSide = (rw >= lw) ? 1 : -1;
                        } else {
                            m_iFeelerSide = (r1 >= l1) ? 1 : -1; // toward the clearer diagonal (+1 = right)
                        }
                        m_iFeelerCommitTime = level.inttime + 450;
                    }
                    // apply the committed steer, blended onto the path's own rightmove
                    float newR = (float)botcmd.rightmove + (float)(m_iFeelerSide * 100);
                    Q_clamp(newR, -127.0f, 127.0f);
                    botcmd.rightmove = (signed char)newR;
                    if (c < 0.28f && botcmd.forwardmove > 85) {
                        botcmd.forwardmove = 85; // wall very close: ease throttle so the steer arcs, not rams
                    }
                }
            }
        }
    }

    // [HZM bot wall-slide] Reactive unstick. The stock block-recovery only re-checks every ~1s and takes another
    // ~1s to react, so a bot pushing into a wall / thin opening grinds in place for up to ~2s each time - the
    // dominant "running-in-place" seen in the Push bot study. Here, the MOMENT the bot is pushing hard (high
    // forwardmove) but its horizontal speed is near zero, we inject a strafe so it slides ALONG the obstruction
    // to find the gap, alternating side every ~600ms so it probes both ways. This composes with (does not
    // replace) the slower reroute/give-up logic above. bot_wallslide 0 restores stock. Bot-only (coop
    // instantiates no BotMovement); no shared RecastPather change.
    {
        static cvar_t *s_botSlide = NULL;
        if (!s_botSlide) {
            s_botSlide = gi.Cvar_Get("bot_wallslide", "1", CVAR_ARCHIVE);
        }
        bool bWantsMove = (botcmd.forwardmove > 60 || botcmd.forwardmove < -60 || botcmd.rightmove > 60
                           || botcmd.rightmove < -60);
        if (s_botSlide->integer && controlledEntity && bWantsMove) {
            float velH2 = controlledEntity->velocity.x * controlledEntity->velocity.x
                        + controlledEntity->velocity.y * controlledEntity->velocity.y;
            if (velH2 < Square(20)) {
                if (!m_iStuckPushTime) {
                    m_iStuckPushTime = level.inttime;
                }
                int held = level.inttime - m_iStuckPushTime;
                // grinding for >150ms: run a 3-phase escape, ~450ms each, cycling until we break free -
                //   phase 0: strafe right along the wall   phase 1: strafe left   phase 2: back off the wall
                // The back-off peels a bot out of a dead-end POCKET where both strafes are also blocked (the axis
                // stuck-mode the single-side slide could not fix).
                if (held >= 150) {
                    int phase = (held / 450) % 3;
                    if (phase == 0) {
                        botcmd.rightmove = 127;
                    } else if (phase == 1) {
                        botcmd.rightmove = -127;
                    } else {
                        botcmd.forwardmove = (signed char)(-botcmd.forwardmove * 0.8f); // reverse off the obstruction
                    }
                    // while strafing, ease forward so the lateral motion translates into sideways travel, AND
                    // JUMP: a stuck bot pushing into a low LEDGE/step - too tall for the 18u auto-step and with no
                    // jump link in the world-only navmesh (e.g. the m3l3 log the axis pile against, where real
                    // players must hop up) - clears it with forward momentum + a hop. Harmless bunny-hop if it is
                    // actually a flat wall. CheckJump below only fires along the PATH direction, which is why a
                    // path-less ledge never triggered it.
                    if (phase < 2) {
                        botcmd.upmove = 127;
                        if (botcmd.forwardmove > 80) {
                            botcmd.forwardmove = 80;
                        } else if (botcmd.forwardmove < -80) {
                            botcmd.forwardmove = -80;
                        }
                    }
                }
            } else {
                m_iStuckPushTime = 0;
            }
        } else {
            m_iStuckPushTime = 0;
        }
    }

    CheckJump(botcmd);

    if (!m_bJump) {
        CheckJumpOverEdge(botcmd);
    }
}

Vector BotMovement::CalculateDir(const Vector& delta) const
{
    Vector dir;

    dir    = delta;
    dir[2] = 0;
    VectorNormalize2D(dir);

    return dir;
}

Vector BotMovement::CalculateRelativeWishDirection(const Vector& dir) const
{
    Vector angles;
    Vector wishdir;

    angles = dir.toAngles() - controlledEntity->angles;
    angles.AngleVectorsLeft(&wishdir);

    return wishdir;
}

void BotMovement::CheckAttractiveNodes()
{
    for (int i = m_attractList.NumObjects(); i > 0; i--) {
        nodeAttract_t *a = m_attractList.ObjectAt(i);

        if (a->m_pNode == NULL || !a->m_pNode->CheckTeam(controlledEntity) || level.time > a->m_fRespawnTime) {
            delete a;
            m_attractList.RemoveObjectAt(i);
        }
    }
}

void BotMovement::CheckEndPos(Entity *entity)
{
    Vector  start;
    Vector  end;
    trace_t trace;

    if (!m_pPath->GetNodeCount()) {
        return;
    }

    start = m_pPath->GetDestination();
    end   = m_vTargetPos;

    trace =
        G_Trace(start, entity->mins, entity->maxs, end, entity, MASK_TARGETPATH, true, "BotController::CheckEndPos");

    if (trace.fraction < 0.95f) {
        m_vTargetPos = trace.endpos;
    }
}

void BotMovement::CheckJump(usercmd_t& botcmd)
{
    Vector  start;
    Vector  end;
    Vector  dir;
    Vector  delta;
    trace_t trace;

    if (controlledEntity->GetLadder()) {
        if (g_navigation_legacy->integer) {
            botcmd.upmove = botcmd.upmove ? 0 : 127;
        } else if (!m_pPath->GetNodeCount()) {
            // If the bot is not moving, cancel it
            botcmd.upmove = botcmd.upmove ? 0 : 127;
        }
        return;
    }

    if (!controlledEntity->groundentity && !controlledEntity->client->ps.walking) {
        // Falling
        m_bJump = false;
        return;
    }

    dir = m_vCurrentDir;

    start = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    end =
        controlledEntity->origin + Vector(0, 0, STEPSIZE) + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot needs to jump
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJump"
    );

    // No need to jump
    if (!trace.startsolid && trace.fraction > 0.5f) {
        m_bJump = false;
        return;
    }

    start = controlledEntity->origin;
    end   = controlledEntity->origin;
    end.z += STEPSIZE * 3;
    end.z += STEPSIZE / 1.5;

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot can jump up
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        true,
        "BotController::CheckJump"
    );

    start = trace.endpos;
    end   = trace.endpos + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    Vector bounds[2];
    bounds[0] = Vector(controlledEntity->mins[0], controlledEntity->mins[1], 0);
    bounds[1] = Vector(
        controlledEntity->maxs[0],
        controlledEntity->maxs[1],
        (controlledEntity->maxs[0] + controlledEntity->maxs[1]) * 0.5
    );

    // Check if the bot can jump at the location
    trace = G_Trace(
        start, bounds[0], bounds[1], end, controlledEntity, MASK_PLAYERSOLID, false, "BotController::CheckJump"
    );

    if (trace.plane.normal[2] <= MIN_WALK_NORMAL && trace.fraction < 1) {
        m_bJump = false;
        return;
    }

    if (!m_bJump) {
        m_bJump          = true;
        m_iJumpCheckTime = level.inttime;
        m_vJumpLocation  = controlledEntity->origin;
    } else if (level.inttime > m_iJumpCheckTime + 100) {
        m_bJump = false;

        delta = m_vJumpLocation - controlledEntity->origin;
        if (delta.lengthSquared() < Square(32)) {
            botcmd.upmove = 127;
        }
    }
}

void BotMovement::CheckJumpOverEdge(usercmd_t& botcmd)
{
    Vector  start;
    Vector  end;
    Vector  dir;
    trace_t trace;

    if (!controlledEntity->groundentity && !controlledEntity->client->ps.walking) {
        // Falling
        return;
    }

    dir = m_vCurrentDir;

    start = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    end =
        controlledEntity->origin + Vector(0, 0, STEPSIZE) + dir * (controlledEntity->maxs.y - controlledEntity->mins.y);

    if (ai_debugpath->integer) {
        G_DebugLine(start, end, 1, 0, 1, 1);
    }

    // Check if the bot needs to jump
    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction < 1) {
        // Blocked
        return;
    }

    //
    // Check if falling
    //

    start = trace.endpos;
    end   = start - Vector(0, 0, STEPSIZE * 2);

    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction != 1.0) {
        // Blocked
        return;
    }

    //
    // Check if there is an edge at the end
    //

    end = start + dir * controlledEntity->GetRunSpeed() / 2.0;
    end -= Vector(0, 0, STEPSIZE * 2);

    trace = G_Trace(
        start,
        controlledEntity->mins,
        controlledEntity->maxs,
        end,
        controlledEntity,
        MASK_PLAYERSOLID,
        false,
        "BotController::CheckJumpOverEdge"
    );

    if (trace.fraction == 1) {
        return;
    }

    if (!botcmd.upmove) {
        botcmd.upmove = 127;
    } else {
        botcmd.upmove = 0;
    }
}

/*
====================
AvoidPath

Avoid the specified position within the radius and start from a direction
====================
*/
void BotMovement::AvoidPath(
    Vector vAvoid, float fAvoidRadius, Vector vPreferredDir, float *vLeashHome, float fLeashRadius
)
{
    Vector vDir;

    if (vPreferredDir == vec_zero) {
        vDir = controlledEntity->origin - vAvoid;
        VectorNormalizeFast(vDir);
    } else {
        vDir = vPreferredDir;
    }

    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }
    m_pPath->FindPathAway(controlledEntity->origin, vAvoid, vDir, fAvoidRadius, parameters);

    NewMove();

    if (!m_pPath->GetNodeCount()) {
        // Random movements
        m_vTargetPos = controlledEntity->origin + Vector(G_Random(256) - 128, G_Random(256) - 128, G_Random(256) - 128);
        m_vCurrentGoal = m_vTargetPos;
        return;
    }

    m_iLastMoveTime = level.inttime;
    m_vTargetPos    = m_pPath->GetDestination();
}

/*
====================
MoveNear

Move near the specified position within the radius
====================
*/
void BotMovement::MoveNear(Vector vNear, float fRadius, float *vLeashHome, float fLeashRadius)
{
    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }

    m_pPath->FindPathNear(controlledEntity->origin, vNear, fRadius, parameters);
    NewMove();

    if (!m_pPath->GetNodeCount()) {
        m_bPathing = false;
        return;
    }

    m_iLastMoveTime = level.inttime;
    m_vTargetPos    = m_pPath->GetDestination();
}

/*
====================
MoveTo

Move to the specified position
====================
*/
void BotMovement::MoveTo(Vector vPos, float *vLeashHome, float fLeashRadius)
{
    m_vTargetPos = vPos;

    PathSearchParameter parameters;
    parameters.entity     = controlledEntity;
    parameters.fallHeight = maxFallHeight;
    parameters.leashDist  = fLeashRadius;
    if (vLeashHome) {
        parameters.leashHome = vLeashHome;
    }

    m_pPath->FindPath(controlledEntity->origin, vPos, parameters);

    NewMove();

    if (!m_pPath->GetNodeCount()) {
        m_bPathing = false;
        return;
    }

    m_iLastMoveTime = level.inttime;
    CheckEndPos(controlledEntity);
}

/*
====================
MoveToBestAttractivePoint

Move to the nearest attractive point with a minimum priority
Returns true if no attractive point was found
====================
*/
bool BotMovement::MoveToBestAttractivePoint(int iMinPriority)
{
    Container<AttractiveNode *> list;
    AttractiveNode             *bestNode;
    float                       bestDistanceSquared;
    int                         bestPriority;

    if (m_pPrimaryAttract) {
        // [HZM Phase 2] path to THIS bot's scattered goal (set on acquisition below), not the shared node
        // origin - otherwise every bot re-converges to the same 16u bubble every frame and blocks each other.
        MoveTo(m_vAttractScatterGoal);

        if (!IsMoving()) {
            m_pPrimaryAttract = NULL;
        } else {
            if (MoveDone()) {
                if (!m_fAttractTime) {
                    m_fAttractTime = level.time + m_pPrimaryAttract->m_fMaxStayTime;
                }
                if (level.time > m_fAttractTime) {
                    nodeAttract_t *a  = new nodeAttract_t;
                    a->m_fRespawnTime = level.time + m_pPrimaryAttract->m_fRespawnTime;
                    a->m_pNode        = m_pPrimaryAttract;

                    m_pPrimaryAttract = NULL;
                }
            }

            return true;
        }
    }

    if (!attractiveNodes.NumObjects()) {
        return false;
    }

    bestNode            = NULL;
    bestDistanceSquared = 99999999.0f;
    bestPriority        = iMinPriority;

    for (int i = attractiveNodes.NumObjects(); i > 0; i--) {
        AttractiveNode *node = attractiveNodes.ObjectAt(i);
        float           distSquared;
        bool            m_bRespawning = false;

        for (int j = m_attractList.NumObjects(); j > 0; j--) {
            AttractiveNode *node2 = m_attractList.ObjectAt(j)->m_pNode;

            if (node2 == node) {
                m_bRespawning = true;
                break;
            }
        }

        if (m_bRespawning) {
            continue;
        }

        if (node->m_iPriority < bestPriority) {
            continue;
        }

        if (!node->CheckTeam(controlledEntity)) {
            continue;
        }

        distSquared = VectorLengthSquared(controlledEntity->origin - node->origin);

        if (node->m_fMaxDistanceSquared >= 0 && distSquared > node->m_fMaxDistanceSquared) {
            continue;
        }

        if (!CanMoveTo(node->origin)) {
            continue;
        }

        if (distSquared < bestDistanceSquared) {
            bestDistanceSquared = distSquared;
            bestNode            = node;
            bestPriority        = node->m_iPriority;
        }
    }

    if (bestNode) {
        m_pPrimaryAttract = bestNode;
        m_fAttractTime    = 0;
        // [HZM Phase 2] scatter this bot to a distinct reachable point in a ring around the objective node so
        // N bots do not stack on one origin (the reported clumping/blocking). bot_objective_spread 0 = stock.
        //
        // CRITICAL: re-roll the scattered goal ONLY when the node's origin has actually moved (>64u from the
        // point we last scattered around). The fast path at the top of this function NULLs m_pPrimaryAttract
        // the frame a bot arrives (IsMoving()==false with an empty path), so a fresh selection runs every frame
        // once the bot is standing on its goal. Rolling a new random ring point on each of those re-selections
        // made bots hop endlessly between scatter points - the "stuttering in place" bug (bug-2696). Anchoring
        // the roll to the node origin keeps each bot on ONE stable spread point while the objective is static,
        // yet still re-scatters when a mode moves the node (e.g. the Push frontline advancing).
        if (VectorLengthSquared(bestNode->origin - m_vScatterAnchor) > 4096.0f) {
            m_vScatterAnchor = bestNode->origin;

            static cvar_t *bot_objective_spread = NULL;
            if (!bot_objective_spread) {
                bot_objective_spread = gi.Cvar_Get("bot_objective_spread", "128", CVAR_ARCHIVE);
            }
            m_vAttractScatterGoal = bestNode->origin;
            if (bot_objective_spread->value > 1.0f) {
                for (int tries = 0; tries < 5; tries++) {
                    float  ang  = G_Random(360.0f);
                    float  rad  = bot_objective_spread->value * (0.35f + G_Random(0.65f));
                    Vector cand = bestNode->origin
                                + Vector((float)cos(DEG2RAD(ang)) * rad, (float)sin(DEG2RAD(ang)) * rad, 0);
                    if (CanMoveTo(cand)) {
                        m_vAttractScatterGoal = cand;
                        break;
                    }
                }
            }
        }
        MoveTo(m_vAttractScatterGoal);
        return true;
    } else {
        // No attractive point found
        return false;
    }
}

/*
====================
NewMove

Called when there is a new move
====================
*/
void BotMovement::NewMove()
{
    m_bPathing         = true;
    m_vLastCheckPos[0] = controlledEntity->origin;
    m_vLastCheckPos[1] = controlledEntity->origin;
}

void BotMovement::CalculateBestFrontAvoidance(
    const Vector& targetOrg, float maxDist, const Vector& forward, const Vector& right, float& bestFrac, Vector& bestPos
)
{
    Vector  mins, maxs;
    bool    wasOnGround = true;
    Vector  start, step;
    Vector  entityStepOrg;
    trace_t trace;
    int     i;

    bestFrac = 0;
    bestPos  = vec_zero;

    mins = controlledEntity->mins;
    maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;
    entityStepOrg = controlledEntity->origin + Vector(0, 0, STEPSIZE);

    for (i = 1; i < 5; i++) {
        start = entityStepOrg - forward + right * (32 * i);
        if (i == 1) {
            step = start;
        }

        //
        // Trace to the right
        //
        trace = G_Trace(entityStepOrg, mins, maxs, start, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");

        if (trace.startsolid || trace.fraction <= 0) {
            break;
        }

        start   = trace.endpos;
        start.z = step.z;

        // Make sure the bot can jump after falling
        trace = G_Trace(
            start,
            mins,
            maxs,
            start - Vector(0, 0, STEPSIZE + STEPSIZE * 3),
            controlledEntity,
            MASK_PLAYERSOLID,
            qtrue,
            "GetCurrentDelta"
        );
        if (trace.fraction == 1) {
            if (!wasOnGround) {
                break;
            }

            wasOnGround = false;
            continue;
        }

        wasOnGround = true;
        step        = trace.endpos;

        //
        // Trace from the right to the node
        //
        trace = G_Trace(start, mins, maxs, targetOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");
        if (trace.fraction == 0) {
            trace = G_Trace(
                start,
                mins,
                maxs,
                start + forward * Q_min(maxDist, 64),
                controlledEntity,
                MASK_PLAYERSOLID,
                qtrue,
                "GetCurrentDelta"
            );
            trace = G_Trace(
                trace.endpos, mins, maxs, targetOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta"
            );
        }

        if (trace.fraction > bestFrac) {
            bestFrac = trace.fraction;
            bestPos  = start;
        }
        if (trace.fraction >= 0.999) {
            break;
        }
    }
}

Vector BotMovement::FixDeltaFromCollision(const Vector& delta)
{
    trace_t trace;
    Vector  stepOrg;
    Vector  mins;
    Vector  maxs;
    Vector  newDelta;
    Vector  angles;
    Vector  forward, right, up;
    Vector  target;
    Vector  targetStepOrg;
    Vector  dest;
    Vector  front;
    float   dist;
    float   maxDist;

    if (controlledEntity->GetLadder()) {
        return delta;
    }

    if (level.inttime < m_iCollisionCheckTime + 250 || m_bJump) {
        if (m_bAvoidCollision) {
            newDelta = m_vTempCollisionAvoidance - controlledEntity->origin;
            if (newDelta.lengthSquared() > Square(16)) {
                // Not reached
                return newDelta;
            }

            // Path has been reached so clear the collision
            m_bAvoidCollision = false;
        }

        return delta;
    }

    m_iCollisionCheckTime = level.inttime;
    m_bAvoidCollision     = false;

    dest     = controlledEntity->origin + delta;
    newDelta = delta;
    dist     = VectorNormalize2(newDelta, forward);
    VectorToAngles(forward, angles);
    AngleVectors(angles, forward, right, up);

    mins = controlledEntity->mins;
    maxs = controlledEntity->maxs;
    maxs.z -= STEPSIZE;

    maxDist = Q_min(dist, 32);

    stepOrg       = controlledEntity->origin + Vector(0, 0, STEPSIZE);
    target        = controlledEntity->origin + forward * maxDist;
    targetStepOrg = target + Vector(0, 0, STEPSIZE);

    trace = G_Trace(stepOrg, mins, maxs, targetStepOrg, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");
    if (trace.fraction < 1.0) {
        //
        // Try to use a flat plane instead
        //

        trace_t tmpTrace;
        Vector  forwardXY, rightXY, upXY;
        Vector  targetXY, targetStepOrgXY;

        angles.x = 0;
        AngleVectors(angles, forwardXY, rightXY, upXY);
        targetXY        = controlledEntity->origin + forwardXY * maxDist + Vector(0, 0, STEPSIZE);
        targetStepOrgXY = targetXY + Vector(0, 0, STEPSIZE);

        tmpTrace =
            G_Trace(stepOrg, mins, maxs, targetStepOrgXY, controlledEntity, MASK_PLAYERSOLID, qtrue, "GetCurrentDelta");

        if (tmpTrace.fraction > trace.fraction) {
            trace   = tmpTrace;
            forward = forwardXY;
            right   = rightXY;
            up      = upXY;
            target  = targetXY;
        }
    }

    if (trace.fraction < 1.0) {
        Vector start, step;
        float  bestLeftFrac = 0, bestRightFrac = 0;
        Vector bestLeftPos, bestRightPos;

        // 0 = parallel
        // -1 = perpendicular
        // If it's near parallel use the trace normal
        if (DotProduct(trace.plane.normal, forward) < -0.75) {
            VectorCopy(trace.plane.normal, forward);
            VectorNegate(forward, forward);
            VectorToAngles(forward, angles);
            AngleVectors(angles, forward, right, up);
        }

        //
        // Try to resolve following situation (schema from top):
        //
        // ┌───┐
        // ↑   │
        // p▌  t   ← Must be able to avoid the obstacle in front and move left or right, to target
        // ↓   ↑
        // └─→─┘
        //
        CalculateBestFrontAvoidance(target, 64, forward, right, bestRightFrac, bestRightPos);

        if (bestRightFrac != 1) {
            CalculateBestFrontAvoidance(target, 64, forward, -right, bestLeftFrac, bestLeftPos);
        }

        if (bestLeftFrac != 0 || bestRightFrac != 0) {
            m_bAvoidCollision = true;

            //
            // By default use the one with higher fraction
            //
            if (bestLeftFrac > bestRightFrac) {
                m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
            } else if (bestLeftFrac < bestRightFrac) {
                m_vTempCollisionAvoidance = bestRightPos + forward * 64;
            } else {
                // Randomly choose direction if both are the same
                if (Vector::DistanceSquared(bestLeftPos, dest) > Vector::DistanceSquared(bestRightPos, dest)) {
                    m_vTempCollisionAvoidance = bestRightPos + forward * 64;
                } else {
                    m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
                }
            }

            //
            // If falling, make sure to use the one that won't fall
            //
#if 0
            if (leftFallTrace.fraction != rightFallTrace.fraction
                && (leftFallTrace.fraction != 1 || rightFallTrace.fraction != 1)) {
                if (leftFallTrace.fraction == 1 && bestRightFrac) {
                    m_vTempCollisionAvoidance = bestRightPos + forward * 64;
                } else if (rightFallTrace.fraction == 1 && bestLeftFrac) {
                    m_vTempCollisionAvoidance = bestLeftPos + forward * 64;
                }
            }
#endif

            return m_vTempCollisionAvoidance - controlledEntity->origin;
        }
    }

    return delta;
}

/*
====================
CanMoveTo

Returns true if the bot has done moving
====================
*/
bool BotMovement::CanMoveTo(Vector vPos)
{
    PathSearchParameter parameters;
    parameters.fallHeight = maxFallHeight;
    parameters.entity     = controlledEntity;
    return m_pPath->TestPath(controlledEntity->origin, vPos, parameters);
}

/*
====================
MoveDone

Returns true if the bot has done moving
====================
*/
bool BotMovement::MoveDone()
{
    if (!m_bPathing) {
        return true;
    }

    if (m_iTempAwayState != 0) {
        return false;
    }

    if (!m_pPath->GetNodeCount()) {
        return true;
    }

    Vector delta = m_pPath->GetDestination() - controlledEntity->origin;
    if (delta.lengthXYSquared() < Square(16) && (m_pPath->GetNodeCount() == 1 || delta.z < controlledEntity->maxs.z)) {
        return true;
    }

    return false;
}

/*
====================
IsMoving

Returns true if the bot has a current path
====================
*/
bool BotMovement::IsMoving(void)
{
    return m_bPathing;
}

/*
====================
ClearMove

Stop the bot from moving
====================
*/
void BotMovement::ClearMove(void)
{
    m_pPath->Clear();
    m_bPathing   = false;
    m_iNumBlocks = 0;
}

/*
====================
GetCurrentGoal

Return the current goal, usually the nearest node the player should look at
====================
*/
Vector BotMovement::GetCurrentGoal() const
{
    if (!m_pPath->GetNodeCount()) {
        return m_vCurrentGoal;
    }

    if (!m_pPath->HasReachedGoal(controlledEntity->origin) && m_pPath->GetNodeCount()) {
        const Vector delta = m_pPath->GetCurrentDelta();
        return controlledEntity->origin + Vector(delta[0], delta[1], 0);
    }

    return controlledEntity->origin;
}

Vector BotMovement::GetCurrentPathDirection() const
{
    return m_pPath->GetCurrentDirection();
}
