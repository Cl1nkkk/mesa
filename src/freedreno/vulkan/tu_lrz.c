/*
 * Copyright © 2022 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "tu_lrz.h"

#include "tu_clear_blit.h"
#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_image.h"

/* Low-resolution Z buffer is very similar to a depth prepass that helps
 * the HW avoid executing the fragment shader on those fragments that will
 * be subsequently discarded by the depth test afterwards.
 *
 * The interesting part of this feature is that it allows applications
 * to submit the vertices in any order.
 *
 * In the binning pass it is possible to store the depth value of each
 * vertex into internal low resolution depth buffer and quickly test
 * the primitives against it during the render pass.
 *
 * There are a number of limitations when LRZ cannot be used:
 * - Fragment shader side-effects (writing to SSBOs, atomic operations, etc);
 * - Writing to stencil buffer
 * - Writing depth while:
 *   - Changing direction of depth test (e.g. from OP_GREATER to OP_LESS);
 *   - Using OP_ALWAYS or OP_NOT_EQUAL;
 * - Clearing depth with vkCmdClearAttachments;
 * - (pre-a650) Not clearing depth attachment with LOAD_OP_CLEAR;
 * - (pre-a650) Using secondary command buffers;
 * - Sysmem rendering (with small caveat).
 *
 * Pre-a650 (before gen3)
 * ======================
 *
 * The direction is fully tracked on CPU. In renderpass LRZ starts with
 * unknown direction, the direction is set first time when depth write occurs
 * and if it does change afterwards - direction becomes invalid and LRZ is
 * disabled for the rest of the renderpass.
 *
 * Since direction is not tracked by GPU - it's impossible to know whether
 * LRZ is enabled during construction of secondary command buffers.
 *
 * For the same reason it's impossible to reuse LRZ between renderpasses.
 *
 * A650+ (gen3+)
 * =============
 *
 * Now LRZ direction could be tracked on GPU. There are to parts:
 * - Direction byte which stores current LRZ direction;
 * - Parameters of the last used depth view.
 *
 * The idea is the same as when LRZ tracked on CPU: when GRAS_LRZ_CNTL
 * is used - its direction is compared to previously known direction
 * and direction byte is set to disabled when directions are incompatible.
 *
 * Additionally, to reuse LRZ between renderpasses, GRAS_LRZ_CNTL checks
 * if current value of GRAS_LRZ_DEPTH_VIEW is equal to the value
 * stored in the buffer, if not - LRZ is disabled. (This is necessary
 * because depth buffer may have several layers and mip levels, on the
 * other hand LRZ buffer represents only a single layer + mip level).
 *
 * LRZ direction between renderpasses is disabled when underlying depth
 * buffer is changed, the following commands could change depth image:
 * - vkCmdBlitImage*
 * - vkCmdCopyBufferToImage*
 * - vkCmdCopyImage*
 *
 * LRZ Fast-Clear
 * ==============
 *
 * The LRZ fast-clear buffer is initialized to zeroes and read/written
 * when GRAS_LRZ_CNTL.FC_ENABLE (b3) is set. It appears to store 1b/block.
 * '0' means block has original depth clear value, and '1' means that the
 * corresponding block in LRZ has been modified.
 *
 * LRZ fast-clear conservatively clears LRZ buffer, at the point where LRZ is
 * written the LRZ block which corresponds to a single fast-clear bit is cleared:
 * - To 0.0 if depth comparison is GREATER;
 * - To 1.0 if depth comparison is LESS;
 *
 * This way it's always valid to fast-clear. On the other hand we disable
 * fast-clear if depth clear value is not 0.0 or 1.0 because it may be worse
 * for perf if some primitives are expected to fail depth test against the
 * actual depth clear value.
 *
 * LRZ Precision
 * =============
 *
 * LRZ always uses Z16_UNORM. The epsilon for it is 1.f / (1 << 16) which is
 * not enough to represent all values of Z32_UNORM or Z32_FLOAT.
 * This especially rises questions in context of fast-clear, if fast-clear
 * uses a value which cannot be precisely represented by LRZ - we wouldn't
 * be able to round it in the correct direction since direction is tracked
 * on GPU.
 *
 * However, it seems that depth comparisons with LRZ values have some "slack"
 * and nothing special should be done for such depth clear values.
 *
 * How it was tested:
 * - Clear Z32_FLOAT attachment to 1.f / (1 << 17)
 *   - LRZ buffer contains all zeroes
 * - Do draws and check whether all samples are passing:
 *   - OP_GREATER with (1.f / (1 << 17) + float32_epsilon) - passing;
 *   - OP_GREATER with (1.f / (1 << 17) - float32_epsilon) - not passing;
 *   - OP_LESS with (1.f / (1 << 17) - float32_epsilon) - samples;
 *   - OP_LESS with() 1.f / (1 << 17) + float32_epsilon) - not passing;
 *   - OP_LESS_OR_EQ with (1.f / (1 << 17) + float32_epsilon) - not passing;
 * In all cases resulting LRZ buffer is all zeroes and LRZ direction is updated.
 *
 * LRZ Caches
 * ==========
 *
 * ! The policy here is to flush LRZ cache right after it is changed,
 * so if LRZ data is needed afterwards - there is no need to flush it
 * before using LRZ.
 *
 * LRZ_FLUSH flushes and invalidates LRZ caches, there are two caches:
 * - Cache for fast-clear buffer;
 * - Cache for direction byte + depth view params.
 * They could be cleared by LRZ_CLEAR. To become visible in GPU memory
 * the caches should be flushed with LRZ_FLUSH afterwards.
 *
 * GRAS_LRZ_CNTL reads from these caches.
 */

