/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "video_renderer.h"

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <libavformat/avformat.h>

typedef struct video_renderer_restream_s {
    video_renderer_t base;
    AVFormatContext* ofmt_ctx;
    AVStream* video_out;
    AVStream* audio_out;
    AVPacket pkt;
} video_renderer_restream_t;

static const video_renderer_funcs_t video_renderer_restream_funcs;

video_renderer_t *video_renderer_restream_init(logger_t *logger, video_renderer_config_t const *config) {
    video_renderer_restream_t *renderer;
    renderer = calloc(1, sizeof(video_renderer_restream_t));
    if (!renderer) {
        return NULL;
    }
    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_restream_funcs;
    renderer->base.type = VIDEO_RENDERER_RESTREAM;

    static char* out_filename = "test.ts";

    // init format
    avformat_alloc_output_context2(&renderer->ofmt_ctx, NULL, "mpegts", out_filename);
    if (!renderer->ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        exit(1);
    }

    // init streams
    renderer->video_out = avformat_new_stream(renderer->ofmt_ctx, NULL);
    if (!renderer->video_out) {
        fprintf(stderr, "Failed allocating output stream\n");
        exit(1);
    }
    renderer->video_out->codecpar = avcodec_parameters_alloc();
    renderer->video_out->codecpar->codec_id = AV_CODEC_ID_H264;
    renderer->video_out->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    renderer->video_out->codecpar->codec_tag = 0;

    // TODO: stream output instead of file
    AVOutputFormat* ofmt = renderer->ofmt_ctx->oformat;
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&renderer->ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            exit(1);
        }
    }
    // write header
    int ret = avformat_write_header(renderer->ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        exit(1);
    }

    // init packets
    av_init_packet(&renderer->pkt);

    return &renderer->base;
}

static void video_renderer_restream_start(video_renderer_t *renderer) {}

static void video_renderer_restream_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
    printf("Received %d bytes of data with pts %ld...\n", data_len, pts);

    video_renderer_restream_t* restream_renderer = (video_renderer_restream_t*) renderer;

    restream_renderer->pkt.stream_index = 0;
    const AVStream* out_stream = restream_renderer->video_out;

    const uint64_t ntp_packet_time = raop_ntp_get_local_time(ntp);

    // match the ntp time to the speed of the H.264 stream
    const AVRational timebase_in = av_make_q(1, 1000000);
    int64_t packet_pts = av_rescale_q_rnd(
        ntp_packet_time,
        timebase_in,
        restream_renderer->video_out->time_base,
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

    // set packet metadata
    restream_renderer->pkt.pts = packet_pts;
    restream_renderer->pkt.dts = packet_pts;
    restream_renderer->pkt.duration = 0;
    restream_renderer->pkt.pos = -1;

    // set packet payload
    restream_renderer->pkt.data = data;
    restream_renderer->pkt.size = data_len;

    int ret = av_interleaved_write_frame(restream_renderer->ofmt_ctx, &restream_renderer->pkt);
    if (ret < 0) {
        fprintf(stderr, "Error muxing packet\n");
        exit(1);
    }
}

static void video_renderer_restream_flush(video_renderer_t *renderer) {}

static void video_renderer_restream_destroy(video_renderer_t *renderer) {
    video_renderer_restream_t* restream_renderer = (video_renderer_restream_t*) renderer;
    av_write_trailer(restream_renderer->ofmt_ctx);
    avio_closep(&restream_renderer->ofmt_ctx->pb);
    avformat_free_context(restream_renderer->ofmt_ctx);

    if (renderer) {
        free(renderer);
    }
}

static void video_renderer_restream_update_background(video_renderer_t *renderer, int type) {}

static const video_renderer_funcs_t video_renderer_restream_funcs = {
    .start = video_renderer_restream_start,
    .render_buffer = video_renderer_restream_render_buffer,
    .flush = video_renderer_restream_flush,
    .destroy = video_renderer_restream_destroy,
    .update_background = video_renderer_restream_update_background,
};
