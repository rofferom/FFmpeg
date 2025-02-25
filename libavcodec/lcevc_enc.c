/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define USE_VULKAN 1

#include "avcodec.h"
#include "encode.h"
#include "codec_internal.h"
#include "libavutil/pixdesc.h"

#if USE_VULKAN
#include "hwconfig.h"
#include "vulkan.h"
#include "libavutil/mem.h"
#endif

#include "lcevc_eil.h"

typedef struct LCEVCENCCtx {
    AVClass *class;
    EILContext lc;
    AVBufferPool *tmp_pool;

#if USE_VULKAN
    FFVulkanContext s;
    FFVkQueueFamilyCtx qf;
    FFVkExecPool exec_pool;
#endif
} LCEVCENCCtx;

static av_cold int lcevc_encode_close(AVCodecContext *avctx)
{
    LCEVCENCCtx *ctx = avctx->priv_data;

    if (ctx->lc)
        EIL_Close(ctx->lc);

#if USE_VULKAN
    ff_vk_exec_pool_free(&ctx->s, &ctx->exec_pool);
    ff_vk_uninit(&ctx->s);
#endif

    return 0;
}

static const EILColourFormat fmt_map[] = {
    [AV_PIX_FMT_YUV420P] = EIL_YUV_420P,
    [AV_PIX_FMT_YUV420P10] = EIL_YUV_420P10,
    [AV_PIX_FMT_YUV420P12] = EIL_YUV_420P12,
    [AV_PIX_FMT_YUV420P14] = EIL_YUV_420P14,
    [AV_PIX_FMT_YUV422P] = EIL_YUV_422P,
    [AV_PIX_FMT_YUV422P10] = EIL_YUV_422P10,
    [AV_PIX_FMT_YUV422P12] = EIL_YUV_422P12,
    [AV_PIX_FMT_YUV422P14] = EIL_YUV_422P14,
    [AV_PIX_FMT_YUV444P] = EIL_YUV_444P,
    [AV_PIX_FMT_YUV444P10] = EIL_YUV_444P10,
    [AV_PIX_FMT_YUV444P12] = EIL_YUV_444P12,
    [AV_PIX_FMT_YUV444P14] = EIL_YUV_444P14,
    [AV_PIX_FMT_RGB8] = EIL_RGB_24,
    [AV_PIX_FMT_BGR8] = EIL_BGR_24,
    [AV_PIX_FMT_RGBA] = EIL_RGBA_32,
    [AV_PIX_FMT_BGRA] = EIL_BGRA_32,
    [AV_PIX_FMT_ARGB] = EIL_ARGB_32,
    [AV_PIX_FMT_ABGR] = EIL_ABGR_32,
};

static void log_cb(void *opaque, int32_t level, const char *msg)
{
    static const int averrc[] = {
        [EIL_LL_Error] = AV_LOG_ERROR,
        [EIL_LL_Warning] = AV_LOG_WARNING,
        [EIL_LL_Info] = AV_LOG_INFO,
        [EIL_LL_Verbose] = AV_LOG_VERBOSE,
        [EIL_LL_Debug] = AV_LOG_DEBUG,
    };
    av_log(opaque, averrc[level], "%s", msg);
}

static av_cold int lcevc_encode_init(AVCodecContext *avctx)
{
    EILReturnCode ret;
    LCEVCENCCtx *ctx = avctx->priv_data;

    EILColourFormat color_format = fmt_map[avctx->pix_fmt];
    EILOpenSettings lc_open_info;
    EILInitSettings lc_init_info;

#if USE_VULKAN
    int err = ff_vk_init(&ctx->s, avctx, NULL, avctx->hw_frames_ctx);
    if (err < 0) {
        lcevc_encode_close(avctx);
        return err;
    }

    err = ff_vk_qf_init(&ctx->s, &ctx->qf, VK_QUEUE_COMPUTE_BIT);
    if (err < 0) {
        lcevc_encode_close(avctx);
        return err;
    }

    err = ff_vk_exec_pool_init(&ctx->s, &ctx->qf, &ctx->exec_pool, 1,
                               0, 0, 0, NULL);
    if (err < 0) {
        lcevc_encode_close(avctx);
        return err;
    }

    color_format = ctx->s.frames->sw_format;
#endif

    lc_open_info = (EILOpenSettings) {
        .base_encoder = "nvenc_h264",
        .log_callback = log_cb,
        .log_userdata = avctx,
    };

    ret = EIL_Open(&lc_open_info, &ctx->lc);
    if (ret != EIL_RC_Success) {
        av_log(avctx, AV_LOG_ERROR, "Unable to open: %i\n", ret);
        lcevc_encode_close(avctx);
        return AVERROR_EXTERNAL;
    }

    lc_init_info = (EILInitSettings) {
        .color_format = color_format,
        .memory_type = USE_VULKAN ? EIL_MT_VulkanBuffer : EIL_MT_Host,
        .width = avctx->width,
        .height = avctx->height,
        .fps_num = avctx->framerate.num,
        .fps_denom = avctx->framerate.den,
        .bitrate = avctx->bit_rate / 1000,
        .gop_length = avctx->gop_size,
        .properties_json = USE_VULKAN ?
#if 1
                           "{\"lcevc_encoder_type\": \"gpu\", \"gpu_device\": \"NVIDIA\"}" :
#else
                           "{\"lcevc_encoder_type\": \"gpu\", \"gpu_device\": \"NVIDIA\", \"log_groups\": \"gpu\"}" :
#endif
                           NULL,
        .external_input = 1,
    };

    ret = EIL_Initialise(ctx->lc, &lc_init_info);
    if (ret != EIL_RC_Success) {
        av_log(avctx, AV_LOG_ERROR, "Unable to initialize: %i\n", ret);
        lcevc_encode_close(avctx);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static void lo_free_cb(void *opaque, uint8_t *data)
{
    EILOutput *lo = (EILOutput *)data;
    EILContext lc = opaque;
    EIL_ReleaseOutput(lc, lo);
}

#if USE_VULKAN
static inline void get_plane_wh(uint32_t *w, uint32_t *h, enum AVPixelFormat format,
                                int frame_w, int frame_h, int plane)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);

    /* Currently always true unless gray + alpha support is added */
    if (!plane || (plane == 3) || desc->flags & AV_PIX_FMT_FLAG_RGB ||
        !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) {
        *w = frame_w;
        *h = frame_h;
        return;
    }

    *w = AV_CEIL_RSHIFT(frame_w, desc->log2_chroma_w);
    *h = AV_CEIL_RSHIFT(frame_h, desc->log2_chroma_h);
}
#endif