static void
tu6_emit_lrz_buffer(struct tu_cs *cs, struct tu_image *depth_image)
{
   if (!depth_image) {
      tu_cs_emit_regs(cs,
                      A6XX_GRAS_LRZ_BUFFER_BASE(0),
                      A6XX_GRAS_LRZ_BUFFER_PITCH(0),
                      A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE(0));
      return;
   }

   uint64_t lrz_iova = depth_image->iova + depth_image->lrz_offset;
   uint64_t lrz_fc_iova = depth_image->iova + depth_image->lrz_fc_offset;
   if (!depth_image->lrz_fc_offset)
      lrz_fc_iova = 0;

   tu_cs_emit_regs(cs,
                   A6XX_GRAS_LRZ_BUFFER_BASE(.qword = lrz_iova),
                   A6XX_GRAS_LRZ_BUFFER_PITCH(.pitch = depth_image->lrz_pitch),
                   A6XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE(.qword = lrz_fc_iova));
}

static void
tu6_write_lrz_reg(struct tu_cmd_buffer *cmd, struct tu_cs *cs,
                  struct tu_reg_value reg)
{
   if (cmd->device->physical_device->info->a6xx.lrz_track_quirk) {
      tu_cs_emit_pkt7(cs, CP_REG_WRITE, 3);
      tu_cs_emit(cs, CP_REG_WRITE_0_TRACKER(TRACK_LRZ));
      tu_cs_emit(cs, reg.reg);
      tu_cs_emit(cs, reg.value);
   } else {
      tu_cs_emit_pkt4(cs, reg.reg, 1);
      tu_cs_emit(cs, reg.value);
   }
}

static void
tu6_disable_lrz_via_depth_view(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   /* Disable direction by writing invalid depth view. */
   tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_DEPTH_VIEW(
      .base_layer = 0b11111111111,
      .layer_count = 0b11111111111,
      .base_mip_level = 0b1111,
   ));

   tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_CNTL(
      .enable = true,
      .disable_on_wrong_dir = true,
   ));

   tu6_emit_event_write(cmd, cs, LRZ_CLEAR);
   tu6_emit_event_write(cmd, cs, LRZ_FLUSH);
}

static void
tu_lrz_init_state(struct tu_cmd_buffer *cmd,
                  const struct tu_render_pass_attachment *att,
                  const struct tu_image_view *view)
{
   if (!view->image->lrz_height) {
      assert((cmd->device->instance->debug_flags & TU_DEBUG_NOLRZ) ||
             !vk_format_has_depth(att->format));
      return;
   }

   bool clears_depth = att->clear_mask &
      (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT);
   bool has_gpu_tracking =
      cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking;

   if (!has_gpu_tracking && !clears_depth)
      return;

   /* We need to always have an LRZ view just to disable it if there is a
    * depth attachment, there are any secondaries, and GPU tracking is
    * enabled, in order not to rely on loadOp state which doesn't exist with
    * dynamic rendering in secondaries. Otherwise the secondary will have LRZ
    * enabled and there will be a NULL/garbage LRZ buffer.
    */
   cmd->state.lrz.image_view = view;

   if (!clears_depth && !att->load)
      return;

   cmd->state.lrz.valid = true;
   cmd->state.lrz.prev_direction = TU_LRZ_UNKNOWN;
   /* Be optimistic and unconditionally enable fast-clear in
    * secondary cmdbufs and when reusing previous LRZ state.
    */
   cmd->state.lrz.fast_clear = view->image->lrz_fc_size > 0;

   cmd->state.lrz.gpu_dir_tracking = has_gpu_tracking;
   cmd->state.lrz.reuse_previous_state = !clears_depth;
}

