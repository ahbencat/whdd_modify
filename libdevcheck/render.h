#ifndef RENDER_H
#define RENDER_H

#include "procedure.h"

struct dc_renderer_ctx {
    void *priv;
    DC_Renderer *renderer;
    DC_ProcedureCtx *procedure_ctx;
};

struct dc_renderer {
    char *name;
    int priv_data_size;
    int (*open)(DC_RendererCtx *renderer_ctx);
    int (*handle_report)(DC_RendererCtx *renderer_ctx);
    void (*close)(DC_RendererCtx *renderer_ctx);

    DC_Renderer *next;
};


int dc_renderer_register(DC_Renderer *renderer);
#define RENDERER_REGISTER(x) { \
        extern DC_Renderer x; \
        dc_renderer_register(&x); }

DC_Renderer *dc_find_renderer(char *name);

int render_procedure(DC_ProcedureCtx *actctx, DC_Renderer *renderer);

/* SIGWINCH support: render_procedure() installs the handler for the duration
 * of a rendering session. Renderers poll render_sigwinch_caught() from their
 * display thread and pause drawing while the terminal is smaller than their
 * fixed layout. */
void render_sigwinch_install(void);
void render_sigwinch_restore(void);
int render_sigwinch_caught(void);

#endif // RENDER_H
