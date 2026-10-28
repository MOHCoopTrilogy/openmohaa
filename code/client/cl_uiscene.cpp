/*
 * [HZM menus3d spike] Scene3D - a menu widget that renders a LIVE 3D scene (static models, no world / no server) behind the menu
 * widgets, with a flyable camera and 3D hotspots that follow it.  Same refdef path as the armory operator / inventory preview
 * (cl_invrender.cpp: ClearScene / AddRefEntityToScene / RenderScene with RDF_NOWORLDMODEL), so nothing new is needed in the renderer.
 *
 *   Scene3D
 *   {
 *       name "atticscene"
 *       scenemodel "models/menu/attic_a.tik" 0 0 0 0     // model, origin x y z, yaw
 *       scenecam   x y z  pitch yaw roll  fov_y          // set the camera (and the "home" the widget returns to)
 *       scenehot   "radio" x y z radius "pushmenu hzm_radios"
 *   }
 *   widgetcommand / globalwidgetcommand atticscene scenefly <seconds> x y z pitch yaw roll fov_y   // eased fly, key/click skips
 *
 * A hotspot is a sphere in world space; hovering it sets the cvar ui_scene_hot to its name, a click runs its command.  When the
 * widget is shown again after being hidden away from its home camera (BACK from the opened menu) it flies home by itself.
 */

#include "cl_ui.h"
void CL_CompositeAttachOnTagXf(refEntity_t &base, qhandle_t attachModel, const char *attachTag, const float *xfIn);   // cl_invrender.cpp
#include "../qcommon/tiki.h"

// bumped after every renderer restart (vid_restart): the cached model handles of every Scene3D are stale then and must be registered again
static int s_sceneRendererEpoch = 0;
void       UI_SceneRendererRestart(void)
{
    s_sceneRendererEpoch++;
}

class UIScene3D : public UIWidget
{
    struct animPlay_t {
        str   alts;
        float t0;
        bool  loop;
        float rate;
    };
    // ---- staged actors (actorstage / actorcycle / actorprop): a model stands at a fixed spot and loops animation cycles; no walking ----
    struct stageCycle_t {
        float weight, tmin, tmax;
        str   seq;             // comma list: 'a|alt' once, 'b|alt~' looped for the rest of the cycle
    };
    struct stageProp_t {
        str       model, tag, when;   // when: attached only while the playing animation alias contains this text ('' = always)
        float     xf[7];
        qhandle_t handle;
    };
    struct stage_t {
        float pos[2], yaw;
        str   idle;
        float idleMin, idleMax;
        Container<stageCycle_t *> cycles;
        Container<stageProp_t *>  props;
        int   state, tok, cyc;
        float clock, timer, total, cycT0, seqT0;
        unsigned rng;
        animPlay_t cur, prev;
        float blendT0;
    };
    struct sceneModel_t {
        stage_t  *stage;
        str       name;
        qhandle_t handle;
        vec3_t    origin;
        float     yaw;
        str       anim;      // optional: loop this TIKI animation (actors)
        str       hide;      // optional: comma list of surface names to nodraw (helmet indoors)
        str       group;     // optional: models sharing a group name are alternatives - ONE is chosen at random when the world first draws (actor coin flip)
        bool      off;
        int       animStart;
    };
    struct sceneHot_t {
        str    name;
        vec3_t pos;
        float  radius;
        str    command;
        str    caption;
        int    stop;                   // optional: the hotspot is live only while cvar ui_atcStop == stop (-1 = always)
        str    widget;                 // optional: name of a sibling Button that is re-framed to this hotspot every frame (mouse / keyboard / pad focus all work on it)
        SafePtr<UIWidget> w;
        float  glow;                   // 0..1 hover light, faded smoothly (~0.2 s) toward 1 while hovered / focused
        float  box[4];                 // optional hit box (scenehotbox): centre offset x y and half size w h, in pixels at 1080 px screen height (scaled with the screen); box[2] == 0 = the square of `radius`
    };
    struct sceneLabel_t {
        str               widget;            // a sibling Label re-framed every frame to the projected rectangle of a 3D board (the chalkboard under the MEDAL OF HONOR sign)
        vec3_t            c;
        float             hw, hh;           // half extents: hw along world Y, hh along world Z
        SafePtr<UIWidget> w;
    };
    struct sceneLight_t {
        vec3_t org;
        float  radius;
        vec3_t color;
    };
    struct cam_t {
        vec3_t org;
        vec3_t ang;
        float  fov;
    };

    Container<sceneModel_t *> m_models;
    Container<sceneHot_t *>   m_hots;
    Container<sceneLight_t *> m_lights;
    Container<sceneLabel_t *> m_labels;
    int                       m_glowLast;
    int                       m_drawn;
    cam_t                     m_cur, m_from, m_to, m_home, m_ret;
    bool                      m_retSet;
    int                       m_flyStart;
    float                     m_flyDur;
    bool                      m_flying;
    bool                      m_root;      // the scene is the main menu itself (sceneroot): ESC at the shelf flies home through UI_SceneEscape()
    bool                      m_leaving;   // sceneleave: a flat menu is about to open over the scene; fly back as soon as it is shown again
    bool                      m_snap;      // a fly just ended: with keyboard / pad navigation active, put the cursor on the first live hotspot so the D-pad has somewhere to start
    int                       m_lastDraw;
    int                       m_hover;

    // ---- actor life director (ui_atticActorLife): the choice-group "actor1" model walks a round of stations on the attic floor, each with a stock animation ----
    struct dirStation_t {
        str   name;
        float yaw, tmin, tmax;
        int   n;
        float wp[10][2];       // path from the hub to the station position (last point)
        str   seq;             // comma list: "a|alt" once, "b|alt~" looped for the rest of the stay
    };
    Container<dirStation_t *> m_stations;
    bool   m_dirOn;
    int    m_rendEpoch;          // s_sceneRendererEpoch the model handles were registered under
    float  m_hub[3];
    float  m_speed;
    float  m_turnRate;
    str    m_idleAlts, m_walkAlts, m_breakAlts;
    float  m_breakMin, m_breakMax;
    // runtime
    int    m_dState;           // 0 start, 1 wait, 2 walk, 3 turn to the station yaw, 4 station sequence
    float  m_dTimer, m_dPos[2], m_dYaw, m_dTargetYaw, m_aClock;
    int    m_dAt;              // station index the actor stands at (-1 = hub)
    int    m_dNext;
    int    m_bag[16], m_bagN, m_lastStation;
    float  m_path[40][2];
    int    m_pathN, m_pathI;
    int    m_seqTok;
    float  m_seqT0, m_seqTotal;
    float  m_stClock0;          // director clock when the station sequence began (the stay is measured from here)
    unsigned m_rng;
    animPlay_t m_aCur, m_aPrev;
    float      m_blendT0;
    int        m_dirLast;
    int        m_dirFrame;
    int        m_stageLast;

public:
    CLASS_PROTOTYPE(UIScene3D);

    UIScene3D();
    void SceneModel(Event *ev);
    void SceneCam(Event *ev);
    void SceneFly(Event *ev);
    void SceneReturn(Event *ev);
    void SceneBack(Event *ev);
    friend bool UI_SceneEscape(void);
    void SceneRoot(Event *ev);
    void SceneLeave(Event *ev);
    void ActorHub(Event *ev);
    void ActorStation(Event *ev);
    void ActorBreak(Event *ev);
    void ActorStage(Event *ev);
    void ActorCycle(Event *ev);
    void ActorProp(Event *ev);
    void StageUpdate(sceneModel_t *m, dtiki_t *tiki, float dt);
    void StageSetAnim(stage_t *s, const str& alts, bool loop, float rate);
    float StageRand(stage_t *s, float a, float b);
    float DirRand(float a, float b);
    void  DirSetAnim(const str& alts, bool loop, float rate);
    void  DirPickNext();
    void  DirUpdate(dtiki_t *tiki, float dt);
    int   DirResolve(dtiki_t *tiki, const str& alts);
    float DirAnimTime(dtiki_t *tiki, const str& alts, float clockNow, const animPlay_t& a, bool *looped);
    bool TryEscape();
    static UIScene3D *s_rootScene;
    static int        s_rootDrawn;
    void SceneHot(Event *ev);
    void SceneLabel(Event *ev);
    void SceneHotBox(Event *ev);
    void SceneLight(Event *ev);
    void OnClick(Event *ev);
    void Draw(void) override;

private:
    void StartFly(const cam_t& to, float seconds);
    void Project(const vec3_t p, const vec3_t org, vec3_t axis[3], float w, float h, float fovx, float fovy, float *sx, float *sy, float *depth) const;
};

