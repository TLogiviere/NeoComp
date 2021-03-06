#include "shadow.h"

#include "assets/assets.h"
#include "assets/shader.h"
#include "shaders/shaderinfo.h"
#include "textureeffects.h"

#include "renderutil.h"

#include <assert.h>

#define SHADOW_RADIUS 64

int shadow_cache_init(struct glx_shadow_cache* cache) {
    Vector2 border = {{SHADOW_RADIUS, SHADOW_RADIUS}};
    cache->border = border;

    if(texture_init(&cache->texture, GL_TEXTURE_2D, NULL) != 0) {
        printf("Couldn't create texture for shadow\n");
        return 1;
    }

    if(texture_init(&cache->effect, GL_TEXTURE_2D, NULL) != 0) {
        printf("Couldn't create effect texture for shadow\n");
        texture_delete(&cache->texture);
        return 1;
    }

    if(renderbuffer_stencil_init(&cache->stencil, NULL) != 0) {
        printf("Couldn't create renderbuffer stencil for shadow\n");
        texture_delete(&cache->texture);
        texture_delete(&cache->effect);
        return 1;
    }
    cache->initialized = true;
    return 0;
}

int shadow_cache_resize(struct glx_shadow_cache* cache, const Vector2* size) {
    assert(cache->initialized == true);
    Vector2 border = {{SHADOW_RADIUS, SHADOW_RADIUS}};
    cache->wSize = *size;

    Vector2 overflowSize = border;
    vec2_imul(&overflowSize, 2);
    vec2_add(&overflowSize, size);

    texture_resize(&cache->texture, &overflowSize);
    texture_resize(&cache->effect, &overflowSize);

    renderbuffer_resize(&cache->stencil, &overflowSize);

    return 0;
}

void shadow_cache_delete(struct glx_shadow_cache* cache) {
    if(!cache->initialized)
        return;

    texture_delete(&cache->texture);
    texture_delete(&cache->effect);
    renderbuffer_delete(&cache->stencil);
    cache->initialized = false;
    return;
}

void windowlist_updateShadow(session_t* ps, Vector* paints) {
    Vector shadow_updates;
    vector_init(&shadow_updates, sizeof(win_id), paints->size);

    struct Framebuffer framebuffer;
    if(!framebuffer_init(&framebuffer)) {
        printf("Couldn't create framebuffer for shadow\n");
        return;
    }
    framebuffer_resetTarget(&framebuffer);
    framebuffer_bind(&framebuffer);

    Vector blurDatas;
    vector_init(&blurDatas, sizeof(struct TextureBlurData), ps->win_list.size);

    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);

    glClearColor(0.0, 0.0, 0.0, 0.0);

    glStencilMask(0xFF);
    glClearStencil(0);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

    for_components(it, &ps->win_list,
        COMPONENT_MUD, COMPONENT_TEXTURED, COMPONENT_PHYSICAL, COMPONENT_SHADOW_DAMAGED, COMPONENT_SHADOW,
        COMPONENT_SHAPED, CQ_END) {
        struct TexturedComponent* textured = swiss_getComponent(&ps->win_list, COMPONENT_TEXTURED, it.id);
        struct PhysicalComponent* physical = swiss_getComponent(&ps->win_list, COMPONENT_PHYSICAL, it.id);
        struct glx_shadow_cache* shadow = swiss_getComponent(&ps->win_list, COMPONENT_SHADOW, it.id);
        struct ShapedComponent* shaped = swiss_getComponent(&ps->win_list, COMPONENT_SHAPED, it.id);

        framebuffer_resetTarget(&framebuffer);
        framebuffer_targetTexture(&framebuffer, &shadow->texture);
        framebuffer_targetRenderBuffer_stencil(&framebuffer, &shadow->stencil);
        framebuffer_rebind(&framebuffer);

        Matrix old_view = view;
        view = mat4_orthogonal(0, shadow->texture.size.x, 0, shadow->texture.size.y, -1, 1);

        glViewport(0, 0, shadow->texture.size.x, shadow->texture.size.y);

        glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

        texture_bind(&textured->texture, GL_TEXTURE0);

        struct shader_program* shadow_program = assets_load("shadow.shader");
        if(shadow_program->shader_type_info != &shadow_info) {
            printf_errf("Shader was not a shadow shader\n");
            framebuffer_delete(&framebuffer);
            view = old_view;
            return;
        }
        struct Shadow* shadow_type = shadow_program->shader_type;

        shader_set_future_uniform_bool(shadow_type->flip, textured->texture.flipped);
        shader_set_future_uniform_sampler(shadow_type->tex_scr, 0);

        shader_use(shadow_program);

        Vector3 pos = vec3_from_vec2(&shadow->border, 0.0);
        draw_rect(shaped->face, shadow_type->mvp, pos, physical->size);

        view = old_view;

        // Do the blur
        struct TextureBlurData blurData = {
            .depth = &shadow->stencil,
            .tex = &shadow->texture,
            .swap = &shadow->effect,
        };
        vector_putBack(&blurDatas, &blurData);
    }

    glDisable(GL_STENCIL_TEST);

    textures_blur(&blurDatas, &framebuffer, 4, false);

    vector_kill(&blurDatas);

    framebuffer_resetTarget(&framebuffer);
    if(framebuffer_bind(&framebuffer) != 0) {
        printf("Failed binding framebuffer to clip shadow\n");
    }

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glStencilMask(0xFF);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    glEnable(GL_STENCIL_TEST);

    for_components(it, &ps->win_list,
        COMPONENT_MUD, COMPONENT_TEXTURED, COMPONENT_PHYSICAL, COMPONENT_SHADOW_DAMAGED, COMPONENT_SHADOW,
        COMPONENT_SHAPED, CQ_END) {
        struct glx_shadow_cache* shadow = swiss_getComponent(&ps->win_list, COMPONENT_SHADOW, it.id);
        struct ShapedComponent* shaped = swiss_getComponent(&ps->win_list, COMPONENT_SHAPED, it.id);

        framebuffer_resetTarget(&framebuffer);
        framebuffer_targetTexture(&framebuffer, &shadow->effect);
        framebuffer_targetRenderBuffer_stencil(&framebuffer, &shadow->stencil);
        if(framebuffer_rebind(&framebuffer) != 0) {
            printf("Failed binding framebuffer to clip shadow\n");
            return;
        }

        Matrix old_view = view;
        view = mat4_orthogonal(0, shadow->effect.size.x, 0, shadow->effect.size.y, -1, 1);
        glViewport(0, 0, shadow->effect.size.x, shadow->effect.size.y);

        glClear(GL_COLOR_BUFFER_BIT);

        draw_tex(shaped->face, &shadow->texture, &VEC3_ZERO, &shadow->effect.size);

        view = old_view;
    }

    swiss_resetComponent(&ps->win_list, COMPONENT_SHADOW_DAMAGED);

    glDisable(GL_STENCIL_TEST);

    vector_kill(&shadow_updates);
    framebuffer_delete(&framebuffer);
}

