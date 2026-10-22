/*
 * nv2a HUD layout diagnostic (log only)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * For each draw of a captured frame the vertex positions are recomputed on
 * the CPU exactly as the generated vertex shader computes them before its
 * clip-space conversion (vsh-ff.c / vsh-prog.c), from the same attribute
 * state the renderer binds (gl/vertex.c). That gives screen-space x/y in
 * guest framebuffer units, 0..surfaceSize, before the widescreen hack.
 *
 *   FF (skinning off): oPos[j] = dot(v0, c[j]), j = 0..3,
 *                      w = clampAwayZeroInf(oPos.w), xy = xy / w + c[59].xy
 *   programmable:      the program leaves screen space in oPos; it is run by
 *                      nv2a_vsh_cpu (the emulator LAUNCH_TRANSFORM_PROGRAM
 *                      uses), after its steps are checked so that nothing a
 *                      stray program does can trip the library's asserts.
 *
 * A draw is 2D when w is the same for all its vertices (orthographic or
 * pretransformed); a 3D draw is rejected at its first differing w, so the
 * cost stays small even for a full frame.
 *
 * Copy-pass fix (XEMU_WS_HACK=1, disable with XEMU_WS_COPY=0): the clip x
 * scale of the widescreen hack is baked into every vertex shader, so it also
 * narrows the screen-space passes that copy one render target into another
 * (final copy to the scanout, downsample, feedback blur). Those must stay
 * 1:1. For such a draw - programmable, constant w, texture 0 is a surface
 * rendered earlier, covering the full width or height of its target, data in
 * the inline array/buffer - the x of v0 is pre-compensated on the CPU so the
 * shader's scale cancels out: sx' = W/2 + (sx - W/2) / ws. Guest memory is
 * never touched; only the batch copy the renderer is about to upload.
 *
 * HUD anchoring (disable with XEMU_WS_HUD=0), same mechanism: a screen-space
 * programmable draw on a surface that already received fixed-function (3D)
 * draws this frame, and no full-surface 2D backdrop at any point before it
 * in the frame (also before the 3D), is moved by
 * -/+ W/2 * (1 - ws) / ws when its centre lies within XEMU_WS_HUD_EDGE
 * percent (default 26) of the left/right edge, so it keeps its size and its
 * distance from the screen edge. Consecutive draws with the same texture and
 * program whose boxes touch (glyphs of a string, outline passes) follow the
 * first one's side; a draw inside an element anchored earlier in the frame
 * (a needle in its dial, digits in their panel) follows that element.
 *
 * 16:9 menu background (opt-in with XEMU_WS_MENUBG=1): when the backdrop
 * is an untextured fill (the menus), every later untextured full-width
 * layer (except the fades along the top and bottom edge, which the panel
 * frame covers), and the textured full-width layers drawn right after the backdrop
 * (before any other draw on that surface), are stretched to 16:9 like a
 * copy pass. The panel - including its full-width top and bottom frame,
 * drawn later - stays 4:3 in the centre; 3D is drawn full width.
 *
 * 4:3 clip on 2D screens (disable with XEMU_WS_MENU43=0): after a backdrop
 * (any, or only a textured one with XEMU_WS_MENUBG=1) the rest of the frame
 * on that surface is clipped to the 4:3 area, and so is the whole next frame
 * if this one had no anchored HUD on it and the backdrop was followed by a
 * layout (not a fade at the end of the frame) - menus that draw a 3D
 * background before their backdrop get black bars too. The clip goes through the
 * window clip regions of the
 * pixel shader (glsl/psh.c calls pgraph_hud_ws_clip_regions), so geometry
 * the clip x scale reveals beyond the original frame stays hidden.
 */

#include "qemu/osdep.h"
#include <math.h>

#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-widescreen.h"
#include "ui/xemu-settings.h"
#include "nv2a_vsh_emulator.h"
#include "hud_diag.h"

#define HD_MAX_EVAL_VERTS   65536u /* larger draws: classified, not measured */
#define HD_CLASSIFY_VERTS   64u
#define HD_MAX_DRAWS        50000u /* per captured frame */
#define HD_MAX_ELEMS_LISTED 24u
#define HD_PROG_CACHE       32u
#define HD_SEEN_MAX         1024u
#define HD_W_REL_EPS        1e-4f
#define HD_Z_REL_EPS        1e-6f

/* Context registers as the program sees them: 192 constants, then zero rows.
 * The 8-bit index field reaches 255; a relative index that lands outside
 * 0..191 reads HD_CTX_ZERO_ROW instead of wandering off the array. */
#define HD_CTX_ROWS     256
#define HD_CTX_ZERO_ROW 192

#define HD_FNV_INIT 0xcbf29ce484222325ull

bool pgraph_hud_diag_capturing;

typedef struct HdBuf {
    char *p;
    size_t len, cap;
} HdBuf;

typedef struct HdProg {
    bool used;
    bool ok;
    uint64_t hash;
    unsigned int len;
    const char *why;
    uint16_t in_mask;
    Nv2aVshProgram prog;
} HdProg;

enum { HD_SRC_CONST, HD_SRC_MEM, HD_SRC_IBUF };

typedef struct HdAttr {
    int src;
    const uint8_t *base;
    uint64_t avail;
    uint32_t stride;
    unsigned int esize;
    const float *ibuf;
    float cval[4];
    bool bad;
    VertexAttribute tmpl;
} HdAttr;

typedef struct HdRegs {
    float in[16 * 4];
    float out[13 * 4];
    float tmp[16 * 4];
    float addr[4];
} HdRegs;

enum { HD_K_EMPTY, HD_K_DA, HD_K_IE, HD_K_IB, HD_K_IA };
enum { HD_CL_EMPTY, HD_CL_2D, HD_CL_3D, HD_CL_UNK };

static struct {
    bool init, enabled;
    unsigned int every, max_frames, captured;
    uint64_t flips;
    FILE *f;
    char path[1024];

    /* captured frame */
    HdBuf defs, lines;
    unsigned int draws, n_ff, n_prg, n_2d, n_3d, n_unk, n_empty, n_nop;
    bool truncated;
    bool surf_set;
    unsigned int surf_w, surf_h, aa_w, aa_h, scale;

    /* current run of consecutive non-2D draws */
    bool run_open;
    unsigned int run_first, run_last;
    unsigned int run_3d, run_unk, run_empty, run_ff, run_prg, run_nop;
    uint32_t run_surf;
    bool run_mixed;

    uint64_t seen[HD_SEEN_MAX];
    unsigned int n_seen;

    HdProg progs[HD_PROG_CACHE];
    unsigned int prog_next;

    uint32_t *keys;
    float *sx, *sy;
    size_t scratch_cap;
    uint32_t *seq;
    size_t seq_cap;

    int ws_action; /* what the widescreen pass did to the current draw */
    bool cur_c43;  /* 4:3 clip active on this draw's surface */
    unsigned int run_c43;
} hd;

enum { HD_WS_NONE, HD_WS_COPY, HD_WS_FAIL, HD_WS_BD, HD_WS_L, HD_WS_C,
       HD_WS_R, HD_WS_WIDE };

bool pgraph_hud_ws_active;

static struct {
    bool init;
    bool copy_on, hud_on, clip_on, menubg_on;
    float ws;
    uint32_t rt[16];           /* surfaces rendered to so far */
    unsigned int n_rt, rt_next;
    uint32_t scene[8];         /* surfaces with a FF draw this frame */
    unsigned int n_scene;
    uint32_t bd[8];            /* surfaces with a 2D backdrop this frame */
    unsigned int bd_after[8];  /* ...and the number of draws after it */
    unsigned int n_bd;
    uint32_t wide[8];          /* ...whose backdrop starts a 16:9 background */
    unsigned int n_wide;
    uint32_t phase[8];         /* ...still in the run of full-width layers */
    unsigned int n_phase;      /*    right after that backdrop */
    uint32_t fade[8];          /* surfaces with an overscan fade this frame */
    unsigned int n_fade;
    uint32_t lr[8];            /* surfaces with an anchored draw this frame */
    unsigned int n_lr;
    uint32_t s43[8];           /* surfaces that were a 2D screen last frame */
    unsigned int n_s43;
    /* calls from glsl/psh.c this frame and how many narrowed the regions;
     * last_* hold the frame that just ended, for the log */
    unsigned int clip_calls, clip_applied, last_calls, last_applied;
    unsigned int clears_split, last_clears_split;
    struct {
        bool valid;
        uint32_t surf, t0;
        uint64_t prog;
        float x0, x1, y0, y1;
        int side;
    } run;
    float edge;                /* side threshold, fraction of the width */
    struct {                   /* anchored boxes this frame (before the move) */
        uint32_t surf;
        float x0, x1, y0, y1;
        int side;
    } anch[256];
    unsigned int n_anch;
} hw;

static float hd_ctx[HD_CTX_ROWS * 4];

static bool hw_c43_now(PGRAPHState *pg);

/* ------------------------------------------------------------------------ */