Event EV_Scene_Model
(
    "scenemodel",
    EV_DEFAULT,
    "sffffSSS",
    "model x y z yaw [anim] [hidesurfaces] [choicegroup]",
    "Scene3D: add a model (tik) at an origin and yaw; optional looping animation and a comma list of surfaces to hide."
);
Event EV_Scene_Cam
(
    "scenecam",
    EV_DEFAULT,
    "fffffff",
    "x y z pitch yaw roll fov_y",
    "Scene3D: set the camera (also its home)."
);
Event EV_Scene_Fly
(
    "scenefly",
    EV_DEFAULT,
    "ffffffff",
    "seconds x y z pitch yaw roll fov_y",
    "Scene3D: fly the camera to a pose, smoothstep eased; any key / click / pad button skips to the end."
);
Event EV_Scene_Return
(
    "scenereturn",
    EV_DEFAULT,
    "fffffff",
    "x y z pitch yaw roll fov_y",
    "Scene3D: the pose the camera flies to by itself when the widget is shown again (BACK from an opened menu); default = the home pose."
);

Event EV_Scene_Back
(
    "sceneback",
    EV_DEFAULT,
    NULL,
    NULL,
    "Scene3D: ESC. When ui_atcStop is above 0 the camera flies back to the home pose (ui_atcStop 0, the return pose is dropped; with ui_atcAuto 1 the menu then pops); otherwise the menu is popped."
);

Event EV_Scene_Root
(
    "sceneroot",
    EV_DEFAULT,
    NULL,
    NULL,
    "Scene3D: this scene IS the main menu (ui_atticMain): ESC while ui_atcStop is 1 (the shelf) flies back home instead of popping, nothing else changes."
);

Event EV_Scene_Leave
(
    "sceneleave",
    EV_DEFAULT,
    NULL,
    NULL,
    "Scene3D: a flat menu is about to be pushed over the scene; when the scene is drawn again the camera flies back (to the return pose, else home) at once instead of after the 1.5 s hitch guard."
);

Event EV_Scene_ActorHub
(
    "actorhub",
    EV_DEFAULT,
    "fffssf",
    "x y yaw idle walk speed",
    "Scene3D: the actor life director - home spot (MOHAA units), idle / walk animation alternatives ('a|b'), walk speed (units per second)."
);

Event EV_Scene_ActorStation
(
    "actorstation",
    EV_DEFAULT,
    "sfffss",
    "name yaw tmin tmax path seq",
    "Scene3D: a station of the round: facing yaw, stay seconds (random between), the floor path from the hub 'x y,x y,...' (the last point is where he stands), the animation sequence 'a,b~,c' (~ = looped)."
);

Event EV_Scene_ActorBreak
(
    "actorbreak",
    EV_DEFAULT,
    "sff",
    "anims tmin tmax",
    "Scene3D: the pause between stations (idle / look-around alternatives, stay seconds)."
);

Event EV_Scene_ActorStage
(
    "actorstage",
    EV_DEFAULT,
    "sfffsff",
    "model x y yaw idle idlemin idlemax",
    "Scene3D: a staged actor - the named model stands at x y facing yaw and loops animation cycles in place (the idle alternatives between them, idlemin..idlemax seconds)."
);

Event EV_Scene_ActorCycle
(
    "actorcycle",
    EV_DEFAULT,
    "sfffs",
    "model weight tmin tmax seq",
    "Scene3D: a cycle of a staged actor: picked by weight, lasts tmin..tmax seconds; seq = comma list of animations (a|alt once, b|alt~ looped for the rest)."
);

Event EV_Scene_ActorProp
(
    "actorprop",
    EV_DEFAULT,
    "ssssfffffff",
    "model propmodel tag when offx offy offz scale pitch yaw roll",
    "Scene3D: a prop model held on a bone tag of a staged actor, shown only while the playing animation alias contains `when` (empty = always)."
);

Event EV_Scene_Hot
(
    "scenehot",
    EV_DEFAULT,
    "sffffsSSF",
    "name x y z radius command [caption] [focuswidget] [stop]",
    "Scene3D: a clickable sphere in the scene; hover sets ui_scene_hot (1-based index, 0 = none) and prints the caption beside the object, a click stuffs the command."
);
Event EV_Scene_Label
(
    "scenelabel",
    EV_DEFAULT,
    "sfffff",
    "widget x y z halfw halfh",
    "Scene3D: a Label widget that is re-framed every frame (while the camera rests at home) to the projected rectangle of a 3D board centred at x y z, half width along world Y, half height along world Z - the chalk caption of the hovered hotspot."
);

Event EV_Scene_HotBox
(
    "scenehotbox",
    EV_DEFAULT,
    "sffff",
    "name offx offy halfw halfh",
    "Scene3D: gives the named hotspot a rectangular hit box (pixels at 1080 px screen height, scaled with the screen): centre offset from the projected point and half width / height - for tall narrow things such as doors."
);

Event EV_Scene_Light
(
    "scenelight",
    EV_DEFAULT,
    "fffffff",
    "x y z radius r g b",
    "Scene3D: a dynamic light of the scene's fake light set (lights characters; baked models ignore it)."
);

CLASS_DECLARATION(UIWidget, UIScene3D, NULL) {
    {&EV_Scene_Model,  &UIScene3D::SceneModel},
    {&EV_Scene_Cam,    &UIScene3D::SceneCam  },
    {&EV_Scene_Fly,    &UIScene3D::SceneFly  },
    {&EV_Scene_Hot,    &UIScene3D::SceneHot  },
    {&EV_Scene_HotBox, &UIScene3D::SceneHotBox},
    {&EV_Scene_Label,  &UIScene3D::SceneLabel},
    {&EV_Scene_Return, &UIScene3D::SceneReturn},
    {&EV_Scene_Back,   &UIScene3D::SceneBack},
    {&EV_Scene_Root,   &UIScene3D::SceneRoot},
    {&EV_Scene_Leave,  &UIScene3D::SceneLeave},
    {&EV_Scene_ActorHub, &UIScene3D::ActorHub},
    {&EV_Scene_ActorStation, &UIScene3D::ActorStation},
    {&EV_Scene_ActorBreak, &UIScene3D::ActorBreak},
    {&EV_Scene_ActorStage, &UIScene3D::ActorStage},
    {&EV_Scene_ActorCycle, &UIScene3D::ActorCycle},
    {&EV_Scene_ActorProp,  &UIScene3D::ActorProp},
    {&EV_Scene_Light,  &UIScene3D::SceneLight},
    {&W_LeftMouseDown, &UIScene3D::OnClick   },
    {NULL,             NULL                  }
};

UIScene3D::UIScene3D()
{
    memset(&m_cur, 0, sizeof(m_cur));
    m_cur.fov = 60;
    m_from = m_to = m_home = m_ret = m_cur;
    m_retSet = false;
    m_flyStart = 0;
    m_flyDur   = 0;
    m_flying   = false;
    m_snap     = false;
    m_lastDraw = 0;
    m_root     = false;
    m_leaving  = false;
    m_hover    = -1;
    m_drawn    = 0;
    m_dirOn    = false;
    m_stageLast = 0;
    m_glowLast  = 0;
    m_rendEpoch = s_sceneRendererEpoch;
    m_speed    = 60.0f;
    m_turnRate = 150.0f;
    m_breakMin = 1.5f;
    m_breakMax = 3.5f;
    m_dState   = 0;
    m_dAt      = -1;
    m_dNext    = -1;
    m_bagN     = 0;
    m_lastStation = -1;
    m_aClock   = 0.0f;
    m_rng      = 2463534242u;
    m_blendT0  = -10.0f;
    m_dirLast  = 0;
    m_dirFrame = -1;
    m_pathN = m_pathI = 0;
    m_seqTok = 0;
    m_seqT0 = m_seqTotal = m_stClock0 = 0.0f;
}

