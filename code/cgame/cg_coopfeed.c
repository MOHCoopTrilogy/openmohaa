/*
===========================================================================
HZM coop [user 2026-10-05] EVENT FEED - replaces the engine game-message box (gmbox) for coop chatter.

Why (docs/proposals/hud_declutter_2026-10-05/PLAN.md): the gmbox holds 5 lines, decays them ONE AT A TIME
(cl_uigmbox.cpp PostDecayEvent), holds a bold line 10 s and beeps "objective_text" for every bold line
(cl_ui.cpp UI_PrintConsole). A five-line officer burst stayed on screen for most of a minute. This feed:
  - max 3 lines, bottom-left above the health/stamina cluster, newest at the bottom;
  - every line has its OWN timer (parallel fade), no sound;
  - a burst larger than the three rows is never dropped [user 2026-10-06, feed round 2]: a line that finds every
    row taken by a line younger than FEED_MIN_SHOW waits its turn (s_pend, alerts first) and goes up as soon as
    the oldest row has had that long, so four lines in one frame are four lines read, not three and a 200 ms flash;
  - a repeat of the same key restarts the hold and shows a counter ("x3") instead of a new line;
  - drawn procedurally like the stamina arc (no ihuddraw slot - the fade-exempt band is full - and no
    s_hudFadeAlpha, so a stationary player still sees it).

WIRE: an ordinary script print (Player iprint / ScriptThread iprintln*), so it is a RELIABLE server command,
whose text starts with a tag:   ~f<cat>[:<key>[:<icon>]]~<text>
  cat 0 alert (amber - the only category drawn in Hardcore), 1 team, 2 self, 3 pickup (icon), 4 hint (once
  per profile: coop_hintSeen/coop_hintSeen2), 5 armory status (sets coop_armoryStatus, not drawn),
  6 last-mission line (key 0-9 -> archived coop_lm<key>, read by ui/coop_lastmission.urc, not drawn).
The server only tags for a client that advertises cg_hzmFeed 1 in its userinfo (coop_mod/feed.scr); an older
cgame gets the plain text. The text is echoed to the console WITHOUT a colour byte, so it stays in the console
and qconsole.log but never reaches the gmbox (UI_PrintConsole routes only prefixed text there).

Client cvars: coop_feed 0 (no feed lines) / 1 (default) / 2 (feed + mirror into the old box, non-bold),
coop_hints 0/1, coop_feedX (hi-res HUD units from the left) / coop_feedYv (640x480 VIRTUAL units from the bottom,
the script huddraw space the item counters live in),
coop_feedDebug 1 (one ^~^~^ FEED line per accepted message, for the harness).
===========================================================================
*/

#include "cg_local.h"

#define FEED_SLOTS     6
#define FEED_VISIBLE   3
#define FEED_MAXROWS   5   // hints (category 4) may use all 5 rows; everything else 3
#define FEED_TEXT      200
#define FEED_KEY       40
#define FEED_ICON      64
#define FEED_FADE_IN   150
#define FEED_FADE_OUT  600
#define FEED_FORCE_OUT 200
#define FEED_BUMP      260
#define FEED_ROW_H     13.0f
#define FEED_GAP       3.0f
#define FEED_WRAP_W    300.0f
#define FEED_ICON_SZ   16.0f

typedef struct {
    int       active;
    int       cat;
    char      key[FEED_KEY];
    char      text[FEED_TEXT];
    char      icon[FEED_ICON];
    qhandle_t hIcon;
    int       count;
    int       born;
    int       refresh;
    int       hold;
    int       dieAt;  // 0 = natural expiry; else a forced fast fade started at this time
    int       nrows;
    int       rowStart[FEED_MAXROWS];
    int       rowLen[FEED_MAXROWS];
    int       trunc;      // the text did not fit: the last row ends in "..."
    int       hintMark;   // category 4: mark the key seen on its first visible frame
    float     y;      // current bottom offset (units, animated)
    int       placed;
    int       seq;    // arrival order on the rows: two lines placed in one frame have the same born
} feedLine_t;

// [user 2026-10-06, feed round 2] "No line may be lost when several arrive in one frame". Until now a 4th line pushed
// the oldest out with the 200 ms fast fade whatever its age, so of four lines sent in one frame the first was on
// screen for a fifth of a second, and of seven the first four were. Lines that find no row now wait here.
#define FEED_PENDING  24   // lines waiting for a row; a longer burst than 3 + 24 shows its oldest waiting line early
#define FEED_MIN_SHOW 1500 // ms a line is guaranteed on screen before a newer line may take its row

typedef struct {
    int  cat;
    int  count;
    int  at; // cg.time it arrived
    char key[FEED_KEY];
    char text[FEED_TEXT];
    char icon[FEED_ICON];
} feedPend_t;

static feedLine_t    s_feed[FEED_SLOTS];
static feedPend_t    s_pend[FEED_PENDING];
static int           s_pendN      = 0;
static fontheader_t *s_feedFont   = NULL;
static int           s_feedInit   = 0;
static int           s_feedLastT  = 0;
// [user 2026-10-12, feed round 3, bug-3543] "the feed holds a line while the HUD is hidden and starts its clock when
// the HUD returns". Until now a line went on ageing under drawhud 0 / a cinematic and was gone before anyone saw it
// (e3l3's "weapons are secured" fires during the mission title). While hidden, every clock stands still
// (CG_DrawCoopFeed) and at most FEED_HELD_MAX lines are kept: a newer line pushes out the oldest that is NOT an
// alert, and an alert is never pushed out (CG_FeedHeldCap). Not held: the intermission (the map is over).
#define FEED_HELD_MAX 6
static int           s_feedHidden = 0;
static int           s_feedSeq    = 0;
static cvar_t       *s_cvFeed     = NULL;
static cvar_t       *s_cvHints    = NULL;
static cvar_t       *s_cvX        = NULL;
static cvar_t       *s_cvY        = NULL;
static cvar_t       *s_cvDebug    = NULL;

void CG_CoopFeedInit(void)
{
    if (s_feedInit) {
        return;
    }
    s_feedInit = 1;
    memset(s_feed, 0, sizeof(s_feed));
    s_cvFeed  = cgi.Cvar_Get("coop_feed", "1", CVAR_ARCHIVE);
    s_cvHints = cgi.Cvar_Get("coop_hints", "1", CVAR_ARCHIVE);
    s_cvX     = cgi.Cvar_Get("coop_feedX", "22", CVAR_ARCHIVE);
    // [user 2026-10-06 / 2026-10-11, bug-3464] "your new small text for intel etc overlaps with the medkit, ammo and
    // sandbag icons". The feed sat coop_feedY (116) HI-RES units above the bottom, but the counters it has to clear are
    // script huddraw elements in the 640x480 VIRTUAL screen (medkit.scr: rect 5 -62 16 16, i.e. their top edge is
    // 62 * vidHeight / 480 above the bottom). Hi-res units are 1 px up to 1920x1080 and vidHeight / 1080 above it, so
    // the two only agreed at the 720p the harness ran: counters top 93 px vs feed 116 px there, but 139 px vs 116 px
    // at 1080p and 186 px vs 155 px at 1440p (16:9 or ultrawide alike) - the feed sat ON the counters everywhere real.
    // The anchor is now in the SAME virtual-480 units as the counters: 70 = 8 units above their top at every size.
    // New cvar name on purpose: an archived coop_feedY 116 from an old config would mean something else here.
    s_cvY     = cgi.Cvar_Get("coop_feedYv", "70", CVAR_ARCHIVE);
    s_cvDebug = cgi.Cvar_Get("coop_feedDebug", "0", 0);
    cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE);
    cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE);
    cgi.Cvar_Get("coop_armoryStatus", "", 0);
    // CAPABILITY in the userinfo (same pattern as cg_hzmLt): this cgame understands the ~f tags, so the server
    // may send them. ROM: the player cannot change what the binary can do.
    cgi.Cvar_Get("cg_hzmFeed", "1", CVAR_USERINFO | CVAR_ROM);
}

static fontheader_t *CG_FeedFont(void)
{
    if (!s_feedFont) {
        s_feedFont = cgi.R_LoadFont("verdana-12");
    }
    return s_feedFont;
}

// strip ^-colour codes and line breaks; collapse to one line of plain text
static void CG_FeedClean(const char *in, char *out, int outSize)
{
    int n = 0;
    while (*in && n < outSize - 1) {
        if (*in == '^' && in[1]) {
            in += 2;
            continue;
        }
        if (*in == '\n' || *in == '\r') {
            in++;
            continue;
        }
        out[n++] = *in++;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = 0;
}

static int CG_FeedHoldMs(int cat)
{
    switch (cat) {
    case 0:
        return 6000;
    case 1:
        return 4000;
    case 2:
        return 3000;
    case 3:
        return 2500;
    case 4:
        return 8000;
    default:
        return 3000;
    }
}

static float CG_FeedTextW(const char *s, int len)
{
    fontheader_t *f = CG_FeedFont();
    if (!f) {
        return (float)len * 6.0f;
    }
    return (float)cgi.UI_FontStringWidth(f, s, len);
}

// word wrap into at most FEED_MAXROWS rows of FEED_WRAP_W units (the last row may be cut with "...")
static void CG_FeedWrap(feedLine_t *e)
{
    const char *s   = e->text;
    int         len = (int)strlen(s);
    int         pos = 0;
    float       maxW;

    int         maxRows = (e->cat == 4) ? FEED_MAXROWS : 3;

    maxW      = FEED_WRAP_W - ((e->cat == 3) ? (FEED_ICON_SZ + 4.0f) : 0.0f);
    e->nrows  = 0;
    e->trunc  = 0;
    while (pos < len && e->nrows < maxRows) {
        int lastSpace = -1;
        int end       = pos;
        while (end < len) {
            if (s[end] == ' ') {
                lastSpace = end;
            }
            if (CG_FeedTextW(s + pos, end - pos + 1) > maxW) {
                break;
            }
            end++;
        }
        if (end >= len) {
            e->rowStart[e->nrows] = pos;
            e->rowLen[e->nrows]   = len - pos;
            e->nrows++;
            return;
        }
        if (lastSpace > pos) {
            end = lastSpace;
        }
        if (end <= pos) {
            end = pos + 1;
        }
        e->rowStart[e->nrows] = pos;
        e->rowLen[e->nrows]   = end - pos;
        e->nrows++;
        pos = end;
        while (pos < len && s[pos] == ' ') {
            pos++;
        }
    }
    if (pos < len) {
        e->trunc = 1;
    }
    if (e->nrows <= 0) {
        e->nrows       = 1;
        e->rowStart[0] = 0;
        e->rowLen[0]   = 0;
    }
}

// hint memory: two archived cvars of space-separated keys
static qboolean CG_FeedHintSeen(const char *key)
{
    char        pat[FEED_KEY + 4];
    const char *a = cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE)->string;
    const char *b = cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE)->string;
    char        buf[600];

    Com_sprintf(pat, sizeof(pat), " %s ", key);
    Com_sprintf(buf, sizeof(buf), " %s %s ", a, b);
    return strstr(buf, pat) ? qtrue : qfalse;
}

static void CG_FeedHintMark(const char *key)
{
    const char *a = cgi.Cvar_Get("coop_hintSeen", "", CVAR_ARCHIVE)->string;
    const char *b = cgi.Cvar_Get("coop_hintSeen2", "", CVAR_ARCHIVE)->string;
    char        buf[300];

    if (strlen(a) + strlen(key) + 2 < 240) {
        Com_sprintf(buf, sizeof(buf), "%s%s%s", a, a[0] ? " " : "", key);
        cgi.Cvar_Set("coop_hintSeen", buf);
    } else if (strlen(b) + strlen(key) + 2 < 240) {
        Com_sprintf(buf, sizeof(buf), "%s%s%s", b, b[0] ? " " : "", key);
        cgi.Cvar_Set("coop_hintSeen2", buf);
    }
    // both full: the hint keeps showing - better than silently swallowing a new one
}

