// HZM coop [scopes1025] sight rows for SCOPED rifles: the same adsSight_t geometry as the generated cg_adssights.h, but the aim
// feature F is the objective bell centre and the rear feature R the ocular lens centre, so the ADS rig solve puts the SCOPE AXIS
// through the screen centre and the ocular at the eye. Hand-written (the generator has no scope pass yet); measured from the
// tiki meshes with docs/proposals/ironsights_2026-09-28/tools/gungeo.py (the "lens" surface = R, the tube's front cap = F).
// delta 0 (the scope axis is parallel to the bore), U = gun up (-z in model space), relief = eye -> ocular distance in units.
#pragma once

static const adsSight_t s_adsSightsScope[] = {
    {"models/weapons/springfield.tik", "Springfield '03 Sniper", {16.9100f, -0.8700f, -5.2400f}, {2.5860f, -0.8660f, -5.2340f}, 0.000000f, {0.000f, 0.000f, -1.000f}, 1.500f},
};