void UIScene3D::SceneModel(Event *ev)
{
    sceneModel_t *m = new sceneModel_t;
    m->name         = ev->GetString(1);
    m->handle       = 0;
    m->origin[0]    = ev->GetFloat(2);
    m->origin[1]    = ev->GetFloat(3);
    m->origin[2]    = ev->GetFloat(4);
    m->yaw          = ev->GetFloat(5);
    m->anim         = ev->NumArgs() >= 6 ? ev->GetString(6) : "";
    m->hide         = ev->NumArgs() >= 7 ? ev->GetString(7) : "";
    m->group        = ev->NumArgs() >= 8 ? ev->GetString(8) : "";
    m->off          = false;
    m->animStart    = 0;
    m->stage        = NULL;
    m_models.AddObject(m);
}

void UIScene3D::SceneCam(Event *ev)
{
    for (int i = 0; i < 3; i++) {
        m_cur.org[i] = ev->GetFloat(1 + i);
        m_cur.ang[i] = ev->GetFloat(4 + i);
    }
    m_cur.fov = ev->GetFloat(7);
    m_home    = m_cur;
    m_flying  = false;
}

void UIScene3D::StartFly(const cam_t& to, float seconds)
{
    m_from     = m_cur;
    m_to       = to;
    m_flyStart = uid.time;
    m_flyDur   = seconds * UIWidget::CamSlow();
    m_flying   = true;
    UIWidget::NoteCamMove(m_flyDur);
}

void UIScene3D::SceneFly(Event *ev)
{
    cam_t to;
    for (int i = 0; i < 3; i++) {
        to.org[i] = ev->GetFloat(2 + i);
        to.ang[i] = ev->GetFloat(5 + i);
    }
    to.fov = ev->GetFloat(8);
    StartFly(to, ev->GetFloat(1));
}

void UIScene3D::SceneReturn(Event *ev)
{
    for (int i = 0; i < 3; i++) {
        m_ret.org[i] = ev->GetFloat(1 + i);
        m_ret.ang[i] = ev->GetFloat(4 + i);
    }
    m_ret.fov = ev->GetFloat(7);
    m_retSet  = true;
}

void UIScene3D::SceneBack(Event *ev)
{
    if (Cvar_VariableIntegerValue("ui_atcStop") > 0) {
        Cvar_Set("ui_atcStop", "0");
        m_retSet = false;
        StartFly(m_home, 0.6f);
        if (Cvar_VariableIntegerValue("ui_atcAuto")) {
            Cbuf_AddText("uiafter 0.62 popmenu 1\n");     // entered straight from the flat main menu: BACK from the shelf pulls out and leaves
        }
    } else {
        Cbuf_AddText("popmenu 1\n");
    }
}

UIScene3D *UIScene3D::s_rootScene = NULL;
int        UIScene3D::s_rootDrawn = -100000;

void UIScene3D::SceneRoot(Event *ev)
{
    m_root = true;
}

void UIScene3D::SceneLeave(Event *ev)
{
    m_leaving = true;
}

bool UIScene3D::TryEscape()
{
    if (!m_root || uid.time - s_rootDrawn > 400 || Cvar_VariableIntegerValue("ui_atcStop") != 1) {
        return false;
    }
    Cvar_Set("ui_atcStop", "0");
    m_retSet = false;
    StartFly(m_home, 0.6f);
    return true;
}

// ESC from cl_ui.cpp: true when the root attic scene consumed it (shelf -> home)
bool UI_SceneEscape(void)
{
    if (Cvar_VariableIntegerValue("ui_atticActive") != 1) {
        return false;          // in a game the flat main menu is shown: ESC pops it as usual
    }
    return UIScene3D::s_rootScene && UIScene3D::s_rootScene->TryEscape();
}

void UIScene3D::ActorHub(Event *ev)
{
    m_hub[0]   = ev->GetFloat(1);
    m_hub[1]   = ev->GetFloat(2);
    m_hub[2]   = ev->GetFloat(3);
    m_idleAlts = ev->GetString(4);
    m_walkAlts = ev->GetString(5);
    m_speed    = ev->GetFloat(6);
    m_dirOn    = true;
}

void UIScene3D::ActorStation(Event *ev)
{
    if (m_stations.NumObjects() >= 16) {
        return;
    }
    dirStation_t *st = new dirStation_t;
    st->name = ev->GetString(1);
    st->yaw  = ev->GetFloat(2);
    st->tmin = ev->GetFloat(3);
    st->tmax = ev->GetFloat(4);
    st->n    = 0;
    {
        const char *q = ev->GetString(5).c_str();
        float       x, y;
        int         used;
        while (st->n < 10 && sscanf(q, " %f %f%n", &x, &y, &used) == 2) {
            st->wp[st->n][0] = x;
            st->wp[st->n][1] = y;
            st->n++;
            q += used;
            while (*q == ',' || *q == ' ') {
                q++;
            }
        }
    }
    st->seq = ev->GetString(6);
    m_stations.AddObject(st);
}

void UIScene3D::ActorBreak(Event *ev)
{
    m_breakAlts = ev->GetString(1);
    m_breakMin  = ev->GetFloat(2);
    m_breakMax  = ev->GetFloat(3);
}

float UIScene3D::DirRand(float a, float b)
{
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return a + (b - a) * ((m_rng & 0xFFFFFF) / 16777215.0f);
}

void UIScene3D::DirSetAnim(const str& alts, bool loop, float rate)
{
    if (!m_aCur.alts.length() || str::cmp(m_aCur.alts, alts) || m_aCur.loop != loop) {
        m_aPrev    = m_aCur;
        m_blendT0  = m_aClock;
        m_aCur.alts = alts;
        m_aCur.t0   = m_aClock;
        m_aCur.loop = loop;
        m_aCur.rate = rate;
    }
}

// the next station from a shuffled bag (never the one he stands at), path = back to the hub along his own path, then the new one
void UIScene3D::DirPickNext()
{
    const int ns = m_stations.NumObjects();
    if (ns <= 0) {
        m_dNext = -1;
        return;
    }
    if (m_bagN <= 0) {
        m_bagN = ns;
        for (int i = 0; i < ns; i++) {
            m_bag[i] = i;
        }
        for (int i = ns - 1; i > 0; i--) {
            const int j = (int)DirRand(0.0f, (float)(i + 1) - 0.001f);
            const int t = m_bag[i];
            m_bag[i] = m_bag[j];
            m_bag[j] = t;
        }
        if (ns > 1 && m_bag[m_bagN - 1] == m_lastStation) {
            const int t = m_bag[0];
            m_bag[0] = m_bag[m_bagN - 1];
            m_bag[m_bagN - 1] = t;
        }
    }
    m_dNext = m_bag[--m_bagN];
    m_lastStation = m_dNext;
    m_pathN = 0;
    if (m_dAt >= 0) {
        dirStation_t *c = m_stations.ObjectAt(m_dAt + 1);
        for (int i = c->n - 2; i >= 0 && m_pathN < 38; i--) {
            m_path[m_pathN][0] = c->wp[i][0];
            m_path[m_pathN][1] = c->wp[i][1];
            m_pathN++;
        }
    }
    m_path[m_pathN][0] = m_hub[0];
    m_path[m_pathN][1] = m_hub[1];
    m_pathN++;
    dirStation_t *nx = m_stations.ObjectAt(m_dNext + 1);
    for (int i = 0; i < nx->n && m_pathN < 40; i++) {
        m_path[m_pathN][0] = nx->wp[i][0];
        m_path[m_pathN][1] = nx->wp[i][1];
        m_pathN++;
    }
    m_pathI = 0;
}

int UIScene3D::DirResolve(dtiki_t *tiki, const str& alts)
{
    const char *p = alts.c_str();
    while (*p) {
        const char *e = strchr(p, '|');
        char        name[64];
        size_t      n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
        }
        memcpy(name, p, n);
        name[n] = 0;
        const int idx = TIKI_Anim_NumForName(tiki, name);
        if (idx >= 0) {
            return idx;
        }
        p += n + (e ? 1 : 0);
    }
    return -1;
}