static float CG_FeedAlpha(const feedLine_t *e, int now)
{
    float a = 1.0f;
    int   t;

    if (!e->active) {
        return 0.0f;
    }
    t = now - e->born;
    if (t < FEED_FADE_IN) {
        a = (t <= 0) ? 0.0f : (float)t / (float)FEED_FADE_IN;
    }
    if (e->dieAt) {
        t = now - e->dieAt;
        if (t >= FEED_FORCE_OUT) {
            return 0.0f;
        }
        if (t > 0) {
            a *= 1.0f - (float)t / (float)FEED_FORCE_OUT;
        }
        return a;
    }
    t = now - (e->refresh + e->hold);
    if (t >= FEED_FADE_OUT) {
        return 0.0f;
    }
    if (t > 0) {
        a *= 1.0f - (float)t / (float)FEED_FADE_OUT;
    }
    return a;
}

static qboolean CG_FeedExpired(const feedLine_t *e, int now)
{
    if (!e->active) {
        return qtrue;
    }
    if (e->dieAt) {
        return (now - e->dieAt >= FEED_FORCE_OUT) ? qtrue : qfalse;
    }
    return (now - (e->refresh + e->hold) >= FEED_FADE_OUT) ? qtrue : qfalse;
}

static qboolean CG_FeedLive(const feedLine_t *e, int now)
{
    return (e->active && !e->dieAt && now < e->refresh + e->hold) ? qtrue : qfalse;
}

static qboolean CG_FeedMakeRoom(int now, qboolean force, int cat);
static void     CG_FeedPlace(int cat, const char *key, const char *icon, const char *text, int count, int now, int arrived);

/*
The HUD is hidden and one more line has arrived: keep at most FEED_HELD_MAX (rows + waiting). Over that, the oldest
line that is not an alert goes - from the rows first (they are older than anything waiting), then from the waiting
list. Returns qfalse when everything held is an alert and the new line is not: then the new line is the one not kept.
*/
static qboolean CG_FeedHeldCap(int cat, int now)
{
    int i, held = s_pendN, oldest = -1;

    for (i = 0; i < FEED_SLOTS; i++) {
        if (CG_FeedLive(&s_feed[i], now)) {
            held++;
            if (s_feed[i].cat != 0 && (oldest < 0 || s_feed[i].born < s_feed[oldest].born)) {
                oldest = i;
            }
        }
    }
    if (held < FEED_HELD_MAX) {
        return qtrue;
    }
    if (oldest >= 0) {
        if (s_cvDebug && s_cvDebug->integer) {
            cgi.Printf("^~^~^ FEEDHELD drop row cat=%d key=%s\n", s_feed[oldest].cat, s_feed[oldest].key);
        }
        s_feed[oldest].active = 0;
        return qtrue;
    }
    for (i = 0; i < s_pendN; i++) {
        if (s_pend[i].cat != 0) {
            if (s_cvDebug && s_cvDebug->integer) {
                cgi.Printf("^~^~^ FEEDHELD drop waiting cat=%d key=%s\n", s_pend[i].cat, s_pend[i].key);
            }
            s_pendN--;
            memmove(&s_pend[i], &s_pend[i + 1], sizeof(s_pend[0]) * (s_pendN - i));
            return qtrue;
        }
    }
    return (cat == 0) ? qtrue : qfalse;
}

static void CG_FeedPush(int cat, const char *key, const char *icon, const char *text)
{
    int         i, now = cg.time, slot;
    feedLine_t *e;

    if (!key[0]) {
        key = text;
    }
    // same key still on screen: restart its hold, bump the counter, show the newest wording
    for (i = 0; i < FEED_SLOTS; i++) {
        e = &s_feed[i];
        if (e->active && !e->dieAt && !Q_stricmpn(e->key, key, FEED_KEY - 1) && !CG_FeedExpired(e, now)) {
            if (!Q_stricmp(e->text, text)) {
                e->count++;
            } else {
                e->count = 1; // same key, new wording (e.g. a different squad): no misleading "x2"
            }
            e->refresh = now;
            if (e->born > now - FEED_FADE_IN) {
                e->born = now - FEED_FADE_IN;
            }
            Q_strncpyz(e->text, text, sizeof(e->text));
            CG_FeedWrap(e);
            return;
        }
    }
    // the same key is already waiting for a row: one waiting line, newest wording, counted
    for (i = 0; i < s_pendN; i++) {
        if (!Q_stricmpn(s_pend[i].key, key, FEED_KEY - 1)) {
            if (!Q_stricmp(s_pend[i].text, text)) {
                s_pend[i].count++;
            } else {
                s_pend[i].count = 1;
            }
            Q_strncpyz(s_pend[i].text, text, sizeof(s_pend[i].text));
            return;
        }
    }
    if (s_feedHidden && !CG_FeedHeldCap(cat, now)) {
        if (s_cvDebug && s_cvDebug->integer) {
            cgi.Printf("^~^~^ FEEDHELD not kept cat=%d key=%s\n", cat, key);
        }
        return;
    }
    // a row is free (or its line has had FEED_MIN_SHOW) and nobody is ahead: show it now
    if (!s_pendN && CG_FeedMakeRoom(now, qfalse, cat)) {
        CG_FeedPlace(cat, key, icon, text, 1, now, now);
        return;
    }
    // otherwise it waits. A full queue sends its head up early rather than drop anything.
    if (s_pendN >= FEED_PENDING) {
        CG_FeedMakeRoom(now, qtrue, s_pend[0].cat);
        CG_FeedPlace(s_pend[0].cat, s_pend[0].key, s_pend[0].icon, s_pend[0].text, s_pend[0].count, now, s_pend[0].at);
        s_pendN--;
        memmove(&s_pend[0], &s_pend[1], sizeof(s_pend[0]) * s_pendN);
    }
    // an alert (a teammate down, an enemy wave) goes ahead of everything waiting that is not an alert
    slot = s_pendN;
    if (cat == 0) {
        for (slot = 0; slot < s_pendN && s_pend[slot].cat == 0; slot++) {}
        memmove(&s_pend[slot + 1], &s_pend[slot], sizeof(s_pend[0]) * (s_pendN - slot));
    }
    s_pendN++;
    memset(&s_pend[slot], 0, sizeof(s_pend[slot]));
    s_pend[slot].cat   = cat;
    s_pend[slot].count = 1;
    s_pend[slot].at    = now;
    Q_strncpyz(s_pend[slot].key, key, sizeof(s_pend[slot].key));
    Q_strncpyz(s_pend[slot].text, text, sizeof(s_pend[slot].text));
    Q_strncpyz(s_pend[slot].icon, icon, sizeof(s_pend[slot].icon));
    if (s_cvDebug && s_cvDebug->integer) {
        cgi.Printf("^~^~^ FEEDQ t=%d waiting=%d cat=%d key=%s\n", now, s_pendN, cat, key);
    }
}

/*
The lines that waited: each goes up as soon as there is a row for it. Called every frame from CG_DrawCoopFeed,
before anything is drawn and also while the HUD is hidden (lines age there too).
*/
static void CG_FeedDrain(int now)
{
    // cg.time ran backwards over a map load: what waited belongs to the previous map
    if (s_pendN > 0 && s_pend[0].at > now + 1000) {
        s_pendN = 0;
    }
    while (s_pendN > 0 && CG_FeedMakeRoom(now, qfalse, s_pend[0].cat)) {
        CG_FeedPlace(s_pend[0].cat, s_pend[0].key, s_pend[0].icon, s_pend[0].text, s_pend[0].count, now, s_pend[0].at);
        s_pendN--;
        memmove(&s_pend[0], &s_pend[1], sizeof(s_pend[0]) * s_pendN);
    }
}

/*
Is there a row for one more line right now? Yes when fewer than FEED_VISIBLE lines are live, or when the oldest live
line has been up for FEED_MIN_SHOW - it then leaves with the fast fade. force = take its row whatever its age.
[user 2026-10-12, feed round 3] An alert keeps its row for its whole time: the row that is given up is the oldest
that is NOT an alert, and when all three are alerts only another alert (cat 0) may take one. Until now three
informational lines waiting behind an alert took its row after FEED_MIN_SHOW - seen when a held alert came back with
the HUD and was gone 1.5 s later.
*/
static qboolean CG_FeedMakeRoom(int now, qboolean force, int cat)
{
    int i, live = 0, oldest = -1, oldestInfo = -1;

    for (i = 0; i < FEED_SLOTS; i++) {
        if (CG_FeedLive(&s_feed[i], now)) {
            live++;
            if (oldest < 0 || s_feed[i].born < s_feed[oldest].born) {
                oldest = i;
            }
            if (s_feed[i].cat != 0 && (oldestInfo < 0 || s_feed[i].born < s_feed[oldestInfo].born)) {
                oldestInfo = i;
            }
        }
    }
    if (live < FEED_VISIBLE || oldest < 0) {
        return qtrue;
    }
    if (oldestInfo >= 0) {
        oldest = oldestInfo;
    } else if (cat != 0 && !force) {
        return qfalse;
    }
    if (!force && now - s_feed[oldest].born < FEED_MIN_SHOW) {
        return qfalse;
    }
    s_feed[oldest].dieAt = now;
    return qtrue;
}

// put a line on screen now (the caller has made room)
static void CG_FeedPlace(int cat, const char *key, const char *icon, const char *text, int count, int now, int arrived)
{
    int         i, slot = -1;
    feedLine_t *e;

    for (i = 0; i < FEED_SLOTS; i++) {
        if (CG_FeedExpired(&s_feed[i], now)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // every slot still fading: take the oldest of those on their way out (never a live line - one that was
        // bumped can be older than a line that is already fading)
        for (i = 0; i < FEED_SLOTS; i++) {
            if (!CG_FeedLive(&s_feed[i], now) && (slot < 0 || s_feed[i].born < s_feed[slot].born)) {
                slot = i;
            }
        }
    }
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < FEED_SLOTS; i++) {
            if (s_feed[i].born < s_feed[slot].born) {
                slot = i;
            }
        }
    }
    if (s_cvDebug && s_cvDebug->integer) {
        cgi.Printf("^~^~^ FEEDSHOW t=%d waited=%d cat=%d key=%s\n", now, now - arrived, cat, key);
    }
    e = &s_feed[slot];
    memset(e, 0, sizeof(*e));
    e->active  = 1;
    e->seq     = ++s_feedSeq;
    e->cat     = cat;
    e->count   = count > 0 ? count : 1;
    e->born    = now;
    e->refresh = now;
    e->hold    = CG_FeedHoldMs(cat);
    Q_strncpyz(e->key, key, sizeof(e->key));
    Q_strncpyz(e->text, text, sizeof(e->text));
    Q_strncpyz(e->icon, icon, sizeof(e->icon));
    e->hintMark = (cat == 4) ? 1 : 0;
    if (cat == 3 && icon[0]) {
        e->hIcon = cgi.R_RegisterShaderNoMip(icon);
    }
    CG_FeedWrap(e);
}

