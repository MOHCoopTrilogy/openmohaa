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
#ifdef _WIN32
#include <windows.h>
// [spike diagnostic] log the faulting address + stack of the first access violation to a file (resolved offline against the PDB)
static LONG WINAPI Scene3D_VEH(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 || ep->ExceptionRecord->ExceptionCode == 0xC0000094) {
        FILE *f = fopen("G:\\mohaa-hudfeed3\\menus3d\\crash.txt", "a");
        if (f) {
            void *st[48];
            USHORT n = RtlCaptureStackBackTrace(0, 48, st, NULL);
            fprintf(f, "code %08lx addr %p base %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, (void *)GetModuleHandleA(NULL));
            for (USHORT i = 0; i < n; i++) {
                fprintf(f, "  %p\n", st[i]);
            }
            fclose(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#include "../qcommon/tiki.h"

class UIScene3D : public UIWidget
{
    struct sceneModel_t {
        str       name;
        qhandle_t handle;
        vec3_t    origin;
        float     yaw;
        str       anim;      // optional: loop this TIKI animation (actors)
        str       hide;      // optional: comma list of surface names to nodraw (helmet indoors)
        int       animStart;
    };
    struct sceneHot_t {
        str    name;
        vec3_t pos;
        float  radius;
        str    command;
        str    caption;
        str    widget;                 // optional: name of a sibling Button that is re-framed to this hotspot every frame (mouse / keyboard / pad focus all work on it)
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
    int                       m_drawn;
    cam_t                     m_cur, m_from, m_to, m_home;
    int                       m_flyStart;
    float                     m_flyDur;
    bool                      m_flying;
    int                       m_lastDraw;
    int                       m_hover;

public:
    CLASS_PROTOTYPE(UIScene3D);

    UIScene3D();
    void SceneModel(Event *ev);
    void SceneCam(Event *ev);
    void SceneFly(Event *ev);
    void SceneHot(Event *ev);
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
    "sffffSS",
    "model x y z yaw [anim] [hidesurfaces]",
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
Event EV_Scene_Hot
(
    "scenehot",
    EV_DEFAULT,
    "sffffsSS",
    "name x y z radius command [caption] [focuswidget]",
    "Scene3D: a clickable sphere in the scene; hover sets ui_scene_hot (1-based index, 0 = none) and prints the caption beside the object, a click stuffs the command."
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
    {&EV_Scene_Light,  &UIScene3D::SceneLight},
    {&W_LeftMouseDown, &UIScene3D::OnClick   },
    {NULL,             NULL                  }
};

UIScene3D::UIScene3D()
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, Scene3D_VEH);
#endif
    memset(&m_cur, 0, sizeof(m_cur));
    m_cur.fov = 60;
    m_from = m_to = m_home = m_cur;
    m_flyStart = 0;
    m_flyDur   = 0;
    m_flying   = false;
    m_lastDraw = 0;
    m_hover    = -1;
    m_drawn    = 0;
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
    m->animStart    = 0;
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
    m_hots.AddObject(h);
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
    if (m_lastDraw && uid.time - m_lastDraw > 250 && !m_flying
        && (VectorCompare(m_cur.org, m_home.org) == 0 || m_cur.fov != m_home.fov)) {
        StartFly(m_home, 0.6f);
    }
    m_lastDraw = uid.time;

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

    m_drawn++;
    re.ClearScene();
    for (int i = 1; i <= m_lights.NumObjects(); i++) {
        sceneLight_t *l = m_lights.ObjectAt(i);
        re.AddLightToScene(l->org, l->radius, l->color[0], l->color[1], l->color[2], 0);
    }
    const bool dbg = Cvar_VariableIntegerValue("ui_scene_debug") != 0;
    for (int i = 1; i <= m_models.NumObjects(); i++) {
        sceneModel_t *m = m_models.ObjectAt(i);
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
        AnglesToAxis(a, ent.axis);
        ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
        if (m->anim.length()) {
            ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = 150;   // actors are fullbright without a world: pull them down to the room's exposure
            const int idx = TIKI_Anim_NumForName(ent.tiki, m->anim.c_str());
            if (idx >= 0) {
                if (!m->animStart) {
                    m->animStart = uid.time;
                }
                float t   = (uid.time - m->animStart) * 0.001f;
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
        if (dbg && m->anim.length()) Com_Printf("^~^~^ SCENE3D actor added\n");
    }
    if (dbg) Com_Printf("^~^~^ SCENE3D render begin\n");
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
        const float rpx = depth > 1.0f ? h->radius / depth * (H * 0.5f) / tan(fovy * M_PI / 360.0) : 0.0f;
        if (h->widget.length()) {
            if (!h->w && m_parent) {
                h->w = m_parent->FindWidgetByName(h->widget.c_str());
            }
            if (h->w) {
                const bool live = depth > 1.0f && !m_flying;
                // frame is relative to the parent container, whose origin is this widget's screen origin
                h->w->setFrame(UIRect2D(live ? sx - rpx - m_screenframe.pos.x : -9999.0f, live ? sy - rpx - m_screenframe.pos.y : -9999.0f, live ? rpx * 2.0f : 1.0f, live ? rpx * 2.0f : 1.0f));
            }
            continue;
        }
        if (depth < 1.0f) {
            continue;
        }
        const float dx = uid.mouseX - sx, dy = uid.mouseY - sy;
        if (dx * dx + dy * dy <= rpx * rpx && depth < best) {
            best  = depth;
            hover = i - 1;
        }
    }
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
    if (m_hover >= 0 && m_hover < m_hots.NumObjects() && !m_flying) {
        static UIReggedMaterial *s_ring = NULL;
        if (!s_ring) {
            s_ring = uWinMan.RegisterShader("hzm_hot_ring");
        }
        sceneHot_t *h = m_hots.ObjectAt(m_hover + 1);
        float       sx, sy, depth;
        Project(h->pos, rd.vieworg, rd.viewaxis, W, H, fovx, fovy, &sx, &sy, &depth);
        if (s_ring && depth > 1.0f) {
            const float rpx   = h->radius / depth * (H * 0.5f) / tan(fovy * M_PI / 360.0) * (1.12f + 0.04f * sin(uid.time * 0.006f));
            vec4_t      col   = {1.0f, 0.82f, 0.45f, 0.85f};
            uii.Rend_SetColor(col);
            uii.Rend_DrawPicStretched(sx - rpx, sy - rpx, rpx * 2.0f, rpx * 2.0f, 0, 0, 1, 1, s_ring->GetMaterial());
        }
    }
    if (m_hover >= 0 && m_hover < m_hots.NumObjects() && !m_flying && m_font) {
        sceneHot_t *h = m_hots.ObjectAt(m_hover + 1);
        if (h->caption.length()) {
            float sx, sy, depth;
            Project(h->pos, rd.vieworg, rd.viewaxis, W, H, fovx, fovy, &sx, &sy, &depth);
            if (depth > 1.0f) {
                const float sc[2] = {H / 480.0f, H / 480.0f};
                const int   tw    = m_font->getWidth(h->caption.c_str(), -1);
                const float rpx   = h->radius / depth * (H * 0.5f) / tan(fovy * M_PI / 360.0);
                m_font->Print(sx - tw * sc[0] * 0.5f, sy + rpx * 0.55f, h->caption.c_str(), (size_t)-1, sc);
            }
        }
    }
}