/* Note: if we enable LRZ here, then tu_lrz_init_state() must at least set
 * lrz.image_view, so that an LRZ buffer is present (even if LRZ is
 * dynamically disabled).
 */

static void
tu_lrz_init_secondary(struct tu_cmd_buffer *cmd,
                      const struct tu_render_pass_attachment *att)
{
   bool has_gpu_tracking =
      cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking;

   if (!has_gpu_tracking)
      return;

   if (cmd->device->instance->debug_flags & TU_DEBUG_NOLRZ)
      return;

   if (!vk_format_has_depth(att->format))
      return;

   cmd->state.lrz.valid = true;
   cmd->state.lrz.prev_direction = TU_LRZ_UNKNOWN;
   cmd->state.lrz.gpu_dir_tracking = has_gpu_tracking;

   /* We may not have the depth attachment when executing in a secondary
    * inside a render pass. This means we have to be even more optimistic than
    * the normal case and enable fast clear even if the depth image doesn't
    * support it.
    */
   cmd->state.lrz.fast_clear = true;

   /* These are not used inside secondaries */
   cmd->state.lrz.image_view = NULL;
   cmd->state.lrz.reuse_previous_state = false;
}

/* This is generally the same as tu_lrz_begin_renderpass(), but we skip
 * actually emitting anything. The lrz state needs to be consistent between
 * renderpasses, but only the first should actually emit commands to disable
 * lrz etc.
 */
void
tu_lrz_begin_resumed_renderpass(struct tu_cmd_buffer *cmd,
                                const VkClearValue *clear_values)
{
    /* Track LRZ valid state */
   memset(&cmd->state.lrz, 0, sizeof(cmd->state.lrz));

   uint32_t a;
   for (a = 0; a < cmd->state.pass->attachment_count; a++) {
      if (cmd->state.attachments[a]->image->lrz_height)
         break;
   }

   if (a != cmd->state.pass->attachment_count) {
      const struct tu_render_pass_attachment *att = &cmd->state.pass->attachments[a];
      tu_lrz_init_state(cmd, att, cmd->state.attachments[a]);
      if (att->clear_mask & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT)) {
         VkClearValue clear = clear_values[a];
         cmd->state.lrz.depth_clear_value = clear;
         cmd->state.lrz.fast_clear = cmd->state.lrz.fast_clear &&
                                     (clear.depthStencil.depth == 0.f ||
                                      clear.depthStencil.depth == 1.f);
      }
      cmd->state.dirty |= TU_CMD_DIRTY_LRZ;
   }
}

void
tu_lrz_begin_renderpass(struct tu_cmd_buffer *cmd,
                        const VkClearValue *clear_values)
{
   const struct tu_render_pass *pass = cmd->state.pass;

   int lrz_img_count = 0;
   for (unsigned i = 0; i < pass->attachment_count; i++) {
      if (cmd->state.attachments[i]->image->lrz_height)
         lrz_img_count++;
   }

   if (cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking &&
       cmd->state.pass->subpass_count > 1 && lrz_img_count > 1) {
      /* Theoretically we could switch between LRZ buffers during the binning
       * and tiling passes, but it is untested and would add complexity for
       * presumably extremely rare case.
       */
      perf_debug(cmd->device,
                 "Invalidating LRZ because there are several subpasses with "
                 "different depth attachments in a single renderpass");

      for (unsigned i = 0; i < pass->attachment_count; i++) {
         struct tu_image *image = cmd->state.attachments[i]->image;
         tu_disable_lrz(cmd, &cmd->cs, image);
      }

      /* We need a valid LRZ fast-clear base, in case the render pass contents
       * are in secondaries that enable LRZ, so that they can read that LRZ is
       * dynamically disabled. It doesn't matter which we use, so just leave
       * the last one as emitted in tu_disable_lrz().
       */
      memset(&cmd->state.lrz, 0, sizeof(cmd->state.lrz));
      return;
   }

    /* Track LRZ valid state */
   tu_lrz_begin_resumed_renderpass(cmd, clear_values);

   if (!cmd->state.lrz.valid) {
      tu6_emit_lrz_buffer(&cmd->cs, NULL);
   }
}

void
tu_lrz_begin_secondary_cmdbuf(struct tu_cmd_buffer *cmd)
{
   memset(&cmd->state.lrz, 0, sizeof(cmd->state.lrz));
   uint32_t a = cmd->state.subpass->depth_stencil_attachment.attachment;
   if (a != VK_ATTACHMENT_UNUSED) {
      const struct tu_render_pass_attachment *att = &cmd->state.pass->attachments[a];
      tu_lrz_init_secondary(cmd, att);
   }
}

