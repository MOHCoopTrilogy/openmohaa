// HZM coop - minimal engine-function stub so md5_2_skX links as a standalone CLI tool.
// q_shared.c's parser references Com_DPrintf; Com_Error and Com_Printf are already
// provided by misc_utils.c (defining them here too would be a duplicate-symbol link error). 2026-06-30.
#include "../../qcommon/q_shared.h"

void QDECL Com_DPrintf(const char *fmt, ...)
{
    // quiet: developer prints aren't needed for the tool
    (void)fmt;
}
