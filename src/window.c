#include "window.h"

#include "vmath.h"
#include "bezier.h"
#include "windowlist.h"
#include "blur.h"
#include "assets/assets.h"
#include "profiler/zone.h"
#include "assets/shader.h"
#include "shaders/shaderinfo.h"
#include "xtexture.h"
#include "textureeffects.h"
#include "renderutil.h"
#include "shadow.h"

DECLARE_ZONE(update_window);
DECLARE_ZONE(update_fade);

static bool win_viewable(win* w) {
    return w->state == STATE_DEACTIVATING || w->state == STATE_ACTIVATING
        || w->state == STATE_ACTIVE || w->state == STATE_INACTIVE
        || w->state == STATE_HIDING || w->state == STATE_DESTROYING;
}

bool win_overlap(win* w1, win* w2) {
    const Vector2 w1lpos = {{
        w1->a.x, w1->a.y,
    }};
    const Vector2 w1rpos = {{
        w1->a.x + w1->widthb, w1->a.y + w1->heightb,
    }};
    const Vector2 w2lpos = {{
        w2->a.x, w2->a.y,
    }};
    const Vector2 w2rpos = {{
        w2->a.x + w2->widthb, w2->a.y + w2->heightb,
    }};
    // Horizontal collision
    if (w1lpos.x > w2rpos.x || w2lpos.x > w1rpos.x)
        return false;

    // Vertical collision
    if (w1lpos.y > w2rpos.y || w2lpos.y > w1rpos.y)
        return false;

    return true;
}

bool win_covers(win* w) {
    return w->solid
        && w->fullscreen
        && !w->unredir_if_possible_excluded;
}

static Vector2 X11_rectpos_to_gl(session_t *ps, const Vector2* xpos, const Vector2* size) {
    Vector2 glpos = {{
        xpos->x, ps->root_height - xpos->y - size->y
    }};
    return glpos;
}

bool win_calculate_blur(struct blur* blur, session_t* ps, win* w) {
    const bool have_scissors = glIsEnabled(GL_SCISSOR_TEST);
    const bool have_stencil = glIsEnabled(GL_STENCIL_TEST);

    Vector2 pos = {{w->a.x, w->a.y}};
    Vector2 size = {{w->widthb, w->heightb}};

    struct Texture* tex = &w->glx_blur_cache.texture[0];
    // Read destination pixels into a texture

    Vector2 glpos = X11_rectpos_to_gl(ps, &pos, &size);
    /* texture_read_from(tex, 0, GL_BACK, &glpos, &size); */

    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    framebuffer_resetTarget(&blur->fbo);
    framebuffer_targetRenderBuffer_stencil(&blur->fbo, &w->glx_blur_cache.stencil);
    framebuffer_targetTexture(&blur->fbo, tex);
    framebuffer_bind(&blur->fbo);

    glClearColor(0.0, 1.0, 0.0, 1.0);

    glClearDepth(0.0);
    glDepthFunc(GL_GREATER);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);

    glViewport(0, 0, size.x, size.y);
    Matrix old_view = view;
    view = mat4_orthogonal(glpos.x, glpos.x + size.x, glpos.y, glpos.y + size.y, -1, 1);

    float z = 0;
    windowlist_drawoverlap(ps, w->next_trans, w, &z);

    Vector2 root_size = {{ps->root_width, ps->root_height}};

    struct shader_program* global_program = assets_load("passthough.shader");
    if(global_program->shader_type_info != &passthough_info) {
        printf_errf("Shader was not a passthough shader\n");
        return false;
    }
    struct face* face = assets_load("window.face");

    draw_tex(face, &ps->root_texture.texture, &VEC3_ZERO, &root_size);

    view = old_view;

    // Disable the options. We will restore later
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    int level = ps->o.blur_level;

    struct TextureBlurData blurData = {
        .buffer = &blur->fbo,
        .swap = &w->glx_blur_cache.texture[1],
    };
    // Do the blur
    if(!texture_blur(&blurData, tex, level, false)) {
        printf_errf("Failed blurring the background texture");

        if (have_scissors)
            glEnable(GL_SCISSOR_TEST);
        if (have_stencil)
            glEnable(GL_STENCIL_TEST);
        return false;
    }
    return true;
}

