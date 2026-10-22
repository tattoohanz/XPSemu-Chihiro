/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

static void perform_blit(int operation, uint8_t *source, uint8_t *dest,
                         size_t width, size_t height, size_t width_bytes,
                         size_t source_pitch, size_t dest_pitch,
                         BetaState *beta)
{
    if (operation == NV09F_SET_OPERATION_SRCCOPY) {
        for (unsigned int y = 0; y < height; y++) {
            memmove(dest, source, width_bytes);
            source += source_pitch;
            dest += dest_pitch;
        }
    } else if (operation == NV09F_SET_OPERATION_BLEND_AND) {
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;

        for (unsigned int y = 0; y < height; y++) {
            uint8_t *s = source;
            uint8_t *d = dest;
            for (unsigned int x = 0; x < width; x++) {
                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = s[x * 4 + ch] * beta_mult;
                    uint32_t b = d[x * 4 + ch] * inv_beta_mult;
                    d[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }
            source += source_pitch;
            dest += dest_pitch;
        }
    } else {
        fprintf(stderr, "Unknown blit operation: 0x%x\n", operation);
        assert(false && "Unknown blit operation");
    }
}

static void patch_alpha(uint8_t *dest, size_t width_pixels, size_t height,
                        size_t dest_pitch, uint8_t alpha_val)
{
    for (unsigned int y = 0; y < height; y++) {
        uint8_t *d = dest;
        for (unsigned int x = 0; x < width_pixels; x++) {
            d[x * 4 + 3] = alpha_val;
        }
        dest += dest_pitch;
    }
}

/*
 * XPSemu Chihiro: a plain copy between two rendered color surfaces (OutRun 2
 * copies its 160x120 effect buffers twice a frame) is done on the GPU,
 * image to image. On the CPU it read the source back into RAM and then
 * uploaded the destination again, each waiting for the PS5 GPU.
 * XEMU_GPU_BLIT=0 in env.txt does every blit on the CPU as before.
 */
static bool gpu_blit_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XEMU_GPU_BLIT");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* NULL when the GPU can do this blit, else why not (for the game log). */
static const char *gpu_blit_refusal(ContextSurfaces2DState *cs,
                                    ImageBlitState *b, SurfaceBinding *src,
                                    SurfaceBinding *dst, bool clipped)
{
    if (!gpu_blit_enabled()) {
        return "CPU: off";
    }
    if (b->operation != NV09F_SET_OPERATION_SRCCOPY) {
        return "CPU: blend";
    }
    if (cs->color_format != NV062_SET_COLOR_FORMAT_LE_A8R8G8B8) {
        return "CPU: blit format";
    }
    if (!src) {
        return "CPU: no surface at source";
    }
    if (!dst) {
        return "CPU: no surface at dest";
    }
    if (!src->color || !dst->color) {
        return "CPU: depth";
    }
    if (src == dst) {
        return "CPU: same surface";
    }
    if (src->swizzle || dst->swizzle) {
        return "CPU: swizzled";
    }
    if (src->pitch != cs->source_pitch || dst->pitch != cs->dest_pitch) {
        return "CPU: pitch";
    }
    if (src->fmt.bytes_per_pixel != 4 || dst->fmt.bytes_per_pixel != 4 ||
        src->host_fmt.vk_format != dst->host_fmt.vk_format) {
        return "CPU: surface format";
    }
    if (!b->width || !b->height ||
        b->in_x + b->width > src->width || b->in_y + b->height > src->height ||
        b->out_x + b->width > dst->width ||
        b->out_y + b->height > dst->height) {
        return "CPU: outside";
    }
    if (clipped) {
        return "CPU: tiled";
    }
    if (!src->initialized || !dst->initialized) {
        return "CPU: not initialized";
    }
    if (src->upload_pending) {
        return "CPU: source newer in RAM";
    }
    if (dst->upload_pending &&
        (b->out_x || b->out_y || b->width < dst->width ||
         b->height < dst->height)) {
        return "CPU: dest newer in RAM";
    }
    return NULL;
}

static void gpu_blit(NV2AState *d, ImageBlitState *b, SurfaceBinding *src,
                     SurfaceBinding *dst)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(pg, cmd, src->image,
                                      src->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, dst->image,
                                      dst->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    unsigned int in_x = b->in_x, in_y = b->in_y;
    unsigned int out_x = b->out_x, out_y = b->out_y;
    unsigned int w = b->width, h = b->height;
    pgraph_apply_scaling_factor(pg, &in_x, &in_y);
    pgraph_apply_scaling_factor(pg, &out_x, &out_y);
    pgraph_apply_scaling_factor(pg, &w, &h);

    VkImageCopy region = {
        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .srcSubresource.layerCount = 1,
        .srcOffset = { (int32_t)in_x, (int32_t)in_y, 0 },
        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .dstSubresource.layerCount = 1,
        .dstOffset = { (int32_t)out_x, (int32_t)out_y, 0 },
        .extent = { w, h, 1 },
    };
    vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);

    pgraph_vk_transition_image_layout(pg, cmd, src->image,
                                      src->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, dst->image,
                                      dst->host_fmt.vk_format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    /* The destination is now newer on the GPU than in RAM: an Xbox CPU
     * access reads it back (a download already asked for still runs, after
     * this copy), a texture copies it again. */
    dst->upload_pending = false;
    dst->draw_dirty = true;
    dst->cleared = false;
    pg->draw_time++;
    dst->draw_time = pg->draw_time;
}

void pgraph_vk_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;

    pgraph_vk_surface_update(d, false, true, true);

    assert(context_surfaces->object_instance == image_blit->context_surfaces);

    unsigned int bytes_per_pixel;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_Y8:
        bytes_per_pixel = 1;
        break;
    case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
        bytes_per_pixel = 2;
        break;
    case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
    case NV062_SET_COLOR_FORMAT_LE_Y32:
        bytes_per_pixel = 4;
        break;
    default:
        fprintf(stderr, "Unknown blit surface format: 0x%x\n",
                context_surfaces->color_format);
        assert(!"Unknown blit surface format");
        break;
    }

    hwaddr source_dma_len;
    uint8_t *source = (uint8_t *)nv_dma_map(
        d, context_surfaces->dma_image_source, &source_dma_len);
    assert(context_surfaces->source_offset < source_dma_len);
    source += context_surfaces->source_offset;
    hwaddr source_addr = source - d->vram_ptr;

    hwaddr dest_dma_len;
    uint8_t *dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                          &dest_dma_len);
    assert(context_surfaces->dest_offset < dest_dma_len);
    dest += context_surfaces->dest_offset;
    hwaddr dest_addr = dest - d->vram_ptr;

    SurfaceBinding *surf_src = pgraph_vk_surface_get(d, source_addr);

    /* XPSemu Chihiro: on the GPU when both ends are rendered color surfaces
     * that hold the whole copy (gpu_blit_refusal) */
    SurfaceBinding *surf_dest_at = pgraph_vk_surface_get(d, dest_addr);
    bool gpu_clipped = true;
    if (image_blit->width && image_blit->height) {
        hwaddr out_offset = image_blit->out_y * context_surfaces->dest_pitch +
                            image_blit->out_x * bytes_per_pixel;
        hwaddr out_size =
            (image_blit->height - 1) * context_surfaces->dest_pitch +
            image_blit->width * bytes_per_pixel;
        gpu_clipped = nv_clip_gpu_tile_blit(d, dest_addr + out_offset,
                                            out_size) < out_size;
    }
    const char *gpu_why = gpu_blit_refusal(context_surfaces, image_blit,
                                           surf_src, surf_dest_at, gpu_clipped);

    /* XPSemu Chihiro: what the blit copies (log only, game log "blits") */
    {
        char s_desc[96], d_desc[96];
        pgraph_vk_diag_surface(s_desc, sizeof(s_desc), surf_src);
        pgraph_vk_diag_surface(d_desc, sizeof(d_desc), surf_dest_at);
        pgraph_vk_diag_note(
            &pgraph_vk_diag_blit,
            "%08" PRIx64 " (%s) -> %08" PRIx64 " (%s), %ux%u from %u,%u to "
            "%u,%u, fmt %u, pitch %u -> %u, op %u, %s",
            (uint64_t)source_addr, s_desc, (uint64_t)dest_addr, d_desc,
            image_blit->width, image_blit->height, image_blit->in_x,
            image_blit->in_y, image_blit->out_x, image_blit->out_y,
            context_surfaces->color_format, context_surfaces->source_pitch,
            context_surfaces->dest_pitch, image_blit->operation,
            gpu_why ? gpu_why : "GPU");
    }

    if (!gpu_why) {
        gpu_blit(d, image_blit, surf_src, surf_dest_at);
        return;
    }

    if (surf_src) {
        PGRAPH_VK_DL_CAUSE(PGRAPH_VK_DL_BLIT_SRC,
                           pgraph_vk_surface_download_if_dirty(d, surf_src));
    }

    hwaddr source_offset = image_blit->in_y * context_surfaces->source_pitch +
                           image_blit->in_x * bytes_per_pixel;
    hwaddr dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                         image_blit->out_x * bytes_per_pixel;

    size_t max_row_pixels =
        MIN(context_surfaces->source_pitch, context_surfaces->dest_pitch) /
        bytes_per_pixel;
    size_t row_pixels = MIN(max_row_pixels, image_blit->width);

    hwaddr dest_size = (image_blit->height - 1) * context_surfaces->dest_pitch +
                       image_blit->width * bytes_per_pixel;

    uint8_t *source_row = source + source_offset;
    uint8_t *dest_row = dest + dest_offset;
    size_t row_bytes = row_pixels * bytes_per_pixel;

    size_t adjusted_height = image_blit->height;
    size_t leftover_bytes = 0;

    hwaddr clipped_dest_size =
        nv_clip_gpu_tile_blit(d, dest_addr + dest_offset, dest_size);

    if (clipped_dest_size < dest_size) {
        adjusted_height = clipped_dest_size / context_surfaces->dest_pitch;
        size_t consumed_bytes = adjusted_height * context_surfaces->dest_pitch;

        leftover_bytes = clipped_dest_size - consumed_bytes;
    }

    SurfaceBinding *surf_dest = pgraph_vk_surface_get(d, dest_addr);
    if (surf_dest) {
        if (adjusted_height < surf_dest->height ||
            row_pixels < surf_dest->width) {
            PGRAPH_VK_DL_CAUSE(
                PGRAPH_VK_DL_BLIT_DST,
                pgraph_vk_surface_download_if_dirty(d, surf_dest));
        } else {
            // The blit will completely replace the surface so any pending
            // download should be discarded.
            surf_dest->download_pending = false;
            surf_dest->draw_dirty = false;
        }
        surf_dest->upload_pending = true;
        pg->draw_time++;
    }

    NV2A_DPRINTF("  blit 0x%tx -> 0x%tx (Size: %llu, Clipped Height: %zu)\n",
                 source_addr, dest_addr, dest_size, adjusted_height);

    if (adjusted_height > 0) {
        perform_blit(image_blit->operation, source_row, dest_row, row_pixels,
                     adjusted_height, row_bytes, context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    if (leftover_bytes > 0) {
        uint8_t *src =
            source_row + adjusted_height * context_surfaces->source_pitch;
        uint8_t *dest =
            dest_row + adjusted_height * context_surfaces->dest_pitch;

        perform_blit(image_blit->operation, src, dest,
                     leftover_bytes / bytes_per_pixel, 1, leftover_bytes,
                     context_surfaces->source_pitch,
                     context_surfaces->dest_pitch, beta);
    }

    bool needs_alpha_patching;
    uint8_t alpha_override;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0xff;
        break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0;
        break;
    default:
        needs_alpha_patching = false;
        alpha_override = 0;
    }

    if (needs_alpha_patching) {
        if (adjusted_height > 0) {
            patch_alpha(dest_row, row_pixels, adjusted_height,
                        context_surfaces->dest_pitch, alpha_override);
        }

        if (leftover_bytes > 0) {
            uint8_t *dest =
                dest_row + adjusted_height * context_surfaces->dest_pitch;
            patch_alpha(dest, leftover_bytes / 4, 1, 0, alpha_override);
        }
    }

    dest_addr += dest_offset;
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, dest_addr, clipped_dest_size,
                                   DIRTY_MEMORY_NV2A_TEX);
}
