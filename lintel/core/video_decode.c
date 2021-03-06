/**
 * Copyright 2018 Brendan Duke.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "video_decode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * Receives a complete frame from the video stream in format_context that
 * corresponds to video_stream_index.
 *
 * @param vid_ctx Context needed to decode frames from the video stream.
 *
 * @return SUCCESS on success, VID_DECODE_EOF if no frame was received, and
 * VID_DECODE_FFMPEG_ERR if an FFmpeg error occurred..
 */
static int32_t
receive_frame(struct video_stream_context *vid_ctx)
{
        AVPacket packet;
        int32_t status;
        bool was_frame_received;

        av_init_packet(&packet);

        status = avcodec_receive_frame(vid_ctx->codec_context,
                                       vid_ctx->frame);
        if (status == 0)
                return VID_DECODE_SUCCESS;
        else if (status == AVERROR_EOF)
                return VID_DECODE_EOF;
        else if (status != AVERROR(EAGAIN))
                return VID_DECODE_FFMPEG_ERR;

        was_frame_received = false;
        while (!was_frame_received &&
               (av_read_frame(vid_ctx->format_context, &packet) == 0)) {
                if (packet.stream_index == vid_ctx->video_stream_index) {
                        status = avcodec_send_packet(vid_ctx->codec_context,
                                                     &packet);
                        if (status != 0) {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }

                        status = avcodec_receive_frame(vid_ctx->codec_context,
                                                       vid_ctx->frame);
                        if (status == 0) {
                                was_frame_received = true;
                        } else if (status != AVERROR(EAGAIN)) {
                                av_packet_unref(&packet);
                                return VID_DECODE_FFMPEG_ERR;
                        }
                }

                av_packet_unref(&packet);
        }

        if (was_frame_received)
                return VID_DECODE_SUCCESS;

        /**
         * NOTE(brendan): Flush/drain the codec. After this, subsequent calls
         * to receive_frame will return frames until EOF.
         *
         * See FFmpeg's libavcodec/avcodec.h.
         */
        av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;

        status = avcodec_send_packet(vid_ctx->codec_context,
                                     &packet);
        if (status == 0) {
                status = avcodec_receive_frame(vid_ctx->codec_context,
                                               vid_ctx->frame);
                if (status == 0) {
                        av_packet_unref(&packet);

                        return VID_DECODE_SUCCESS;
                }
        }

        av_packet_unref(&packet);

        return VID_DECODE_EOF;
}

/**
 * Allocates an RGB image frame.
 *
 * @param codec_context Decoder context from the video stream, from which the
 * RGB frame will get its dimensions.
 *
 * @return The allocated RGB frame on success, NULL on failure.
 */
static AVFrame *
allocate_rgb_image(AVCodecContext *codec_context)
{
        int32_t status;
        AVFrame *frame_rgb;

        frame_rgb = av_frame_alloc();
        if (frame_rgb == NULL)
                return NULL;

        frame_rgb->format = AV_PIX_FMT_RGB24;
        frame_rgb->width = codec_context->width;
        frame_rgb->height = codec_context->height;

        status = av_image_alloc(frame_rgb->data,
                                frame_rgb->linesize,
                                frame_rgb->width,
                                frame_rgb->height,
                                AV_PIX_FMT_RGB24,
                                32);
        if (status < 0) {
                av_frame_free(&frame_rgb);
                return NULL;
        }

        return frame_rgb;
}

/**
 * Copies the received frame in `frame` to `dest`, using `frame_rgb` as
 * temporary storage for `sws_scale`.
 *
 * @param dest Destination buffer for RGB24 frame.
 * @param frame Received frame.
 * @param frame_rgb Temporary RGB frame.
 * @param sws_context Context to use for sws_scale operation.
 * @param copied_bytes Number of bytes already copied into dest from the video.
 * @param bytes_per_row Number of bytes per row in the video.
 *
 * @return Number of bytes copied to `dest`, including the frame copied over by
 * this function.
 */
