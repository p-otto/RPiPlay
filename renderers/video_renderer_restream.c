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

#include "h264-bitstream/h264_stream.h"
#include "fdk-aac/libAACdec/include/aacdecoder_lib.h"

typedef struct video_renderer_restream_s {
    video_renderer_t base;
    AVFormatContext* ofmt_ctx;
    AVStream* video_out;
    AVStream* audio_out;
    AVPacket pkt;
    AVPacket pkt_audio;

    HANDLE_AACDECODER audio_decoder;
    uint64_t last_video_pts;
    uint64_t last_audio_pts;
} video_renderer_restream_t;

static const video_renderer_funcs_t video_renderer_restream_funcs;

static int AUDIO_PACKET_SIZE = 4 * 480;

static int video_renderer_restream_init_audio_decoder(video_renderer_restream_t *renderer) {
    int ret = 0;
    renderer->audio_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    if (renderer->audio_decoder == NULL) {
        logger_log(renderer->base.logger, LOGGER_ERR, "aacDecoder open faild!");
        return -1;
    }
    /* ASC config binary data */
    UCHAR eld_conf[] = { 0xF8, 0xE8, 0x50, 0x00 };
    UCHAR *conf[] = { eld_conf };
    static UINT conf_len = sizeof(eld_conf);
    ret = aacDecoder_ConfigRaw(renderer->audio_decoder, conf, &conf_len);
    if (ret != AAC_DEC_OK) {
        logger_log(renderer->base.logger, LOGGER_ERR, "Unable to set configRaw");
        return -2;
    }
    CStreamInfo *aac_stream_info = aacDecoder_GetStreamInfo(renderer->audio_decoder);
    if (aac_stream_info == NULL) {
        logger_log(renderer->base.logger, LOGGER_ERR, "aacDecoder_GetStreamInfo failed!");
        return -3;
    }

    logger_log(renderer->base.logger, LOGGER_DEBUG, "> stream info: channel = %d\tsample_rate = %d\tframe_size = %d\taot = %d\tbitrate = %d",   \
            aac_stream_info->channelConfig, aac_stream_info->aacSampleRate,
            aac_stream_info->aacSamplesPerFrame, aac_stream_info->aot, aac_stream_info->bitRate);

    return 0;
}

video_renderer_t *video_renderer_restream_init(logger_t *logger, video_renderer_config_t const *config) {
    video_renderer_restream_t *renderer;
    renderer = calloc(1, sizeof(video_renderer_restream_t));
    if (!renderer) {
        return NULL;
    }
    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_restream_funcs;
    renderer->base.type = VIDEO_RENDERER_RESTREAM;

    static char* out_filename = "tcp://localhost:9999"; // for tcp (any muxer)
    // static char *out_filename = "rtsp://localhost:8554/live.sdp"; // for rtsp

    // init format
    avformat_alloc_output_context2(&renderer->ofmt_ctx, NULL, "matroska", out_filename);
    // avformat_alloc_output_context2(&renderer->ofmt_ctx, NULL, "rtsp", out_filename);
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
    renderer->video_out->codecpar->format = AV_PIX_FMT_YUV420P;
    renderer->video_out->codecpar->codec_tag = 0;
    // TODO: just testing with these
    renderer->video_out->codecpar->width = 1920;
    renderer->video_out->codecpar->height = 1080;

    // mkv expects extradata to be non-empty, for streaming you have to insert NAL units apparently:
    // https://stackoverflow.com/questions/56620131/initializing-an-output-file-for-muxing-mkv-with-ffmpeg
    const int buf_size = 1024;
    renderer->video_out->codecpar->extradata = (uint8_t*)av_malloc(buf_size);
    renderer->video_out->codecpar->extradata_size = buf_size;

    // renderer->audio_out = avformat_new_stream(renderer->ofmt_ctx, NULL);
    // if (!renderer->audio_out) {
    //     fprintf(stderr, "Failed allocating output stream\n");
    //     exit(1);
    // }
    // renderer->audio_out->codecpar = avcodec_parameters_alloc();
    // // renderer->audio_out->codecpar->codec_id = AV_CODEC_ID_AAC;
    // renderer->audio_out->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    // renderer->audio_out->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    // renderer->audio_out->codecpar->codec_tag = 0;
    // renderer->audio_out->codecpar->sample_rate = 44100;
    // renderer->audio_out->codecpar->channels = 2;
    // // renderer->audio_out->codecpar->channel_layout = 0;
    // renderer->audio_out->codecpar->channel_layout = 0b11;

    AVOutputFormat* ofmt = renderer->ofmt_ctx->oformat;
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        fprintf(stdout, "Opening output file\n");

        int ret = avio_open(&renderer->ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            exit(1);
        }
    }
    // write header
    AVDictionary *options = NULL;
    av_dict_set(&options, "live", "1", 0);

    int ret = avformat_write_header(renderer->ofmt_ctx, &options);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file. Ret: %d\n", ret);
        exit(1);
    }

    // init packets
    av_init_packet(&renderer->pkt);
    av_new_packet(&renderer->pkt_audio, AUDIO_PACKET_SIZE);

    // init audio decoding
    int err = video_renderer_restream_init_audio_decoder(renderer);
    if (err) {
        printf("Opening audio decoder failed.\n");
        exit(1);
    }

    renderer->last_video_pts = 0;
    renderer->last_audio_pts = 0;

    return &renderer->base;
}

