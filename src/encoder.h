#pragma once
#include <algorithm>
#include <thread>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include "webm_writer.h"
namespace encoding {
inline uint8_t clamp(int v) { return uint8_t(std::clamp(v, 0, 255)); }
void convert(const uint8_t* pixels, int width, int height, vpx_image_t& dst) {
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int rr = 0, gg = 0, bb = 0;
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                const uint8_t* p = pixels + (size_t(y + j) * width + x + i) * 4;
                int b = p[0], g = p[1], r = p[2]; rr += r; gg += g; bb += b;
                dst.planes[0][(y + j) * dst.stride[0] + x + i] = clamp(((66*r + 129*g + 25*b + 128) >> 8) + 16);
            }
            int r = (rr + 2) / 4, g = (gg + 2) / 4, b = (bb + 2) / 4;
            dst.planes[1][(y/2) * dst.stride[1] + x/2] = clamp(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
            dst.planes[2][(y/2) * dst.stride[2] + x/2] = clamp(((112*r - 94*g - 18*b + 128) >> 8) + 128);
        }
    }
}
struct Encoder {
    vpx_codec_ctx_t context{}; vpx_image_t image{}; bool initialized = false;
    Encoder(int width, int height, int fps) {
        vpx_codec_enc_cfg_t cfg{};
        if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0)) throw std::runtime_error("Cannot configure VP8");
        cfg.g_w = width; cfg.g_h = height; cfg.g_timebase = {1, 1000};
        cfg.g_threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
        cfg.g_lag_in_frames = 0; cfg.rc_end_usage = VPX_CQ;
        cfg.rc_target_bitrate = unsigned(std::clamp(uint64_t(width)*height*fps/6000, uint64_t(400), uint64_t(50000)));
        cfg.rc_min_quantizer = 4; cfg.rc_max_quantizer = 40;
        cfg.kf_mode = VPX_KF_AUTO; cfg.kf_max_dist = fps * 2;
        if (vpx_codec_enc_init(&context, vpx_codec_vp8_cx(), &cfg, 0)) throw std::runtime_error("Cannot initialize VP8 encoder");
        initialized = true;
        if (!vpx_img_alloc(&image, VPX_IMG_FMT_I420, width, height, 32)) {
            vpx_codec_destroy(&context); initialized = false; throw std::runtime_error("Not enough memory for encoder image");
        }
        if (vpx_codec_control(&context, VP8E_SET_CPUUSED, 6) || vpx_codec_control(&context, VP8E_SET_CQ_LEVEL, 18)) {
            vpx_img_free(&image); vpx_codec_destroy(&context); initialized = false;
            throw std::runtime_error("Cannot set VP8 quality options");
        }
    }
    ~Encoder() { vpx_img_free(&image); if (initialized) vpx_codec_destroy(&context); }
    void encode(webm::Writer& out, vpx_image_t* img, int64_t pts, unsigned long duration) {
        if (vpx_codec_encode(&context, img, pts, duration, 0, VPX_DL_REALTIME))
            throw std::runtime_error(vpx_codec_error(&context));
        vpx_codec_iter_t it = nullptr;
        while (const auto* packet = vpx_codec_get_cx_data(&context, &it)) if (packet->kind == VPX_CODEC_CX_FRAME_PKT)
            out.frame(static_cast<const uint8_t*>(packet->data.frame.buf), packet->data.frame.sz,
                      uint64_t(packet->data.frame.pts), (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0);
    }
};
}