void win_start_opacity(win* w, double opacity, double duration) {
    // Fast path for skipping fading
    if(duration == 0) {
        w->opacity_fade.head = 0;
        w->opacity_fade.tail = 0;
        w->opacity_fade.keyframes[0].target = opacity;
        w->opacity_fade.keyframes[0].time = 0;
        w->opacity_fade.keyframes[0].duration = -1;
    }

    size_t nextIndex = (w->opacity_fade.tail + 1) % 4;
    if(nextIndex == w->opacity_fade.head) {
        printf("Warning: Shoving something off the opacity animation\n");
        w->opacity_fade.head++;
    }

    struct FadeKeyframe* keyframe = &w->opacity_fade.keyframes[nextIndex];
    keyframe->target = opacity;
    keyframe->duration = duration;
    keyframe->time = 0;
    keyframe->ignore = true;
    w->opacity_fade.tail = nextIndex;
}

bool fade_done(struct Fading* fade) {
    return fade->tail == fade->head;
}

static void finish_destroy_win(session_t *ps, Window id) {
    win **prev = NULL, *w = NULL;

#ifdef DEBUG_EVENTS
    printf_dbgf("(%#010lx): Starting...\n", id);
#endif

    for (prev = &ps->list; (w = *prev); prev = &w->next) {
        if (w->id == id && w->destroyed) {
#ifdef DEBUG_EVENTS
            printf_dbgf("(%#010lx \"%s\"): %p\n", id, w->name, w);
#endif

            *prev = w->next;

            // Clear active_win if it's pointing to the destroyed window
            if (w == ps->active_win)
                ps->active_win = NULL;

            // Drop w from all prev_trans to avoid accessing freed memory in
            for (win *w2 = ps->list; w2; w2 = w2->next) {
                if (w == w2->prev_trans)
                    w2->prev_trans = NULL;
                if (w == w2->next_trans)
                    w2->next_trans = NULL;
            }

            free(w);
            break;
        }
    }
}

void win_update(session_t* ps, win* w, double dt) {
    Vector2 pos = {{w->a.x, w->a.y}};
    Vector2 size = {{w->widthb, w->heightb}};

    w->opacity_fade.value = w->opacity_fade.keyframes[w->opacity_fade.head].target;

    zone_enter(&ZONE_update_window);
    if(!fade_done(&w->opacity_fade)) {
        zone_enter(&ZONE_update_fade);
        // @CLEANUP: Maybe a while loop?
        for(size_t i = w->opacity_fade.head; i != w->opacity_fade.tail; ) {
            // +1 to skip the head
            i = (i+1) % 4;

            struct FadeKeyframe* keyframe = &w->opacity_fade.keyframes[i];
            if(keyframe->ignore == false){
                keyframe->time += dt;
            } else {
                keyframe->ignore = false;
            }

            double x = keyframe->time / keyframe->duration;
            if(x >= 1.0) {
                // We're done, clean out the time and set this as the head
                keyframe->time = 0.0;
                w->opacity_fade.head = i;

                // Force the value. We are still going to blend it with stuff
                // on top of this
                w->opacity_fade.value = keyframe->target;
            } else {
                double t = bezier_getTForX(&ps->curve, x);
                w->opacity_fade.value = lerp(w->opacity_fade.value, keyframe->target, t);
            }
        }
        ps->skip_poll = true;

        // If the fade isn't done, then damage the blur of everyone above
        for (win *t = w->prev_trans; t; t = t->prev_trans) {
            // @CLEANUP: Ideally we should just recalculate the blur right now. We need
            // to render the windows behind this though, and that takes time. For now we
            // just do it indirectly
            if(win_overlap(w, t))
                t->glx_blur_cache.damaged = true;
        }
        zone_leave(&ZONE_update_fade);
    }

    if(fade_done(&w->opacity_fade)) {
        if(w->state == STATE_ACTIVATING) {
            w->state = STATE_ACTIVE;

            w->in_openclose = false;
        } else if(w->state == STATE_DEACTIVATING) {
            w->state = STATE_INACTIVE;
        } else if(w->state == STATE_HIDING) {
            w->damaged = false;

            w->in_openclose = false;

            free_region(ps, &w->border_size);
            if(ps->redirected)
                wd_unbind(&w->drawable);

            w->state = STATE_INVISIBLE;
        } else if(w->state == STATE_DESTROYING) {
            w->state = STATE_DESTROYED;
        }
        void (*old_callback) (session_t *ps, win *w) = w->fade_callback;
        w->fade_callback = NULL;
        if(old_callback != NULL) {
            old_callback(ps, w);
            ps->idling = false;
        }
    }
    w->opacity = w->opacity_fade.value;

    //Process after unstable transitions to avoid running on destroyed windows

    if(win_viewable(w) && ps->redirected) {
        if (w->blur_background && (!w->solid || ps->o.blur_background_frame)) {
            if(w->glx_blur_cache.damaged == true) {
                win_calculate_blur(&ps->psglx->blur, ps, w);
                w->glx_blur_cache.damaged = false;
            }
        }

        if(!vec2_eq(&size, &w->shadow_cache.wSize)) {
            win_calc_shadow(ps, w);
        }
    }
    zone_leave(&ZONE_update_window);
}