static void video_renderer_restream_start(video_renderer_t *renderer) {}

void video_renderer_restream_get_audio(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts) {
    if (data_len == 0) {
        return;
    }

    // video_renderer_restream_t* restream_renderer = (video_renderer_restream_t*) renderer;
    // const AVStream* out_stream = restream_renderer->audio_out;

    // // decode AAC-ELD packets
    // UCHAR *p_buffer[1] = { data };
    // UINT buffer_size = data_len;
    // UINT bytes_valid = data_len;
    // AAC_DECODER_ERROR error = 0;
    // error = aacDecoder_Fill(restream_renderer->audio_decoder, p_buffer, &buffer_size, &bytes_valid);

    // if (error != AAC_DEC_OK) {
    //     fprintf(stderr, "aacDecoder_Fill error\n");
    //     exit(1);
    // }

    // INT time_data_size = AUDIO_PACKET_SIZE;
    // INT_PCM *p_time_data = malloc(time_data_size); // The buffer for the decoded AAC frames
    // error = aacDecoder_DecodeFrame(restream_renderer->audio_decoder, p_time_data, time_data_size, 0);
    // if (error != AAC_DEC_OK) {
    //     fprintf(stderr, "aacDecoder_DecodeFrame error\n");
    //     exit(1);
    // }

    // // mux decoded PCM data
    // const AVRational timebase_in = av_make_q(1, 1000000);
    // int64_t packet_pts = av_rescale_q_rnd(
    //     pts,
    //     timebase_in,
    //     out_stream->time_base,
    //     AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

    // if (packet_pts == 0) {
    //     packet_pts = restream_renderer->last_audio_pts + 1;
    // } else if (packet_pts <= restream_renderer->last_audio_pts) {
    //     fprintf(stdout, "Warning: dropping audio frame to avoid non-monotonic dts\n");
    //     return;
    // }

    // restream_renderer->last_audio_pts = packet_pts;

    // // set packet metadata
    // restream_renderer->pkt_audio.stream_index = out_stream->index;
    // restream_renderer->pkt_audio.pts = packet_pts;
    // restream_renderer->pkt_audio.dts = packet_pts;
    // restream_renderer->pkt_audio.duration = 0;
    // restream_renderer->pkt_audio.pos = -1;

    // // set packet payload
    // // restream_renderer->pkt.data = p_time_data;
    // memcpy(restream_renderer->pkt_audio.data, p_time_data, time_data_size);
    // restream_renderer->pkt_audio.size = time_data_size;

    // int ret = av_write_frame(restream_renderer->ofmt_ctx, &restream_renderer->pkt_audio);
    // // int ret = av_interleaved_write_frame(restream_renderer->ofmt_ctx, &restream_renderer->pkt_audio);
    // if (ret < 0) {
    //     fprintf(stderr, "Error muxing packet\n");
    //     exit(1);
    // }

    // free(p_time_data);
}

static void video_renderer_restream_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
    video_renderer_restream_t* restream_renderer = (video_renderer_restream_t*) renderer;

    const AVStream* out_stream = restream_renderer->video_out;

    const AVRational timebase_in = av_make_q(1, 1000000);
    int64_t packet_pts = av_rescale_q_rnd(
        pts,
        timebase_in,
        out_stream->time_base,
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

    uint8_t *modified_data = NULL;

    // matroska needs monotonically increasing pts
    // packets which contain sps and pps have pts 0, so generate an artificial pts
    if (packet_pts == 0) {
        packet_pts = restream_renderer->last_video_pts + 1;
    } else if (packet_pts <= restream_renderer->last_video_pts) {
        fprintf(stdout, "Warning: dropping video frame to avoid non-monotonic dts\n");
        return;
    }

    restream_renderer->last_video_pts = packet_pts;

    // set packet metadata
    restream_renderer->pkt.stream_index = out_stream->index;
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