static void G_GNUC_PRINTF(2, 3) hb_printf(HdBuf *b, const char *fmt, ...)
{
    for (;;) {
        size_t room = b->cap - b->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if ((size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap ? b->cap : 4096;
        while (cap - b->len <= (size_t)n) {
            cap *= 2;
        }
        b->p = g_realloc(b->p, cap);
        b->cap = cap;
    }
}

static void hb_reset(HdBuf *b)
{
    b->len = 0;
    if (b->p) {
        b->p[0] = '\0';
    }
}

static uint64_t hd_fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n--) {
        h ^= *p++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint32_t hd_h32(uint64_t h)
{
    return (uint32_t)(h ^ (h >> 32));
}

static bool hd_first_sight(uint64_t key)
{
    for (unsigned int i = 0; i < hd.n_seen; i++) {
        if (hd.seen[i] == key) {
            return false;
        }
    }
    if (hd.n_seen >= HD_SEEN_MAX) {
        return false;
    }
    hd.seen[hd.n_seen++] = key;
    return true;
}

/* clampAwayZeroInf() of the generated shaders */
static float hd_clamp_away(float t)
{
    uint32_t u;
    memcpy(&u, &t, sizeof(u));
    if (t > 0.0f || u == 0) {
        return fminf(fmaxf(t, 0x1p-64f), 0x1p64f);
    }
    return fminf(fmaxf(t, -0x1p64f), -0x1p-64f);
}

static float hd_dot4(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void hd_reserve(size_t n)
{
    if (n <= hd.scratch_cap) {
        return;
    }
    size_t cap = hd.scratch_cap ? hd.scratch_cap : 1024;
    while (cap < n) {
        cap *= 2;
    }
    hd.keys = g_realloc(hd.keys, cap * sizeof(*hd.keys));
    hd.sx = g_realloc(hd.sx, cap * sizeof(*hd.sx));
    hd.sy = g_realloc(hd.sy, cap * sizeof(*hd.sy));
    hd.scratch_cap = cap;
}

static void hd_reserve_seq(size_t n)
{
    if (n > hd.seq_cap) {
        hd.seq = g_realloc(hd.seq, n * sizeof(*hd.seq));
        hd.seq_cap = n;
    }
}

/* ------------------------------------------------------------------------ */
/* Vertex fetch, mirroring pgraph_gl_bind_vertex_attributes()                */

/* Array data: UB_D3D is bound as GL_BGRA in GL (.bgra in Vulkan) */
static void hd_decode(const VertexAttribute *tmpl, const uint8_t *p,
                      float out[4])
{
    if (tmpl->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D) {
        out[0] = p[2] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[0] / 255.0f;
        out[3] = p[3] / 255.0f;
        return;
    }
    VertexAttribute t = *tmpl; /* the decoder writes inline_value */
    pgraph_update_inline_value(&t, p);
    memcpy(out, t.inline_value, sizeof(t.inline_value));
}

static void hd_attr_const(HdAttr *a, const float v[4])
{
    a->src = HD_SRC_CONST;
    memcpy(a->cval, v, sizeof(a->cval));
}

static void hd_setup_attr(NV2AState *d, int kind, int i, unsigned int ia_off,
                          unsigned int ia_vsize, HdAttr *a)
{
    static const float zero_w1[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    PGRAPHState *pg = &d->pgraph;
    VertexAttribute *attr = &pg->vertex_attributes[i];

    memset(a, 0, sizeof(*a));
    a->tmpl = *attr;
    a->esize = attr->size * attr->count;

    if (kind == HD_K_IB) {
        if (attr->inline_buffer_populated) {
            a->src = HD_SRC_IBUF;
            a->ibuf = attr->inline_buffer;
        } else {
            hd_attr_const(a, attr->inline_value);
        }
        return;
    }

    if (!attr->count) {
        hd_attr_const(a, attr->inline_value);
        return;
    }

    if (kind == HD_K_IA) {
        uint64_t total = (uint64_t)pg->inline_array_length * 4;
        a->src = HD_SRC_MEM;
        a->base = (const uint8_t *)pg->inline_array + ia_off;
        a->avail = total > ia_off ? total - ia_off : 0;
        a->stride = ia_vsize;
        return;
    }

    /* DRAW_ARRAYS / ARRAY_ELEMENT: guest memory through the vertex DMA */
    hwaddr dma_len;
    uint8_t *dma = (uint8_t *)nv_dma_map(
        d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a, &dma_len);
    const uint8_t *vram_end = d->vram_ptr + memory_region_size(d->vram);
    if (attr->offset > dma_len || dma + attr->offset >= vram_end) {
        hd_attr_const(a, zero_w1);
        a->bad = true;
        return;
    }
    a->src = HD_SRC_MEM;
    a->base = dma + attr->offset;
    a->avail = MIN((uint64_t)(dma_len - attr->offset) + 1,
                   (uint64_t)(vram_end - a->base));
    a->stride = attr->stride;

    if (!attr->stride) {
        /* stride 0: the first element only, set as a constant (unswizzled,
         * as the renderer's attr_value path does) */
        float v[4];
        memcpy(v, zero_w1, sizeof(v));
        if (a->esize <= a->avail) {
            VertexAttribute t = *attr;
            pgraph_update_inline_value(&t, a->base);
            memcpy(v, t.inline_value, sizeof(v));
        } else {
            a->bad = true;
        }
        hd_attr_const(a, v);
    }
}

static void hd_fetch(const HdAttr *a, uint32_t key, float out[4], bool *oob)
{
    switch (a->src) {
    case HD_SRC_IBUF:
        memcpy(out, a->ibuf + (size_t)key * 4, 4 * sizeof(float));
        return;
    case HD_SRC_MEM: {
        uint64_t off = (uint64_t)key * a->stride;
        if (off + a->esize <= a->avail) {
            hd_decode(&a->tmpl, a->base + off, out);
            return;
        }
        *oob = true;
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    default:
        memcpy(out, a->cval, sizeof(a->cval));
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Vertex programs                                                          */

#define HD_TOK_MAC(t)   (((t)[1] >> 21) & 0xF)
#define HD_TOK_FINAL(t) ((t)[3] & 1)

static bool hd_prog_check_op(HdProg *p, Nv2aVshOperation *op, bool paired_mac)
{
    if (op->opcode == NV2AOP_NOP) {
        return true;
    }
    if (op->inputs[0].type == NV2ART_NONE) {
        p->why = "operation without input";
        return false;
    }
    for (int j = 0; j < 3; j++) {
        Nv2aVshInput *in = &op->inputs[j];
        if (in->type == NV2ART_NONE) {
            break;
        }
        switch (in->type) {
        case NV2ART_TEMPORARY:
            if (in->index > 12) {
                p->why = "reads R13+";
                return false;
            }
            break;
        case NV2ART_INPUT:
            p->in_mask |= (uint16_t)(1u << (in->index & 15));
            break;
        case NV2ART_CONTEXT:
            if (in->index >= HD_CTX_ROWS) {
                p->why = "bad c[] index";
                return false;
            }
            break;
        default:
            p->why = "bad input type";
            return false;
        }
    }
    for (int j = 0; j < 2; j++) {
        Nv2aVshOutput *o = &op->outputs[j];
        switch (o->type) {
        case NV2ART_NONE:
            break;
        case NV2ART_ADDRESS:
            /* the parser leaves ARL's writemask at 0, so the library would
             * never update A0 */
            o->writemask = NV2AWM_XYZW;
            break;
        case NV2ART_TEMPORARY:
            if (paired_mac && o->index == 1) {
                /* as vsh-prog.c: a paired MAC's R1 write is dropped */
                o->type = NV2ART_NONE;
            } else if (o->index == 12) {
                /* R12 mirrors oPos */
                o->type = NV2ART_OUTPUT;
                o->index = 0;
            } else if (o->index > 12) {
                p->why = "writes R13+";
                return false;
            }
            break;
        case NV2ART_OUTPUT:
            if (o->index >= 13) {
                p->why = "bad output register";
                return false;
            }
            break;
        default:
            p->why = "writes c[]";
            return false;
        }
    }
    return true;
}

static void hd_prog_build(HdProg *p, const uint32_t *tok, unsigned int len,
                          bool final)
{
    p->ok = false;
    p->why = NULL;
    p->in_mask = 0;
    p->prog.steps = NULL;

    if (!final) {
        p->why = "no FINAL";
        return;
    }
    for (unsigned int i = 0; i < len; i++) {
        if (HD_TOK_MAC(&tok[i * 4]) > 13) {
            p->why = "bad MAC opcode";
            return;
        }
    }
    if (nv2a_vsh_parse_program(&p->prog, tok, len) != NV2AVPR_SUCCESS) {
        p->prog.steps = NULL;
        p->why = "parse failed";
        return;
    }
    for (unsigned int i = 0; i < len; i++) {
        Nv2aVshStep *s = &p->prog.steps[i];
        bool paired = s->mac.opcode != NV2AOP_NOP &&
                      s->ilu.opcode != NV2AOP_NOP;
        if (!hd_prog_check_op(p, &s->mac, paired) ||
            !hd_prog_check_op(p, &s->ilu, false)) {
            nv2a_vsh_program_destroy(&p->prog);
            return;
        }
    }
    p->ok = true;
}

static HdProg *hd_get_prog(PGRAPHState *pg, unsigned int *start_out)
{
    unsigned int start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                                  NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    *start_out = start;
    if (start >= NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
        return NULL;
    }

    const uint32_t *tok = pg->program_data[start];
    unsigned int len = 0;
    bool final = false;
    for (unsigned int i = start; i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; i++) {
        len++;
        if (HD_TOK_FINAL(pg->program_data[i])) {
            final = true;
            break;
        }
    }

    uint64_t h = hd_fnv(HD_FNV_INIT ^ 'P', tok, (size_t)len * 16);
    for (unsigned int i = 0; i < HD_PROG_CACHE; i++) {
        HdProg *p = &hd.progs[i];
        if (p->used && p->hash == h && p->len == len) {
            return p;
        }
    }

    HdProg *p = &hd.progs[hd.prog_next];
    hd.prog_next = (hd.prog_next + 1) % HD_PROG_CACHE;
    if (p->used && p->prog.steps) {
        nv2a_vsh_program_destroy(&p->prog);
    }
    p->used = true;
    p->hash = h;
    p->len = len;
    hd_prog_build(p, tok, len, final);
    return p;
}

static void hd_run_prog(const HdProg *p, HdRegs *r)
{
    Nv2aVshExecutionState st = {
        .input_regs = r->in,
        .output_regs = r->out,
        .temp_regs = r->tmp,
        .context_regs = hd_ctx,
        .address_reg = r->addr,
        .context_dirty = NULL,
    };

    for (unsigned int i = 0; i < p->len; i++) {
        const Nv2aVshStep *s = &p->prog.steps[i];
        bool rel = false;
        for (int j = 0; j < 3; j++) {
            rel |= s->mac.inputs[j].type == NV2ART_CONTEXT &&
                   s->mac.inputs[j].is_relative;
        }
        rel |= s->ilu.inputs[0].type == NV2ART_CONTEXT &&
               s->ilu.inputs[0].is_relative;

        if (!rel) {
            nv2a_vsh_emu_apply(&st, s);
        } else {
            /* resolve c[A0+n] here: the library adds A0 unchecked */
            float af = r->addr[0];
            int a0 = (af >= -1e6f && af <= 1e6f) ? (int)af : 1000000;
            Nv2aVshStep t = *s;
            Nv2aVshInput *ins[4] = { &t.mac.inputs[0], &t.mac.inputs[1],
                                     &t.mac.inputs[2], &t.ilu.inputs[0] };
            for (int j = 0; j < 4; j++) {
                if (ins[j]->type == NV2ART_CONTEXT && ins[j]->is_relative) {
                    int e = (int)ins[j]->index + a0;
                    ins[j]->index = (e >= 0 && e < 192) ? (uint32_t)e
                                                        : HD_CTX_ZERO_ROW;
                    ins[j]->is_relative = false;
                }
            }
            nv2a_vsh_emu_apply(&st, &t);
        }
        if (s->is_final) {
            break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Definitions dumped once per log                                          */

static const char *const hd_opname[] = {
    "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN",
    "MAX", "SLT", "SGE", "ARL", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT",
};

static const char *const hd_outname[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts",
    "oB0", "oB1", "oT0", "oT1", "oT2", "oT3",
};

static void hd_dis_out(HdBuf *b, const Nv2aVshOutput *o)
{
    switch (o->type) {
    case NV2ART_TEMPORARY:
        hb_printf(b, "R%u", o->index);
        break;
    case NV2ART_OUTPUT:
        hb_printf(b, "%s", o->index < 13 ? hd_outname[o->index] : "o?");
        break;
    case NV2ART_ADDRESS:
        hb_printf(b, "a0.x");
        return;
    default:
        hb_printf(b, "c[%u]", o->index);
        break;
    }
    unsigned int m = o->writemask;
    if (m != 0xF) {
        hb_printf(b, ".%s%s%s%s", (m & 8) ? "x" : "", (m & 4) ? "y" : "",
                  (m & 2) ? "z" : "", (m & 1) ? "w" : "");
    }
}

static void hd_dis_in(HdBuf *b, const Nv2aVshInput *in)
{
    static const char sw[] = "xyzw";
    if (in->is_negated) {
        hb_printf(b, "-");
    }
    switch (in->type) {
    case NV2ART_TEMPORARY:
        hb_printf(b, "R%u", in->index);
        break;
    case NV2ART_INPUT:
        hb_printf(b, "v%u", in->index);
        break;
    default:
        if (in->is_relative) {
            hb_printf(b, "c[a0.x+%u]", in->index);
        } else {
            hb_printf(b, "c[%u]", in->index);
        }
        break;
    }
    const uint8_t *s = in->swizzle;
    if (s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3) {
        return;
    }
    if (s[0] == s[1] && s[1] == s[2] && s[2] == s[3]) {
        hb_printf(b, ".%c", sw[s[0] & 3]);
    } else {
        hb_printf(b, ".%c%c%c%c", sw[s[0] & 3], sw[s[1] & 3], sw[s[2] & 3],
                  sw[s[3] & 3]);
    }
}

static void hd_dis_op(HdBuf *b, const Nv2aVshOperation *op)
{
    unsigned int opc = op->opcode;
    hb_printf(b, "%s", opc < ARRAY_SIZE(hd_opname) ? hd_opname[opc] : "???");
    const char *sep = " ";
    for (int j = 0; j < 2; j++) {
        if (op->outputs[j].type != NV2ART_NONE) {
            hb_printf(b, "%s", sep);
            hd_dis_out(b, &op->outputs[j]);
            sep = ", ";
        }
    }
    for (int j = 0; j < 3; j++) {
        if (op->inputs[j].type == NV2ART_NONE) {
            break;
        }
        hb_printf(b, "%s", sep);
        hd_dis_in(b, &op->inputs[j]);
        sep = ", ";
    }
}

static void hd_dump_prog(const HdProg *p, unsigned int start,
                         const uint32_t *tok)
{
    hb_printf(&hd.defs, "# prog p%08x len=%u start=%u", hd_h32(p->hash),
              p->len, start);
    if (!p->ok) {
        hb_printf(&hd.defs, " REJECTED (%s)\n", p->why ? p->why : "?");
        for (unsigned int i = 0; i < p->len; i++) {
            hb_printf(&hd.defs, "#   %02u: %08x %08x %08x %08x\n", i,
                      tok[i * 4], tok[i * 4 + 1], tok[i * 4 + 2],
                      tok[i * 4 + 3]);
        }
        return;
    }
    hb_printf(&hd.defs, " in=");
    const char *sep = "";
    for (int i = 0; i < 16; i++) {
        if (p->in_mask & (1u << i)) {
            hb_printf(&hd.defs, "%sv%d", sep, i);
            sep = ",";
        }
    }
    hb_printf(&hd.defs, "\n");
    for (unsigned int i = 0; i < p->len; i++) {
        const Nv2aVshStep *s = &p->prog.steps[i];
        hb_printf(&hd.defs, "#   %02u: ", i);
        if (s->mac.opcode == NV2AOP_NOP && s->ilu.opcode == NV2AOP_NOP) {
            hb_printf(&hd.defs, "NOP");
        }
        if (s->mac.opcode != NV2AOP_NOP) {
            hd_dis_op(&hd.defs, &s->mac);
        }
        if (s->ilu.opcode != NV2AOP_NOP) {
            hb_printf(&hd.defs, "%s",
                      s->mac.opcode != NV2AOP_NOP ? "  +  " : "");
            hd_dis_op(&hd.defs, &s->ilu);
        }
        hb_printf(&hd.defs, "\n");
    }
}

static void hd_dump_ffmat(uint32_t id, const float c[4][4], const float *vp)
{
    hb_printf(&hd.defs, "# ffmat m%08x", id);
    for (int j = 0; j < 4; j++) {
        hb_printf(&hd.defs, " c%d=(%g %g %g %g)", j, c[j][0], c[j][1],
                  c[j][2], c[j][3]);
    }
    hb_printf(&hd.defs, " vpoff=(%g %g)\n", vp[0], vp[1]);
}

/* ------------------------------------------------------------------------ */
/* Per-frame bookkeeping                                                    */

static void hd_run_close(void)
{
    if (!hd.run_open) {
        return;
    }
    if (hd.run_first == hd.run_last) {
        hb_printf(&hd.lines, "  %04u     ", hd.run_first);
    } else {
        hb_printf(&hd.lines, "  %04u-%04u", hd.run_first, hd.run_last);
    }
    hb_printf(&hd.lines, " 3d=%u unk=%u empty=%u  (ff %u, prg %u, nop %u)",
              hd.run_3d, hd.run_unk, hd.run_empty, hd.run_ff, hd.run_prg,
              hd.run_nop);
    if (hd.run_c43) {
        hb_printf(&hd.lines, " c43=%u", hd.run_c43);
    }
    if (hd.run_mixed) {
        hb_printf(&hd.lines, " s=mixed\n");
    } else {
        hb_printf(&hd.lines, " s=@%08x\n", hd.run_surf);
    }
    hd.run_open = false;
}

static void hd_run_add(unsigned int idx, int cl, bool ff, bool prg, bool nop,
                       uint32_t surf)
{
    if (!hd.run_open) {
        hd.run_open = true;
        hd.run_first = idx;
        hd.run_surf = surf;
        hd.run_mixed = false;
        hd.run_3d = hd.run_unk = hd.run_empty = 0;
        hd.run_ff = hd.run_prg = hd.run_nop = 0;
        hd.run_c43 = 0;
    }
    hd.run_last = idx;
    hd.run_mixed |= surf != hd.run_surf;
    hd.run_3d += cl == HD_CL_3D;
    hd.run_unk += cl == HD_CL_UNK;
    hd.run_empty += cl == HD_CL_EMPTY;
    hd.run_ff += ff;
    hd.run_prg += prg;
    hd.run_nop += nop;
    hd.run_c43 += hd.cur_c43;
}

static void hd_frame_begin(void)
{
    hb_reset(&hd.defs);
    hb_reset(&hd.lines);
    hd.draws = hd.n_ff = hd.n_prg = 0;
    hd.n_2d = hd.n_3d = hd.n_unk = hd.n_empty = hd.n_nop = 0;
    hd.truncated = false;
    hd.surf_set = false;
    hd.run_open = false;
}

static void hd_frame_end(void)
{
    hd_run_close();
    fprintf(hd.f,
            "== F%06" PRIu64 " draws=%u ff=%u prg=%u | 2d=%u 3d=%u unk=%u "
            "empty=%u nop=%u | surf %ux%u aa %ux%u scale %u | psh clip "
            "calls=%u applied=%u | clear43=%u%s\n",
            hd.flips, hd.draws, hd.n_ff, hd.n_prg, hd.n_2d, hd.n_3d,
            hd.n_unk, hd.n_empty, hd.n_nop, hd.surf_w, hd.surf_h, hd.aa_w,
            hd.aa_h, hd.scale, hw.last_calls, hw.last_applied,
            hw.last_clears_split,
            hd.truncated ? " TRUNCATED" : "");
    if (hd.defs.len) {
        fwrite(hd.defs.p, 1, hd.defs.len, hd.f);
    }
    if (hd.lines.len) {
        fwrite(hd.lines.p, 1, hd.lines.len, hd.f);
    }
    fflush(hd.f);
    hd.captured++;
}

static unsigned int hd_env_uint(const char *name, unsigned int def,
                                unsigned int lo, unsigned int hi)
{
    const char *e = getenv(name);
    if (!e || !*e) {
        return def;
    }
    char *end;
    unsigned long v = strtoul(e, &end, 10);
    if (*end || v < lo || v > hi) {
        return def;
    }
    return (unsigned int)v;
}

static void hd_init(void)
{
    hd.init = true;

    const char *e = getenv("XEMU_HUD_DIAG");
    if (!e || e[0] != '1') {
        return;
    }

    hd.every = hd_env_uint("XEMU_HUD_DIAG_EVERY", 120, 1, 1000000);
    hd.max_frames = hd_env_uint("XEMU_HUD_DIAG_MAX", 300, 1, 1000000);

    const char *log = getenv("XEMU_HUD_DIAG_LOG");
    if (log && *log) {
        snprintf(hd.path, sizeof(hd.path), "%s", log);
    } else {
#ifdef _WIN32
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (n > 0 && n < sizeof(exe)) {
            char *slash = strrchr(exe, '\\');
            if (slash) {
                slash[1] = '\0';
                snprintf(hd.path, sizeof(hd.path), "%sxemu-hud-diag.log", exe);
            }
        }
#endif
#ifdef __PROSPERO__
        /* XPSemu Chihiro: the app's folder (its own is read-only). */
        snprintf(hd.path, sizeof(hd.path), "%sxemu-hud-diag.log",
                 xemu_settings_get_base_path());
#endif
        if (!hd.path[0]) {
            snprintf(hd.path, sizeof(hd.path), "xemu-hud-diag.log");
        }
    }

    hd.f = fopen(hd.path, "w");
    if (!hd.f) {
        fprintf(stderr, "HUD diag: cannot open %s\n", hd.path);
        return;
    }
    hd.enabled = true;
    fprintf(stderr, "HUD diag: logging to %s\n", hd.path);

    float ws = xemu_get_ws_scale();
    fprintf(hd.f,
        "# xemu HUD diag (log only)  ws_scale=%.3f  every=%u flips  "
        "max=%u frames\n"
        "# x/y: guest framebuffer units 0..surfaceSize, before the ws hack; "
        "x%% = x / surfaceSize.x\n"
        "# on screen with the hack: 50 + (x%% - 50) * %.3f\n"
        "# 2D: w equal on every vertex of the draw; 3D: stopped at the first "
        "differing w\n"
        "# FF: oPos[j] = dot(v0, c[j]), xy = xy/w + c[59].xy   "
        "c[] index = D3D index + 96\n"
        "# el: primitives for PT/LN/TRI/QD, else whole ranges; L/C/R: "
        "element centre in the left/centre/right third\n"
        "# kind: DAn = n DRAW_ARRAYS ranges in one submit, IE = elements, "
        "IB = inline buffer, IA = inline array\n"
        "# copy-pass fix: %s (ws=copy: x of v0 pre-compensated, so the "
        "logged x spans W/2 +- W/2/ws)\n"
        "# HUD anchor: %s (ws=L/R: moved by -/+ W/2*(1-ws)/ws = %.1f px at "
        "W=640; ws=C kept; ws=bd: backdrop, no anchoring after it on that "
        "surface this frame)\n"
        "# HUD side: centre within %.0f%% of an edge, or inside an element "
        "anchored earlier in the frame, or continuing a run\n"
        "# logged x of ws=copy/L/R draws is after the move\n"
        "# 4:3 clip on 2D screens: %s (c43: window clip narrowed to the 4:3 "
        "area on this surface, after a backdrop, and all of the next frame "
        "after a frame with a backdrop and no anchored HUD)\n"
        "# 16:9 menu background: %s (ws=wide: full-width layer stretched, "
        "after an untextured backdrop)\n",
        ws, hd.every, hd.max_frames, ws,
        pgraph_hud_ws_active && hw.copy_on ? "on" : "off",
        pgraph_hud_ws_active && hw.hud_on ? "on" : "off",
        320.0f * (1.0f - ws) / ws,
        hw.edge * 100.0f,
        pgraph_hud_ws_active && hw.clip_on ? "on" : "off",
        pgraph_hud_ws_active && hw.menubg_on ? "on" : "off");
    fflush(hd.f);
}

/* ------------------------------------------------------------------------ */

static const char *hd_prim_name(uint32_t m)
{
    static const char *const n[] = { "---", "PT", "LN", "LLP", "LST", "TRI",
                                     "TST", "TFN", "QD", "QST", "PLY" };
    return m < ARRAY_SIZE(n) ? n[m] : "?";
}

static unsigned int hd_prim_verts(uint32_t m)
{
    switch (m) {
    case NV097_SET_BEGIN_END_OP_POINTS:
        return 1;
    case NV097_SET_BEGIN_END_OP_LINES:
        return 2;
    case NV097_SET_BEGIN_END_OP_TRIANGLES:
        return 3;
    case NV097_SET_BEGIN_END_OP_QUADS:
        return 4;
    default:
        return 0; /* strips, fans, loops, polygons: one element per range */
    }
}

static void hd_emit_2d(unsigned int idx, bool ff, const char *id,
                       const char *kind, uint32_t prim, size_t n,
                       size_t m, bool measured, unsigned int nseq, float W,
                       float H, float w0, bool zconst, bool oob, bool nop,
                       PGRAPHState *pg)
{
    HdBuf *b = &hd.lines;
    hd_run_close();
    hb_printf(b, "  %04u 2D %s %s %-3s %-5s v=%-5zu", idx, ff ? "FF " : "PRG",
              id, hd_prim_name(prim), kind, n);

    unsigned int el = 0, cl = 0, cc = 0, cr = 0;
    HdBuf elist = { 0 };

    if (measured && m > 0) {
        float x0 = hd.sx[0], x1 = hd.sx[0], y0 = hd.sy[0], y1 = hd.sy[0];
        for (size_t i = 1; i < m; i++) {
            x0 = fminf(x0, hd.sx[i]);
            x1 = fmaxf(x1, hd.sx[i]);
            y0 = fminf(y0, hd.sy[i]);
            y1 = fmaxf(y1, hd.sy[i]);
        }
        float px0 = W > 0 ? 100.0f * x0 / W : 0.0f;
        float px1 = W > 0 ? 100.0f * x1 / W : 0.0f;
        hb_printf(b, " x=[%7.1f %7.1f] %5.1f-%5.1f%% y=[%6.1f %6.1f]", x0,
                  x1, px0, px1, y0, y1);

        unsigned int per = hd_prim_verts(prim);
        for (unsigned int s = 0; s < nseq; s++) {
            size_t a = hd.seq[s], e = hd.seq[s + 1];
            size_t step = per ? per : (e - a);
            if (!step) {
                continue;
            }
            for (size_t k = a; k + step <= e; k += step) {
                float ex0 = hd.sx[k], ex1 = hd.sx[k];
                float ey0 = hd.sy[k], ey1 = hd.sy[k];
                for (size_t v = k + 1; v < k + step; v++) {
                    ex0 = fminf(ex0, hd.sx[v]);
                    ex1 = fmaxf(ex1, hd.sx[v]);
                    ey0 = fminf(ey0, hd.sy[v]);
                    ey1 = fmaxf(ey1, hd.sy[v]);
                }
                float c = W > 0 ? 0.5f * (ex0 + ex1) / W : 0.5f;
                if (c < 1.0f / 3.0f) {
                    cl++;
                } else if (c > 2.0f / 3.0f) {
                    cr++;
                } else {
                    cc++;
                }
                if (el < HD_MAX_ELEMS_LISTED) {
                    hb_printf(&elist, " [%.0f-%.0f/%.0f-%.0f]", ex0, ex1, ey0,
                              ey1);
                }
                el++;
            }
        }
    } else if (!measured) {
        hb_printf(b, " big: classified on the first %u vertices",
                  HD_CLASSIFY_VERTS);
    }

    if (fabsf(w0 - 1.0f) <= 1e-5f) {
        hb_printf(b, " w1");
    } else {
        hb_printf(b, " w=%.4g", w0);
    }
    static const char *const wsn[] = { "", " ws=copy", " ws=FAIL",
                                       " ws=bd", " ws=L", " ws=C", " ws=R",
                                       " ws=wide" };
    hb_printf(b, "%s%s%s%s%s", zconst ? " zc" : "", oob ? " OOB" : "",
              nop ? " nop" : "", hd.cur_c43 ? " c43" : "",
              (unsigned int)hd.ws_action < ARRAY_SIZE(wsn) ?
              wsn[hd.ws_action] : " ws=?");
    if (measured) {
        hb_printf(b, " el=%u L%u C%u R%u", el, cl, cc, cr);
    }

    if (pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0) & NV_PGRAPH_TEXCTL0_0_ENABLE) {
        hb_printf(b, " t0=%08x", pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0));
    } else {
        hb_printf(b, " t0=-");
    }
    hb_printf(b, " s=%.0fx%.0f@%08x\n", W, H,
              (uint32_t)pg->surface_color.offset);

    if (el >= 2 && el <= HD_MAX_ELEMS_LISTED && elist.len) {
        hb_printf(b, "       e:%s\n", elist.p);
    }
    g_free(elist.p);
}

void pgraph_hud_diag_draw_impl(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (!hd.enabled) {
        return;
    }
    if (hd.draws >= HD_MAX_DRAWS) {
        hd.truncated = true;
        return;
    }
    unsigned int idx = hd.draws++;
    hd.cur_c43 = hw_c43_now(pg);

    unsigned int aa_w = 1, aa_h = 1;
    pgraph_apply_anti_aliasing_factor(pg, &aa_w, &aa_h);
    float W = (float)pg->surface_binding_dim.width / aa_w;
    float H = (float)pg->surface_binding_dim.height / aa_h;
    if (!hd.surf_set) {
        hd.surf_set = true;
        hd.surf_w = pg->surface_binding_dim.width;
        hd.surf_h = pg->surface_binding_dim.height;
        hd.aa_w = aa_w;
        hd.aa_h = aa_h;
        hd.scale = pg->surface_scale_factor;
    }

    uint32_t c0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool color_write = c0 & (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
                             NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
                             NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
                             NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
    bool depth_test = c0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool nop = !(color_write || depth_test || stencil_test);
    hd.n_nop += nop;

    uint32_t csv0_d = pgraph_reg_r(pg, NV_PGRAPH_CSV0_D);
    unsigned int mode = GET_MASK(csv0_d, NV_PGRAPH_CSV0_D_MODE);
    bool ff = mode == 0;
    bool prg = mode == 2;
    hd.n_ff += ff;
    hd.n_prg += prg;

    /* the batch, in the order the renderers' flush_draw tests it */
    int kind = HD_K_EMPTY;
    size_t n = 0;
    unsigned int nseq = 1;
    unsigned int ia_off[NV2A_VERTEXSHADER_ATTRIBUTES] = { 0 };
    unsigned int ia_vsize = 0;
    char kind_s[16];

    if (pg->draw_arrays_length) {
        kind = HD_K_DA;
        nseq = pg->draw_arrays_length;
        for (unsigned int i = 0; i < nseq; i++) {
            n += (size_t)MAX(pg->draw_arrays_count[i], 0);
        }
        snprintf(kind_s, sizeof(kind_s), "DA%u", nseq);
    } else if (pg->inline_elements_length) {
        kind = HD_K_IE;
        n = pg->inline_elements_length;
        snprintf(kind_s, sizeof(kind_s), "IE");
    } else if (pg->inline_buffer_length) {
        kind = HD_K_IB;
        n = pg->inline_buffer_length;
        snprintf(kind_s, sizeof(kind_s), "IB");
    } else if (pg->inline_array_length) {
        kind = HD_K_IA;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count) {
                /* XPSemu Chihiro: the layout this renderer uploads
                 * (vk/draw.c, inline array): each attribute aligned to its
                 * element size. */
                ia_vsize = ROUND_UP(ia_vsize, attr->size);
                ia_off[i] = ia_vsize;
                ia_vsize += attr->size * attr->count;
                ia_vsize = ROUND_UP(ia_vsize, attr->size);
            }
        }
        n = ia_vsize ? (size_t)pg->inline_array_length * 4 / ia_vsize : 0;
        snprintf(kind_s, sizeof(kind_s), "IA");
    } else {
        snprintf(kind_s, sizeof(kind_s), "--");
    }

    if (n == 0) {
        hd.n_empty++;
        hd_run_add(idx, HD_CL_EMPTY, ff, prg, nop,
                   (uint32_t)pg->surface_color.offset);
        return;
    }

    /* the program, or the FF matrix */
    HdProg *prog = NULL;
    unsigned int pstart = 0;
    uint16_t need = 0x0001;
    char id[16] = "";
    float cm[4][4] = { { 0 } }, vp[4] = { 0 };
    uint64_t mat_key = 0;

    if (prg) {
        prog = hd_get_prog(pg, &pstart);
        if (!prog || !prog->ok) {
            if (prog && hd_first_sight(prog->hash)) {
                hd_dump_prog(prog, pstart, pg->program_data[pstart]);
            }
            hd.n_unk++;
            hd_run_add(idx, HD_CL_UNK, ff, prg, nop,
                   (uint32_t)pg->surface_color.offset);
            return;
        }
        need = prog->in_mask;
        snprintf(id, sizeof(id), "p%08x", hd_h32(prog->hash));
        memcpy(hd_ctx, pg->vsh_constants, sizeof(pg->vsh_constants));
    } else if (ff) {
        if (GET_MASK(csv0_d, NV_PGRAPH_CSV0_D_SKIN) !=
            NV_PGRAPH_CSV0_D_SKIN_OFF) {
            hd.n_3d++; /* skinned: always 3D geometry, not evaluated */
            hd_run_add(idx, HD_CL_3D, ff, prg, nop,
                   (uint32_t)pg->surface_color.offset);
            return;
        }
        memcpy(cm, &pg->vsh_constants[NV_IGRAPH_XF_XFCTX_CMAT0], sizeof(cm));
        memcpy(vp, pg->vsh_constants[NV_IGRAPH_XF_XFCTX_VPOFF], sizeof(vp));
        mat_key = hd_fnv(HD_FNV_INIT ^ 'M', cm, sizeof(cm));
        mat_key = hd_fnv(mat_key, vp, 2 * sizeof(float));
        snprintf(id, sizeof(id), "m%08x", hd_h32(mat_key));
    } else {
        hd.n_unk++;
        hd_run_add(idx, HD_CL_UNK, ff, prg, nop,
                   (uint32_t)pg->surface_color.offset);
        return;
    }

    /* vertices to evaluate, in submission order */
    bool measured = n <= HD_MAX_EVAL_VERTS;
    size_t m = measured ? n : HD_CLASSIFY_VERTS;
    hd_reserve(m);
    hd_reserve_seq((size_t)nseq + 1);

    if (kind == HD_K_DA) {
        size_t o = 0;
        for (unsigned int s = 0; s < nseq; s++) {
            hd.seq[s] = (uint32_t)MIN(o, m);
            int32_t cnt = MAX(pg->draw_arrays_count[s], 0);
            for (int32_t j = 0; j < cnt && o < m; j++) {
                hd.keys[o++] = (uint32_t)pg->draw_arrays_start[s] + (uint32_t)j;
            }
            if (o >= m) {
                /* remaining ranges start past the evaluated part */
                for (unsigned int t = s + 1; t < nseq; t++) {
                    hd.seq[t] = (uint32_t)m;
                }
                break;
            }
        }
        hd.seq[nseq] = (uint32_t)m;
    } else {
        for (size_t i = 0; i < m; i++) {
            hd.keys[i] = kind == HD_K_IE ? pg->inline_elements[i] : (uint32_t)i;
        }
        hd.seq[0] = 0;
        hd.seq[1] = (uint32_t)m;
    }

    HdAttr at[NV2A_VERTEXSHADER_ATTRIBUTES];
    memset(at, 0, sizeof(at));
    bool oob = false;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (need & (1u << i)) {
            hd_setup_attr(d, kind, i, ia_off[i], ia_vsize, &at[i]);
            oob |= at[i].bad;
        }
    }

    /* evaluate; a 3D draw stops at its first differing w */
    bool is2d = true, zconst = true;
    float w0 = 0.0f, z0 = 0.0f;
    HdRegs r;

    for (size_t i = 0; i < m; i++) {
        float x, y, z, w;
        if (ff) {
            float p[4];
            hd_fetch(&at[0], hd.keys[i], p, &oob);
            w = hd_clamp_away(hd_dot4(p, cm[3]));
            x = hd_dot4(p, cm[0]) / w + vp[0];
            y = hd_dot4(p, cm[1]) / w + vp[1];
            z = hd_dot4(p, cm[2]);
        } else {
            memset(&r, 0, sizeof(r));
            r.out[3] = 1.0f; /* oPos starts as (0,0,0,1) */
            for (int a = 0; a < NV2A_VERTEXSHADER_ATTRIBUTES; a++) {
                if (need & (1u << a)) {
                    hd_fetch(&at[a], hd.keys[i], &r.in[a * 4], &oob);
                }
            }
            hd_run_prog(prog, &r);
            x = r.out[0];
            y = r.out[1];
            z = r.out[2];
            w = hd_clamp_away(r.out[3]);
        }
        if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
            is2d = false;
            break;
        }
        if (i == 0) {
            w0 = w;
            z0 = z;
        } else {
            if (fabsf(w - w0) > HD_W_REL_EPS * fmaxf(1.0f, fabsf(w0))) {
                is2d = false;
                break;
            }
            if (fabsf(z - z0) > HD_Z_REL_EPS * fmaxf(1.0f, fabsf(z0))) {
                zconst = false;
            }
        }
        hd.sx[i] = x;
        hd.sy[i] = y;
    }

    if (!is2d) {
        hd.n_3d++;
        hd_run_add(idx, HD_CL_3D, ff, prg, nop,
                   (uint32_t)pg->surface_color.offset);
        return;
    }

    hd.n_2d++;
    if (prg) {
        if (hd_first_sight(prog->hash)) {
            hd_dump_prog(prog, pstart, pg->program_data[pstart]);
        }
    } else if (fabsf(w0 - 1.0f) <= 1e-5f && hd_first_sight(mat_key)) {
        /* w != 1: flat 3D objects (billboards), not worth a dump */
        hd_dump_ffmat(hd_h32(mat_key), (const float (*)[4])cm, vp);
    }

    hd_emit_2d(idx, ff, id, kind_s, pg->primitive_mode, n, m, measured, nseq,
               W, H, w0, zconst, oob, nop, pg);
}

/* ------------------------------------------------------------------------ */
/* Widescreen passes: copy-pass fix and HUD anchoring                       */

static void hw_init(void)
{
    hw.init = true;
    hw.ws = xemu_get_ws_scale();
    const char *e = getenv("XEMU_WS_COPY");
    hw.copy_on = !(e && e[0] == '0');
    e = getenv("XEMU_WS_HUD");
    hw.hud_on = !(e && e[0] == '0');
    e = getenv("XEMU_WS_MENU43");
    hw.clip_on = !(e && e[0] == '0');
    e = getenv("XEMU_WS_MENUBG");
    hw.menubg_on = e && e[0] == '1';   /* opt-in: menus stay 4:3 */
    /* XEMU_WS_HUD_EDGE: percent of the width from each edge within which
     * an element's centre anchors it to that edge (default 26) */
    hw.edge = 0.26f;
    e = getenv("XEMU_WS_HUD_EDGE");
    if (e && *e) {
        char *end;
        double v = strtod(e, &end);
        if (!*end && v > 0.0 && v < 50.0) {
            hw.edge = (float)(v / 100.0);
        }
    }
    pgraph_hud_ws_active =
        (hw.copy_on || hw.hud_on || hw.clip_on || hw.menubg_on) &&
        hw.ws != 1.0f && hw.ws > 0.0f;
}

static bool hw_has(const uint32_t *set, unsigned int n, uint32_t v)
{
    for (unsigned int i = 0; i < n; i++) {
        if (set[i] == v) {
            return true;
        }
    }
    return false;
}

static void hw_add(uint32_t *set, unsigned int *n, unsigned int cap,
                   uint32_t v)
{
    if (*n < cap && !hw_has(set, *n, v)) {
        set[(*n)++] = v;
    }
}

/* true if v was in the set; it is removed either way */
static bool hw_take(uint32_t *set, unsigned int *n, uint32_t v)
{
    for (unsigned int i = 0; i < *n; i++) {
        if (set[i] == v) {
            set[i] = set[--(*n)];
            return true;
        }
    }
    return false;
}

static bool hw_is_rt(uint32_t off)
{
    return hw_has(hw.rt, hw.n_rt, off);
}

static bool hw_color_write(PGRAPHState *pg)
{
    return pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
           (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
}

static void hw_note_rt(uint32_t off)
{
    if (hw_is_rt(off)) {
        return;
    }
    hw.rt[hw.rt_next] = off;
    hw.rt_next = (hw.rt_next + 1) % ARRAY_SIZE(hw.rt);
    if (hw.n_rt < ARRAY_SIZE(hw.rt)) {
        hw.n_rt++;
    }
}

#define HW_SCREEN_MIN_DRAWS 4u

static void hw_frame_reset(void)
{
    hw.last_calls = hw.clip_calls;
    hw.last_applied = hw.clip_applied;
    hw.clip_calls = 0;
    hw.clip_applied = 0;
    hw.last_clears_split = hw.clears_split;
    hw.clears_split = 0;

    /* a surface that had a backdrop and no anchored HUD this frame is a 2D
     * screen: clip it to 4:3 from the first draw of the next frame, so a
     * 3D background drawn before the backdrop is cut as well */
    hw.n_s43 = 0;
    for (unsigned int i = 0; i < hw.n_bd; i++) {
        /* a fade at the end of a race or flyby frame is followed by a draw
         * or two; a screen's backdrop by its whole layout */
        if (hw.bd_after[i] >= HW_SCREEN_MIN_DRAWS &&
            !hw_has(hw.wide, hw.n_wide, hw.bd[i]) &&
            !hw_has(hw.lr, hw.n_lr, hw.bd[i])) {
            hw_add(hw.s43, &hw.n_s43, ARRAY_SIZE(hw.s43), hw.bd[i]);
        }
    }
    hw.n_lr = 0;
    hw.n_fade = 0;
    hw.n_scene = 0;
    hw.n_bd = 0;
    hw.n_wide = 0;
    hw.n_phase = 0;
    hw.n_anch = 0;
    hw.run.valid = false;
}

/* Screen x of one vertex through the program; dx is added to v0.x first. */
static float hw_prog_sx(const HdProg *p, const HdAttr *at, uint16_t need,
                        uint32_t key, float dx, float *sy, float *w)
{
    HdRegs r;
    bool oob = false;
    memset(&r, 0, sizeof(r));
    r.out[3] = 1.0f;
    for (int a = 0; a < NV2A_VERTEXSHADER_ATTRIBUTES; a++) {
        if (need & (1u << a)) {
            hd_fetch(&at[a], key, &r.in[a * 4], &oob);
        }
    }
    r.in[0] += dx;
    hd_run_prog(p, &r);
    if (sy) {
        *sy = r.out[1];
    }
    if (w) {
        *w = hd_clamp_away(r.out[3]);
    }
    return r.out[0];
}

#define HW_MAX_VERTS 64u
#define HW_RUN_GAP   4.0f /* px: a draw touching the run's box continues it */

/* Move every vertex so its screen x becomes target[i], through v0.x in the
 * batch. sx must be affine in v0.x; the result is re-checked and undone if
 * any vertex misses its target. */
static bool hw_write_x(PGRAPHState *pg, const HdProg *prog, const HdAttr *at,
                       uint16_t need, int kind, size_t n,
                       const unsigned int *ia_off, unsigned int ia_vsize,
                       const float *sx, const float *target)
{
    float *xp[HW_MAX_VERTS];
    if (kind == HD_K_IB) {
        if (at[0].src != HD_SRC_IBUF) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            xp[i] = pg->vertex_attributes[0].inline_buffer + i * 4;
        }
    } else {
        if (at[0].src != HD_SRC_MEM ||
            at[0].tmpl.format != NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            xp[i] = (float *)((uint8_t *)pg->inline_array + i * ia_vsize +
                              ia_off[0]);
        }
    }

    float a1 = hw_prog_sx(prog, at, need, 0, 1.0f, NULL, NULL) - sx[0];
    float a2 = hw_prog_sx(prog, at, need, 0, 2.0f, NULL, NULL) - sx[0];
    if (!isfinite(a1) || fabsf(a1) < 1e-6f ||
        fabsf(a2 - 2.0f * a1) > 1e-3f * fmaxf(1.0f, fabsf(a1))) {
        return false;
    }

    float orig[HW_MAX_VERTS];
    for (size_t i = 0; i < n; i++) {
        memcpy(&orig[i], xp[i], sizeof(float));
    }
    for (size_t i = 0; i < n; i++) {
        float x = orig[i] + (target[i] - sx[i]) / a1;
        memcpy(xp[i], &x, sizeof(float));
    }
    for (size_t i = 0; i < n; i++) {
        float s = hw_prog_sx(prog, at, need, (uint32_t)i, 0.0f, NULL, NULL);
        if (!(fabsf(s - target[i]) <= 0.1f + 1e-5f * fabsf(target[i]))) {
            for (size_t j = 0; j < n; j++) {
                memcpy(xp[j], &orig[j], sizeof(float));
            }
            return false;
        }
    }
    return true;
}

void pgraph_hud_ws_draw_impl(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    uint32_t surf = (uint32_t)pg->surface_color.offset;
    bool cw = hw_color_write(pg);

    hd.ws_action = HD_WS_NONE;
    if (cw) {
        hw_note_rt(surf);
    }
    /* any draw ends the background run unless it is itself a full-width
     * layer; that case puts the flag back below */
    bool phase = hw_take(hw.phase, &hw.n_phase, surf);
    for (unsigned int i = 0; i < hw.n_bd; i++) {
        if (hw.bd[i] == surf) {
            hw.bd_after[i]++;
        }
    }

    unsigned int mode = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                                 NV_PGRAPH_CSV0_D_MODE);
    if (mode == 0) {
        /* fixed function is the 3D scene in these titles */
        if (cw) {
            hw_add(hw.scene, &hw.n_scene, ARRAY_SIZE(hw.scene), surf);
        }
        hw.run.valid = false;
        return;
    }

    bool tex_on = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0) &
                  NV_PGRAPH_TEXCTL0_0_ENABLE;
    uint32_t t0 = tex_on ? pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0) : 0xFFFFFFFFu;
    bool tex_rt = tex_on && hw_is_rt(t0);
    bool bd = hw_has(hw.bd, hw.n_bd, surf);
    bool anchor = hw.hud_on && !bd && !hw_has(hw.fade, hw.n_fade, surf) &&
                  hw_has(hw.scene, hw.n_scene, surf);
    /* a backdrop must be caught even before the scene starts: menus draw
     * theirs first and a 3D model (the car on the card screen) after it */
    bool wide = bd && hw_has(hw.wide, hw.n_wide, surf);
    bool want_bd = (hw.hud_on || hw.clip_on || hw.menubg_on) && !bd;

    if (mode != 2 ||
        !((tex_rt && hw.copy_on) || anchor || want_bd || wide)) {
        hw.run.valid = false;
        return;
    }

    /* only batches held in PGRAPH itself can be adjusted */
    int kind;
    size_t n;
    unsigned int ia_off[NV2A_VERTEXSHADER_ATTRIBUTES] = { 0 };
    unsigned int ia_vsize = 0;
    if (pg->draw_arrays_length || pg->inline_elements_length) {
        n = 0;
        kind = HD_K_EMPTY;
    } else if (pg->inline_buffer_length) {
        kind = HD_K_IB;
        n = pg->inline_buffer_length;
    } else if (pg->inline_array_length) {
        kind = HD_K_IA;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count) {
                /* XPSemu Chihiro: the layout this renderer uploads
                 * (vk/draw.c, inline array): each attribute aligned to its
                 * element size. */
                ia_vsize = ROUND_UP(ia_vsize, attr->size);
                ia_off[i] = ia_vsize;
                ia_vsize += attr->size * attr->count;
                ia_vsize = ROUND_UP(ia_vsize, attr->size);
            }
        }
        n = ia_vsize ? (size_t)pg->inline_array_length * 4 / ia_vsize : 0;
    } else {
        n = 0;
        kind = HD_K_EMPTY;
    }
    if (n == 0 || n > HW_MAX_VERTS) {
        hw.run.valid = false;
        return;
    }

    unsigned int pstart;
    HdProg *prog = hd_get_prog(pg, &pstart);
    if (!prog || !prog->ok || !(prog->in_mask & 1)) {
        hw.run.valid = false;
        return;
    }
    uint16_t need = prog->in_mask;

    HdAttr at[NV2A_VERTEXSHADER_ATTRIBUTES];
    memset(at, 0, sizeof(at));
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        if (need & (1u << i)) {
            hd_setup_attr(d, kind, i, ia_off[i], ia_vsize, &at[i]);
        }
    }
    memcpy(hd_ctx, pg->vsh_constants, sizeof(pg->vsh_constants));

    unsigned int aa_w = 1, aa_h = 1;
    pgraph_apply_anti_aliasing_factor(pg, &aa_w, &aa_h);
    float W = (float)pg->surface_binding_dim.width / aa_w;
    float H = (float)pg->surface_binding_dim.height / aa_h;
    if (W <= 0.0f || H <= 0.0f) {
        hw.run.valid = false;
        return;
    }

    float sx[HW_MAX_VERTS], target[HW_MAX_VERTS];
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0, w0 = 0;
    for (size_t i = 0; i < n; i++) {
        float sy, w;
        sx[i] = hw_prog_sx(prog, at, need, (uint32_t)i, 0.0f, &sy, &w);
        if (!isfinite(sx[i]) || !isfinite(sy)) {
            hw.run.valid = false;
            return;
        }
        if (i == 0) {
            w0 = w;
            x0 = x1 = sx[0];
            y0 = y1 = sy;
        } else {
            if (fabsf(w - w0) > HD_W_REL_EPS * fmaxf(1.0f, fabsf(w0))) {
                hw.run.valid = false;
                return; /* not screen space */
            }
            x0 = fminf(x0, sx[i]);
            x1 = fmaxf(x1, sx[i]);
            y0 = fminf(y0, sy);
            y1 = fmaxf(y1, sy);
        }
    }
    bool full_w = x0 <= 1.0f && x1 >= W - 1.0f;
    bool full_h = y0 <= 1.0f && y1 >= H - 1.0f;

    /* copy pass: keep it 1:1 */
    if (tex_rt && hw.copy_on && (full_w || full_h)) {
        hw.run.valid = false;
        for (size_t i = 0; i < n; i++) {
            target[i] = 0.5f * W + (sx[i] - 0.5f * W) / hw.ws;
        }
        hd.ws_action = hw_write_x(pg, prog, at, need, kind, n, ia_off,
                                  ia_vsize, sx, target) ? HD_WS_COPY
                                                        : HD_WS_FAIL;
        return;
    }
    /* A full-screen quad drawn past the surface edges (the race-start fade:
     * -9.5 .. W+0.5) is an overlay, not a screen: stretch it to 16:9, and
     * as before leave what follows it in the frame unanchored, but do not
     * clip or carry anything into the next frame. A backdrop is aligned to
     * the surface. */
    bool exact = fabsf(x0) <= 1.0f && fabsf(y0) <= 1.0f &&
                 fabsf(x1 - W) <= 1.0f && fabsf(y1 - H) <= 1.0f;
    if (full_w && full_h && !exact && !tex_rt) {
        hw_add(hw.fade, &hw.n_fade, ARRAY_SIZE(hw.fade), surf);
        hw.run.valid = false;
        for (size_t i = 0; i < n; i++) {
            target[i] = 0.5f * W + (sx[i] - 0.5f * W) / hw.ws;
        }
        hd.ws_action = hw_write_x(pg, prog, at, need, kind, n, ia_off,
                                  ia_vsize, sx, target) ? HD_WS_WIDE
                                                        : HD_WS_FAIL;
        return;
    }

    /* a full-surface screen-space draw is a backdrop: whatever follows on
     * this surface in this frame belongs to a 2D screen, not to a HUD. An
     * untextured one (a plain fill: the menus start with one) makes the
     * screen's background 16:9 - it and every later full-width layer are
     * stretched like a copy pass - while the panel on top stays 4:3. A
     * textured one (a picture: the stage intro) keeps the screen 4:3. */
    if (want_bd && exact) {
        if (hw.n_bd < ARRAY_SIZE(hw.bd)) {
            hw.bd_after[hw.n_bd] = 0;
        }
        hw_add(hw.bd, &hw.n_bd, ARRAY_SIZE(hw.bd), surf);
        hw.run.valid = false;
        hd.ws_action = HD_WS_BD;
        if (hw.menubg_on && !tex_on) {
            hw_add(hw.wide, &hw.n_wide, ARRAY_SIZE(hw.wide), surf);
            wide = true;
            phase = true;
        } else {
            return;
        }
    }
    if (wide) {
        /* plain fills are stretched wherever they come; textured full-width
         * layers only in the run right after the backdrop - later ones
         * (the top and bottom frame with the TIME and button sockets) are
         * part of the 4:3 panel */
        hw.run.valid = false;
        /* untextured strips along the top or bottom edge are the fades the
         * frame hides in 4:3; stretched, they would show beside it */
        bool edge_fade = !tex_on && !full_h &&
                         (y0 <= 1.0f || y1 >= H - 1.0f);
        if (full_w && (!tex_on || phase) && !edge_fade) {
            for (size_t i = 0; i < n; i++) {
                target[i] = 0.5f * W + (sx[i] - 0.5f * W) / hw.ws;
            }
            hd.ws_action = hw_write_x(pg, prog, at, need, kind, n, ia_off,
                                      ia_vsize, sx, target) ? HD_WS_WIDE
                                                            : HD_WS_FAIL;
        }
        if (full_w && phase) {
            hw_add(hw.phase, &hw.n_phase, ARRAY_SIZE(hw.phase), surf);
        }
        return;
    }
    if (!want_bd) {
        hw.run.valid = false;
        return;
    }
    if (!anchor) {
        hw.run.valid = false;
        return;
    }

    /* side, in this order:
     * - inside an element already anchored this frame (a needle in its
     *   dial, digits in their panel): that element's side;
     * - continuing the current run - consecutive draws with the same
     *   texture and program whose boxes touch (glyphs of a string, outline
     *   passes): the run's side;
     * - otherwise by the centre, only within hw.edge of an edge: a centred
     *   block (results, "meters to goal ... m") reaches into the outer
     *   thirds without belonging to either side. */
    int side = 0;
    bool inside = false;
    for (unsigned int i = hw.n_anch; i-- > 0;) {
        if (hw.anch[i].surf == surf &&
            hw.anch[i].x1 - hw.anch[i].x0 >= 8.0f &&
            hw.anch[i].y1 - hw.anch[i].y0 >= 8.0f &&
            x0 >= hw.anch[i].x0 - 2.0f && x1 <= hw.anch[i].x1 + 2.0f &&
            y0 >= hw.anch[i].y0 - 2.0f && y1 <= hw.anch[i].y1 + 2.0f) {
            side = hw.anch[i].side;
            inside = true;
            break;
        }
    }
    bool cont = hw.run.valid && hw.run.surf == surf && hw.run.t0 == t0 &&
                hw.run.prog == prog->hash &&
                x0 <= hw.run.x1 + HW_RUN_GAP && x1 >= hw.run.x0 - HW_RUN_GAP &&
                y0 <= hw.run.y1 + HW_RUN_GAP && y1 >= hw.run.y0 - HW_RUN_GAP;
    if (cont) {
        if (!inside) {
            side = hw.run.side;
        }
        hw.run.x0 = fminf(hw.run.x0, x0);
        hw.run.x1 = fmaxf(hw.run.x1, x1);
        hw.run.y0 = fminf(hw.run.y0, y0);
        hw.run.y1 = fmaxf(hw.run.y1, y1);
    } else {
        if (!inside) {
            float c = 0.5f * (x0 + x1) / W;
            side = c < hw.edge ? -1 : (c > 1.0f - hw.edge ? 1 : 0);
        }
        hw.run.valid = true;
        hw.run.surf = surf;
        hw.run.t0 = t0;
        hw.run.prog = prog->hash;
        hw.run.x0 = x0;
        hw.run.x1 = x1;
        hw.run.y0 = y0;
        hw.run.y1 = y1;
        hw.run.side = side;
    }

    if (side == 0) {
        hd.ws_action = HD_WS_C;
        return;
    }
    /* a HUD on this surface: not a 2D screen, stop carrying last frame's
     * 4:3 clip into it */
    hw_add(hw.lr, &hw.n_lr, ARRAY_SIZE(hw.lr), surf);
    hw_take(hw.s43, &hw.n_s43, surf);
    if (hw.n_anch < ARRAY_SIZE(hw.anch)) {
        hw.anch[hw.n_anch].surf = surf;
        hw.anch[hw.n_anch].x0 = x0;
        hw.anch[hw.n_anch].x1 = x1;
        hw.anch[hw.n_anch].y0 = y0;
        hw.anch[hw.n_anch].y1 = y1;
        hw.anch[hw.n_anch].side = side;
        hw.n_anch++;
    }

    /* the hack narrows x by ws around the centre; moving by
     * W/2 * (1 - ws) / ws puts the element as far from the 16:9 edge as it
     * was from the 4:3 edge, at unchanged size */
    float dx = (float)side * 0.5f * W * (1.0f - hw.ws) / hw.ws;
    for (size_t i = 0; i < n; i++) {
        target[i] = sx[i] + dx;
    }
    if (hw_write_x(pg, prog, at, need, kind, n, ia_off, ia_vsize, sx,
                   target)) {
        hd.ws_action = side < 0 ? HD_WS_L : HD_WS_R;
    } else {
        hd.ws_action = HD_WS_FAIL;
    }
}