void
tu_lrz_tiling_begin(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   /* TODO: If lrz was never valid for the entire renderpass, we could exit
    * early here. Sometimes we know this ahead of time and null out
    * image_view, but with LOAD_OP_DONT_CARE this only happens if there were
    * no secondaries.
    */
   if (!cmd->state.lrz.image_view)
      return;

   struct tu_lrz_state *lrz = &cmd->state.lrz;

   tu6_emit_lrz_buffer(cs, lrz->image_view->image);

   if (lrz->reuse_previous_state) {
      /* Reuse previous LRZ state, LRZ cache is assumed to be
       * already invalidated by previous renderpass.
       */
      assert(lrz->gpu_dir_tracking);

      tu6_write_lrz_reg(cmd, cs,
         A6XX_GRAS_LRZ_DEPTH_VIEW(.dword = lrz->image_view->view.GRAS_LRZ_DEPTH_VIEW));
      return;
   }

   bool invalidate_lrz = !lrz->valid && lrz->gpu_dir_tracking;
   if (invalidate_lrz) {
      /* Following the blob we elect to disable LRZ for the whole renderpass
       * if it is known that LRZ is disabled somewhere in the renderpass.
       *
       * This is accomplished by making later GRAS_LRZ_CNTL (in binning pass)
       * to fail the comparison of depth views.
       */
      tu6_disable_lrz_via_depth_view(cmd, cs);
      tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_DEPTH_VIEW(.dword = 0));
   } else if (lrz->fast_clear || lrz->gpu_dir_tracking) {
      if (lrz->gpu_dir_tracking) {
         tu6_write_lrz_reg(cmd, cs,
            A6XX_GRAS_LRZ_DEPTH_VIEW(.dword = lrz->image_view->view.GRAS_LRZ_DEPTH_VIEW));
      }

      tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_CNTL(
         .enable = true,
         .fc_enable = lrz->fast_clear,
         .disable_on_wrong_dir = lrz->gpu_dir_tracking,
      ));

      /* LRZ_CLEAR.fc_enable + LRZ_CLEAR - clears fast-clear buffer;
       * LRZ_CLEAR.disable_on_wrong_dir + LRZ_CLEAR - sets direction to
       *  CUR_DIR_UNSET.
       */
      tu6_emit_event_write(cmd, cs, LRZ_CLEAR);
   }

   if (!lrz->fast_clear && !invalidate_lrz) {
      tu6_clear_lrz(cmd, cs, lrz->image_view->image, &lrz->depth_clear_value);

      /* Even though we disable fast-clear we still have to dirty
       * fast-clear buffer because both secondary cmdbufs and following
       * renderpasses won't know that fast-clear is disabled.
       *
       * TODO: we could avoid this if we don't store depth and don't
       * expect secondary cmdbufs.
       */
      if (lrz->image_view->image->lrz_fc_size) {
         tu6_dirty_lrz_fc(cmd, cs, lrz->image_view->image);
      }
   }
}

void
tu_lrz_tiling_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   if (cmd->state.lrz.fast_clear || cmd->state.lrz.gpu_dir_tracking) {
      tu6_emit_lrz_buffer(cs, cmd->state.lrz.image_view->image);

      if (cmd->state.lrz.gpu_dir_tracking) {
         tu6_write_lrz_reg(cmd, &cmd->cs,
            A6XX_GRAS_LRZ_DEPTH_VIEW(.dword = cmd->state.lrz.image_view->view.GRAS_LRZ_DEPTH_VIEW));
      }

      /* Enable flushing of LRZ fast-clear and of direction buffer */
      tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_CNTL(
         .enable = true,
         .fc_enable = cmd->state.lrz.fast_clear,
         .disable_on_wrong_dir = cmd->state.lrz.gpu_dir_tracking,
      ));
   } else {
      tu6_write_lrz_reg(cmd, cs, A6XX_GRAS_LRZ_CNTL(0));
   }

   tu6_emit_event_write(cmd, cs, LRZ_FLUSH);

   /* If gpu_dir_tracking is enabled and lrz is not valid blob, at this point,
    * additionally clears direction buffer:
    *  GRAS_LRZ_DEPTH_VIEW(.dword = 0)
    *  GRAS_LRZ_DEPTH_VIEW(.dword = 0xffffffff)
    *  A6XX_GRAS_LRZ_CNTL(.enable = true, .disable_on_wrong_dir = true)
    *  LRZ_CLEAR
    *  LRZ_FLUSH
    * Since it happens after all of the rendering is done there is no known
    * reason to do such clear.
    */
}