static int lcevc_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    int err;
    EILReturnCode ret;
    LCEVCENCCtx *ctx = avctx->priv_data;

    AVFrame *frame = NULL;
    AVBufferRef *out_ref;
    EILOutput *lo;

#if USE_VULKAN
    FFVulkanFunctions *vk = &ctx->s.vkfn;
    FFVkExecContext *exec;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(ctx->s.frames->sw_format);

    AVVkFrame *vkf;
    VkImageMemoryBarrier2 img_bar[AV_NUM_DATA_POINTERS];
    int nb_img_bar = 0;

    VkBufferImageCopy region[AV_NUM_DATA_POINTERS];
    int nb_images;
    static const VkImageAspectFlags plane_aspect[] = { VK_IMAGE_ASPECT_COLOR_BIT,
                                                       VK_IMAGE_ASPECT_PLANE_0_BIT,
                                                       VK_IMAGE_ASPECT_PLANE_1_BIT,
                                                       VK_IMAGE_ASPECT_PLANE_2_BIT, };
#endif
    EILPicture lp_tmp;

    EILPicture *lp = NULL;

    /* Check if we have something to output first before pulling frames */
    lo = NULL;
    ret = EIL_GetOutput(ctx->lc, &lo);
    if (ret == EIL_RC_Success)
        goto output;

start:
    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    err = ff_encode_get_frame(avctx, frame);
    if (err < 0) {
        av_frame_free(&frame);
        return err;
    }

#if USE_VULKAN
    vkf = (AVVkFrame *)frame->data[0];
    nb_images = ff_vk_count_images(vkf);
    EILVulkanMemoryInfo vkmems[3] = { 0 };

    lp_tmp = (EILPicture) {
        .memory_type = EIL_MT_VulkanBuffer,
        .num_planes = av_pix_fmt_count_planes(avctx->sw_pix_fmt),
        .plane = { &vkmems[0],
                   &vkmems[1],
                   &vkmems[2], },
        .stride = { FFALIGN(frame->width, 64),
                    FFALIGN(frame->width/2, 64),
                    FFALIGN(frame->width/2, 64), },
    };
    lp = &lp_tmp;

    VkExportMemoryAllocateInfo exp_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
    };

    FFVkBuffer tmp_buf[3];
    for (int i = 0; i < 3; i++) {
        err = ff_vk_create_buf(&ctx->s, &tmp_buf[i],
                               frame->width*frame->height*16,
                               NULL, &exp_info,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (err < 0)
            return err;
    }

    int buf_handle[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++) {
        VkMemoryGetFdInfoKHR buf_handle_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = tmp_buf[i].mem,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
        VkResult vret = vk->GetMemoryFdKHR(ctx->s.hwctx->act_dev,
                                           &buf_handle_info,
                                           &buf_handle[i]);
        if (vret != VK_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Unable to export FD handle!\n");
            return AVERROR_EXTERNAL;
        }
    }

    size_t off = 0;
    for (int i = 0; i < 3; i++) {
        vkmems[i] = (EILVulkanMemoryInfo) {
            .handle = buf_handle[i],
            .size = tmp_buf[i].size,
            .offset = 0,
            .total_size = tmp_buf[i].size,
            .memory_property_flags = tmp_buf[i].flags,
            .buffer_usage_flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        off += vkmems[i].size;
    }

    exec = ff_vk_exec_get(&ctx->exec_pool);

    ff_vk_exec_start(&ctx->s, exec);

    /* Prep destination Vulkan frame */
    err = ff_vk_exec_add_dep_frame(&ctx->s, exec, frame,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    if (err < 0) {
        av_frame_free(&frame);
        return err;
    }

    ff_vk_frame_barrier(&ctx->s, exec, frame, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_QUEUE_FAMILY_IGNORED);

    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pImageMemoryBarriers = img_bar,
            .imageMemoryBarrierCount = nb_img_bar,
    });

    for (int i = 0; i < lp->num_planes; i++) {
        int img_idx = FFMIN(i, (nb_images - 1));
        EILVulkanMemoryInfo *pb = lp->plane[i];

        uint32_t p_w, p_h;
        get_plane_wh(&p_w, &p_h, ctx->s.frames->sw_format,
                     avctx->width, avctx->height, i);

        region[i] = (VkBufferImageCopy) {
            .bufferOffset = pb->offset,
            .bufferRowLength = lp->stride[i],
            .bufferImageHeight = p_h,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ p_w, p_h, 1 },
        };
    }

    for (int i = 0; i < lp->num_planes; i++) {
        int img_idx = FFMIN(i, (nb_images - 1));

        uint32_t orig_stride = region[i].bufferRowLength;
        region[i].bufferRowLength /= desc->comp[i].step;
        region[i].imageSubresource.aspectMask = plane_aspect[(lp->num_planes != nb_images) +
                                                             i*(lp->num_planes != nb_images)];

        vk->CmdCopyImageToBuffer(exec->buf, vkf->img[img_idx],
                                 img_bar[img_idx].newLayout,
                                 tmp_buf[i].buf,
                                 1, &region[i]);
    };

    err = ff_vk_exec_submit(&ctx->s, exec);
    if (err < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error copying frame: %i\n", ret);
        av_frame_free(&frame);
    }
    ff_vk_exec_wait(&ctx->s, exec);

    for (int i = 0; i < 3; i++)
        ff_vk_free_buf(&ctx->s, &tmp_buf[i]);