/*
CG_CoopFeedIntercept - called from CG_ServerCommand for "print"/"hudprint" BEFORE the text reaches the console.
Returns qtrue when the message was a feed tag and has been handled (the caller must not print it again).
*/
qboolean CG_CoopFeedIntercept(const char *msg)
{
    const char *p = msg;
    int         cat, n;
    char        key[FEED_KEY];
    char        icon[FEED_ICON];
    char        text[FEED_TEXT];

    if (!p) {
        return qfalse;
    }
    // ONLY the two colour bytes a server script print uses (Player iprint / iprintln:  yellow,  white). Chat
    // (), team chat and obituaries (/) carry player-typed text and names, so a "~f6:0~" typed by a player
    // can never become a feed line or write a coop_lm cvar.
    if ((unsigned char)*p != 1 && (unsigned char)*p != 3) {
        return qfalse;
    }
    p++;
    if (p[0] != '~' || p[1] != 'f' || p[2] < '0' || p[2] > '9') {
        return qfalse;
    }
    CG_CoopFeedInit();
    cat = p[2] - '0';
    p += 3;
    key[0] = icon[0] = 0;
    if (*p == ':') {
        p++;
        for (n = 0; *p && *p != ':' && *p != '~'; p++) {
            if (n < FEED_KEY - 1) {
                key[n++] = *p;
            }
        }
        key[n] = 0;
        if (*p == ':') {
            p++;
            for (n = 0; *p && *p != '~'; p++) {
                if (n < FEED_ICON - 1) {
                    icon[n++] = *p;
                }
            }
            icon[n] = 0;
        }
    }
    if (*p != '~') {
        return qfalse; // not a well-formed tag: let it print as it is
    }
    p++;
    CG_FeedClean(p, text, sizeof(text));

    if (s_cvDebug && s_cvDebug->integer) {
        cgi.Printf("^~^~^ FEED cat=%d key=%s t=%d %s\n", cat, key, cg.time, text);
    }

    if (cat == 5) {
        cgi.Cvar_Set("coop_armoryStatus", text);
        cgi.Printf("%s\n", text);
        return qtrue;
    }
    if (cat == 6) {
        if (key[0] >= '0' && key[0] <= '9' && !key[1]) {
            char name[16];
            Com_sprintf(name, sizeof(name), "coop_lm%c", key[0]);
            cgi.Cvar_Get(name, "", CVAR_ARCHIVE);
            cgi.Cvar_Set(name, text);
        }
        return qtrue;
    }

    if (!text[0]) {
        return qtrue;
    }
    // filters first, so a filtered line is neither drawn nor mirrored (the console always keeps it)
    if (CG_CoopHardcoreActive() && cat != 0) {
        cgi.Printf("%s\n", text); // Hardcore: only the amber threat alerts reach the screen
        return qtrue;
    }
    if (cat == 4 && (!s_cvHints->integer || !key[0] || CG_FeedHintSeen(key))) {
        cgi.Printf("%s\n", text);
        return qtrue;
    }
    if (s_cvFeed->integer <= 0) {
        // feed off: the threat alerts (a teammate down, an enemy wave) still reach the old box, non-bold
        cgi.Printf((cat == 0) ? "\x01%s\n" : "%s\n", text);
        return qtrue;
    }
    // no colour byte = console only, never the gmbox; coop_feed 2 = also mirror into the old box, non-bold, no beep
    cgi.Printf((s_cvFeed->integer == 2) ? "\x01%s\n" : "%s\n", text);
    CG_FeedPush(cat, key, icon, text);
    return qtrue;
}

static void CG_FeedTick(const float *col)
{
    cgi.R_SetColor(col);
}

/*
CG_DrawCoopFeed - called every frame from the 2D HUD pass (cg_drawtools.cpp, next to the stamina arc).
cineHide = the coop cinematic HUD suppression is active.
*/
/*
====================
CG_CoopScoreHudMirror

HZM coop [bug-3485] ui/hud_score.urc (the retail "Score: n (n)" line) is shown in multiplayer only. The menu file
is shared by coop and MP, and the MP/coop isolation contract (docs/tools/check_mp_isolation.py clause 7) does not
let a shared file name an MP cvar - so the HUD is gated on this neutral cvar, and the one place that knows which
kind of session this is keeps it current. Written only when it changes.
====================
*/
static void CG_CoopScoreHudMirror(void)
{
    static cvar_t *cvMp = NULL, *cvHud = NULL;
    int            want;

    if (!cvMp) {
        // HZM-MP-BEGIN(mp_score_hud)
        // read-only: which kind of session this is, so the shared Score HUD shows in multiplayer only
        cvMp  = cgi.Cvar_Get("coop_mp_session", "0", 0);
        // HZM-MP-END(mp_score_hud)
        cvHud = cgi.Cvar_Get("cg_hudScore", "0", 0);
    }
    want = cvMp->integer ? 1 : 0;
    if (cvHud->integer != want) {
        cgi.Cvar_Set("cg_hudScore", want ? "1" : "0");
    }
}

/*
====================
CG_DrawCoopPinned

HZM coop [bug-3485, user 2026-10-11: "Make the enemies remaining text smaller"] The enemies-remaining readout
(coop_mod/enemycount.scr) as ONE pinned line at the top right, in the feed's own type: verdana-12 in hi-res units,
i.e. 12 px at 1080p and below and scaled above - the script HUD could only draw that font at virtual-480 scale,
1.5x to 2.25x larger, and has no smaller font. The server sets the text with `set coop_ecLine <text>` and clears
it with "-"; CG_Init clears it on every connect and map load (cg_main.c), so a line can never outlive
the mission that set it.
====================
*/
/*
[bug-3522] The right edge the pinned line may use, in pixels. The line shares the top right corner with the script
HUD: the retail mission-item icons (global/items.scr slots 21-26, 64 units square from 8 units down, stepping left
from the edge) and the officer system's binoculars / paradrop icons. The line was always right-aligned 16 units from
the edge, i.e. on top of the first icon's box; on a small screen its last characters sat on the icon art. It now ends
left of the leftmost visible element that reaches into its row (elements in the right half of the screen only - a
centred toast or a full-width overlay is not "an icon in the corner"). A faded icon still counts, so the line does
not slide about as the HUD fades in and out.
*/
static float CG_CoopPinnedRightPx(float topPx, float bottomPx, float *lowPx)
{
    float W     = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
    float sx    = cgs.uiHiResScale[0], sy = cgs.uiHiResScale[1];
    float right = W - 16.0f * sx;
    int   i;

    for (i = 0; i < MAX_HUDDRAW_ELEMENTS; i++) {
        const hdelement_t *e = &cgi.HudDrawElements[i];
        float              fX, fY, fW, fH, kx, ky;

        if ((!e->hShader && !e->string[0]) || !e->vColor[3]) {
            continue;
        }
        // the same placement maths as CG_HudDrawElements (cg_drawtools.cpp)
        fX = (float)e->iX;
        fY = (float)e->iY;
        fW = (float)e->iWidth;
        fH = (float)e->iHeight;
        if (e->bVirtualScreen) {
            kx = W / 640.0f;
            ky = H / 480.0f;
            if (e->iHorizontalAlign == HUD_ALIGN_X_CENTER) {
                fX += 320.0f - fW * 0.5f;
            } else if (e->iHorizontalAlign == HUD_ALIGN_X_RIGHT) {
                fX += 640.0f;
            }
            if (e->iVerticalAlign == HUD_ALIGN_Y_CENTER) {
                fY += 240.0f - fH * 0.5f;
            } else if (e->iVerticalAlign == HUD_ALIGN_Y_BOTTOM) {
                fY += 480.0f;
            }
            fX *= kx;
            fY *= ky;
            fW *= kx;
            fH *= ky;
        } else {
            fX *= sx;
            fY *= sy;
            fW *= sx;
            fH *= sy;
            if (e->iHorizontalAlign == HUD_ALIGN_X_CENTER) {
                fX += W * 0.5f - fW * 0.5f;
            } else if (e->iHorizontalAlign == HUD_ALIGN_X_RIGHT) {
                fX += W;
            }
            if (e->iVerticalAlign == HUD_ALIGN_Y_CENTER) {
                fY += H * 0.5f - fH * 0.5f;
            } else if (e->iVerticalAlign == HUD_ALIGN_Y_BOTTOM) {
                fY += H;
            }
        }
        if (fH < 14.0f * sy) {
            fH = 14.0f * sy; // a text element has no height of its own
        }
        if (fX < W * 0.5f || fX >= W || fY >= bottomPx || fY + fH <= topPx) {
            continue;
        }
        if (fX - 8.0f * sx < right) {
            right = fX - 8.0f * sx;
        }
        if (lowPx && fY + fH > *lowPx) {
            *lowPx = fY + fH; // the lowest edge of what is in the way
        }
    }
    return right;
}

static void CG_DrawCoopPinned(void)
{
    static cvar_t *cvLine = NULL;
    fontheader_t  *font;
    char           text[96];
    vec4_t         c;
    float          x, y;

    if (!cvLine) {
        cvLine = cgi.Cvar_Get("coop_ecLine", "", 0);
    }
    // "" or "-" = nothing to show ("-" because a script cannot send an empty value: stufftext has no inner quotes)
    if (!cvLine->string[0] || (cvLine->string[0] == '-' && !cvLine->string[1])) {
        return;
    }
    font = CG_FeedFont();
    if (!font) {
        return;
    }
    CG_FeedClean(cvLine->string, text, sizeof(text));
    if (!text[0]) {
        return;
    }
    {
        float sx = cgs.uiHiResScale[0], sy = cgs.uiHiResScale[1];
        float W = (float)cgs.glconfig.vidWidth, H = (float)cgs.glconfig.vidHeight;
        float tw = CG_FeedTextW(text, -1), low = 0.0f, right, compassRight;

        y     = 8.0f;
        right = CG_CoopPinnedRightPx((y - 2.0f) * sy, (y + 16.0f) * sy, &low);
        x     = right / sx - tw;
        // Beside the icons only when that keeps it off the compass bar, which ends at W/2 + half its width
        // (cg_drawtools.cpp CG_CompassBarGeometry: min(0.42 W, 0.75 H)). Otherwise - 640x480 with two icons,
        // any size with five or six - the line goes UNDER the icons, back at the right edge.
        compassRight = W * 0.5f + 0.5f * ((0.42f * W < 0.75f * H) ? 0.42f * W : 0.75f * H) + 10.0f * sx;
        if (low > 0.0f && x * sx < compassRight) {
            y     = low / sy + 3.0f;
            low   = 0.0f;
            right = CG_CoopPinnedRightPx((y - 2.0f) * sy, (y + 16.0f) * sy, &low);
            x     = right / sx - tw;
        }
        if (x < 4.0f) {
            x = 4.0f;
        }
    }
    c[0] = c[1] = c[2] = 0.0f;
    c[3]                = 0.85f;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, text, x + 1.0f, y + 1.0f, -1, cgs.uiHiResScale);
    c[0] = 1.00f;
    c[1] = 0.86f;
    c[2] = 0.55f;
    c[3] = 0.95f;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, text, x, y, -1, cgs.uiHiResScale);
    cgi.R_SetColor(NULL);
}

/*
====================
CG_DrawCoopItems

HZM coop [user 2026-10-11, bug-3495] "For the icons for the medkit, ammo box, and sandbags, can we use subscript for
their amounts and underneath of them put the key that is bound to use them? Please keep all of it small so it isn't
obnoxious and make sure it doesn't overlap with anything else on other resolutions."

Was: three script-HUD icons with a verdana-14 number beside each, drawn at virtual-480 scale (31 px tall digits at
1080p), and no way to show a key - a script cannot read the player's bindings. Now, per item that the server has
published a count for (client cvars coop_hudMed / coop_hudCov / coop_hudAmmo; "" or "-" = not shown):

    [icon]          the same art, a little smaller than before
          2         the amount, in the feed's small type, hung off the icon's lower right corner
      ]             the key bound to it, dimmer, centred underneath (nothing when it is unbound)

Everything is laid out in PIXELS from the bottom-left corner: icon size and the row's height above the bottom follow
the screen HEIGHT (so the row sits where the old one did, just under the feed's bottom edge at 70 virtual units, at
every aspect ratio), text is the feed's own verdana-12 in hi-res units (12 px up to 1080p, scaled above), and the
column pitch is whichever is larger - the icon plus a gap, or room for a six-character key name - so a key label
can never run into its neighbour at any resolution; a longer name is cut to fit.
====================
*/
#define COOPITEM_ICON_V   14.0f // icon edge, virtual-480 units
#define COOPITEM_TOP_V    66.0f // top of the icons above the bottom of the screen, virtual-480 units (feed: 70)
#define COOPITEM_LEFT_V   6.0f
#define COOPITEM_GAP_V    12.0f
#define COOPITEM_MIN_ICON 14.0f // pixels
#define COOPITEM_KEY_PX   48.0f // room a key label is guaranteed, in hi-res units