void
tu_lrz_sysmem_begin(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   if (!cmd->state.lrz.image_view)
      return;

   /* Actually, LRZ buffer could be filled in sysmem, in theory to
    * be used in another renderpass, but the benefit is rather dubious.
    */

   struct tu_lrz_state *lrz = &cmd->state.lrz;

   if (cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking) {
      tu_disable_lrz(cmd, cs, lrz->image_view->image);
      /* Make sure depth view comparison will fail. */
      tu6_write_lrz_reg(cmd, cs,
         A6XX_GRAS_LRZ_DEPTH_VIEW(.dword = 0));
   } else {
      tu6_emit_lrz_buffer(cs, lrz->image_view->image);
      /* Even though we disable LRZ writes in sysmem mode - there is still
       * LRZ test, so LRZ should be cleared.
       */
      if (lrz->fast_clear) {
         tu6_write_lrz_reg(cmd, &cmd->cs, A6XX_GRAS_LRZ_CNTL(
            .enable = true,
            .fc_enable = true,
         ));
         tu6_emit_event_write(cmd, &cmd->cs, LRZ_CLEAR);
         tu6_emit_event_write(cmd, &cmd->cs, LRZ_FLUSH);
      } else {
         tu6_clear_lrz(cmd, cs, lrz->image_view->image, &lrz->depth_clear_value);
      }
   }
}

void
tu_lrz_sysmem_end(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   tu6_emit_event_write(cmd, &cmd->cs, LRZ_FLUSH);
}

/* Disable LRZ outside of renderpass. */
void
tu_disable_lrz(struct tu_cmd_buffer *cmd, struct tu_cs *cs,
               struct tu_image *image)
{
   if (!cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking)
      return;

   if (!image->lrz_height)
      return;

   tu6_emit_lrz_buffer(cs, image);
   tu6_disable_lrz_via_depth_view(cmd, cs);
}

/* Clear LRZ, used for out of renderpass depth clears. */
void
tu_lrz_clear_depth_image(struct tu_cmd_buffer *cmd,
                         struct tu_image *image,
                         const VkClearDepthStencilValue *pDepthStencil,
                         uint32_t rangeCount,
                         const VkImageSubresourceRange *pRanges)
{
   if (!rangeCount || !image->lrz_height ||
       !cmd->device->physical_device->info->a6xx.has_lrz_dir_tracking)
      return;

   /* We cannot predict which depth subresource would be used later on,
    * so we just pick the first one with depth cleared and clear the LRZ.
    */
   const VkImageSubresourceRange *range = NULL;
   for (unsigned i = 0; i < rangeCount; i++) {
      if (pRanges[i].aspectMask &
            (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT)) {
         range = &pRanges[i];
         break;
      }
   }

   if (!range)
      return;

   bool fast_clear = image->lrz_fc_size && (pDepthStencil->depth == 0.f ||
                                            pDepthStencil->depth == 1.f);

   tu6_emit_lrz_buffer(&cmd->cs, image);

   tu6_write_lrz_reg(cmd, &cmd->cs, A6XX_GRAS_LRZ_DEPTH_VIEW(
         .base_layer = range->baseArrayLayer,
         .layer_count = vk_image_subresource_layer_count(&image->vk, range),
         .base_mip_level = range->baseMipLevel,
   ));

   tu6_write_lrz_reg(cmd, &cmd->cs, A6XX_GRAS_LRZ_CNTL(
      .enable = true,
      .fc_enable = fast_clear,
      .disable_on_wrong_dir = true,
   ));

   tu6_emit_event_write(cmd, &cmd->cs, LRZ_CLEAR);
   tu6_emit_event_write(cmd, &cmd->cs, LRZ_FLUSH);

   if (!fast_clear) {
      tu6_clear_lrz(cmd, &cmd->cs, image, (const VkClearValue*) pDepthStencil);
   }
}

void
tu_lrz_disable_during_renderpass(struct tu_cmd_buffer *cmd)
{
   assert(cmd->state.pass);

   cmd->state.lrz.valid = false;
   cmd->state.dirty |= TU_CMD_DIRTY_LRZ;

   if (cmd->state.lrz.gpu_dir_tracking) {
      tu6_write_lrz_reg(cmd, &cmd->cs, A6XX_GRAS_LRZ_CNTL(
         .enable = true,
         .dir = LRZ_DIR_INVALID,
         .disable_on_wrong_dir = true,
      ));
   }
}