static void win_drawcontents(session_t* ps, win* w, float z) {
    glx_mark(ps, w->id, true);

    glEnable(GL_BLEND);

    // This is all weird, but X Render is using premultiplied ARGB format, and
    // we need to use those things to correct it. Thanks to derhass for help.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    /* glColor4f(opacity, opacity, opacity, opacity); */

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    struct shader_program* global_program = assets_load("global.shader");
    if(global_program->shader_type_info != &global_info) {
        printf_errf("Shader was not a global shader");
        // @INCOMPLETE: Make sure the config is correct
        return;
    }

    struct Global* global_type = global_program->shader_type;
    shader_use(global_program);

    // Bind texture
    texture_bind(&w->drawable.texture, GL_TEXTURE0);

    shader_set_uniform_float(global_type->invert, w->invert_color);
    shader_set_uniform_float(global_type->flip, w->drawable.texture.flipped);
    shader_set_uniform_float(global_type->opacity, w->opacity / 100);
    shader_set_uniform_sampler(global_type->tex_scr, 0);

    // Dimming the window if needed
    if (w->dim) {
        double dim_opacity = ps->o.inactive_dim;
        if (!ps->o.inactive_dim_fixed)
            dim_opacity *= w->opacity / 100.0;
        shader_set_uniform_float(global_type->dim, dim_opacity);
    } else {
        shader_set_uniform_float(global_type->dim, 0.0);
    }

#ifdef DEBUG_GLX
    printf_dbgf("(): Draw: %d, %d, %d, %d -> %d, %d (%d, %d) z %d\n", x, y, width, height, dx, dy, ptex->width, ptex->height, z);
#endif

    struct face* face = assets_load("window.face");

    // Painting
    {
        Vector2 rectPos = {{w->a.x, w->a.y}};
        Vector2 rectSize = {{w->widthb, w->heightb}};
        Vector2 glRectPos = X11_rectpos_to_gl(ps, &rectPos, &rectSize);
        Vector3 winpos = vec3_from_vec2(&glRectPos, z);

#ifdef DEBUG_GLX
        printf_dbgf("(): Rect %f, %f, %f, %f\n", relpos.x, relpos.y, scale.x, scale.y);
#endif

        draw_rect(face, global_type->mvp, winpos, rectSize);
    }

    glx_mark(ps, w->id, false);
}

