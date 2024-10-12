#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/fifo.h>
 
AVFormatContext *out_context = NULL;
AVCodecContext *c = NULL;
struct SwrContext *swr_ctx = NULL;
AVStream *out_stream = NULL;
AVFrame *output_frame = NULL;
int fsize = 0, thread_encode_exit = 0;
AVFifoBuffer *fifo = NULL;
pthread_mutex_t lock;
 
void *thread_encode(void *);

// gcc audio1.c -o audio1 -I /usr/local/include -L /usr/local/lib -lavdevice -lavformat -lavcodec -lavutil -lswscale -lswresample -lpthread

// ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental rtsp://localhost:8554/mic
int main(void)
{
    const char *input_format_name = "alsa"; 
    const char *device_name = "hw:0,0,0";
    const char *in_sample_rate = "48000";
    const char *in_channels = "2";
    const char *url = "rtsp://localhost:8554/mic"; 
    int ret = -1;
    int streamid = -1;
    AVDictionary *options = NULL;
    AVInputFormat *fmt = NULL;
    AVFormatContext *in_context = NULL;
    AVCodec *codec = NULL;
    AVStream *stream = NULL;
    int64_t channel_layout;
 
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
 
    // Set microphone audio parameters
    av_dict_set(&options, "sample_rate", in_sample_rate, 0);
    av_dict_set(&options, "channels", in_channels, 0);
 
    // Open input stream and initialize format context
    ret = avformat_open_input(&in_context, device_name, fmt, &options);
    if (ret != 0)
    {
        // Release options in case of error, otherwise avformat_open_input will release it internally
        av_dict_free(&options);
        printf("avformat_open_input error\n");
        return -1;
    }
 
    // Find stream information
    if (avformat_find_stream_info(in_context, 0) < 0)
    {
        printf("avformat_find_stream_info failed\n");
        return -1;
    }
 
    // Find the audio stream index
    streamid = av_find_best_stream(in_context, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (streamid < 0)
    {
        printf("cannot find audio stream");
        goto end;
    }
    stream = in_context->streams[streamid];
    printf("audio stream, sample_rate: %d, channels: %d, format: %s\n",
           stream->codecpar->sample_rate, stream->codecpar->channels,
           av_get_sample_fmt_name((enum AVSampleFormat)stream->codecpar->format));
 
    // Get default channel layout based on number of channels
    channel_layout = av_get_default_channel_layout(stream->codecpar->channels);
    // Initialize resampling context, converting input audio format to the format required by the encoder
    swr_ctx = swr_alloc_set_opts(NULL,
                                 channel_layout, AV_SAMPLE_FMT_FLTP, stream->codecpar->sample_rate,
                                 channel_layout, stream->codecpar->format, stream->codecpar->sample_rate,
                                 0, NULL);
    if (!swr_ctx || swr_init(swr_ctx) < 0)
    {
        printf("allocate resampler context failed\n");
        goto end;
    }
 
    // Allocate output format context
    // RTSP-rtsp, RTMP-flv, HLS-m3u8, UDP-mpegts, TCP-mpegts, FILE-mp4, MP4-mp4, MP3-mp3, AAC-adts, AC3-ac3, FLAC-flac, WAV-wav, OGG-ogg, WEBM-webm, MPEG-mpeg, MPEGTS-mpegts
    avformat_alloc_output_context2(&out_context, NULL, "rtsp", url); 
    if (!out_context)
    {
        printf("avformat_alloc_output_context2 failed\n");
        goto end;
    }
 
    // Find the encoder
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
    {
        printf("Codec not found\n");
        goto end;
    }
    printf("codec name: %s\n", codec->name);
 
    // Create new stream
    out_stream = avformat_new_stream(out_context, NULL);
    if (!out_stream)
    {
        printf("avformat_new_stream failed\n");
        goto end;
    }
 
    // Allocate codec context
    c = avcodec_alloc_context3(codec);
    if (!c)
    {
        printf("avcodec_alloc_context3 failed\n");
        goto end;
    }
 
    // Set codec parameters
    c->codec_id = AV_CODEC_ID_AAC;
    c->codec_type = AVMEDIA_TYPE_AUDIO;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->sample_rate = stream->codecpar->sample_rate;
    c->channels = stream->codecpar->channels;
    c->channel_layout = channel_layout;
    c->bit_rate = 128 * 1000; // 128k
    c->profile = FF_PROFILE_AAC_LOW;
    c->thread_count = 4;

    av_opt_set(out_context->priv_data, "rtsp_transport", "udp", 0); // Use UDP to reduce latency
    av_opt_set(out_context->priv_data, "muxdelay", "0", 0);         // Set mux delay to 0

    if (out_context->oformat->flags & AVFMT_GLOBALHEADER)
    {
        printf("set AV_CODEC_FLAG_GLOBAL_HEADER\n");
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
 
    // Open the encoder
    if (avcodec_open2(c, codec, NULL) < 0)
    {
        printf("avcodec_open2 failed\n");
        goto end;
    }
 
    // Copy encoder parameters to stream
    ret = avcodec_parameters_from_context(out_stream->codecpar, c);
    if (ret < 0)
    {
        printf("avcodec_parameters_from_context failed\n");
        goto end;
    }
 
    // Allocate memory for output frame
    output_frame = av_frame_alloc();
    if (!output_frame)
    {
        printf("av_frame_alloc failed\n");
        goto end;
    }
    AVPacket *recv_ptk = av_packet_alloc();
    if (!recv_ptk)
    {
        printf("av_packet_alloc failed\n");
        goto end;
    }
 
    // Set frame parameters, used by av_frame_get_buffer when allocating buffer
    output_frame->format = c->sample_fmt;
    output_frame->nb_samples = c->frame_size;
    output_frame->channel_layout = c->channel_layout;
 
    // Allocate buffer for frame
    ret = av_frame_get_buffer(output_frame, 0);
    if (ret < 0)
    {
        printf("av_frame_get_buffer failed\n");
        goto end;
    }
 
    // Calculate the size of PCM data required per AAC frame = number of samples * size per sample * number of channels
    fsize = c->frame_size * av_get_bytes_per_sample(stream->codecpar->format) *
            stream->codecpar->channels;
    printf("frame size: %d\n", fsize);
 
    fifo = av_fifo_alloc(fsize * 5);
    if (!fifo)
    {
        printf("av_fifo_alloc failed\n");
        goto end;
    }
 
    // Open the url
    if (!(out_context->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&out_context->pb, url, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            printf("avio_open error (errmsg '%s')\n", av_err2str(ret));
            goto end;
        }
    }
 
    // Write file header
    ret = avformat_write_header(out_context, NULL);
    if (ret < 0)
    {
        printf("avformat_write_header failed\n");
        goto end;
    }
 
    pthread_t tid;
    // Initialize mutex
    pthread_mutex_init(&lock, NULL);
    // Create thread
    pthread_create(&tid, NULL, thread_encode, NULL);
 
    // Read frame, resample, encode, and send
    AVPacket read_pkt;

    while ((av_read_frame(in_context, &read_pkt) >= 0) && (!thread_encode_exit))
    {
        if (read_pkt.stream_index == streamid)
        {
            pthread_mutex_lock(&lock);
            av_fifo_generic_write(fifo, read_pkt.buf->data, read_pkt.size, NULL);
            pthread_mutex_unlock(&lock);
        }
        av_packet_unref(&read_pkt);
    }
    thread_encode_exit = 1;
 
end:
    pthread_join(tid, NULL);
    if (fifo)
    {
        av_fifo_free(fifo);
    }
    if (swr_ctx)
    {
        swr_free(&swr_ctx);
    }
    if (output_frame)
    {
        av_frame_free(&output_frame);
    }
    if (in_context)
    {
        avformat_close_input(&in_context);
    }
    if (out_context)
    {
        if (!(out_context->oformat->flags & AVFMT_NOFILE))
        {
            avio_close(out_context->pb);
        }
        avformat_free_context(out_context);
    }
    pthread_mutex_destroy(&lock);
    return 0;
}

void *thread_encode(void *arg)
{
    int ret;
    AVPacket pkt;
    int got_packet;
    uint8_t *fdata = malloc(fsize);
    while (1)
    {
        if (thread_encode_exit)
            break;
        pthread_mutex_lock(&lock);
        if (av_fifo_size(fifo) >= fsize)
        {
            av_fifo_generic_read(fifo, fdata, fsize, NULL);
            pthread_mutex_unlock(&lock);
            // Resample
            const uint8_t *in[] = {fdata};
            uint8_t **out = output_frame->data;
            int len = swr_convert(swr_ctx, out, output_frame->nb_samples,
                                  in, c->frame_size);
            if (len < 0)
            {
                printf("swr_convert failed\n");
                break;
            }
            // Encode
            av_init_packet(&pkt);
            pkt.data = NULL;
            pkt.size = 0;
            got_packet = 0;
            ret = avcodec_encode_audio2(c, &pkt, output_frame, &got_packet);
            if (ret < 0)
            {
                printf("avcodec_encode_audio2 failed\n");
                break;
            }
            if (got_packet)
            {
                ret = av_interleaved_write_frame(out_context, &pkt);
                if (ret < 0)
                {
                    printf("av_interleaved_write_frame failed\n");
                    break;
                }
                av_packet_unref(&pkt);
            }
        }
        else
        {
            pthread_mutex_unlock(&lock);
            usleep(5000);
        }
    }
    free(fdata);
    return NULL;
}