/* update lrz state based on stencil-test func:
 *
 * Conceptually the order of the pipeline is:
 *
 *
 *   FS -> Alpha-Test  ->  Stencil-Test  ->  Depth-Test
 *                              |                |
 *                       if wrmask != 0     if wrmask != 0
 *                              |                |
 *                              v                v
 *                        Stencil-Write      Depth-Write
 *
 * Because Stencil-Test can have side effects (Stencil-Write) prior
 * to depth test, in this case we potentially need to disable early
 * lrz-test. See:
 *
 * https://www.khronos.org/opengl/wiki/Per-Sample_Processing
 */
static bool
tu6_stencil_op_lrz_allowed(struct A6XX_GRAS_LRZ_CNTL *gras_lrz_cntl,
                           VkCompareOp func,
                           bool stencil_write)
{
   switch (func) {
   case VK_COMPARE_OP_ALWAYS:
      /* nothing to do for LRZ, but for stencil test when stencil-
       * write is enabled, we need to disable lrz-test, since
       * conceptually stencil test and write happens before depth-test.
       */
      if (stencil_write) {
         return false;
      }
      break;
   case VK_COMPARE_OP_NEVER:
      /* fragment never passes, disable lrz_write for this draw. */
      gras_lrz_cntl->lrz_write = false;
      break;
   default:
      /* whether the fragment passes or not depends on result
       * of stencil test, which we cannot know when doing binning
       * pass.
       */
      gras_lrz_cntl->lrz_write = false;
      /* similarly to the VK_COMPARE_OP_ALWAYS case, if there are side-
       * effects from stencil test we need to disable lrz-test.
       */
      if (stencil_write) {
         return false;
      }
      break;
   }

   return true;
}