typedef struct {
    const char *cvarName;
    const char *bindCmd;
    const char *shaderName;
    cvar_t     *cv;
    qhandle_t   hShader;
    char        key[16];
} coopItem_t;

// [user 2026-10-12] "the new text on the left side also seems to overlap with the paradrop and airstrike icons when
// you get those" / "those icons could also be made smaller and higher def like your new icons you just did with
// hotkeys". The two officer rewards were script HUD pairs (officer.scr slots 50-53) in this same corner. They are
// entries of this row now: coop_hudBinoc = bombing-run strikes left (the binoculars; selected with the pistol
// key), coop_hudSmoke = "y" while the signal smoke is carried (no amount; thrown from the grenade key). Only the
// carrier's client is sent a value, so the entry is absent for everyone else. Art: docs/tools/gen_reward_icons.py.
#define COOPITEM_N 5
static coopItem_t s_coopItems[COOPITEM_N] = {
    {"coop_hudMed",   "append name ,8mk",       "textures/hud/coop_medkit_icon",     NULL, 0, ""},
    {"coop_hudCov",   "append name ,9cv",       "textures/hud/coop_cover_icon",      NULL, 0, ""},
    {"coop_hudAmmo",  "append name ,abx",       "textures/hud/coop_ammobox_icon",    NULL, 0, ""},
    {"coop_hudBinoc", "useweaponclass pistol",  "textures/hud/coop_reward_strike",   NULL, 0, ""},
    {"coop_hudSmoke", "useweaponclass grenade", "textures/hud/coop_reward_paradrop", NULL, 0, ""},
};

// A key name as it is shown under an icon: upper case, and the long ones shortened so that what tells two keys
// apart survives ("MOUSE4" was being cut to "MOUSE"). MOUSE4 -> M4, MWHEELUP -> WH-UP, KP_ENTER -> KP ENTER,
// PAD0_DPAD_UP -> D-UP, PAD0_LEFTSTICK_UP -> LS UP. Whatever is still too wide is cut by the caller.
static void CG_CoopItemKeyLabel(const char *nm, char *out, int outSize)
{
    static const struct {
        const char *from, *to;
    } sub[] = {
        {"PAD0_", ""}, {"MWHEELUP", "WH-UP"}, {"MWHEELDOWN", "WH-DN"}, {"MOUSE", "M"},
        {"DPAD_RIGHT", "D-RT"}, {"DPAD_LEFT", "D-LT"}, {"DPAD_DOWN", "D-DN"}, {"DPAD_UP", "D-UP"}, {"DPAD_", "D-"},
        {"LEFTSTICK", "LS"}, {"RIGHTSTICK", "RS"}, {"SHOULDER", "SH"}, {"TRIGGER", "TR"}, {"LEFTARROW", "LEFT"},
        {"RIGHTARROW", "RIGHT"}, {"UPARROW", "UP"}, {"DOWNARROW", "DOWN"}, {"SEMICOLON", ";"},
    };
    char up[48];
    int  i, n = 0, k;

    for (i = 0; nm[i] && i < (int)sizeof(up) - 1; i++) {
        up[i] = (char)toupper((unsigned char)nm[i]);
    }
    up[i] = 0;
    i     = 0;
    while (up[i] && n < outSize - 1) {
        for (k = 0; k < (int)(sizeof(sub) / sizeof(sub[0])); k++) {
            int len = (int)strlen(sub[k].from);

            if (!strncmp(up + i, sub[k].from, len)) {
                const char *t = sub[k].to;

                while (*t && n < outSize - 1) {
                    out[n++] = *t++;
                }
                i += len;
                break;
            }
        }
        if (k < (int)(sizeof(sub) / sizeof(sub[0]))) {
            continue;
        }
        out[n++] = (up[i] == '_') ? ' ' : up[i];
        i++;
    }
    out[n] = 0;
}

// [bug-3521, user 2026-10-11 "make sure it doesn't overlap with anything else on other resolutions"] hi-res units
// above the bottom that stay clear. Was 48 = the retail team badge alone, and the row then sat INSIDE the health
// cluster whenever that cluster was visible: the stamina arc (cg_drawtools.cpp CG_DrawStaminaArc) starts 95 units
// up and the health cross of ui/hud_health.urc 92 up, both in hi-res units, x 32..288. The bug-3495 stills never
// showed it because the cluster fades out on a calm player. The cluster does not scale below 1080p and the row
// does, so the two met at every height under about 1070 px (covered icons and key labels at 1280x720, the whole
// row at 640x480). 98 = the arc's top edge + 3.
#define COOPITEM_FLOOR_U  98.0f

/*
The row's geometry in pixels, shared with the feed. The top follows the screen HEIGHT (66 virtual units up, where
the old script icons were), unless that would put the key labels on the retail health cluster, which is laid out in
fixed hi-res units - below about 1070 px of height it does, and then the row is lifted to clear it (the feed sits
on top of the row and rises with it).
*/
static float CG_CoopItemsTopPx(float *iconPxOut)
{
    float sy = cgs.uiHiResScale[1] > 0.0f ? cgs.uiHiResScale[1] : 1.0f;
    float vs = (float)cgs.glconfig.vidHeight / 480.0f;
    float iconPx, up, need;

    iconPx = COOPITEM_ICON_V * vs;
    if (iconPx < COOPITEM_MIN_ICON) {
        iconPx = COOPITEM_MIN_ICON;
    }
    up   = COOPITEM_TOP_V * vs;                             // where it wants to be
    need = COOPITEM_FLOOR_U * sy + 20.0f * sy + iconPx;     // cluster + (gap and one line of label) + icon
    if (up < need) {
        up = need;
    }
    if (iconPxOut) {
        *iconPxOut = iconPx;
    }
    return (float)cgs.glconfig.vidHeight - up;
}

static void CG_CoopItemKeys(void)
{
    static int nextMs = 0;
    int        i, k1, k2;

    if (cg.time < nextMs && cg.time + 2000 > nextMs) {
        return;
    }
    nextMs = cg.time + 1000; // bindings change in a menu, never mid-frame: once a second is plenty
    for (i = 0; i < COOPITEM_N; i++) {
        coopItem_t *it = &s_coopItems[i];
        const char *nm;

        it->key[0] = 0;
        k1 = k2 = -1;
        cgi.Key_GetKeysForCommand(it->bindCmd, &k1, &k2);
        if (k1 < 0) {
            continue;
        }
        nm = cgi.Key_KeynumToBindString(k1);
        // two bindings: show the keyboard / mouse one. A gamepad control bound to the same action (the mod binds the
        // d-pad by default) is not what a player at a keyboard is looking for, and its name is long.
        if (k2 >= 0 && nm && !Q_stricmpn(nm, "PAD", 3)) {
            const char *nm2 = cgi.Key_KeynumToBindString(k2);

            if (nm2 && nm2[0] && Q_stricmpn(nm2, "PAD", 3)) {
                nm = nm2;
            }
        }
        if (!nm || !nm[0]) {
            continue;
        }
        CG_CoopItemKeyLabel(nm, it->key, sizeof(it->key));
    }
}

#define COOPITEM_WAKE_MS 3500 // how long a changed amount keeps the readout up while the HUD is faded

static void CG_DrawCoopItems(void)
{
    fontheader_t *font = CG_FeedFont();
    float         sx   = cgs.uiHiResScale[0], sy = cgs.uiHiResScale[1];
    float         vs   = (float)cgs.glconfig.vidHeight / 480.0f;
    float         iconPx, topPx, pitchPx, xPx, textH;
    vec4_t        c;
    int           i, shown = 0;
    float         fade;

    if (!font || sx <= 0.0f || sy <= 0.0f) {
        return;
    }
    // a briefing map has no kit to show, and the ready gate's roster is drawn in this corner there
    if (strstr(cgs.mapname, "briefing/")) {
        return;
    }
    CG_CoopItemKeys();

    // [user 2026-10-12] the readout fades with the rest of the HUD (it sat in the feed's fade-exempt file and never
    // did). ui_hudAlpha is the HUD auto-fade as cg_drawtools.cpp publishes it (1 when coop_hudFade is off). A change
    // in any amount brings the readout back for COOPITEM_WAKE_MS by itself - using a medkit does not otherwise wake
    // the HUD - and it then eases down to the HUD's own value again.
    {
        static cvar_t *pHudA = NULL;
        static char    last[COOPITEM_N][8];
        static int     wakeUntil = 0;
        float          wake = 0.0f;

        if (!pHudA) {
            pHudA = cgi.Cvar_Get("ui_hudAlpha", "1", 0);
        }
        for (i = 0; i < COOPITEM_N; i++) {
            coopItem_t *it = &s_coopItems[i];

            if (!it->cv) {
                it->cv = cgi.Cvar_Get(it->cvarName, "", 0);
            }
            if (Q_strncmp(last[i], it->cv->string, sizeof(last[i]) - 1)) {
                Q_strncpyz(last[i], it->cv->string, sizeof(last[i]));
                wakeUntil = cg.time + COOPITEM_WAKE_MS;
            }
        }
        if (cg.time < wakeUntil) {
            wake = (float)(wakeUntil - cg.time) / 600.0f; // the last 0.6 s of the wake is the ease-out
            if (wake > 1.0f) {
                wake = 1.0f;
            }
        } else if (wakeUntil - cg.time > COOPITEM_WAKE_MS) {
            wakeUntil = 0; // cg.time went backwards (new map)
        }
        fade = pHudA->value;
        if (fade < 0.0f) {
            fade = 0.0f;
        }
        if (fade > 1.0f) {
            fade = 1.0f;
        }
        if (wake > fade) {
            fade = wake;
        }
        if (fade <= 0.02f) {
            return;
        }
    }

    topPx   = CG_CoopItemsTopPx(&iconPx);
    pitchPx = iconPx + COOPITEM_GAP_V * vs;
    if (pitchPx < COOPITEM_KEY_PX * sx) {
        pitchPx = COOPITEM_KEY_PX * sx;
    }
    textH = 12.0f * sy;
    xPx   = COOPITEM_LEFT_V * vs;

    for (i = 0; i < COOPITEM_N; i++) {
        coopItem_t *it = &s_coopItems[i];
        const char *v;
        char        num[8], key[16];
        float       tw, tx, ty;
        int         n;

        if (!it->cv) {
            it->cv = cgi.Cvar_Get(it->cvarName, "", 0);
        }
        v = it->cv->string;
        if (!v[0] || v[0] == '-') {
            continue;
        }
        if (!it->hShader) {
            it->hShader = cgi.R_RegisterShaderNoMip(it->shaderName);
        }

        // icon
        c[0] = c[1] = c[2] = 1.0f;
        c[3]                = 0.92f * fade;
        cgi.R_SetColor(c);
        cgi.R_DrawStretchPic(xPx, topPx, iconPx, iconPx, 0.0f, 0.0f, 1.0f, 1.0f, it->hShader);

        // amount: a subscript - starts at the icon's right edge, sits half a line below its bottom edge
        Q_strncpyz(num, v, sizeof(num));
        for (n = 0; num[n]; n++) {
            if (num[n] < '0' || num[n] > '9') {
                num[n] = 0; // digits only: this text came from a server
                break;
            }
        }
        if (num[0]) {
            tx = (xPx + iconPx - 1.0f) / sx;
            ty = (topPx + iconPx - textH * 0.55f) / sy;
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.85f * fade;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, num, tx + 1.0f, ty + 1.0f, -1, cgs.uiHiResScale);
            c[0] = c[1] = c[2] = (num[0] == '0' && !num[1]) ? 0.62f : 1.0f; // an empty slot reads dimmer
            c[3]                = 0.95f * fade;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, num, tx, ty, -1, cgs.uiHiResScale);
        }

        // bound key: centred under the icon, cut to the column so it can never touch the next one
        Q_strncpyz(key, it->key, sizeof(key));
        n = (int)strlen(key);
        while (n > 0 && CG_FeedTextW(key, -1) * sx > pitchPx - 4.0f) {
            key[--n] = 0;
        }
        if (key[0]) {
            tw = CG_FeedTextW(key, -1);
            tx = (xPx + iconPx * 0.5f) / sx - tw * 0.5f;
            if (tx * sx < 2.0f) {
                tx = 2.0f / sx;
            }
            ty = (topPx + iconPx + textH * 0.50f) / sy;
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.70f * fade;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, key, tx + 1.0f, ty + 1.0f, -1, cgs.uiHiResScale);
            c[0] = 0.80f;
            c[1] = 0.76f;
            c[2] = 0.62f;
            c[3] = 0.78f * fade;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, key, tx, ty, -1, cgs.uiHiResScale);
        }

        xPx += pitchPx;
        shown++;
    }
    if (shown) {
        cgi.R_SetColor(NULL);
    }
}