// local time of an animation play at clock `clockNow`: looped = wraps, else holds the last frame
float UIScene3D::DirAnimTime(dtiki_t *tiki, const str& alts, float clockNow, const animPlay_t& a, bool *looped)
{
    const int idx = DirResolve(tiki, alts);
    float     t   = (clockNow - a.t0) * a.rate;
    if (t < 0.0f) {
        t = 0.0f;
    }
    const float len = idx >= 0 ? TIKI_Anim_Time(tiki, idx) : 0.0f;
    if (len > 0.05f) {
        if (a.loop) {
            t = t - len * floor(t / len);
        } else if (t > len) {
            t = len - 0.001f;
        }
    }
    if (looped) {
        *looped = a.loop;
    }
    return t;
}

void UIScene3D::DirUpdate(dtiki_t *tiki, float dt)
{
    m_aClock += dt;
    if (m_dState == 0) {
        m_dPos[0] = m_hub[0];
        m_dPos[1] = m_hub[1];
        m_dYaw    = m_hub[2];
        m_dAt     = -1;
        DirSetAnim(m_idleAlts, true, 1.0f);
        m_dTimer  = DirRand(0.8f, 1.5f);
        m_dState  = 1;
        return;
    }
    if (dt <= 0.0f) {
        return;
    }
    switch (m_dState) {
    case 1:       // wait (idle / look-around), then the next station
        m_dTimer -= dt;
        if (m_dTimer <= 0.0f) {
            DirPickNext();
            if (m_dNext >= 0) {
                m_dState = 2;
            } else {
                m_dTimer = 4.0f;
            }
        }
        break;
    case 2: {     // walk the path
        if (m_pathI >= m_pathN) {
            m_dAt         = m_dNext;
            m_dTargetYaw  = m_stations.ObjectAt(m_dAt + 1)->yaw;
            DirSetAnim(m_idleAlts, true, 1.0f);
            m_dState = 3;
            break;
        }
        const float dx = m_path[m_pathI][0] - m_dPos[0], dy = m_path[m_pathI][1] - m_dPos[1];
        const float dist = sqrt(dx * dx + dy * dy);
        if (dist < 3.0f) {
            m_pathI++;
            break;
        }
        const float want = atan2(dy, dx) * 180.0f / M_PI;
        float       da   = want - m_dYaw;
        da               = da - 360.0f * floor(da / 360.0f + 0.5f);
        const float maxT = m_turnRate * dt;
        m_dYaw += (da > maxT ? maxT : (da < -maxT ? -maxT : da));
        if (fabs(da) < 45.0f) {
            DirSetAnim(m_walkAlts, true, m_speed / 75.0f);
            const float step = m_speed * dt < dist ? m_speed * dt : dist;
            const float ry   = m_dYaw * M_PI / 180.0f;
            m_dPos[0] += cos(ry) * step;
            m_dPos[1] += sin(ry) * step;
        } else {
            DirSetAnim(m_idleAlts, true, 1.0f);
        }
        break;
    }
    case 3: {     // turn to face the station, then start its sequence
        float da = m_dTargetYaw - m_dYaw;
        da       = da - 360.0f * floor(da / 360.0f + 0.5f);
        const float maxT = (m_turnRate * 0.8f) * dt;
        m_dYaw += (da > maxT ? maxT : (da < -maxT ? -maxT : da));
        if (fabs(da) < 2.0f) {
            dirStation_t *st = m_stations.ObjectAt(m_dAt + 1);
            m_seqTok   = 0;
            m_seqT0    = m_aClock;
            m_stClock0 = m_aClock;
            m_seqTotal = DirRand(st->tmin, st->tmax);
            m_dTimer   = 0.0f;
            m_dState   = 4;
        }
        break;
    }
    case 4: {     // the station's animation sequence
        dirStation_t *st = m_stations.ObjectAt(m_dAt + 1);
        // find the token m_seqTok
        const char *p = st->seq.c_str();
        for (int k = 0; k < m_seqTok && *p; k++) {
            const char *e = strchr(p, ',');
            if (!e) {
                p += strlen(p);
                break;
            }
            p = e + 1;
        }
        if (!*p) {
            DirSetAnim(m_breakAlts.length() ? m_breakAlts : m_idleAlts, true, 1.0f);
            m_dTimer = DirRand(m_breakMin, m_breakMax);
            m_dState = 1;
            break;
        }
        const char *e = strchr(p, ',');
        char        tok[96];
        size_t      n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(tok)) {
            n = sizeof(tok) - 1;
        }
        memcpy(tok, p, n);
        tok[n] = 0;
        bool loop = false;
        if (n > 0 && tok[n - 1] == '~') {
            loop     = true;
            tok[n - 1] = 0;
        }
        str alts(tok);
        if (m_dTimer == 0.0f) {          // token start
            DirSetAnim(alts, loop, 1.0f);
            m_dTimer = 0.001f;
            m_seqT0  = m_aClock;
        }
        const int   idx = DirResolve(tiki, alts);
        const float len = idx >= 0 ? TIKI_Anim_Time(tiki, idx) : 1.0f;
        const float run = m_aClock - m_seqT0;
        bool        done;
        const float stayed = m_aClock - m_stClock0;
        if (loop) {
            // a looped token fills what is left of the stay (the once-tokens before it count against it)
            done = stayed >= m_seqTotal && run >= 1.5f;
        } else {
            // a once-token plays out, but is cut short once the stay is over
            done = run >= len - 0.05f || (stayed >= m_seqTotal && run >= 2.0f);
        }
        if (done) {
            m_seqTok++;
            m_dTimer = 0.0f;
        }
        break;
    }
    }
}

void UIScene3D::ActorStage(Event *ev)
{
    const str nm = ev->GetString(1);
    for (int i = 1; i <= m_models.NumObjects(); i++) {
        sceneModel_t *m = m_models.ObjectAt(i);
        if (str::icmp(m->name, nm)) {
            continue;
        }
        if (!m->stage) {
            m->stage         = new stage_t;
            m->stage->state  = 0;
            m->stage->tok    = 0;
            m->stage->cyc    = -1;
            m->stage->clock  = m->stage->timer = m->stage->total = m->stage->cycT0 = m->stage->seqT0 = 0.0f;
            m->stage->blendT0 = -10.0f;
            m->stage->rng    = 2463534242u ^ (unsigned)(i * 40503);
            m->stage->cur.t0 = m->stage->prev.t0 = 0.0f;
            m->stage->cur.loop = m->stage->prev.loop = true;
            m->stage->cur.rate = m->stage->prev.rate = 1.0f;
        }
        m->stage->pos[0]  = ev->GetFloat(2);
        m->stage->pos[1]  = ev->GetFloat(3);
        m->stage->yaw     = ev->GetFloat(4);
        m->stage->idle    = ev->GetString(5);
        m->stage->idleMin = ev->GetFloat(6);
        m->stage->idleMax = ev->GetFloat(7);
        return;
    }
}

void UIScene3D::ActorCycle(Event *ev)
{
    const str nm = ev->GetString(1);
    for (int i = 1; i <= m_models.NumObjects(); i++) {
        sceneModel_t *m = m_models.ObjectAt(i);
        if (!m->stage || str::icmp(m->name, nm)) {
            continue;
        }
        stageCycle_t *c = new stageCycle_t;
        c->weight = ev->GetFloat(2);
        c->tmin   = ev->GetFloat(3);
        c->tmax   = ev->GetFloat(4);
        c->seq    = ev->GetString(5);
        m->stage->cycles.AddObject(c);
        return;
    }
}

void UIScene3D::ActorProp(Event *ev)
{
    const str nm = ev->GetString(1);
    for (int i = 1; i <= m_models.NumObjects(); i++) {
        sceneModel_t *m = m_models.ObjectAt(i);
        if (!m->stage || str::icmp(m->name, nm)) {
            continue;
        }
        stageProp_t *p = new stageProp_t;
        p->model  = ev->GetString(2);
        p->tag    = ev->GetString(3);
        p->when   = ev->GetString(4);
        for (int k = 0; k < 7; k++) {
            p->xf[k] = ev->GetFloat(5 + k);
        }
        p->handle = 0;
        m->stage->props.AddObject(p);
        return;
    }
}