static struct A6XX_GRAS_LRZ_CNTL
tu6_calculate_lrz_state(struct tu_cmd_buffer *cmd,
                        const uint32_t a)
{
   struct tu_pipeline *pipeline = cmd->state.pipeline;
   bool z_test_enable = cmd->state.rb_depth_cntl & A6XX_RB_DEPTH_CNTL_Z_TEST_ENABLE;
   bool z_write_enable = cmd->state.rb_depth_cntl & A6XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE;
   bool z_bounds_enable = cmd->state.rb_depth_cntl & A6XX_RB_DEPTH_CNTL_Z_BOUNDS_ENABLE;
   VkCompareOp depth_compare_op = (cmd->state.rb_depth_cntl & A6XX_RB_DEPTH_CNTL_ZFUNC__MASK) >> A6XX_RB_DEPTH_CNTL_ZFUNC__SHIFT;

   struct A6XX_GRAS_LRZ_CNTL gras_lrz_cntl = { 0 };

   if (!cmd->state.lrz.valid) {
      return gras_lrz_cntl;
   }

   /* If depth test is disabled we shouldn't touch LRZ.
    * Same if there is no depth attachment.
    */
   if (a == VK_ATTACHMENT_UNUSED || !z_test_enable ||
       (cmd->device->instance->debug_flags & TU_DEBUG_NOLRZ))
      return gras_lrz_cntl;

   if (!cmd->state.lrz.gpu_dir_tracking && !cmd->state.attachments) {
      /* Without on-gpu LRZ direction tracking - there is nothing we
       * can do to enable LRZ in secondary command buffers.
       */
      return gras_lrz_cntl;
   }

   gras_lrz_cntl.enable = true;
   gras_lrz_cntl.lrz_write =
      z_write_enable &&
      !(pipeline->lrz.force_disable_mask & TU_LRZ_FORCE_DISABLE_WRITE);
   gras_lrz_cntl.z_test_enable = z_write_enable;
   gras_lrz_cntl.z_bounds_enable = z_bounds_enable;
   gras_lrz_cntl.fc_enable = cmd->state.lrz.fast_clear;
   gras_lrz_cntl.dir_write = cmd->state.lrz.gpu_dir_tracking;
   gras_lrz_cntl.disable_on_wrong_dir = cmd->state.lrz.gpu_dir_tracking;


   /* See comment in tu_pipeline about disabling LRZ write for blending. */
   if ((cmd->state.pipeline->dynamic_state_mask & BIT(TU_DYNAMIC_STATE_LOGIC_OP)) &&
       cmd->state.logic_op_enabled && cmd->state.rop_reads_dst) {
      if (gras_lrz_cntl.lrz_write)
         perf_debug(cmd->device, "disabling lrz write due to dynamic logic op");
      gras_lrz_cntl.lrz_write = false;
   }

   if ((cmd->state.pipeline->dynamic_state_mask & BIT(TU_DYNAMIC_STATE_BLEND_ENABLE)) &&
       cmd->state.blend_enable) {
      if (gras_lrz_cntl.lrz_write)
         perf_debug(cmd->device, "disabling lrz write due to dynamic blend");
      gras_lrz_cntl.lrz_write = false;
   }

   if ((cmd->state.pipeline->dynamic_state_mask & BIT(TU_DYNAMIC_STATE_BLEND))) {
      for (unsigned i = 0; i < cmd->state.subpass->color_count; i++) {
         unsigned a = cmd->state.subpass->color_attachments[i].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         VkFormat format = cmd->state.pass->attachments[a].format;
         unsigned mask = MASK(vk_format_get_nr_components(format));
         if ((cmd->state.rb_mrt_control[i] &
              A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK) >>
             A6XX_RB_MRT_CONTROL_COMPONENT_ENABLE__SHIFT != mask) {
            if (gras_lrz_cntl.lrz_write)
               perf_debug(cmd->device, "disabling lrz write due to dynamic color write mask");
            gras_lrz_cntl.lrz_write = false;
            break;
         }
      }
   }

   if ((cmd->state.pipeline->dynamic_state_mask &
        BIT(TU_DYNAMIC_STATE_COLOR_WRITE_ENABLE)) &&
       (cmd->state.color_write_enable &
        MASK(cmd->state.subpass->color_count)) !=
          MASK(cmd->state.pipeline->blend.num_rts)) {
      if (gras_lrz_cntl.lrz_write) {
         perf_debug(
            cmd->device,
            "disabling lrz write due to dynamic color write enables (%x/%x)",
            cmd->state.color_write_enable,
            MASK(cmd->state.pipeline->blend.num_rts));
      }
      gras_lrz_cntl.lrz_write = false;
   }

   /* LRZ is disabled until it is cleared, which means that one "wrong"
    * depth test or shader could disable LRZ until depth buffer is cleared.
    */
   bool disable_lrz = false;
   bool temporary_disable_lrz = false;

   /* What happens in FS could affect LRZ, e.g.: writes to gl_FragDepth or early
    * fragment tests.  We have to skip LRZ testing and updating, but as long as
    * the depth direction stayed the same we can continue with LRZ testing later.
    */
   if (pipeline->lrz.force_disable_mask & TU_LRZ_FORCE_DISABLE_LRZ) {
      if (cmd->state.lrz.prev_direction != TU_LRZ_UNKNOWN || !cmd->state.lrz.gpu_dir_tracking) {
         perf_debug(cmd->device, "Skipping LRZ due to FS");
         temporary_disable_lrz = true;
      } else {
         perf_debug(cmd->device, "Disabling LRZ due to FS (TODO: fix for gpu-direction-tracking case");
         disable_lrz = true;
      }
   }

   /* If Z is not written - it doesn't affect LRZ buffer state.
    * Which means two things:
    * - Don't lock direction until Z is written for the first time;
    * - If Z isn't written and direction IS locked it's possible to just
    *   temporary disable LRZ instead of fully bailing out, when direction
    *   is changed.
    */

   enum tu_lrz_direction lrz_direction = TU_LRZ_UNKNOWN;
   switch (depth_compare_op) {
   case VK_COMPARE_OP_ALWAYS:
   case VK_COMPARE_OP_NOT_EQUAL:
      /* OP_ALWAYS and OP_NOT_EQUAL could have depth value of any direction,
       * so if there is a depth write - LRZ must be disabled.
       */
      if (z_write_enable) {
         perf_debug(cmd->device, "Invalidating LRZ due to ALWAYS/NOT_EQUAL");
         disable_lrz = true;
         gras_lrz_cntl.dir = LRZ_DIR_INVALID;
      } else {
         perf_debug(cmd->device, "Skipping LRZ due to ALWAYS/NOT_EQUAL");
         temporary_disable_lrz = true;
      }
      break;
   case VK_COMPARE_OP_EQUAL:
   case VK_COMPARE_OP_NEVER:
      /* Blob disables LRZ for OP_EQUAL, and from our empirical
       * evidence it is a right thing to do.
       *
       * Both OP_EQUAL and OP_NEVER don't change LRZ buffer so
       * we could just temporary disable LRZ.
       */
      temporary_disable_lrz = true;
      break;
   case VK_COMPARE_OP_GREATER:
   case VK_COMPARE_OP_GREATER_OR_EQUAL:
      lrz_direction = TU_LRZ_GREATER;
      gras_lrz_cntl.greater = true;
      gras_lrz_cntl.dir = LRZ_DIR_GE;
      break;
   case VK_COMPARE_OP_LESS:
   case VK_COMPARE_OP_LESS_OR_EQUAL:
      lrz_direction = TU_LRZ_LESS;
      gras_lrz_cntl.greater = false;
      gras_lrz_cntl.dir = LRZ_DIR_LE;
      break;
   default:
      unreachable("bad VK_COMPARE_OP value or uninitialized");
      break;
   };

   /* If depthfunc direction is changed, bail out on using LRZ. The
    * LRZ buffer encodes a min/max depth value per block, but if
    * we switch from GT/GE <-> LT/LE, those values cannot be
    * interpreted properly.
    */
   if (cmd->state.lrz.prev_direction != TU_LRZ_UNKNOWN &&
       lrz_direction != TU_LRZ_UNKNOWN &&
       cmd->state.lrz.prev_direction != lrz_direction) {
      if (z_write_enable) {
         perf_debug(cmd->device, "Invalidating LRZ due to direction change");
         disable_lrz = true;
      } else {
         perf_debug(cmd->device, "Skipping LRZ due to direction change");
         temporary_disable_lrz = true;
      }
   }

   /* Consider the following sequence of depthfunc changes:
    *
    * - COMPARE_OP_GREATER -> COMPARE_OP_EQUAL -> COMPARE_OP_GREATER
    * LRZ is disabled during COMPARE_OP_EQUAL but could be enabled
    * during second VK_COMPARE_OP_GREATER.
    *
    * - COMPARE_OP_GREATER -> COMPARE_OP_EQUAL -> COMPARE_OP_LESS
    * Here, LRZ is disabled during COMPARE_OP_EQUAL and should become
    * invalid during COMPARE_OP_LESS.
    *
    * This shows that we should keep last KNOWN direction.
    */
   if (z_write_enable && lrz_direction != TU_LRZ_UNKNOWN)
      cmd->state.lrz.prev_direction = lrz_direction;

   /* Invalidate LRZ and disable write if stencil test is enabled */
   bool stencil_test_enable = cmd->state.rb_stencil_cntl & A6XX_RB_STENCIL_CONTROL_STENCIL_ENABLE;
   if (!disable_lrz && stencil_test_enable) {
      VkCompareOp stencil_front_compare_op =
         (cmd->state.rb_stencil_cntl & A6XX_RB_STENCIL_CONTROL_FUNC__MASK) >> A6XX_RB_STENCIL_CONTROL_FUNC__SHIFT;

      VkCompareOp stencil_back_compare_op =
         (cmd->state.rb_stencil_cntl & A6XX_RB_STENCIL_CONTROL_FUNC_BF__MASK) >> A6XX_RB_STENCIL_CONTROL_FUNC_BF__SHIFT;

      bool lrz_allowed = true;
      lrz_allowed = lrz_allowed && tu6_stencil_op_lrz_allowed(
                                      &gras_lrz_cntl, stencil_front_compare_op,
                                      cmd->state.stencil_front_write);

      lrz_allowed = lrz_allowed && tu6_stencil_op_lrz_allowed(
                                      &gras_lrz_cntl, stencil_back_compare_op,
                                      cmd->state.stencil_back_write);

      /* Without depth write it's enough to make sure that depth test
       * is executed after stencil test, so temporary disabling LRZ is enough.
       */
      if (!lrz_allowed) {
         if (z_write_enable) {
            perf_debug(cmd->device, "Invalidating LRZ due to stencil write");
            disable_lrz = true;
         } else {
            perf_debug(cmd->device, "Skipping LRZ due to stencil write");
            temporary_disable_lrz = true;
         }
      }
   }

   if (disable_lrz)
      cmd->state.lrz.valid = false;

   if (disable_lrz && cmd->state.lrz.gpu_dir_tracking) {
      /* Direction byte on GPU should be set to CUR_DIR_DISABLED,
       * for this it's not enough to emit empty GRAS_LRZ_CNTL.
       */
      gras_lrz_cntl.enable = true;
      gras_lrz_cntl.dir = LRZ_DIR_INVALID;

      return gras_lrz_cntl;
   }

   if (temporary_disable_lrz)
      gras_lrz_cntl.enable = false;

   cmd->state.lrz.enabled = cmd->state.lrz.valid && gras_lrz_cntl.enable;
   if (!cmd->state.lrz.enabled)
      memset(&gras_lrz_cntl, 0, sizeof(gras_lrz_cntl));

   return gras_lrz_cntl;
}

void
tu6_emit_lrz(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const uint32_t a = cmd->state.subpass->depth_stencil_attachment.attachment;
   struct A6XX_GRAS_LRZ_CNTL gras_lrz_cntl = tu6_calculate_lrz_state(cmd, a);

   tu6_write_lrz_reg(cmd, cs, pack_A6XX_GRAS_LRZ_CNTL(gras_lrz_cntl));
   tu_cs_emit_regs(cs, A6XX_RB_LRZ_CNTL(.enable = gras_lrz_cntl.enable));
}