// where the feed drew this frame, in pixels: the use-to-loot prompt (CG_DrawCoopLoot, below) steps up out of its
// way on a screen small enough for the two to meet (640x480 with four or more feed rows)
static int   s_feedBoxMs      = -1;
static float s_feedBoxTopPx   = 0.0f;
static float s_feedBoxRightPx = 0.0f;

void CG_DrawCoopFeed(qboolean cineHide)
{
    static const float tick[5][3] = {
        {1.00f, 0.66f, 0.20f}, // 0 alert - amber
        {0.80f, 0.68f, 0.40f}, // 1 team - brass
        {0.62f, 0.67f, 0.72f}, // 2 self - steel
        {0.55f, 0.78f, 0.45f}, // 3 pickup - green
        {0.55f, 0.72f, 0.90f}, // 4 hint - pale blue
    };
    int           i, j, k, now, order[FEED_SLOTS], n = 0, dt;
    float         sx, sy, unitsH, x0, bottom, target, a, ex, ey, rowsH, w, maxW;
    vec4_t        c;
    fontheader_t *font;

    CG_CoopFeedInit();
    CG_CoopScoreHudMirror();
    if (!cineHide) {
        CG_DrawCoopItems();         // [bug-3495] medkit / sandbag / ammo box: icon, subscript amount, bound key
        CG_DrawCoopPinned();        // [bug-3485] not part of the feed: coop_feed 0 does not hide it
    }
    now = cg.time;
    dt  = now - s_feedLastT;
    // hidden by drawhud 0 or a cinematic: hold the lines - every clock is moved along with the time, so nothing ages,
    // fades or gives up its row. Not for a gap of more than a second between two calls (a map load).
    i = (cineHide || (cg.snap && (cg.snap->ps.pm_flags & PMF_NO_HUD) && !(cg.snap->ps.pm_flags & PMF_INTERMISSION))) ? 1 : 0;
    if (i) {
        if (dt > 0 && dt <= 1000) {
            for (j = 0; j < FEED_SLOTS; j++) {
                if (s_feed[j].active) {
                    s_feed[j].born    += dt;
                    s_feed[j].refresh += dt;
                    if (s_feed[j].dieAt) {
                        s_feed[j].dieAt += dt;
                    }
                }
            }
        }
    } else if (s_feedHidden) {
        // the HUD is back: what was held starts its clock now; a line that was already leaving is not brought back
        k = 0;
        for (j = 0; j < FEED_SLOTS; j++) {
            if (!s_feed[j].active) {
                continue;
            }
            if (!CG_FeedLive(&s_feed[j], now)) {
                s_feed[j].active = 0;
                continue;
            }
            s_feed[j].refresh = now;
            k++;
        }
        // all start now, in the order they arrived (the rows are sorted by born): 1 ms apart is enough
        for (j = 0; j < FEED_SLOTS; j++) {
            int m;

            order[j] = 0; // how many held rows are NEWER than row j
            if (!s_feed[j].active) {
                continue;
            }
            for (m = 0; m < FEED_SLOTS; m++) {
                // by arrival, not by born: every line placed while hidden has had its born moved to the same frame
                if (m != j && s_feed[m].active && s_feed[m].seq > s_feed[j].seq) {
                    order[j]++;
                }
            }
        }
        for (j = 0; j < FEED_SLOTS; j++) {
            if (s_feed[j].active) {
                s_feed[j].born = now - order[j];
            }
        }
        if (s_cvDebug && s_cvDebug->integer) {
            cgi.Printf("^~^~^ FEEDHELD release rows=%d waiting=%d t=%d\n", k, s_pendN, now);
        }
    }
    s_feedHidden = i;
    CG_FeedDrain(now);
    if (dt < 0 || dt > 250) {
        dt = 16;
    }
    s_feedLastT = now;

    // collect visible entries, oldest first
    for (i = 0; i < FEED_SLOTS; i++) {
        if (!s_feed[i].active) {
            continue;
        }
        // expired (the fade-OUT is over - never the alpha 0 at the very start of the fade-IN), or cg.time ran
        // backwards over a map load
        if (s_feed[i].born > now + 1000 || CG_FeedExpired(&s_feed[i], now)) {
            s_feed[i].active = 0;
            continue;
        }
        order[n++] = i;
    }
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && (s_feed[order[j]].born < s_feed[order[j - 1]].born
                             || (s_feed[order[j]].born == s_feed[order[j - 1]].born && s_feed[order[j]].seq < s_feed[order[j - 1]].seq)); j--) {
            k            = order[j];
            order[j]     = order[j - 1];
            order[j - 1] = k;
        }
    }
    if (!n) {
        return;
    }
    if (!cg.snap || (cg.snap->ps.pm_flags & (PMF_NO_HUD | PMF_INTERMISSION)) || cineHide) {
        static int s_hideLog = 0;
        if (s_cvDebug && s_cvDebug->integer && (now - s_hideLog > 1000 || now < s_hideLog)) {
            s_hideLog = now;
            cgi.Printf(
                "^~^~^ FEEDHIDE n=%d snap=%d nohud=%d inter=%d cine=%d\n",
                n,
                cg.snap ? 1 : 0,
                (cg.snap && (cg.snap->ps.pm_flags & PMF_NO_HUD)) ? 1 : 0,
                (cg.snap && (cg.snap->ps.pm_flags & PMF_INTERMISSION)) ? 1 : 0,
                cineHide ? 1 : 0
            );
        }
        return; // keep ageing, draw nothing
    }
    font = CG_FeedFont();
    if (s_cvDebug && s_cvDebug->integer) {
        static int s_drawLog = 0;
        if (now - s_drawLog > 1000 || now < s_drawLog) {
            s_drawLog = now;
            cgi.Printf("^~^~^ FEEDDRAW n=%d font=%d vid=%dx%d\n", n, font ? 1 : 0, cgs.glconfig.vidWidth, cgs.glconfig.vidHeight);
        }
    }
    if (!font) {
        return;
    }

    sx     = cgs.uiHiResScale[0];
    sy     = cgs.uiHiResScale[1];
    unitsH = (float)cgs.glconfig.vidHeight / sy;
    x0     = s_cvX->value;
    // bottom edge in feed units: coop_feedYv virtual-480 units above the bottom of the screen (see CG_CoopFeedInit)
    bottom = ((float)cgs.glconfig.vidHeight - s_cvY->value * ((float)cgs.glconfig.vidHeight / 480.0f)) / sy;
    // [bug-3495] on a small screen the item row is lifted clear of the team badge: the feed stays above it
    {
        float itemsTop = (CG_CoopItemsTopPx(NULL) - 4.0f) / sy;

        if (bottom > itemsTop) {
            bottom = itemsTop;
        }
    }

    // newest at the bottom: walk newest -> oldest, stacking upward
    target = 0.0f;
    for (k = n - 1; k >= 0; k--) {
        feedLine_t *e = &s_feed[order[k]];
        float       f;
        rowsH = (float)e->nrows * FEED_ROW_H;
        if (e->cat == 3 && rowsH < FEED_ICON_SZ) {
            rowsH = FEED_ICON_SZ;
        }
        if (!e->placed) {
            e->y      = target;
            e->placed = 1;
        } else {
            f = (float)dt / 90.0f; // ~90 ms slide
            if (f > 1.0f) {
                f = 1.0f;
            }
            e->y += (target - e->y) * f;
        }
        a = CG_FeedAlpha(e, now);
        ey = bottom - e->y - rowsH;
        if (e->hintMark && a > 0.0f) {
            e->hintMark = 0;
            CG_FeedHintMark(e->key); // seen = actually drawn, not merely received (a cinematic may hide it)
        }

        // width of the widest row (+ the counter)
        maxW = 0.0f;
        for (j = 0; j < e->nrows; j++) {
            w = CG_FeedTextW(e->text + e->rowStart[j], e->rowLen[j]);
            if (w > maxW) {
                maxW = w;
            }
        }
        if (e->count > 1) {
            char cnt[16];
            Com_sprintf(cnt, sizeof(cnt), " x%d", e->count);
            maxW += CG_FeedTextW(cnt, -1);
        }
        ex = x0 + 6.0f + ((e->cat == 3 && e->hIcon) ? (FEED_ICON_SZ + 4.0f) : 0.0f);

        if (a > 0.05f) {
            if (s_feedBoxMs != now) {
                s_feedBoxMs      = now;
                s_feedBoxTopPx   = (float)cgs.glconfig.vidHeight;
                s_feedBoxRightPx = 0.0f;
            }
            if ((ey - 2.0f) * sy < s_feedBoxTopPx) {
                s_feedBoxTopPx = (ey - 2.0f) * sy;
            }
            if ((ex + maxW + 6.0f) * sx > s_feedBoxRightPx) {
                s_feedBoxRightPx = (ex + maxW + 6.0f) * sx;
            }
        }

        // plate
        c[0] = 0.0f;
        c[1] = 0.0f;
        c[2] = 0.0f;
        c[3] = 0.38f * a;
        cgi.R_SetColor(c);
        cgi.R_DrawBox(x0 * sx, (ey - 2.0f) * sy, (ex - x0 + maxW + 6.0f) * sx, (rowsH + 3.0f) * sy);

        // category tick (brightens for a moment when the line is bumped)
        c[0] = tick[e->cat < 5 ? e->cat : 2][0];
        c[1] = tick[e->cat < 5 ? e->cat : 2][1];
        c[2] = tick[e->cat < 5 ? e->cat : 2][2];
        c[3] = a;
        if (now - e->refresh < FEED_BUMP && e->count > 1) {
            c[0] = c[1] = c[2] = 1.0f;
        }
        CG_FeedTick(c);
        cgi.R_DrawBox(x0 * sx, (ey - 2.0f) * sy, 2.0f * sx, (rowsH + 3.0f) * sy);

        // pickup icon: pops in at 1.3x and settles
        if (e->cat == 3 && e->hIcon) {
            float sc = 1.0f, isz;
            int   ti = now - e->born;
            if (ti < 220) {
                sc = 1.3f - 0.3f * ((float)ti / 220.0f);
            }
            isz  = FEED_ICON_SZ * sc;
            c[0] = c[1] = c[2] = 1.0f;
            c[3]                = a;
            cgi.R_SetColor(c);
            cgi.R_DrawStretchPic(
                (x0 + 5.0f + (FEED_ICON_SZ - isz) * 0.5f) * sx,
                (ey + (rowsH - isz) * 0.5f) * sy,
                isz * sx,
                isz * sy,
                0.0f,
                0.0f,
                1.0f,
                1.0f,
                e->hIcon
            );
        }

        // text rows: 1-unit drop shadow, then the line
        for (j = 0; j < e->nrows; j++) {
            char  row[FEED_TEXT];
            float ry = ey + (float)j * FEED_ROW_H;
            if (e->cat == 3 && e->nrows == 1) {
                ry = ey + (rowsH - FEED_ROW_H) * 0.5f;
            }
            Q_strncpyz(row, e->text + e->rowStart[j], e->rowLen[j] + 1 < (int)sizeof(row) ? e->rowLen[j] + 1 : (int)sizeof(row));
            c[0] = c[1] = c[2] = 0.0f;
            c[3]                = 0.85f * a;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, row, ex + 1.0f, ry + 1.0f, -1, cgs.uiHiResScale);
            c[0] = 0.94f;
            c[1] = 0.92f;
            c[2] = 0.86f;
            c[3] = a;
            cgi.R_SetColor(c);
            cgi.R_DrawString(font, row, ex, ry, -1, cgs.uiHiResScale);
            if (j == e->nrows - 1 && e->trunc) {
                c[0] = 0.94f;
                c[1] = 0.92f;
                c[2] = 0.86f;
                cgi.R_SetColor(c);
                cgi.R_DrawString(font, "...", ex + CG_FeedTextW(row, -1), ry, -1, cgs.uiHiResScale);
            }
            if (j == e->nrows - 1 && e->count > 1) {
                char cnt[16];
                Com_sprintf(cnt, sizeof(cnt), " x%d", e->count);
                c[0] = 0.96f;
                c[1] = 0.78f;
                c[2] = 0.40f;
                cgi.R_SetColor(c);
                cgi.R_DrawString(font, cnt, ex + CG_FeedTextW(row, -1), ry, -1, cgs.uiHiResScale);
            }
        }
        target += rowsH + FEED_GAP + 3.0f;
    }
    cgi.R_SetColor(NULL);
}