static bool hw_c43_now(PGRAPHState *pg)
{
    uint32_t surf = (uint32_t)pg->surface_color.offset;
    if (!pgraph_hud_ws_active || !hw.clip_on) {
        return false;
    }
    if (hw_has(hw.s43, hw.n_s43, surf)) {
        return true;
    }
    return hw_has(hw.bd, hw.n_bd, surf) && !hw_has(hw.wide, hw.n_wide, surf);
}

/* A 2D screen (a backdrop was drawn on this surface earlier in the frame)
 * keeps the 4:3 frame the game was made for: whatever the clip x scale lets
 * in beyond it - the rest of a 3D car, the floor, off-screen parts of the
 * layout - is cut by narrowing the window clip regions to the 4:3 area.
 * Called by the pixel shader uniform setup of both renderers, with the
 * regions already in host pixels of the bound surface. */
void pgraph_hud_ws_clip_regions(PGRAPHState *pg, int (*region)[4],
                                unsigned int width_px, unsigned int height_px)
{
    hw.clip_calls++;
    if (!hw_c43_now(pg) || width_px == 0) {
        return;
    }
    hw.clip_applied++;
    int c0 = (int)lrintf((float)width_px * 0.5f * (1.0f - hw.ws));
    int c1 = (int)lrintf((float)width_px * 0.5f * (1.0f + hw.ws));

    if (!(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
          NV_PGRAPH_SETUPRASTER_WINDOWCLIPTYPE)) {
        /* inclusive: a fragment must lie inside one region */
        for (int i = 0; i < 8; i++) {
            region[i][0] = MAX(region[i][0], c0);
            region[i][2] = MIN(region[i][2], c1);
            if (region[i][2] < region[i][0]) {
                region[i][2] = region[i][0];
            }
        }
        return;
    }

    /* exclusive: a fragment inside any region is dropped; the two side
     * strips go into unused (empty) regions */
    int strips[2][4] = { { 0, 0, c0, (int)height_px },
                         { c1, 0, (int)width_px, (int)height_px } };
    int k = 0;
    for (int i = 0; i < 8 && k < 2; i++) {
        if (region[i][0] >= region[i][2] || region[i][1] >= region[i][3]) {
            memcpy(region[i], strips[k++], sizeof(strips[0]));
        }
    }
}