static uint32_t
copy_next_frame(uint8_t *dest,
                AVFrame *frame,
                AVFrame *frame_rgb,
                AVCodecContext *codec_context,
                struct SwsContext *sws_context,
                uint32_t copied_bytes,
                const uint32_t bytes_per_row)
{
        sws_scale(sws_context,
                  (const uint8_t * const *)(frame->data),
                  frame->linesize,
                  0,
                  codec_context->height,
                  frame_rgb->data,
                  frame_rgb->linesize);

        uint8_t *next_row = frame_rgb->data[0];
        for (int32_t row_index = 0;
             row_index < frame_rgb->height;
             ++row_index) {
                memcpy(dest + copied_bytes, next_row, bytes_per_row);

                next_row += frame_rgb->linesize[0];
                copied_bytes += bytes_per_row;
        }

        return copied_bytes;
}

/**
 * Loops the frames already received in `dest` until the `num_requested_frames`
 * have been satisfied.
 *
 * @param dest Output RGB24 frame buffer.
 * @param copied_bytes Number of bytes already copied into `dest`.
 * @param frame_number The number of the next frame to copy into `dest`.
 * @param bytes_per_frame The number of bytes per frame (3*width*height).
 * @param num_requested_frames The number of frames that were requested.
 */
static void
loop_to_buffer_end(uint8_t *dest,
                   uint32_t copied_bytes,
                   int32_t frame_number,
                   uint32_t bytes_per_frame,
                   int32_t num_requested_frames)
{
        //fprintf(stderr, "Ran out of frames. Looping.\n");
        if (frame_number == 0) {
                fprintf(stderr, "No frames received after seek.\n");
                return;
        }

        uint32_t bytes_to_copy = copied_bytes;
        int32_t remaining_frames = (num_requested_frames - frame_number);
        while (remaining_frames > 0) {
                if (remaining_frames < frame_number)
                        bytes_to_copy = remaining_frames*bytes_per_frame;

                memcpy(dest + copied_bytes, dest, bytes_to_copy);

                remaining_frames -= frame_number;
                copied_bytes += bytes_to_copy;
        }
}

void
decode_video_to_out_buffer(uint8_t *dest,
                           struct video_stream_context *vid_ctx,
                           int32_t num_requested_frames)
{
        AVCodecContext *codec_context = vid_ctx->codec_context;
        struct SwsContext *sws_context = sws_getContext(codec_context->width,
                                                        codec_context->height,
                                                        codec_context->pix_fmt,
                                                        codec_context->width,
                                                        codec_context->height,
                                                        AV_PIX_FMT_RGB24,
                                                        SWS_BILINEAR,
                                                        NULL,
                                                        NULL,
                                                        NULL);
        assert(sws_context != NULL);

        AVFrame *frame_rgb = allocate_rgb_image(codec_context);
        assert(frame_rgb != NULL);

        const uint32_t bytes_per_row = 3*frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row*frame_rgb->height;
        uint32_t copied_bytes = 0;
        for (int32_t frame_number = 0;
             frame_number < num_requested_frames;
             ++frame_number) {
                int32_t status = receive_frame(vid_ctx);
                if (status == VID_DECODE_EOF) {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           frame_number,
                                           bytes_per_frame,
                                           num_requested_frames);
                        break;
                }
                assert(status == VID_DECODE_SUCCESS);

                copied_bytes = copy_next_frame(dest,
                                               vid_ctx->frame,
                                               frame_rgb,
                                               codec_context,
                                               sws_context,
                                               copied_bytes,
                                               bytes_per_row);
        }

        av_freep(frame_rgb->data);
        av_frame_free(&frame_rgb);

        sws_freeContext(sws_context);
}

int32_t read_memory(void *opaque, uint8_t *buffer, int32_t buf_size_bytes)
{
        struct buffer_data *input_buf = (struct buffer_data *)opaque;
        int32_t bytes_remaining = (input_buf->total_size_bytes -
                                   input_buf->offset_bytes);
        if (bytes_remaining < buf_size_bytes)
                buf_size_bytes = bytes_remaining;

        memcpy(buffer,
               input_buf->ptr + input_buf->offset_bytes,
               buf_size_bytes);

        input_buf->offset_bytes += buf_size_bytes;

        return buf_size_bytes;
}