float UIScene3D::StageRand(stage_t *s, float a, float b)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return a + (b - a) * ((s->rng & 0xFFFFFF) / 16777215.0f);
}

void UIScene3D::StageSetAnim(stage_t *s, const str& alts, bool loop, float rate)
{
    if (!s->cur.alts.length() || str::cmp(s->cur.alts, alts) || s->cur.loop != loop) {
        s->prev        = s->cur;
        s->blendT0     = s->clock;
        s->cur.alts    = alts;
        s->cur.t0      = s->clock;
        s->cur.loop    = loop;
        s->cur.rate    = rate;
    }
}

// one staged actor: idle -> (weighted) cycle -> idle ...; frozen when dt == 0 (camera away from home)
void UIScene3D::StageUpdate(sceneModel_t *m, dtiki_t *tiki, float dt)
{
    stage_t *s = m->stage;
    s->clock += dt;
    if (s->state == 0) {
        StageSetAnim(s, s->idle, true, 1.0f);
        s->timer = StageRand(s, s->idleMin, s->idleMax);
        s->state = 1;
        return;
    }
    if (dt <= 0.0f) {
        return;
    }
    if (s->state == 1) {
        s->timer -= dt;
        if (s->timer > 0.0f) {
            return;
        }
        const int nc = s->cycles.NumObjects();
        if (nc <= 0) {
            s->timer = 3.0f;
            return;
        }
        float tot = 0.0f;
        for (int i = 1; i <= nc; i++) {
            tot += s->cycles.ObjectAt(i)->weight;
        }
        float r = StageRand(s, 0.0f, tot);
        int   pick = nc - 1;
        for (int i = 1; i <= nc; i++) {
            r -= s->cycles.ObjectAt(i)->weight;
            if (r <= 0.0f) {
                pick = i - 1;
                break;
            }
        }
        stageCycle_t *c = s->cycles.ObjectAt(pick + 1);
        s->cyc    = pick;
        s->tok    = 0;
        s->total  = StageRand(s, c->tmin, c->tmax);
        s->cycT0  = s->clock;
        s->timer  = 0.0f;
        s->state  = 2;
        return;
    }
    // state 2: the cycle's animation sequence
    stageCycle_t *c = s->cycles.ObjectAt(s->cyc + 1);
    const char   *p = c->seq.c_str();
    for (int k = 0; k < s->tok && *p; k++) {
        const char *e = strchr(p, ',');
        if (!e) {
            p += strlen(p);
            break;
        }
        p = e + 1;
    }
    if (!*p) {
        StageSetAnim(s, s->idle, true, 1.0f);
        s->timer = StageRand(s, s->idleMin, s->idleMax);
        s->state = 1;
        return;
    }
    const char *e = strchr(p, ',');
    char        tok[96];
    size_t      n = e ? (size_t)(e - p) : strlen(p);
    if (n >= sizeof(tok)) {
        n = sizeof(tok) - 1;
    }
    memcpy(tok, p, n);
    tok[n] = 0;
    bool loop = false;
    if (n > 0 && tok[n - 1] == '~') {
        loop       = true;
        tok[n - 1] = 0;
    }
    const str alts(tok);
    if (s->timer == 0.0f) {
        StageSetAnim(s, alts, loop, 1.0f);
        s->timer = 0.001f;
        s->seqT0 = s->clock;
    }
    const int   idx    = DirResolve(tiki, alts);
    const float len    = idx >= 0 ? TIKI_Anim_Time(tiki, idx) : 1.0f;
    const float run    = s->clock - s->seqT0;
    const float stayed = s->clock - s->cycT0;
    bool        done;
    if (loop) {
        done = stayed >= s->total && run >= 1.5f;
    } else {
        done = run >= len - 0.05f || (stayed >= s->total && run >= 2.0f);
    }
    if (done) {
        s->tok++;
        s->timer = 0.0f;
    }
}

void UIScene3D::SceneHot(Event *ev)
{
    sceneHot_t *h = new sceneHot_t;
    h->name       = ev->GetString(1);
    h->pos[0]     = ev->GetFloat(2);
    h->pos[1]     = ev->GetFloat(3);
    h->pos[2]     = ev->GetFloat(4);
    h->radius     = ev->GetFloat(5);
    h->command    = ev->GetString(6);
    h->caption    = ev->NumArgs() >= 7 ? ev->GetString(7) : "";
    h->widget     = ev->NumArgs() >= 8 ? ev->GetString(8) : "";
    h->stop       = ev->NumArgs() >= 9 ? (int)ev->GetFloat(9) : -1;
    h->box[0] = h->box[1] = h->box[2] = h->box[3] = 0.0f;
    h->glow   = 0.0f;
    m_hots.AddObject(h);
}

void UIScene3D::SceneLabel(Event *ev)
{
    sceneLabel_t *l = new sceneLabel_t;
    l->widget = ev->GetString(1);
    l->c[0]   = ev->GetFloat(2);
    l->c[1]   = ev->GetFloat(3);
    l->c[2]   = ev->GetFloat(4);
    l->hw     = ev->GetFloat(5);
    l->hh     = ev->GetFloat(6);
    m_labels.AddObject(l);
}

void UIScene3D::SceneHotBox(Event *ev)
{
    const str nm = ev->GetString(1);
    for (int i = 1; i <= m_hots.NumObjects(); i++) {
        sceneHot_t *h = m_hots.ObjectAt(i);
        if (!str::icmp(h->name, nm)) {
            h->box[0] = ev->GetFloat(2);
            h->box[1] = ev->GetFloat(3);
            h->box[2] = ev->GetFloat(4);
            h->box[3] = ev->GetFloat(5);
            return;
        }
    }
}

void UIScene3D::SceneLight(Event *ev)
{
    sceneLight_t *l = new sceneLight_t;
    for (int i = 0; i < 3; i++) {
        l->org[i]   = ev->GetFloat(1 + i);
        l->color[i] = ev->GetFloat(5 + i);
    }
    l->radius = ev->GetFloat(4);
    m_lights.AddObject(l);
}

void UIScene3D::Project(const vec3_t p, const vec3_t org, vec3_t axis[3], float w, float h, float fovx, float fovy, float *sx, float *sy, float *depth) const
{
    vec3_t d;
    VectorSubtract(p, org, d);
    const float x = DotProduct(d, axis[0]);
    const float y = DotProduct(d, axis[1]);
    const float z = DotProduct(d, axis[2]);
    *depth        = x;
    if (x < 1.0f) {
        *sx = *sy = -99999.0f;
        return;
    }
    *sx = w * 0.5f - (y / x) * (w * 0.5f) / tan(fovx * M_PI / 360.0);
    *sy = h * 0.5f - (z / x) * (h * 0.5f) / tan(fovy * M_PI / 360.0);
}

void UIScene3D::OnClick(Event *ev)
{
    if (m_hover >= 0 && m_hover < m_hots.NumObjects() && !UIWidget::CamMoveActive()) {
        const str cmd = m_hots.ObjectAt(m_hover + 1)->command;
        Cbuf_AddText(va("%s\n", cmd.c_str()));
    }
}

