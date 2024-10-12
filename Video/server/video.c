#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// gcc test3.c -o test3 -I /usr/local/include -L /usr/local/lib -lavdevice -lavformat -lavcodec -lavutil -lswscale
// ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental rtsp://localhost:8554/live
int main()
{
    const char *input_format_name = "video4linux2";           // Input format name, for Linux use video4linux2 or v4l2
    const char *device_name = "/dev/video0";                  // Camera device name
    const char *camera_resolution = "640x480";                // Camera resolution
    enum AVPixelFormat camera_pix_fmt = AV_PIX_FMT_YUYV422;   // Camera pixel format
    const char *url = "rtsp://localhost:8554/live";           // Change the streaming address to RTSP
    int frame_rate = 30;                                      // Frame rate
    int ret = -1;                                             // Return result for input stream context
    int video_streamid = -1;
    int64_t frame_index = 0;
    AVDictionary *options = NULL;
    AVInputFormat *fmt = NULL;
    AVFormatContext *in_context = NULL, *out_context = NULL;
    struct SwsContext *sws_ctx = NULL;
    AVCodecContext *codec_context = NULL;
    AVStream *out_stream = NULL;
    AVCodec *codec = NULL;
    AVStream *video_stream = NULL;
    AVFrame *input_frame = NULL;
    AVFrame *frame_yuv420p = NULL;
    AVPacket *packet = NULL;
    AVPacket pkt;

    // Timestamp calculation
    clock_t start_time;
    clock_t end_time;

    // Print ffmpeg version information
    printf("ffmpeg version: %s\n", av_version_info());

    // Register all devices
    avdevice_register_all();

    // Find input format
    fmt = av_find_input_format(input_format_name);
    if (!fmt)
    {
        printf("av_find_input_format error");
        return -1;
    }

    // Set resolution
    av_dict_set(&options, "video_size", camera_resolution, 0);

    // Open input stream and initialize format context
    start_time = clock();
    ret = avformat_open_input(&in_context, device_name, fmt, &options);
    if (ret != 0)
    {
        // Release options in case of error, in case of success avformat_open_input will release them internally
        av_dict_free(&options);
        printf("avformat_open_input error");
        return -1;
    }
    end_time = clock();
    double time_spent = (double)(end_time - start_time) / CLOCKS_PER_SEC * 1000; // Convert to milliseconds
    printf("Time spent in avformat_open_input: %f ms\n", time_spent);

    // Find stream information
    if (avformat_find_stream_info(in_context, 0) < 0)
    {
        printf("avformat_find_stream_info failed\n");
        return -1;
    }

    // Find video stream index
    video_streamid = av_find_best_stream(in_context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_streamid < 0)
    {
        printf("cannot find video stream");
        goto end;
    }
    video_stream = in_context->streams[video_streamid];
    printf("input stream, width: %d, height: %d, format: %s\n",
           video_stream->codecpar->width, video_stream->codecpar->height,
           av_get_pix_fmt_name((enum AVPixelFormat)video_stream->codecpar->format));

    // Check if the actual format obtained matches the set camera pixel format
    if (video_stream->codecpar->format != camera_pix_fmt)
    {
        printf("pixel format error");
        goto end;
    }

    // Initialize scaling context
    sws_ctx = sws_getContext(
        video_stream->codecpar->width, video_stream->codecpar->height, camera_pix_fmt,
        video_stream->codecpar->width, video_stream->codecpar->height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx)
    {
        printf("sws_getContext error\n");
        goto end;
    }

    // Allocate output format context
    avformat_alloc_output_context2(&out_context, NULL, "rtsp", url);
    if (!out_context)
    {
        printf("avformat_alloc_output_context2 failed\n");
        goto end;
    }

    // Find encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        printf("Codec not found\n");
        goto end;
    }
    printf("codec name: %s\n", codec->name);

    // Create new video stream
    out_stream = avformat_new_stream(out_context, NULL);
    if (!out_stream)
    {
        printf("avformat_new_stream failed\n");
        goto end;
    }

    // Allocate encoder context
    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context)
    {
        printf("avcodec_alloc_context3 failed\n");
        goto end;
    }

    // Set encoder parameters
    codec_context->codec_id = AV_CODEC_ID_H264;
    codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_context->width = video_stream->codecpar->width;
    codec_context->height = video_stream->codecpar->height;
    codec_context->time_base = (AVRational){1, frame_rate};         // Set time base
    codec_context->framerate = (AVRational){frame_rate, 1};         // Set frame rate
    codec_context->bit_rate = 750 * 1000;                           // Set bit rate
    codec_context->gop_size = frame_rate;                           // Set GOP size
    codec_context->max_b_frames = 0;                                // Set max B frames, set to 0 if not needed
    codec_context->thread_count = 4; // Enable multi-threaded encoding

    av_opt_set(codec_context->priv_data, "profile", "baseline", 0); // Set H264 quality profile
    av_opt_set(codec_context->priv_data, "tune", "zerolatency", 0); // Set H264 encoding optimization parameters
    // Set delay optimization parameters for the format context
    av_opt_set(out_context->priv_data, "rtsp_transport", "udp", 0); // Use UDP transport to reduce delay
    av_opt_set(out_context->priv_data, "muxdelay", "0", 0);         // Set muxing delay to 0
    // Check if the output context's format requires AV_CODEC_FLAG_GLOBAL_HEADER
    // AV_CODEC_FLAG_GLOBAL_HEADER: Instead of adding PPS and SPS before each keyframe, they are added in the extradate byte section
    if (out_context->oformat->flags & AVFMT_GLOBALHEADER)
    {
        printf("set AV_CODEC_FLAG_GLOBAL_HEADER\n");
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open encoder
    if (avcodec_open2(codec_context, codec, NULL) < 0)
    {
        printf("avcodec_open2 failed\n");
        goto end;
    }

    // Copy encoder parameters to stream
    ret = avcodec_parameters_from_context(out_stream->codecpar, codec_context);
    if (ret < 0)
    {
        printf("avcodec_parameters_from_context failed\n");
        goto end;
    }

    // Allocate memory
    input_frame = av_frame_alloc();
    frame_yuv420p = av_frame_alloc();
    if (!input_frame || !frame_yuv420p)
    {
        printf("av_frame_alloc error\n");
        goto end;
    }
    packet = av_packet_alloc();
    if (!packet)
    {
        printf("av_packet_alloc failed\n");
        goto end;
    }

    // Set frame format
    input_frame->format = camera_pix_fmt;
    input_frame->width = video_stream->codecpar->width;
    input_frame->height = video_stream->codecpar->height;

    frame_yuv420p->format = AV_PIX_FMT_YUV420P;
    frame_yuv420p->width = video_stream->codecpar->width;
    frame_yuv420p->height = video_stream->codecpar->height;

    // Allocate frame memory
    ret = av_frame_get_buffer(frame_yuv420p, 0);
    if (ret < 0)
    {
        printf("av_frame_get_buffer error\n");
        goto end;
    }

    // Open URL
    if (!(out_context->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&out_context->pb, url, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            printf("avio_open error\n");
            goto end;
        }
    }

    // Write file header
    ret = avformat_write_header(out_context, NULL);
    if (ret < 0)
    {
        printf("avformat_write_header error\n");
        goto end;
    }
    printf("avformat_write_header success\n");

    // Start encoding
    start_time = clock();
    while (av_read_frame(in_context, &pkt) == 0)
    {
        ret = avcodec_send_packet(codec_context, &pkt);
        if (ret < 0)
        {
            printf("avcodec_send_packet failed\n");
            break;
        }
        while (ret >= 0)
        {
            ret = avcodec_receive_frame(codec_context, frame_yuv420p);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // Need more packets to continue encoding, skip this iteration
                break;
            }
            else if (ret < 0)
            {
                printf("Error during encoding\n");
                goto end;
            }

            // Scale the frame from the input format (YUYV422) to YUV420P
            sws_scale(sws_ctx, (const uint8_t *const *)input_frame->data, input_frame->linesize, 0,
                      video_stream->codecpar->height, frame_yuv420p->data, frame_yuv420p->linesize);

            // Set frame PTS (presentation timestamp)
            frame_yuv420p->pts = frame_index++;

            // Send the frame to the encoder
            ret = avcodec_send_frame(codec_context, frame_yuv420p);
            if (ret < 0)
            {
                printf("Error sending frame to encoder\n");
                goto end;
            }

            // Receive the encoded packet
            ret = avcodec_receive_packet(codec_context, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // Continue if more frames need to be encoded
                continue;
            }
            else if (ret < 0)
            {
                printf("Error receiving encoded packet\n");
                goto end;
            }

            // Rescale PTS to match the output stream timebase
            av_packet_rescale_ts(packet, codec_context->time_base, out_stream->time_base);
            packet->stream_index = out_stream->index;

            // Write the encoded packet to the output stream
            ret = av_interleaved_write_frame(out_context, packet);
            if (ret < 0)
            {
                printf("Error writing frame\n");
                goto end;
            }

            // Free the packet
            av_packet_unref(packet);
        }
    }
    end_time = clock();
    double encoding_time = (double)(end_time - start_time) / CLOCKS_PER_SEC * 1000;
    printf("Encoding completed in: %f ms\n", encoding_time);

    // Write trailer and flush
    av_write_trailer(out_context);

end:
    // Cleanup and free resources
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    if (input_frame)
        av_frame_free(&input_frame);
    if (frame_yuv420p)
        av_frame_free(&frame_yuv420p);
    if (packet)
        av_packet_free(&packet);
    if (codec_context)
        avcodec_free_context(&codec_context);
    if (in_context)
        avformat_close_input(&in_context);
    if (out_context && !(out_context->oformat->flags & AVFMT_NOFILE))
        avio_close(out_context->pb);
    if (out_context)
        avformat_free_context(out_context);

    printf("Cleanup completed\n");
    return 0;
}