/*
====================
CG_DrawCoopLoot

HZM coop [user 2026-10-05 / 2026-10-12] USE-TO-LOOT prompt (fgame Player::TickCoopLoot decides everything; this only
draws it). "If you can do pickup / search icons instead that would be great ... Think of the original call of duty
icons" - so the prompt is an icon group, not a sentence: centred, a little under the crosshair (63.5 % of the way
down, where most shooters put it), and nothing at all when the server names no target.

    [gun silhouette]        [3 rounds]              [3 rounds]              [magnifier]
     [F] Kar98k             [F] ====                [F] ====                [F] ====
     tap = take the gun     hold = take its ammo    a gun you own           an enemy body: hold to search

A keycap alone is a tap; a keycap with a track beside it is a hold, and the track fills while the key is down. The
gun is its own world model's silhouette (textures/hud/loot/w_*.tga, generated from the meshes by
docs/proposals/use_pickup_2026-10-05/tools/build_icons.py together with the table in cg_coopfeed_loot.h); a gun the
table does not list gets the generic rifle / pistol / SMG picture for its class. The key is the real +use binding.

Wire: two client cvars the server sets change-only (`set coop_*` stufftext, one unquoted integer each):
coop_lootT = kind * 100000 + class * 10000 + CS_WEAPONS index (kind 1 gun not owned, 2 owned, 3 owned + ammo full,
4 not owned + ammo full, 5 body; 0 = nothing), coop_lootH = length in ms of the hold now in progress (0 = none).
CG_Init zeroes both (cg_main.c), so a prompt cannot outlive the map it was raised on.

Sizes follow the screen HEIGHT (1080 = 1.0, never below 0.75 so it stays legible at 640x480); text is the feed's
verdana-12 in hi-res units. Images are registered WITH mipmaps (512 px sources drawn at about a third of that).
====================
*/
typedef struct {
    const char *name;      // CS_WEAPONS item name, finish suffix stripped
    const char *tex;       // textures/hud/loot/<tex>
    const char *shortName; // what the prompt calls it
    float       vw, vh;    // visible width / height as fractions of the 2:1 canvas
} lootArt_t;

static const lootArt_t s_lootArt[] = {
#include "cg_coopfeed_loot.h"
};
#define LOOT_ART_N ((int)(sizeof(s_lootArt) / sizeof(s_lootArt[0])))

static const lootArt_t s_lootGeneric[3] = {
    {"", "coop_loot_gun",    "", 0.965f, 0.414f},
    {"", "coop_loot_pistol", "", 0.434f, 0.598f},
    {"", "coop_loot_mg",     "", 0.816f, 0.629f},
};

static qhandle_t s_lootArtShader[LOOT_ART_N];
static qhandle_t s_lootGenericShader[3];
static qhandle_t s_lootAmmoShader = 0, s_lootSearchShader = 0, s_lootKeyShader = 0;

static void CG_LootBox(float x, float y, float w, float h, float r, float g, float b, float a)
{
    vec4_t c;

    c[0] = r;
    c[1] = g;
    c[2] = b;
    c[3] = a;
    cgi.R_SetColor(c);
    cgi.R_DrawBox(x, y, w, h);
}

// text at a PIXEL position, in the feed font (which is laid out in hi-res units), with the feed's 1-unit shadow
static void CG_LootText(fontheader_t *font, const char *s, float xPx, float yPx, float r, float g, float b, float a)
{
    vec4_t c;
    float  x = xPx / cgs.uiHiResScale[0], y = yPx / cgs.uiHiResScale[1];

    c[0] = c[1] = c[2] = 0.0f;
    c[3]                = 0.85f * a;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, s, x + 1.0f, y + 1.0f, -1, cgs.uiHiResScale);
    c[0] = r;
    c[1] = g;
    c[2] = b;
    c[3] = a;
    cgi.R_SetColor(c);
    cgi.R_DrawString(font, s, x, y, -1, cgs.uiHiResScale);
}

// the keycap: a square frame cut in three so a longer key name stretches the middle, never the corners
static float CG_LootKeycap(fontheader_t *font, const char *key, float x, float y, float h, float a)
{
    float  tw    = CG_FeedTextW(key, -1) * cgs.uiHiResScale[0];
    float  th    = 12.0f * cgs.uiHiResScale[1];
    float  w     = tw + h * 0.62f;
    float  third = (float)((int)(h / 3.0f));
    vec4_t c;

    if (w < h) {
        w = h;
    }
    c[0] = c[1] = c[2] = 1.0f;
    c[3]                = a;
    cgi.R_SetColor(c);
    cgi.R_DrawStretchPic(x, y, third, h, 0.0f, 0.0f, 1.0f / 3.0f, 1.0f, s_lootKeyShader);
    cgi.R_DrawStretchPic(x + third, y, w - 2.0f * third, h, 1.0f / 3.0f, 0.0f, 2.0f / 3.0f, 1.0f, s_lootKeyShader);
    cgi.R_DrawStretchPic(x + w - third, y, third, h, 2.0f / 3.0f, 0.0f, 1.0f, 1.0f, s_lootKeyShader);
    CG_LootText(font, key, x + (w - tw) * 0.5f, y + (h - th) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f, 0.91f, 0.84f, a);
    return w;
}