void UIScene3D::Draw(void)
{
    const float W = (float)uid.vidWidth, H = (float)uid.vidHeight;

    // BACK from the opened menu: the widget is shown again away from home -> fly home by itself
    // [debug] ui_scene_pose "x y z pitch yaw roll fov": hold the camera on that pose (no fly, no auto-return)
    const char *forcePose = Cvar_VariableString("ui_scene_pose");
    if (forcePose && forcePose[0]) {
        float v[7] = {0};
        if (sscanf(forcePose, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) {
            for (int i = 0; i < 3; i++) {
                m_cur.org[i] = v[i];
                m_cur.ang[i] = v[3 + i];
            }
            m_cur.fov = v[6];
            m_flying  = false;
        } else {
            forcePose = NULL;
        }
    }
    // a flat menu pushed over the scene stops its Draw (the camera fly may be frozen half way): when the scene is drawn again after a hotspot announced the leave
    // (sceneleave), finish that fly and fly back at once
    const bool resumed = m_leaving && m_lastDraw && uid.time - m_lastDraw > 150;
    if (resumed && m_flying) {
        m_cur    = m_to;
        m_flying = false;
    }
    {
        const cam_t& tgt = m_retSet ? m_ret : m_home;
        // 1500 ms: a covered scene (a flat menu opened over it) is away for seconds; a render hitch (lazy model / shader load, ~800 ms seen) must not fly the camera home
        if (!(forcePose && forcePose[0]) && m_lastDraw && (resumed || uid.time - m_lastDraw > 1500) && !m_flying && (VectorCompare(m_cur.org, tgt.org) == 0 || m_cur.fov != tgt.fov)) {
            StartFly(tgt, 0.6f);
            m_leaving = false;
        }
    }
    if (resumed) {
        m_leaving = false;
        UIWidget::VeilSnap(1.0f);          // BACK from a flat menu: dip - black first, then the attic fades up while the camera flies home
        UIWidget::VeilTo(0.0f, 0.24f);
    }
    m_lastDraw = uid.time;
    if (m_root) {
        s_rootScene = this;
        s_rootDrawn = uid.time;
    }

    if (m_flying) {
        const bool  skipped = UIWidget::s_camSkipMs >= m_flyStart;
        float       u       = (m_flyDur > 0 && !skipped) ? (uid.time - m_flyStart) / (1000.0f * m_flyDur) : 1.0f;
        u                   = u < 0 ? 0 : (u > 1 ? 1 : u);
        const float e       = u * u * (3.0f - 2.0f * u);
        for (int i = 0; i < 3; i++) {
            m_cur.org[i] = m_from.org[i] + (m_to.org[i] - m_from.org[i]) * e;
            float da     = m_to.ang[i] - m_from.ang[i];
            da           = da - 360.0f * floor(da / 360.0f + 0.5f);       // shortest way round
            m_cur.ang[i] = m_from.ang[i] + da * e;
        }
        m_cur.fov = m_from.fov + (m_to.fov - m_from.fov) * e;
        if (u >= 1.0f) {
            m_flying = false;
            m_snap   = true;
        }
    }

    const float fovy = m_cur.fov;
    const float fovx = 2.0f * atan(tan(fovy * M_PI / 360.0) * W / H) * 180.0f / M_PI;

    refdef_t rd {};
    rd.x       = 0;
    rd.y       = 0;
    rd.width   = (int)W;
    rd.height  = (int)H;
    rd.fov_x   = fovx;
    rd.fov_y   = fovy;
    rd.rdflags = RDF_HUD | RDF_NOWORLDMODEL;
    rd.time    = uid.time;
    VectorCopy(m_cur.org, rd.vieworg);
    AnglesToAxis(m_cur.ang, rd.viewaxis);

    if (m_drawn == 0) {
        // coin flip per choice group (each time the world loads): keep exactly one member of every group
        for (int i = 1; i <= m_models.NumObjects(); i++) {
            sceneModel_t *a = m_models.ObjectAt(i);
            if (!a->group.length()) {
                continue;
            }
            int members[16], n = 0;
            for (int j = 1; j <= m_models.NumObjects() && n < 16; j++) {
                if (!str::cmp(m_models.ObjectAt(j)->group, a->group)) {
                    members[n++] = j;
                }
            }
            if (!n || members[0] != i) {
                continue;          // roll each group once, from its first member
            }
            const unsigned seed = (unsigned)Sys_Milliseconds() * 2654435761u + (unsigned)i * 40503u;
            const int      pick = n ? (int)((seed >> 12) % (unsigned)n) : 0;
            for (int k = 0; k < n; k++) {
                m_models.ObjectAt(members[k])->off = (k != pick);
            }
            if (Cvar_VariableIntegerValue("ui_scene_debug")) {
                Com_Printf("^~^~^ SCENE3D choice '%s': member %d of %d -> %s\n", a->group.c_str(), pick + 1, n, m_models.ObjectAt(members[pick])->name.c_str());
            }
        }
    }
    m_drawn++;
    re.ClearScene();
    for (int i = 1; i <= m_lights.NumObjects(); i++) {
        sceneLight_t *l = m_lights.ObjectAt(i);
        re.AddLightToScene(l->org, l->radius, l->color[0], l->color[1], l->color[2], 0);
    }
    if (m_rendEpoch != s_sceneRendererEpoch) {
        // the renderer was restarted under us (vid_restart): every cached handle is stale - register all models again
        m_rendEpoch = s_sceneRendererEpoch;
        for (int i = 1; i <= m_models.NumObjects(); i++) {
            sceneModel_t *mm = m_models.ObjectAt(i);
            mm->handle = 0;
            if (mm->stage) {
                for (int k = 1; k <= mm->stage->props.NumObjects(); k++) {
                    mm->stage->props.ObjectAt(k)->handle = 0;
                }
            }
        }
    }
    const bool dbg = Cvar_VariableIntegerValue("ui_scene_debug") != 0;
    const char *lifeCv0 = Cvar_VariableString("ui_atticActorLife");      // unset = on; 0 = the actors stand in a static idle
    const bool  lifeOn  = !lifeCv0[0] || atoi(lifeCv0) != 0;
    float       stageDt = 0.0f;
    {
        // staged actors advance only while the camera rests at the home view (frozen during a fly / at the shelf)
        // [v1.10.42] user: "they tend to freeze anims when you jump from options and other places instead of constantly doing them" - the
        // exact home-pose compare missed a camera that came back a hair off home, so the cast stayed frozen. They live all the time now.
        const bool atHome0 = true;
        if (m_stageLast && atHome0) {
            stageDt = (uid.time - m_stageLast) * 0.001f / UIWidget::CamSlow();
            if (stageDt > 0.1f) {
                stageDt = 0.1f;
            }
        }
        m_stageLast = uid.time;
    }
    for (int i = 1; i <= m_models.NumObjects(); i++) {
        sceneModel_t *m = m_models.ObjectAt(i);
        if (m->off) {
            continue;
        }
        if (!m->handle && m->anim.length() && m_drawn < 45) {
            continue;       // actors load lazily, a moment after the menu is up
        }
        if (!m->handle) {
            const int t0 = Sys_Milliseconds();
            m->handle = re.RegisterModel(m->name.c_str());
            if (dbg) {
                Com_Printf("^~^~^ SCENE3D load %s %d ms\n", m->name.c_str(), Sys_Milliseconds() - t0);
            }
            if (!m->handle) {
                m->handle = -1;
            } else if (Cvar_VariableIntegerValue("ui_atticStageDbg")) {
                vec3_t bmn, bmx;
                re.ModelBounds(m->handle, bmn, bmx);
                Com_Printf("^~^~^ MODELBOUNDS %s mins %.1f %.1f %.1f maxs %.1f %.1f %.1f\n", m->name.c_str(), bmn[0], bmn[1], bmn[2], bmx[0], bmx[1], bmx[2]);
            }
        }
        if (m->handle <= 0) {
            continue;
        }
        refEntity_t ent {};
        ent.scale        = 1.0f;
        ent.hModel       = m->handle;
        ent.entityNumber = ENTITYNUM_NONE;
        ent.tiki         = re.R_Model_GetHandle(m->handle);
        if (!ent.tiki) {
            continue;
        }
        VectorCopy(m->origin, ent.origin);
        vec3_t a = {0, m->yaw, 0};
        const char *lifeCv = Cvar_VariableString("ui_atticActorLife");      // unset = on; 0 = the old static idle
        const bool directed = m_dirOn && m->group.length() && (!lifeCv[0] || atoi(lifeCv) != 0);
        const bool staged   = m->stage != NULL;
        if (staged) {
            if (lifeOn) {
                StageUpdate(m, ent.tiki, stageDt);
            }
            ent.origin[0] = m->stage->pos[0];
            ent.origin[1] = m->stage->pos[1];
            a[1]          = m->stage->yaw;
        }
        if (directed) {
            // the director advances only while the camera rests at the home view (frozen during a fly / at the shelf; a covered scene is not drawn at all)
            const bool atHome = true;   // [v1.10.42] always live, as the staged cast above
            float      dt     = 0.0f;
            if (m_dirLast && atHome) {
                dt = (uid.time - m_dirLast) * 0.001f / UIWidget::CamSlow();
                if (dt > 0.1f) {
                    dt = 0.1f;
                }
            }
            m_dirLast = uid.time;
            if (m_dirFrame != m_drawn) {
                m_dirFrame = m_drawn;
                DirUpdate(ent.tiki, dt);
            }
            ent.origin[0] = m_dPos[0];
            ent.origin[1] = m_dPos[1];
            a[1]          = m_dYaw;
        }
        AnglesToAxis(a, ent.axis);
        ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
        if (m->anim.length() && staged && lifeOn) {
            stage_t *st = m->stage;
            ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = 150;
            const int idxC = DirResolve(ent.tiki, st->cur.alts);
            const int idxI = DirResolve(ent.tiki, st->idle);
            const int idx0 = idxC >= 0 ? idxC : idxI;
            float     w    = (st->clock - st->blendT0) / 0.30f;
            w              = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
            if (idx0 >= 0) {
                ent.frameInfo[0].index  = idx0;
                ent.frameInfo[0].time   = DirAnimTime(ent.tiki, idxC >= 0 ? st->cur.alts : st->idle, st->clock, st->cur, NULL);
                ent.frameInfo[0].weight = w < 1.0f && st->prev.alts.length() ? w : 1.0f;
                ent.actionWeight        = 1.0f;
                if (w < 1.0f && st->prev.alts.length()) {
                    const int idxP = DirResolve(ent.tiki, st->prev.alts);
                    if (idxP >= 0) {
                        ent.frameInfo[1].index  = idxP;
                        ent.frameInfo[1].time   = DirAnimTime(ent.tiki, st->prev.alts, st->clock, st->prev, NULL);
                        ent.frameInfo[1].weight = 1.0f - w;
                    }
                }
            }
        } else if (m->anim.length() && directed) {
            ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = 150;
            const int idxC = DirResolve(ent.tiki, m_aCur.alts);
            const int idxI = DirResolve(ent.tiki, m_idleAlts);
            const int idx0 = idxC >= 0 ? idxC : idxI;
            float     w    = (m_aClock - m_blendT0) / 0.30f;
            w              = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
            if (idx0 >= 0) {
                ent.frameInfo[0].index  = idx0;
                ent.frameInfo[0].time   = DirAnimTime(ent.tiki, idxC >= 0 ? m_aCur.alts : m_idleAlts, m_aClock, m_aCur, NULL);
                ent.frameInfo[0].weight = w < 1.0f && m_aPrev.alts.length() ? w : 1.0f;
                ent.actionWeight        = 1.0f;
                if (w < 1.0f && m_aPrev.alts.length()) {
                    const int idxP = DirResolve(ent.tiki, m_aPrev.alts);
                    if (idxP >= 0) {
                        ent.frameInfo[1].index  = idxP;
                        ent.frameInfo[1].time   = DirAnimTime(ent.tiki, m_aPrev.alts, m_aClock, m_aPrev, NULL);
                        ent.frameInfo[1].weight = 1.0f - w;
                    }
                }
            }
        } else if (m->anim.length()) {
            ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = 150;   // actors are fullbright without a world: pull them down to the room's exposure
            const int idx = TIKI_Anim_NumForName(ent.tiki, m->anim.c_str());
            if (idx >= 0) {
                if (!m->animStart) {
                    m->animStart = uid.time;
                }
                float t   = (uid.time - m->animStart) * 0.001f / UIWidget::CamSlow();   // ui_camslow (video capture): the actor idles on the slowed clock too
                float len = TIKI_Anim_Time(ent.tiki, idx);
                if (len > 0.05f) {
                    t = t - len * floor(t / len);
                }
                ent.frameInfo[0].index  = idx;
                ent.frameInfo[0].time   = t;
                ent.frameInfo[0].weight = 1.0f;
                ent.actionWeight        = 1.0f;
            }
        }
        if (m->hide.length() && ent.tiki->num_surfaces > 0) {
            for (int sidx = 0; sidx < ent.tiki->num_surfaces && sidx < MAX_MODEL_SURFACES; sidx++) {
                const char *sn = ent.tiki->surfaces[sidx].name;
                const char *h  = m->hide.c_str();
                const size_t L = strlen(sn);
                while (*h) {
                    const char *e = strchr(h, ',');
                    const size_t n = e ? (size_t)(e - h) : strlen(h);
                    if (n == L && !Q_stricmpn(sn, h, (int)n)) {
                        ent.surfaces[sidx] |= TIKI_SURF_NODRAW;
                    }
                    h += n + (e ? 1 : 0);
                }
            }
        }
        if (dbg && m->anim.length()) Com_Printf("^~^~^ SCENE3D actor add idx tiki=%p surfs=%d\n", (void *)ent.tiki, ent.tiki->num_surfaces);
        re.AddRefEntityToScene(&ent, ENTITYNUM_NONE);
        if (m->stage && lifeOn) {
            stage_t *st = m->stage;
            for (int k = 1; k <= st->props.NumObjects(); k++) {
                stageProp_t *pr = st->props.ObjectAt(k);
                if (pr->when.length() && !strstr(st->cur.alts.c_str(), pr->when.c_str())) {
                    continue;
                }
                if (!pr->handle) {
                    pr->handle = re.RegisterModel(pr->model.c_str());
                    if (!pr->handle) {
                        pr->handle = -1;
                    }
                }
                if (pr->handle > 0) {
                    CL_CompositeAttachOnTagXf(ent, pr->handle, pr->tag.c_str(), pr->xf);
                }
            }
            if (Cvar_VariableIntegerValue("ui_atticStageDbg")) {
                static int lastDbg = 0;
                if (uid.time - lastDbg > 500) {
                    lastDbg = uid.time;
                    static const char *tags[] = {"Bip01 L Hand", "Bip01 R Hand", "tag_weapon_left", "tag_weapon_right", "Bip01 L Finger1", "Bip01 R Finger1", "Bip01 Head"};
                    for (int q = 0; q < 7; q++) {
                        const int tn = TIKI_Tag_NameToNum(ent.tiki, tags[q]);
                        if (tn < 0) {
                            Com_Printf("^~^~^ STAGEDBG %s tag %s MISSING\n", m->name.c_str(), tags[q]);
                            continue;
                        }
                        re.ForceUpdatePose(&ent);
                        orientation_t o = re.TIKI_Orientation(&ent, tn);
                        vec3_t        wv;
                        MatrixTransformVector(o.origin, ent.axis, wv);
                        VectorAdd(wv, ent.origin, wv);
                        Com_Printf("^~^~^ STAGEDBG %s %s %.1f %.1f %.1f anim=%s\n", m->name.c_str(), tags[q], wv[0], wv[1], wv[2], st->cur.alts.c_str());
                    }
                }
            }
        }
        if (dbg && m->anim.length()) Com_Printf("^~^~^ SCENE3D actor added\n");
    }
    if (dbg) Com_Printf("^~^~^ SCENE3D render begin\n");
    if (dbg) {
        static int lastMs = 0, nf = 0, accMs = 0;
        const int  nowMs = Sys_Milliseconds();
        if (lastMs) {
            accMs += nowMs - lastMs;
            if (++nf == 240) {
                Com_Printf("^~^~^ SCENE3D frame %.2f ms avg (%.0f fps) over 240 frames, %dx%d\n", accMs / 240.0f, 240000.0f / accMs, (int)W, (int)H);
                nf = accMs = 0;
            }
        }
        lastMs = nowMs;
    }
    const int tr0 = Sys_Milliseconds();
    re.RenderScene(&rd);
    if (dbg) {
        static int   n = 0, acc = 0;
        acc += Sys_Milliseconds() - tr0;
        if (++n == 120) {
            Com_Printf("^~^~^ SCENE3D RenderScene cpu %.2f ms/frame (120 frames)\n", acc / 120.0f);
            n = acc = 0;
        }
    }

    // hotspots follow the camera. A hotspot with a focus widget re-frames that sibling Button to its projected rect, so the stock input stack (mouse hover/click,
    // keyboard + D-pad focus moves the cursor onto it, A/Enter) drives it; the button's own hovercommand sets ui_scene_hot. Unbound hotspots use a plain cursor test.
    int   hover = -1;
    float best  = 1e9f;
    const bool cvarDriven = Cvar_VariableIntegerValue("ui_scene_hot") > 0;
    for (int i = 1; i <= m_hots.NumObjects(); i++) {
        sceneHot_t *h = m_hots.ObjectAt(i);
        float       sx, sy, depth;
        Project(h->pos, rd.vieworg, rd.viewaxis, W, H, fovx, fovy, &sx, &sy, &depth);
        const bool  stopOk = h->stop < 0 || Cvar_VariableIntegerValue("ui_atcStop") == h->stop;
        const float rpx = depth > 1.0f ? h->radius / depth * (H * 0.5f) / tan(fovy * M_PI / 360.0) : 0.0f;
        if (h->widget.length()) {
            if (!h->w && m_parent) {
                h->w = m_parent->FindWidgetByName(h->widget.c_str());
            }
            if (h->w) {
                const bool live = depth > 1.0f && !m_flying && stopOk;
                // frame is relative to the parent container, whose origin is this widget's screen origin
                if (live && m_snap) {
                    m_snap = false;
                    if (Cvar_VariableIntegerValue("ui_keyNavActive")) {
                        CL_SetMousePos((int)sx, (int)sy);
                    }
                }
                float hx = rpx, hy = rpx, cx = sx, cy = sy;
                if (h->box[2] > 0.0f) {
                    const float k = H / 1080.0f;
                    cx += h->box[0] * k;
                    cy += h->box[1] * k;
                    hx = h->box[2] * k;
                    hy = h->box[3] * k;
                }
                h->w->setFrame(UIRect2D(live ? cx - hx - m_screenframe.pos.x : -9999.0f, live ? cy - hy - m_screenframe.pos.y : -9999.0f, live ? hx * 2.0f : 1.0f, live ? hy * 2.0f : 1.0f));
            }
            continue;
        }
        if (depth < 1.0f || !stopOk) {
            continue;
        }
        const float dx = uid.mouseX - sx, dy = uid.mouseY - sy;
        if (dx * dx + dy * dy <= rpx * rpx && depth < best) {
            best  = depth;
            hover = i - 1;
        }
    }
    {
        // chalk captions: frame each label to its 3D board while the camera rests at home; off-screen otherwise
        const bool atHomeL = !m_flying && VectorCompare(m_cur.org, m_home.org) && m_cur.fov == m_home.fov;
        for (int i = 1; i <= m_labels.NumObjects(); i++) {
            sceneLabel_t *l = m_labels.ObjectAt(i);
            if (!l->w && m_parent) {
                l->w = m_parent->FindWidgetByName(l->widget.c_str());
            }
            if (!l->w) {
                continue;
            }
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            bool  ok = atHomeL;
            for (int k = 0; k < 4 && ok; k++) {
                vec3_t p = {l->c[0], l->c[1] + ((k & 1) ? l->hw : -l->hw), l->c[2] + ((k & 2) ? l->hh : -l->hh)};
                float  sx, sy, depth;
                Project(p, rd.vieworg, rd.viewaxis, W, H, fovx, fovy, &sx, &sy, &depth);
                if (depth < 1.0f) {
                    ok = false;
                    break;
                }
                minx = sx < minx ? sx : minx;
                maxx = sx > maxx ? sx : maxx;
                miny = sy < miny ? sy : miny;
                maxy = sy > maxy ? sy : maxy;
            }
            l->w->setFrame(UIRect2D(ok ? minx - m_screenframe.pos.x : -9999.0f, ok ? miny - m_screenframe.pos.y : -9999.0f, ok ? maxx - minx : 1.0f, ok ? maxy - miny : 1.0f));
            // [v1.10.43] user: "font sizes need to be correct for each one so they don't run off" - scale the caption so it fills ~88% of the
            // board's width and ~70% of its height, whichever is tighter, at any resolution and for any caption length.
            if (ok && l->w->getFont() && l->w->getTitle() && l->w->getTitle()[0]) {
                const float tw = (float)l->w->getFont()->getWidth(l->w->getTitle(), -1);
                const float th = (float)l->w->getFont()->getHeight(NULL);
                if (tw > 0.0f && th > 0.0f) {
                    float s  = 0.88f * (maxx - minx) / tw;
                    float s2 = 0.70f * (maxy - miny) / th;
                    if (s2 < s) {
                        s = s2;
                    }
                    l->w->setTextFitScale(s, s);
                }
            }
        }
    }
    m_snap = false;
    bool anyBound = false;
    for (int i = 1; i <= m_hots.NumObjects(); i++) {
        if (m_hots.ObjectAt(i)->widget.length()) {
            anyBound = true;
        }
    }
    if (anyBound) {
        hover = m_flying ? -1 : Cvar_VariableIntegerValue("ui_scene_hot") - 1;          // set by the proxy button's hover / focus commands
        if (hover >= m_hots.NumObjects()) {
            hover = -1;
        }
        m_hover = hover;
    } else if (hover != m_hover) {
        m_hover = hover;
        Cvar_Set("ui_scene_hot", va("%d", hover + 1));    // 1-based hotspot index, 0 = none (enabledcvar compares numerically)
    }
    (void)cvarDriven;
    set2D();
    // hover light: the hovered / focused hotspot brightens as if its lamp were turned up - an additive warm glow over the object / area, faded smoothly (~0.2 s) in and out.
    // (The room is baked, so a dynamic light cannot light it; the glow is the 2D stand-in.)  No ring, no caption: the name goes on the chalkboard (scenelabel).
    {
        static UIReggedMaterial *s_glow = NULL;
        if (!s_glow) {
            s_glow = uWinMan.RegisterShader("hzm_hot_glow");
        }
        float dtg = 0.0f;
        if (m_glowLast) {
            dtg = (uid.time - m_glowLast) * 0.001f / UIWidget::CamSlow();
            if (dtg > 0.1f) {
                dtg = 0.1f;
            }
        }
        m_glowLast = uid.time;
        const float stepg = dtg / 0.2f;
        for (int i = 1; i <= m_hots.NumObjects(); i++) {
            sceneHot_t *h      = m_hots.ObjectAt(i);
            const float target = (i - 1 == m_hover && !m_flying) ? 1.0f : 0.0f;
            h->glow += (target > h->glow) ? (target - h->glow < stepg ? target - h->glow : stepg) : -(h->glow - target < stepg ? h->glow - target : stepg);
            if (h->glow < 0.004f || !s_glow) {
                continue;
            }
            float sx, sy, depth;
            Project(h->pos, rd.vieworg, rd.viewaxis, W, H, fovx, fovy, &sx, &sy, &depth);
            if (depth < 1.0f) {
                continue;
            }
            const float rpx = h->radius / depth * (H * 0.5f) / tan(fovy * M_PI / 360.0);
            float       cx = sx, cy = sy, hx = rpx * 1.9f, hy = rpx * 1.9f;
            if (h->box[2] > 0.0f) {
                const float kk = H / 1080.0f;
                cx += h->box[0] * kk;
                cy += h->box[1] * kk;
                hx = h->box[2] * kk * 1.7f;
                hy = h->box[3] * kk * 1.45f;
            }
            const float I = 0.62f * h->glow * h->glow * (3.0f - 2.0f * h->glow);     // smoothstep
            vec4_t      col = {0.42f * I, 0.74f * I, 1.0f * I, 1.0f};                  // the 2D path takes the vertex colour as B,G,R,A: warm lamp light
            uii.Rend_SetColor(col);
            uii.Rend_DrawPicStretched(cx - m_screenframe.pos.x - hx, cy - m_screenframe.pos.y - hy, hx * 2.0f, hy * 2.0f, 0, 0, 1, 1, s_glow->GetMaterial());
        }
    }
}