#else
    lp_tmp = (EILPicture) {
        .memory_type = EIL_MT_Host,
        .num_planes = av_pix_fmt_count_planes(avctx->pix_fmt),
        .plane = { frame->data[0],
                   frame->data[1],
                   frame->data[2], },
        .stride = { frame->linesize[0],
                    frame->linesize[1],
                    frame->linesize[2], },
    };
    lp = &lp_tmp;
#endif

    lp->frame_type = EIL_FrameType_Progressive;
    lp->pts = frame->pts;

    ret = EIL_Encode(ctx->lc, lp);
    av_frame_free(&frame);
    if (ret != EIL_RC_Success) {
        av_log(avctx, AV_LOG_ERROR, "Unable to encode picture: %i\n", ret);
        av_frame_free(&frame);
        return AVERROR_EXTERNAL;
    }

    lo = NULL;
    ret = EIL_GetOutput(ctx->lc, &lo);

output:
    /* No output */
    if (ret == EIL_RC_Finished)
        goto start;

    if (ret != EIL_RC_Success) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get output data: %i\n", ret);
        return AVERROR_EXTERNAL;
    }

    out_ref = av_buffer_create((uint8_t *)lo, sizeof(lo),
                               lo_free_cb, ctx->lc,
                               AV_BUFFER_FLAG_READONLY);
    if (!out_ref)
        return AVERROR(ENOMEM);

    pkt->data = (uint8_t *)lo->data;
    pkt->size = lo->data_length;
    pkt->pts = lo->pts;
    pkt->dts = lo->dts;
    pkt->flags = lo->keyframe ? AV_PKT_FLAG_KEY : 0;
    pkt->buf = out_ref;

    return 0;
}

static const AVClass lcevc_encode_class = {
    .class_name = "lcevc_h264",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};

#if USE_VULKAN
const AVCodecHWConfigInternal *const ff_vulkan_encode_hw_configs[] = {
    HW_CONFIG_ENCODER_FRAMES(VULKAN, VULKAN),
    NULL,
};
#endif

const FFCodec ff_lcevc_encoder = {
    .p.name         = "lcevc_h264",
    CODEC_LONG_NAME("LCEVC"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(LCEVCENCCtx),
    .init           = &lcevc_encode_init,
    FF_CODEC_RECEIVE_PACKET_CB(&lcevc_receive_packet),
    .flush          = NULL,
    .close          = &lcevc_encode_close,
    .p.priv_class   = &lcevc_encode_class,
    .p.capabilities = (USE_VULKAN ? AV_CODEC_CAP_HARDWARE : 0) |
                      AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
#if USE_VULKAN
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_VULKAN,
        AV_PIX_FMT_NONE,
    },
    .hw_configs     = ff_vulkan_encode_hw_configs,
    .p.wrapper_name = "vulkan",
#else
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV420P10,
        AV_PIX_FMT_YUV420P12,
        AV_PIX_FMT_YUV420P14,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV422P10,
        AV_PIX_FMT_YUV422P12,
        AV_PIX_FMT_YUV422P14,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV444P12,
        AV_PIX_FMT_YUV444P14,
        AV_PIX_FMT_RGB8,
        AV_PIX_FMT_BGR8,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_ARGB,
        AV_PIX_FMT_ABGR,
        AV_PIX_FMT_NONE,
    },
#endif
};