int64_t seek_memory(void *opaque, int64_t offset64, int32_t whence)
{
        struct buffer_data *input_buf = (struct buffer_data *)opaque;
        int32_t offset = (int32_t)offset64;

        switch (whence) {
        case SEEK_CUR:
                input_buf->offset_bytes += offset;
                break;
        case SEEK_END:
                input_buf->offset_bytes = (input_buf->total_size_bytes -
                                           offset);
                break;
        case SEEK_SET:
                input_buf->offset_bytes = offset;
                break;
        case AVSEEK_SIZE:
                return input_buf->total_size_bytes;
        default:
                break;
        }

        return input_buf->offset_bytes;
}

/**
 * Probes the input video and returns the resulting guessed file format.
 *
 * @param input_buf Buffer tracking the input byte string.
 * @param buffer_size Size allocated for the AV I/O context.
 *
 * @return The guessed file format of the video.
 */
static AVInputFormat *
probe_input_format(struct buffer_data *input_buf, const uint32_t buffer_size)
{
        const int32_t probe_buf_size_bytes = (buffer_size +
                                              AVPROBE_PADDING_SIZE);
        AVProbeData probe_data = {NULL,
                                  (uint8_t *)av_malloc(probe_buf_size_bytes),
                                  probe_buf_size_bytes,
                                  NULL};
        assert(probe_data.buf != NULL);

        memset(probe_data.buf, 0, probe_buf_size_bytes);

        read_memory((void *)input_buf, probe_data.buf, buffer_size);
        input_buf->offset_bytes = 0;

        AVInputFormat *io_format = av_probe_input_format(&probe_data, 1);
        av_freep(&probe_data.buf);

        return io_format;
}

/**
 * Finds a video stream for the AV format context and returns the associated
 * stream index.
 *
 * @param format_context AV format where video streams should be searched for.
 *
 * @return Index of video stream on success, negative error code on failure.
 */
static int32_t
find_video_stream_index(AVFormatContext *format_context)
{
        AVStream *video_stream;
        uint32_t stream_index;

        for (stream_index = 0;
             stream_index < format_context->nb_streams;
             ++stream_index) {
                video_stream = format_context->streams[stream_index];

                if (video_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                        break;
        }

        if (stream_index >= format_context->nb_streams)
                return VID_DECODE_FFMPEG_ERR;

        return stream_index;
}

int32_t
setup_format_context(AVFormatContext **format_context_ptr,
                     AVIOContext *avio_ctx,
                     struct buffer_data *input_buf,
                     const uint32_t buffer_size)
{
        AVFormatContext *format_context = *format_context_ptr;

        format_context->pb = avio_ctx;
        format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
        format_context->iformat = probe_input_format(input_buf, buffer_size);

        int32_t status = avformat_open_input(format_context_ptr,
                                             "",
                                             NULL,
                                             NULL);
        if (status < 0) {
                fprintf(stderr,
                        "AVERROR: %d, message: %s\n",
                        status,
#ifdef __cplusplus
                        "");
#else
                       av_err2str(status));
#endif // __cplusplus
                return VID_DECODE_FFMPEG_ERR;
        }

        status = avformat_find_stream_info(format_context, NULL);
        assert(status >= 0);

        return find_video_stream_index(format_context);
}

