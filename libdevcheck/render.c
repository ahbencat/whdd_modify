#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>

#include "render.h"
#include "utils.h"

static volatile sig_atomic_t sigwinch_flag = 0;
static struct sigaction sigwinch_old_action;

static void sigwinch_handler(int signo) {
    (void)signo;
    sigwinch_flag = 1;
}

void render_sigwinch_install(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &act, &sigwinch_old_action);
    sigwinch_flag = 0;  // drop a stale flag from a previous session
}

void render_sigwinch_restore(void) {
    sigaction(SIGWINCH, &sigwinch_old_action, NULL);
}

int render_sigwinch_caught(void) {
    int r = sigwinch_flag;
    sigwinch_flag = 0;
    return r;
}

static int proxy_handle_report(DC_ProcedureCtx *dummy, void *arg) {
    (void)dummy;
    DC_RendererCtx *ctx = arg;
    return ctx->renderer->handle_report(ctx);
}

int render_procedure(DC_ProcedureCtx *actctx, DC_Renderer *renderer) {
    int r;
    DC_RendererCtx *ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    assert(actctx);
    assert(renderer);
    ctx->priv = calloc(1, renderer->priv_data_size);
    assert(ctx->priv);
    ctx->procedure_ctx = actctx;
    ctx->renderer = renderer;
    actctx->user_priv = ctx;
    r = renderer->open(ctx);
    if (r)
        return r;
    render_sigwinch_install();
    // TODO Simplify builtin loop functions
    r = procedure_perform_until_interrupt(actctx, proxy_handle_report, (void*)ctx);
    if (r)
        return r;
    renderer->close(ctx);
    render_sigwinch_restore();
    free(ctx->priv);
    free(ctx);
    return 0;
}

DC_Renderer *dc_find_renderer(char *name) {
    DC_Renderer *iter = dc_ctx_global->renderer_list;
    while (iter) {
        if (!strcmp(iter->name, name))
            break;
        iter = iter->next;
    }
    return iter;
}

int dc_renderer_register(DC_Renderer *renderer) {
    renderer->next = dc_ctx_global->renderer_list;
    dc_ctx_global->renderer_list = renderer;
    return 0;
}