void CG_DrawCoopLoot(void)
{
    static cvar_t *cvT = NULL, *cvH = NULL;
    static int     shownT = 0, lastMs = 0;
    static float   alpha  = 0.0f;
    static int     holdCv = 0, holdMs = 0, holdStart = 0, holdEnd = 0;
    static float   holdFracEnd = 0.0f;
    static char    keyLabel[16] = "";
    static int     keyNextMs    = 0;
    static int     artT = -1, artIdx = -1, artCls = 0;
    static char    artName[64] = "";
    fontheader_t  *font;
    int            t, h, dt, kind, cls, idx, n = 0, i;
    float          s, W, H, yc, rowY, capH, capW, gap, trackW, trackH, total, x, frac, fillA;
    float          textH;
    struct {
        int       what; // 0 gun, 1 ammo, 2 search
        qhandle_t sh;
        float     dw, dh, vw, r2, gw;
    } g[2];
    const lootArt_t *art       = NULL;
    const char      *shortName = "";

    if (!cvT) {
        cvT = cgi.Cvar_Get("coop_lootT", "0", 0);
        cvH = cgi.Cvar_Get("coop_lootH", "0", 0);
    }
    if (!cg.snap || (cg.snap->ps.pm_flags & PMF_INTERMISSION)) {
        return;
    }
    dt = cg.time - lastMs;
    if (dt < 0 || dt > 250) {
        dt = 16;
    }
    lastMs = cg.time;

    // --- what to show, and the fade (120 ms in, 180 ms out; a change of target swaps without a blink) ------
    t = cvT->integer;
    if (t > 0) {
        shownT = t;
        alpha += (float)dt / 120.0f;
        if (alpha > 1.0f) {
            alpha = 1.0f;
        }
    } else {
        alpha -= (float)dt / 180.0f;
        if (alpha <= 0.0f) {
            alpha  = 0.0f;
            shownT = 0;
        }
    }

    // --- the hold: runs on this client's clock from the moment the server says one began -------------------
    h = cvH->integer;
    if (h != holdCv) {
        if (h > 0) {
            holdStart = cg.time;
            holdMs    = h;
        } else if (holdCv > 0) {
            holdEnd     = cg.time;
            holdFracEnd = (holdMs > 100) ? (float)(cg.time - holdStart - 100) / (float)(holdMs - 100) : 0.0f;
            if (holdFracEnd < 0.0f) {
                holdFracEnd = 0.0f;
            } else if (holdFracEnd > 1.0f) {
                holdFracEnd = 1.0f;
            }
        }
        holdCv = h;
    }
    if (holdCv > 0 && (cg.time < holdStart || cg.time - holdStart > holdMs + 3000)) {
        holdCv = 0; // cg.time ran backwards over a map load, or the "ended" message never came
        cgi.Cvar_Set("coop_lootH", "0");
    }
    if (holdCv > 0) {
        // nothing for the first 100 ms, so a tap never flashes the bar
        frac  = (holdMs > 100) ? (float)(cg.time - holdStart - 100) / (float)(holdMs - 100) : 0.0f;
        fillA = 1.0f;
    } else {
        frac  = holdFracEnd;
        fillA = 1.0f - (float)(cg.time - holdEnd) / 150.0f;
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    if (fillA < 0.0f || cg.time < holdEnd) {
        fillA = 0.0f;
    }

    if (!shownT || alpha <= 0.0f) {
        return;
    }
    font = CG_FeedFont();
    if (!font || cgs.uiHiResScale[0] <= 0.0f || cgs.uiHiResScale[1] <= 0.0f) {
        return;
    }
    kind = shownT / 100000;
    cls  = (shownT / 10000) % 10;
    idx  = shownT % 10000;
    if (kind < 1 || kind > 5) {
        return;
    }

    if (!s_lootKeyShader) {
        s_lootKeyShader    = cgi.R_RegisterShader("textures/hud/loot/coop_loot_key");
        s_lootAmmoShader   = cgi.R_RegisterShader("textures/hud/loot/coop_loot_ammo");
        s_lootSearchShader = cgi.R_RegisterShader("textures/hud/loot/coop_loot_search");
    }
    // the bound Use key, as the item row shows its keys; bindings change in a menu, so once a second is plenty
    if (cg.time >= keyNextMs || cg.time + 2000 < keyNextMs) {
        int         k1 = -1, k2 = -1;
        const char *nm;

        keyNextMs = cg.time + 1000;
        cgi.Key_GetKeysForCommand("+use", &k1, &k2);
        nm = (k1 >= 0) ? cgi.Key_KeynumToBindString(k1) : NULL;
        if (nm && nm[0] && nm[0] != '?') {
            CG_CoopItemKeyLabel(nm, keyLabel, sizeof(keyLabel));
            keyLabel[8] = 0;
        } else {
            Q_strncpyz(keyLabel, "USE", sizeof(keyLabel));
        }
    }

    // --- which gun (resolved once per target) ---------------------------------------------------------------
    if (kind == 1 || kind == 4) {
        if (artT != shownT) {
            const char *cs = CG_ConfigString(CS_WEAPONS + idx);
            char       *p;

            artT   = shownT;
            artIdx = -1;
            artCls = (cls >= 0 && cls <= 2) ? cls : 0;
            Q_strncpyz(artName, cs ? cs : "", sizeof(artName));
            p = strstr(artName, " (");
            if (p) {
                *p = 0; // "<Base Gun> (<Finish>)" is the same gun
            }
            for (i = 0; i < LOOT_ART_N; i++) {
                if (!Q_stricmp(artName, s_lootArt[i].name)) {
                    artIdx = i;
                    break;
                }
            }
        }
        if (artIdx >= 0) {
            art = &s_lootArt[artIdx];
            if (!s_lootArtShader[artIdx]) {
                s_lootArtShader[artIdx] = cgi.R_RegisterShader(va("textures/hud/loot/%s", art->tex));
            }
            shortName = art->shortName;
        } else {
            art = &s_lootGeneric[artCls];
            if (!s_lootGenericShader[artCls]) {
                s_lootGenericShader[artCls] = cgi.R_RegisterShader(va("textures/hud/loot/%s", art->tex));
            }
            shortName = artName;
        }
    }

    // --- layout, in pixels ------------------------------------------------------------------------------------
    W = (float)cgs.glconfig.vidWidth;
    H = (float)cgs.glconfig.vidHeight;
    s = H / 1080.0f;
    if (s < 0.75f) {
        s = 0.75f;
    }
    textH = 12.0f * cgs.uiHiResScale[1];
    capH  = (float)((int)(26.0f * s + 0.5f));
    if (capH < textH + 5.0f) {
        capH = textH + 5.0f;
    }
    capW = CG_FeedTextW(keyLabel, -1) * cgs.uiHiResScale[0] + capH * 0.62f;
    if (capW < capH) {
        capW = capH;
    }
    gap    = 30.0f * s;
    trackW = (float)((int)(48.0f * s + 0.5f));
    trackH = (float)((int)(5.0f * s + 0.5f));
    if (trackH < 4.0f) {
        trackH = 4.0f;
    }

    if (art) {
        g[n].what = 0;
        g[n].sh   = (artIdx >= 0) ? s_lootArtShader[artIdx] : s_lootGenericShader[artCls];
        g[n].dw   = 168.0f * s;
        g[n].dh   = 84.0f * s;
        g[n].vw   = art->vw * g[n].dw;
        g[n].r2   = capW + 6.0f * s + CG_FeedTextW(shortName, -1) * cgs.uiHiResScale[0];
        n++;
    }
    if (kind >= 1 && kind <= 3) {
        g[n].what = 1;
        g[n].sh   = s_lootAmmoShader;
        g[n].dw = g[n].dh = 46.0f * s;
        g[n].vw           = 0.66f * g[n].dw;
        g[n].r2           = (kind == 3) ? CG_FeedTextW("Full", -1) * cgs.uiHiResScale[0] : capW + 6.0f * s + trackW;
        n++;
    }
    if (kind == 5) {
        g[n].what = 2;
        g[n].sh   = s_lootSearchShader;
        g[n].dw = g[n].dh = 52.0f * s;
        g[n].vw           = 0.92f * g[n].dw;
        g[n].r2           = capW + 6.0f * s + trackW;
        n++;
    }
    total = 0.0f;
    for (i = 0; i < n; i++) {
        g[i].gw = (g[i].vw > g[i].r2) ? g[i].vw : g[i].r2;
        total  += g[i].gw + (i ? gap : 0.0f);
    }
    x    = (float)((int)(W * 0.5f - total * 0.5f));
    yc   = (float)((int)(H * 0.635f));
    rowY = yc + (float)((int)(32.0f * s));
    // The event feed grows upward from the bottom left. Where its box would reach the prompt, the prompt steps UP
    // just enough to clear it; on a screen with no room for that (640x480: the prompt may not crowd the crosshair)
    // it steps to the RIGHT of the feed instead, by at most an eighth of the screen.
    if (s_feedBoxMs >= 0 && cg.time - s_feedBoxMs >= 0 && cg.time - s_feedBoxMs < 250 && x < s_feedBoxRightPx + 8.0f
        && rowY + capH > s_feedBoxTopPx - 6.0f) {
        float lift = rowY + capH - (s_feedBoxTopPx - 6.0f);
        float room = yc - (H * 0.5f + 84.0f * s);

        if (lift <= room) {
            yc   -= (float)((int)(lift + 0.5f));
            rowY -= (float)((int)(lift + 0.5f));
        } else {
            float shift = s_feedBoxRightPx + 8.0f - x;

            if (shift > W * 0.125f) {
                shift = W * 0.125f;
            }
            x += (float)((int)(shift + 0.5f));
        }
    }

    for (i = 0; i < n; i++) {
        float  gx  = x + g[i].gw * 0.5f;
        float  rx  = (float)((int)(gx - g[i].r2 * 0.5f));
        float  dim = (kind == 3) ? 0.45f : 1.0f;
        vec4_t c;

        c[0] = c[1] = c[2] = 1.0f;
        c[3]                = alpha * dim;
        cgi.R_SetColor(c);
        cgi.R_DrawStretchPic(
            (float)((int)(gx - g[i].dw * 0.5f)), (float)((int)(yc - g[i].dh * 0.5f)), g[i].dw, g[i].dh, 0.0f, 0.0f, 1.0f,
            1.0f, g[i].sh
        );

        if (g[i].what == 0) {
            float w = CG_LootKeycap(font, keyLabel, rx, rowY, capH, alpha);

            CG_LootText(
                font, shortName, rx + w + 6.0f * s, rowY + (capH - textH) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f,
                0.91f, 0.84f, alpha
            );
        } else if (kind == 3) {
            CG_LootText(
                font, "Full", rx, rowY + (capH - textH) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f, 0.91f, 0.84f,
                alpha * 0.55f
            );
        } else {
            float w  = CG_LootKeycap(font, keyLabel, rx, rowY, capH, alpha);
            float tx = (float)((int)(rx + w + 6.0f * s));
            float ty = (float)((int)(rowY + (capH - trackH) * 0.5f));

            // the track: a dark slot with a thin cream rim, so an EMPTY track already says "hold"
            CG_LootBox(tx - 2.0f, ty - 2.0f, trackW + 4.0f, trackH + 4.0f, 0.05f, 0.045f, 0.04f, 0.67f * alpha);
            CG_LootBox(tx - 1.0f, ty - 1.0f, trackW + 2.0f, 1.0f, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
            CG_LootBox(tx - 1.0f, ty + trackH, trackW + 2.0f, 1.0f, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
            CG_LootBox(tx - 1.0f, ty, 1.0f, trackH, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
            CG_LootBox(tx + trackW, ty, 1.0f, trackH, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
            if (frac > 0.0f && fillA > 0.0f) {
                CG_LootBox(tx, ty, (float)((int)(trackW * frac + 0.5f)), trackH, 0.94f, 0.91f, 0.84f, 0.94f * alpha * fillA);
            }
        }
        x += g[i].gw + gap;
    }
    cgi.R_SetColor(NULL);
}

/*
====================
CG_DrawCoopAct

HZM coop [user 2026-10-18] ICON ACTION PROMPTS. Every "press USE to ..." line a script used to draw as text
(heal, resupply, remove cover, convert, contain, distract, plant, truck ...) is now ONE icon + the bound key in
exactly the slot, size, fade and key cap of the use-to-loot prompt above (CG_DrawCoopLoot): centred, centre line at
63.5 % of the height, 52*s px icon, key cap 32*s below it, s = H/1080 floored at 0.75, hi-res units for the text.

Wire (script -> client, change-only `set coop_*` stufftext, see coop_mod/actprompt.scr):
    coop_act  = "<name>[:hold][:fire][:dim][:cap=Word_Word]"   "0" or "" = nothing
                name  textures/hud/loot/coop_act_<name> (any name; a new prompt is one new tga, no code here)
                hold  a track beside the key cap (this action is a HOLD)
                fire  the key shown is +attackprimary instead of +use
                dim   the icon at 45 % with no key: the action is not available (empty box, no medkits);
                      cap= is the one word under it
                cap=  with a key cap (no dim): a word beside the cap, like the gun prompt's name
    coop_actH = length in ms of the hold now in progress (0 = none); the track fills on the CLIENT's own clock from
                the moment it changes, as coop_lootH does. The shared HD progress ring (hdring-engine) can take the
                track over later: it is drawn by CG_ActTrack() below, one place.
CG_Init zeroes both (cg_main.c). The loot prompt wins the slot: while coop_lootT is set nothing is drawn here.

HOLD actions and the HD ring [user 2026-10-18]: with COOP_HDRING defined (the hdring-engine branch, cg_hdprogress.c) a HOLD
action draws NO track of its own: the icon sits in the centre of the HD hold ring round the crosshair (the ring names it
through the client cvar coop_holdIcon), the ring fills clockwise from coop_actH, and the row under it is just the key cap and
the word "Hold", the same as the loot prompt's hold. A press-once action is unchanged (icon + key cap). Without COOP_HDRING
(this branch alone, before hdring-engine is merged) a hold falls back to the icon + key cap + the small track, so the
prompt still works; the merge is: #define COOP_HDRING 1 below, nothing else.
====================
*/
// #define COOP_HDRING 1   // <- the ONE switch for the merge with hdring-engine (needs CG_HdHoldRing / HD_FAM_USE)
#define ACT_ART_MAX 40
static struct {
    char      name[32];
    qhandle_t sh;
} s_actArt[ACT_ART_MAX];
static int s_actArtN = 0;

static qhandle_t CG_ActShader(const char *name)
{
    int i;

    for (i = 0; i < s_actArtN; i++) {
        if (!Q_stricmp(s_actArt[i].name, name)) {
            return s_actArt[i].sh;
        }
    }
    if (s_actArtN >= ACT_ART_MAX) {
        return 0;
    }
    Q_strncpyz(s_actArt[s_actArtN].name, name, sizeof(s_actArt[0].name));
    s_actArt[s_actArtN].sh = cgi.R_RegisterShader(va("textures/hud/loot/coop_act_%s", name));
    return s_actArt[s_actArtN++].sh;
}

#ifdef COOP_HDRING
// the ring names its centre icon through coop_holdIcon; set only on change, cleared whenever this prompt is not showing a hold
static void CG_ActHoldIcon(const char *shader)
{
    static char last[MAX_QPATH] = "";

    if (!shader) {
        shader = "";
    }
    if (strcmp(last, shader)) {
        Q_strncpyz(last, shader, sizeof(last));
        cgi.Cvar_Set("coop_holdIcon", last);
    }
}
#endif

// the hold track (the fallback when the HD ring is not built in): a dark slot with a thin cream rim that fills.
static void CG_ActTrack(float tx, float ty, float trackW, float trackH, float frac, float fillA, float alpha)
{
    CG_LootBox(tx - 2.0f, ty - 2.0f, trackW + 4.0f, trackH + 4.0f, 0.05f, 0.045f, 0.04f, 0.67f * alpha);
    CG_LootBox(tx - 1.0f, ty - 1.0f, trackW + 2.0f, 1.0f, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
    CG_LootBox(tx - 1.0f, ty + trackH, trackW + 2.0f, 1.0f, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
    CG_LootBox(tx - 1.0f, ty, 1.0f, trackH, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
    CG_LootBox(tx + trackW, ty, 1.0f, trackH, 0.94f, 0.91f, 0.84f, 0.58f * alpha);
    if (frac > 0.0f && fillA > 0.0f) {
        CG_LootBox(tx, ty, (float)((int)(trackW * frac + 0.5f)), trackH, 0.94f, 0.91f, 0.84f, 0.94f * alpha * fillA);
    }
}

void CG_DrawCoopAct(void)
{
    static cvar_t *cvA = NULL, *cvH = NULL, *cvL = NULL;
    static char    shownSpec[64] = "";
    static int     lastMs = 0, holdCv = 0, holdMs = 0, holdStart = 0, holdEnd = 0;
    static float   alpha = 0.0f, holdFracEnd = 0.0f;
    static char    keyLabel[2][16] = {"", ""};
    static int     keyNextMs       = 0;
    char           spec[64], *tok, *save;
    char           name[32], caption[24];
    int            dt, h, hold = 0, fire = 0, dim = 0, k, ringMode = 0;
    float          s, W, H, yc, rowY, capH, capW, trackW, trackH, dw, r2, gw, x, rx, frac, fillA, textH, capTextW = 0.0f;
    qhandle_t      sh;
    fontheader_t  *font;
    vec4_t         c;

    if (!cvA) {
        cvA = cgi.Cvar_Get("coop_act", "0", 0);
        cvH = cgi.Cvar_Get("coop_actH", "0", 0);
        cvL = cgi.Cvar_Get("coop_lootT", "0", 0);
    }
    if (!cg.snap || (cg.snap->ps.pm_flags & PMF_INTERMISSION)) {
        return;
    }
    dt = cg.time - lastMs;
    if (dt < 0 || dt > 250) {
        dt = 16;
    }
    lastMs = cg.time;

    // --- what to show, and the fade (120 ms in, 180 ms out; same curve as the loot prompt) ----------------------
    if (cvA->string[0] && Q_stricmp(cvA->string, "0") && cvL->integer <= 0) {
        Q_strncpyz(shownSpec, cvA->string, sizeof(shownSpec));
        alpha += (float)dt / 120.0f;
        if (alpha > 1.0f) {
            alpha = 1.0f;
        }
    } else {
        alpha -= (float)dt / 180.0f;
        if (alpha <= 0.0f) {
            alpha        = 0.0f;
            shownSpec[0] = 0;
        }
    }

    // --- the hold, on this client's clock -------------------------------------------------------------------
    h = cvH->integer;
    if (h != holdCv) {
        if (h > 0) {
            holdStart = cg.time;
            holdMs    = h;
        } else if (holdCv > 0) {
            holdEnd     = cg.time;
            holdFracEnd = (holdMs > 100) ? (float)(cg.time - holdStart - 100) / (float)(holdMs - 100) : 0.0f;
            if (holdFracEnd < 0.0f) {
                holdFracEnd = 0.0f;
            } else if (holdFracEnd > 1.0f) {
                holdFracEnd = 1.0f;
            }
        }
        holdCv = h;
    }
    if (holdCv > 0 && (cg.time < holdStart || cg.time - holdStart > holdMs + 3000)) {
        holdCv = 0; // map load ran cg.time backwards, or the "ended" message never came
        cgi.Cvar_Set("coop_actH", "0");
    }
    if (holdCv > 0) {
        frac  = (holdMs > 100) ? (float)(cg.time - holdStart - 100) / (float)(holdMs - 100) : 0.0f;
        fillA = 1.0f;
    } else {
        frac  = holdFracEnd;
        fillA = 1.0f - (float)(cg.time - holdEnd) / 150.0f;
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    if (fillA < 0.0f || cg.time < holdEnd) {
        fillA = 0.0f;
    }

    if (!shownSpec[0] || alpha <= 0.0f) {
#ifdef COOP_HDRING
        CG_ActHoldIcon(NULL);
#endif
        return;
    }
    font = CG_FeedFont();
    if (!font || cgs.uiHiResScale[0] <= 0.0f || cgs.uiHiResScale[1] <= 0.0f) {
        return;
    }

    // --- parse "<name>[:flag...]" -----------------------------------------------------------------------------
    Q_strncpyz(spec, shownSpec, sizeof(spec));
    name[0] = caption[0] = 0;
    save                 = spec;
    for (k = 0; (tok = strtok(save, ":")) != NULL; k++) {
        save = NULL;
        if (k == 0) {
            Q_strncpyz(name, tok, sizeof(name));
        } else if (!Q_stricmp(tok, "hold")) {
            hold = 1;
        } else if (!Q_stricmp(tok, "fire")) {
            fire = 1;
        } else if (!Q_stricmp(tok, "dim")) {
            dim = 1;
        } else if (!Q_stricmpn(tok, "cap=", 4)) {
            char *p;

            Q_strncpyz(caption, tok + 4, sizeof(caption));
            for (p = caption; *p; p++) {
                if (*p == '_') {
                    *p = ' ';
                }
            }
        }
    }
    if (!name[0]) {
        return;
    }
    sh = CG_ActShader(name);
    if (!sh) {
        return;
    }
    if (!s_lootKeyShader) {
        s_lootKeyShader    = cgi.R_RegisterShader("textures/hud/loot/coop_loot_key");
        s_lootAmmoShader   = cgi.R_RegisterShader("textures/hud/loot/coop_loot_ammo");
        s_lootSearchShader = cgi.R_RegisterShader("textures/hud/loot/coop_loot_search");
    }
    // the bound keys (Use / primary fire), as the loot prompt reads them; bindings change in a menu so once a second
    if (cg.time >= keyNextMs || cg.time + 2000 < keyNextMs) {
        static const char *cmds[2] = {"+use", "+attackprimary"};
        static const char *dflt[2] = {"USE", "FIRE"};
        int                j;

        keyNextMs = cg.time + 1000;
        for (j = 0; j < 2; j++) {
            int         k1 = -1, k2 = -1;
            const char *nm;

            cgi.Key_GetKeysForCommand(cmds[j], &k1, &k2);
            nm = (k1 >= 0) ? cgi.Key_KeynumToBindString(k1) : NULL;
            if (nm && nm[0] && nm[0] != '?') {
                CG_CoopItemKeyLabel(nm, keyLabel[j], sizeof(keyLabel[j]));
                keyLabel[j][8] = 0;
            } else {
                Q_strncpyz(keyLabel[j], dflt[j], sizeof(keyLabel[j]));
            }
        }
    }

    // --- layout, in pixels (identical numbers to CG_DrawCoopLoot) ---------------------------------------------
    W = (float)cgs.glconfig.vidWidth;
    H = (float)cgs.glconfig.vidHeight;
    s = H / 1080.0f;
    if (s < 0.75f) {
        s = 0.75f;
    }
    textH = 12.0f * cgs.uiHiResScale[1];
    capH  = (float)((int)(26.0f * s + 0.5f));
    if (capH < textH + 5.0f) {
        capH = textH + 5.0f;
    }
    capW = CG_FeedTextW(keyLabel[fire], -1) * cgs.uiHiResScale[0] + capH * 0.62f;
    if (capW < capH) {
        capW = capH;
    }
    trackW = (float)((int)(48.0f * s + 0.5f));
#ifdef COOP_HDRING
    trackW   = CG_FeedTextW("Hold", -1) * cgs.uiHiResScale[0]; // no track bar: the HD ring round the crosshair is the progress
    ringMode = (hold && !dim);
    CG_ActHoldIcon(ringMode ? va("textures/hud/loot/coop_act_%s", name) : NULL);
#endif
    trackH = (float)((int)(5.0f * s + 0.5f));
    if (trackH < 4.0f) {
        trackH = 4.0f;
    }
    dw = 52.0f * s;
    if (caption[0]) {
        capTextW = CG_FeedTextW(caption, -1) * cgs.uiHiResScale[0];
    }
    if (dim) {
        r2 = capTextW;
    } else if (hold) {
        r2 = capW + 6.0f * s + trackW;
    } else if (caption[0]) {
        r2 = capW + 6.0f * s + capTextW;
    } else {
        r2 = capW;
    }
    gw   = (0.92f * dw > r2) ? 0.92f * dw : r2;
    x    = (float)((int)(W * 0.5f - gw * 0.5f)) + gw * 0.5f;
    yc   = (float)((int)(H * 0.635f));
    rowY = yc + (float)((int)(32.0f * s));
    if (ringMode) {
        rowY = yc - capH * 0.5f; // the icon is in the ring, so the key cap row takes the icon's place
    }
    // step clear of the event feed exactly as the loot prompt does (up where there is room, else to the right)
    if (s_feedBoxMs >= 0 && cg.time - s_feedBoxMs >= 0 && cg.time - s_feedBoxMs < 250
        && x - gw * 0.5f < s_feedBoxRightPx + 8.0f && rowY + capH > s_feedBoxTopPx - 6.0f) {
        float lift = rowY + capH - (s_feedBoxTopPx - 6.0f);
        float room = yc - (H * 0.5f + 84.0f * s);

        if (lift <= room) {
            yc   -= (float)((int)(lift + 0.5f));
            rowY -= (float)((int)(lift + 0.5f));
        } else {
            float shift = s_feedBoxRightPx + 8.0f - (x - gw * 0.5f);

            if (shift > W * 0.125f) {
                shift = W * 0.125f;
            }
            x += (float)((int)(shift + 0.5f));
        }
    }

    c[0] = c[1] = c[2] = 1.0f;
    c[3]                = alpha * (dim ? 0.45f : 1.0f);
    cgi.R_SetColor(c);
    if (!ringMode) {
        cgi.R_DrawStretchPic(
            (float)((int)(x - dw * 0.5f)), (float)((int)(yc - dw * 0.5f)), dw, dw, 0.0f, 0.0f, 1.0f, 1.0f, sh
        );
    }
    rx = (float)((int)(x - r2 * 0.5f));
    if (dim) {
        if (caption[0]) {
            CG_LootText(
                font, caption, rx, rowY + (capH - textH) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f, 0.91f, 0.84f,
                alpha * 0.55f
            );
        }
    } else {
        float w = CG_LootKeycap(font, keyLabel[fire], rx, rowY, capH, alpha);

        if (hold) {
#ifdef COOP_HDRING
            CG_LootText(
                font, "Hold", rx + w + 6.0f * s, rowY + (capH - textH) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f, 0.91f,
                0.84f, alpha * 0.85f
            );
            CG_HdHoldRing(frac, HD_FAM_USE, alpha, 1, 0);
            if (fillA > 0.0f) {
                CG_HdHoldRing(frac, HD_FAM_USE, alpha * fillA, 0, 1);
            }
#else
            CG_ActTrack(
                (float)((int)(rx + w + 6.0f * s)), (float)((int)(rowY + (capH - trackH) * 0.5f)), trackW, trackH, frac,
                fillA, alpha
            );
#endif
        } else if (caption[0]) {
            CG_LootText(
                font, caption, rx + w + 6.0f * s, rowY + (capH - textH) * 0.5f - 1.0f * cgs.uiHiResScale[1], 0.94f,
                0.91f, 0.84f, alpha
            );
        }
    }
    cgi.R_SetColor(NULL);
}