/* The 4:3 clip keeps draws out of the sides of a 2D screen, but the game's
 * clear still paints them in its clear colour (light grey in the WMMT2
 * menus). On such a surface the colour clear is done twice: first the whole
 * rectangle in black (alpha kept), then the 4:3 part in the game's colour.
 * Returns true if it issued the clear itself. */
bool pgraph_hud_ws_clear(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;

    if (!(parameter & NV097_CLEAR_SURFACE_COLOR) || !hw_c43_now(pg)) {
        return false;
    }
    unsigned int w = pg->surface_shape.clip_x + pg->surface_shape.clip_width;
    if (w == 0) {
        return false;
    }
    uint32_t rect = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    unsigned int xmin = GET_MASK(rect, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(rect, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int c0 = (unsigned int)lrintf((float)w * 0.5f * (1.0f - hw.ws));
    unsigned int c1 = (unsigned int)lrintf((float)w * 0.5f * (1.0f + hw.ws));
    unsigned int ixmin = MAX(xmin, c0);
    unsigned int ixmax = MIN(xmax, c1 - 1);
    if (xmin >= c0 && xmax <= c1 - 1) {
        return false; /* nothing outside the 4:3 area */
    }

    uint32_t color = pgraph_reg_r(pg, NV_PGRAPH_COLORCLEARVALUE);
    pgraph_reg_w(pg, NV_PGRAPH_COLORCLEARVALUE, color & 0xFF000000u);
    pg->renderer->ops.clear_surface(d, parameter);
    pgraph_reg_w(pg, NV_PGRAPH_COLORCLEARVALUE, color);

    if (ixmin <= ixmax) {
        uint32_t r43 = rect;
        SET_MASK(r43, NV_PGRAPH_CLEARRECTX_XMIN, ixmin);
        SET_MASK(r43, NV_PGRAPH_CLEARRECTX_XMAX, ixmax);
        pgraph_reg_w(pg, NV_PGRAPH_CLEARRECTX, r43);
        pg->renderer->ops.clear_surface(
            d, parameter & ~(uint32_t)(NV097_CLEAR_SURFACE_Z |
                                       NV097_CLEAR_SURFACE_STENCIL));
        pgraph_reg_w(pg, NV_PGRAPH_CLEARRECTX, rect);
    }
    hw.clears_split++;
    return true;
}

void pgraph_hud_diag_flip(NV2AState *d)
{
    (void)d;

    if (!hw.init) {
        hw_init();
    }
    hw_frame_reset();
    if (!hd.init) {
        hd_init();
    }
    if (!hd.enabled) {
        return;
    }

    if (pgraph_hud_diag_capturing) {
        pgraph_hud_diag_capturing = false;
        hd_frame_end();
        if (hd.captured >= hd.max_frames) {
            fprintf(hd.f, "# done: %u frames captured\n", hd.captured);
            fclose(hd.f);
            hd.f = NULL;
            hd.enabled = false;
            return;
        }
    }

    hd.flips++;
    if (hd.flips % hd.every == 0) {
        hd_frame_begin();
        pgraph_hud_diag_capturing = true;
    }
}