AVCodecContext *open_video_codec_ctx(AVStream *video_stream)
{
        int32_t status;
        AVCodecContext *codec_context;
        AVCodec *video_codec;

        video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (video_codec == NULL)
                return NULL;

        codec_context = avcodec_alloc_context3(video_codec);
        if (codec_context == NULL)
                return NULL;

        status = avcodec_parameters_to_context(codec_context,
                                               video_stream->codecpar);
        if (status != 0) {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        status = avcodec_open2(codec_context, video_codec, NULL);
        if (status != 0) {
                avcodec_free_context(&codec_context);
                return NULL;
        }

        return codec_context;
}

int64_t
seek_to_closest_keypoint(float *seek_distance_out,
                         struct video_stream_context *vid_ctx,
                         bool should_random_seek,
                         uint32_t num_requested_frames)
{
        if (!should_random_seek)
                return 0;

        int64_t start_time;
        AVStream *video_stream =
                vid_ctx->format_context->streams[vid_ctx->video_stream_index];
        /**
         * TODO(brendan): Do something smarter to guess the start time, if the
         * container doesn't have it?
         */
        if (video_stream->start_time != AV_NOPTS_VALUE)
                start_time = video_stream->start_time;
        else
                start_time = 0;

        int64_t valid_seek_frame_limit = (vid_ctx->nb_frames -
                                          num_requested_frames);
        if (valid_seek_frame_limit <= 0)
                return AV_NOPTS_VALUE;

        /**
         * NOTE(brendan): skip_past_timestamp looks at the PTS of each frame
         * until it crosses timestamp. Therefore if the video has N frames and
         * we request one, timestamp should be in {0, 1, ..., N - 2}, because
         * the PTS corresponding to timestamp will be dropped (i.e., frame
         * N - 2 could be dropped, leaving N - 1).
         */
        int64_t timestamp = rand() % (valid_seek_frame_limit + 1);
        if (timestamp == 0)
                /* NOTE(brendan): Use AV_NOPTS_VALUE to represent no skip. */
                return AV_NOPTS_VALUE;
        else
                timestamp -= 1;

        timestamp = av_rescale_rnd(timestamp,
                                   vid_ctx->duration,
                                   vid_ctx->nb_frames,
                                   AV_ROUND_DOWN);
        timestamp += start_time;

        /**
         * NOTE(brendan): Convert seek distance from stream timebase units to
         * seconds.
         */
        int64_t tb_num = video_stream->time_base.num;
        int64_t tb_den = video_stream->time_base.den;
        /**
         * TODO(brendan): This seek distance is off by one frame...
         */
        float seek_distance = ((double)timestamp*tb_num)/tb_den;
        if (seek_distance_out != NULL)
                *seek_distance_out = seek_distance;

        int32_t status = av_seek_frame(vid_ctx->format_context,
                                       vid_ctx->video_stream_index,
                                       timestamp,
                                       AVSEEK_FLAG_BACKWARD);
        assert(status >= 0);

        return timestamp;
}

int32_t
skip_past_timestamp(struct video_stream_context *vid_ctx, int64_t timestamp)
{
        if (timestamp == AV_NOPTS_VALUE)
                return VID_DECODE_SUCCESS;

        do {
                int32_t status = receive_frame(vid_ctx);
                if (status < 0) {
                        fprintf(stderr, "Ran out of frames during seek.\n");
                        return status;
                }
                assert(status == VID_DECODE_SUCCESS);
        } while (vid_ctx->frame->pts < timestamp);

        return VID_DECODE_SUCCESS;
}

void
decode_video_from_frame_nums(uint8_t *dest,
                             struct video_stream_context *vid_ctx,
                             int32_t num_requested_frames,
                             const int32_t *frame_numbers,
                             bool should_seek,
                             bool use_frame)
{
        if (num_requested_frames <= 0)
                return;

        AVCodecContext *codec_context = vid_ctx->codec_context;
        struct SwsContext *sws_context = sws_getContext(codec_context->width,
                                                        codec_context->height,
                                                        codec_context->pix_fmt,
                                                        codec_context->width,
                                                        codec_context->height,
                                                        AV_PIX_FMT_RGB24,
                                                        SWS_BILINEAR,
                                                        NULL,
                                                        NULL,
                                                        NULL);
        assert(sws_context != NULL);

        AVFrame *frame_rgb = allocate_rgb_image(codec_context);
        assert(frame_rgb != NULL);

        int32_t status;
        uint32_t copied_bytes = 0;
        const uint32_t bytes_per_row = 3*frame_rgb->width;
        const uint32_t bytes_per_frame = bytes_per_row*frame_rgb->height;
        int32_t current_frame_index = 0;
        int32_t out_frame_index = 0;
        int64_t prev_pts = 0;

        //TODO
        AVStream *video_stream =
                vid_ctx->format_context->streams[vid_ctx->video_stream_index];
//        printf("4->> video_steam->time_base.num, %d \n",  video_stream->time_base.num);
//        printf("5->> video_steam->time_base.den, %d \n",  video_stream->time_base.den);

        int32_t time_unit =  video_stream->time_base.den / video_stream->time_base.num;
        if (should_seek) {
                /**
                 * NOTE(brendan): Convert from frame number to video stream
                 * time base by multiplying by the _average_ time (in
                 * video_stream->time_base units) per frame.
                 */
                int32_t avg_frame_duration = (vid_ctx->duration /
                                              vid_ctx->nb_frames);
//                printf('duration %d, nb_frames %d',vid_ctx->duration,vid_ctx->nb_frames);
                int64_t timestamp;
                if (use_frame){
                     timestamp = frame_numbers[0]*avg_frame_duration;
                    }
                else{
                    timestamp = frame_numbers[0]*time_unit;
                    }
                status = av_seek_frame(vid_ctx->format_context,
                                       vid_ctx->video_stream_index,
                                       timestamp,
                                       AVSEEK_FLAG_BACKWARD);
                assert(status >= 0);

                /**
                 * NOTE(brendan): Here we are handling seeking, where we need
                 * to decode the first frame in order to get the current PTS in
                 * the video stream.
                 *
                 * Most likely, the seek brought the video stream to a keyframe
                 * before the first desired frame, in which case we need to:
                 *
                 * 1. Determine which frame the video stream is at, by decoding
                 * the first frame and using its PTS and using the average
                 * frame duration approximation again.
                 *
                 * 2. Possibly copy this decoded frame into the output buffer,
                 * if by chance the frame seeked to is the first desired frame.
                 */
                status = receive_frame(vid_ctx);
                if (status == VID_DECODE_EOF)
                        goto out_free_frame_rgb_and_sws;
                assert(status == VID_DECODE_SUCCESS);

                if (use_frame){
                    current_frame_index = vid_ctx->frame->pts/avg_frame_duration;
                    if (current_frame_index > frame_numbers[0])
                            current_frame_index = frame_numbers[0];
                            }
                else{
                    current_frame_index = vid_ctx->frame->pts;
                    current_frame_index = current_frame_index / time_unit;
                }


//                printf("current_frame_index -->> %d\n", current_frame_index);
//                printf("frame_numbers[0] -->> %d\n", frame_numbers[0]);

                assert(current_frame_index <= frame_numbers[0]);
//                printf("current_frame_index -->> %d\n", current_frame_index);
//                printf("frame_numbers[0] -->> %d\n", frame_numbers[0]);

                /**
                 * NOTE(brendan): Handle the chance that the seek brought the
                 * stream exactly to the first desired frame index.
                 */
                if (current_frame_index == frame_numbers[0]) {
                        copied_bytes = copy_next_frame(dest,
                                                       vid_ctx->frame,
                                                       frame_rgb,
                                                       codec_context,
                                                       sws_context,
                                                       copied_bytes,
                                                       bytes_per_row);
                        ++out_frame_index;
                }
                ++current_frame_index;

                prev_pts = vid_ctx->frame->pts;
        }

        for (;
             out_frame_index < num_requested_frames;
             ++out_frame_index) {
                int32_t desired_frame_num = frame_numbers[out_frame_index];
                assert((desired_frame_num >= current_frame_index) &&
                       (desired_frame_num >= 0));

                /* Loop frames instead of aborting if we asked for too many. */
                if (desired_frame_num > vid_ctx->nb_frames) {
                        loop_to_buffer_end(dest,
                                           copied_bytes,
                                           out_frame_index,
                                           bytes_per_frame,
                                           num_requested_frames);
                        goto out_free_frame_rgb_and_sws;
                }
                if (use_frame){
                     while (current_frame_index <= desired_frame_num) {
//                        if (current_frame_index == desired_frame_num){
//                        printf("current_frame")
//                        printf("1->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
////                        printf("2->> vid_ctx->frame->pkt_dts, %d \n",  vid_ctx->frame->pkt_dts);
////                        printf("3->> vid_ctx->frame->pkt_pts, %d \n",  vid_ctx->frame->pkt_pts);
//                        printf("4->> video_steam->time_base.num, %d \n",  video_stream->time_base.num);
//                        printf("5->> video_steam->time_base.den, %d \n",  video_stream->time_base.den);
////                        printf("6->> video_steam->cur_dts, %d \n",  video_stream->cur_dts);
//                        }
//                        int64_t tb_num = video_stream->time_base.num;
//        int64_t tb_den = video_stream->time_base.den;

                        status = receive_frame(vid_ctx);
//                        printf("2->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                        if (status == VID_DECODE_EOF) {
                                loop_to_buffer_end(dest,
                                                   copied_bytes,
                                                   out_frame_index,
                                                   bytes_per_frame,
                                                   num_requested_frames);
                                goto out_free_frame_rgb_and_sws;
                        }
                        assert(status == VID_DECODE_SUCCESS);

                        /**
                         * NOTE(brendan): Only advance the frame index if the
                         * current frame's PTS is greater than the previous
                         * frame's PTS. This is to workaround an FFmpeg oddity
                         * where the first frame decoded gets duplicated.
                         */
//                        printf("3->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                        if (vid_ctx->frame->pts > prev_pts) {
                                ++current_frame_index;
                                prev_pts = vid_ctx->frame->pts;
                        }
//                        printf("4->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                }
                }
                else{
                while (vid_ctx->frame->pts <= desired_frame_num*time_unit) {
//                        printf("1->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
//                        printf("2->> vid_ctx->frame->pkt_dts, %d \n",  vid_ctx->frame->pkt_dts);
//                        printf("3->> vid_ctx->frame->pkt_pts, %d \n",  vid_ctx->frame->pkt_pts);
//                        printf("4->> video_steam->time_base.num, %d \n",  video_stream->time_base.num);
//                        printf("5->> video_steam->time_base.den, %d \n",  video_stream->time_base.den);
//                        printf("6->> video_steam->cur_dts, %d \n",  video_stream->cur_dts);
//                        int64_t tb_num = video_stream->time_base.num;
//        int64_t tb_den = video_stream->time_base.den;

                        status = receive_frame(vid_ctx);
//                        printf("2->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                        if (status == VID_DECODE_EOF) {
                                loop_to_buffer_end(dest,
                                                   copied_bytes,
                                                   out_frame_index,
                                                   bytes_per_frame,
                                                   num_requested_frames);
                                goto out_free_frame_rgb_and_sws;
                        }
                        assert(status == VID_DECODE_SUCCESS);

                        /**
                         * NOTE(brendan): Only advance the frame index if the
                         * current frame's PTS is greater than the previous
                         * frame's PTS. This is to workaround an FFmpeg oddity
                         * where the first frame decoded gets duplicated.
                         */
//                        printf("3->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                        if (vid_ctx->frame->pts > prev_pts) {
//                                ++current_frame_index;
                                prev_pts = vid_ctx->frame->pts;
                        }
//                        printf("4->> vid_ctx->frame->pts, %d \n",  vid_ctx->frame->pts);
                }
                }

                copied_bytes = copy_next_frame(dest,
                                               vid_ctx->frame,
                                               frame_rgb,
                                               codec_context,
                                               sws_context,
                                               copied_bytes,
                                               bytes_per_row);
        }

out_free_frame_rgb_and_sws:
        av_freep(frame_rgb->data);
        av_frame_free(&frame_rgb);
        sws_freeContext(sws_context);
}