static void win_draw_debug(session_t* ps, win* w, float z) {
    Vector2 scale = {{1, 1}};

    glDisable(GL_DEPTH_TEST);
    Vector2 pen;
    {
        Vector2 xPen = {{w->a.x, w->a.y}};
        Vector2 size = {{w->widthb, w->heightb}};
        pen = X11_rectpos_to_gl(ps, &xPen, &size);
    }

    {
        Vector2 op = {{0, w->heightb - 20}};
        vec2_add(&pen, &op);
    }

    {
        char* text;
        asprintf(&text, "State: %s", StateNames[w->state]);
        text_draw(&debug_font, text, &pen, &scale);

        Vector2 size = {{0}};
        text_size(&debug_font, text, &scale, &size);
        pen.y -= size.y;
        free(text);
    }

    {
        char* text;
        asprintf(&text, "blur-background: %d", w->blur_background);
        text_draw(&debug_font, text, &pen, &scale);

        Vector2 size = {{0}};
        text_size(&debug_font, text, &scale, &size);
        pen.y -= size.y;
        free(text);
    }

    {
        char* text;
        asprintf(&text, "fade-status: %d", fade_done(&w->opacity_fade));
        text_draw(&debug_font, text, &pen, &scale);

        Vector2 size = {{0}};
        text_size(&debug_font, text, &scale, &size);
        pen.y -= size.y;
        free(text);
    }
    glEnable(GL_DEPTH_TEST);
}

void win_draw(session_t* ps, win* w, float z) {
    struct face* face = assets_load("window.face");

    Vector2 pos = {{w->a.x, w->a.y}};
    Vector2 size = {{w->widthb, w->heightb}};
    Vector2 glPos = X11_rectpos_to_gl(ps, &pos, &size);
    // Blur the backbuffer behind the window to make transparent areas blurred.
    // @PERFORMANCE: We are also blurring things that are opaque
    if (w->blur_background && (!w->solid || ps->o.blur_background_frame)) {
        Vector3 dglPos = vec3_from_vec2(&glPos, z - 0.00001);

        glDepthMask(GL_FALSE);
        draw_tex(face, &w->glx_blur_cache.texture[0], &dglPos, &size);
    }

    win_drawcontents(ps, w, z);

    /* win_draw_debug(ps, w, z); */
}

void win_postdraw(session_t* ps, win* w, float z) {
    Vector2 pos = {{w->a.x, w->a.y}};
    Vector2 size = {{w->widthb, w->heightb}};
    Vector2 glPos = X11_rectpos_to_gl(ps, &pos, &size);

    if(win_viewable(w)) {
        // Painting shadow
        if (w->shadow) {
            win_paint_shadow(ps, w, &glPos, &size, z + 0.00001);
        }
    }
}

bool wd_init(struct WindowDrawable* drawable, struct X11Context* context, Window wid) {
    assert(drawable != NULL);

    XWindowAttributes attribs;
    XGetWindowAttributes(context->display, wid, &attribs);

    drawable->wid = wid;
    drawable->fbconfig = xorgContext_selectConfig(context, XVisualIDFromVisual(attribs.visual));

    return xtexture_init(&drawable->xtexture, context);
}

bool wd_bind(struct WindowDrawable* drawable) {
    assert(drawable != NULL);

    Pixmap pixmap = XCompositeNameWindowPixmap(drawable->context->display, drawable->wid);
    if(pixmap == 0) {
        printf_errf("Failed getting window pixmap");
        return false;
    }

    return xtexture_bind(&drawable->xtexture, drawable->fbconfig, pixmap);
}

bool wd_unbind(struct WindowDrawable* drawable) {
    assert(drawable != NULL);
    xtexture_unbind(&drawable->xtexture);
    return true;
}

void wd_delete(struct WindowDrawable* drawable) {
    assert(drawable != NULL);
    // In debug mode we want to crash if we do this.
    assert(!drawable->bound);
    if(drawable->bound) {
        wd_unbind(drawable);
    }
    texture_delete(&drawable->texture);
}