// HZM coop - visual filter names (r_ppFilter 1..HZM_NUM_FILTERS). One list for the client command, the
// cgame label and both renderers' shaders (renderergl2/glsl/hzmfilter_fp.glsl, renderergl1 FILTER_FS).
#ifndef HZM_FILTERS_H
#define HZM_FILTERS_H

#define HZM_NUM_FILTERS 10

static const char *const hzmFilterNames[HZM_NUM_FILTERS + 1] = {
    "Off",
    "Newsreel",
    "Sepia",
    "Noir",
    "Technicolor",
    "Bleach bypass",
    "Cold front",
    "Golden hour",
    "Old film",
    "Gun camera",
    "Cinematic",
};

#endif
