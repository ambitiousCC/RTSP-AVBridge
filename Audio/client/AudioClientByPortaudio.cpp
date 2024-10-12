#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>

extern "C" {
#include <portaudio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <Windows.h>
}

const char* RTSP_URL = "rtsp://192.168.1.27:8554/mic";
const int CHANNELS = 2;
const int RATE = 48000;
const int FRAMES_PER_BUFFER = 16;
const AVSampleFormat INPUT_FORMAT = AV_SAMPLE_FMT_FLTP;
const AVSampleFormat OUTPUT_FORMAT = AV_SAMPLE_FMT_S16;

struct AudioData {
    SwrContext* swr_ctx;
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    AVPacket* pkt;
    AVFrame* frame;
    int stream_index;
};

PaStream* initialize_pa_stream(int deviceIndex) {
    PaStream* stream;
    PaStreamParameters outputParameters;
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(deviceIndex);

    if (deviceInfo == nullptr) {
        std::cerr << "Failed to get device info for selected device." << std::endl;
        return nullptr;
    }

    outputParameters.device = deviceIndex;
    outputParameters.channelCount = CHANNELS;
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(
        &stream,
        nullptr,
        &outputParameters,
        RATE,
        FRAMES_PER_BUFFER,
        paClipOff,
        nullptr,
        nullptr
    );

    if (err != paNoError) {
        std::cerr << "Failed to open stream: " << Pa_GetErrorText(err) << std::endl;
        return nullptr;
    }

    return stream;
}

void list_audio_devices() {
    Pa_Initialize();
    SetConsoleOutputCP(CP_UTF8);
    int numDevices = Pa_GetDeviceCount();
    const PaDeviceInfo* deviceInfo;

    std::cout << "Available audio devices:\n";
    for (int i = 0; i < numDevices; ++i) {
        deviceInfo = Pa_GetDeviceInfo(i);
        std::string deviceName = deviceInfo->name;
        std::string targetName = "VB-Audio Cable A";
        if (deviceName.find(targetName) == std::string::npos) continue;
        std::cout << i << ": " << deviceInfo->name
            << " (Max Input Channels: " << deviceInfo->maxInputChannels
            << ", Max Output Channels: " << deviceInfo->maxOutputChannels
            << ", Host API: " << Pa_GetHostApiInfo(deviceInfo->hostApi)->name << ")\n";
    }
    SetConsoleOutputCP(GetACP());
    Pa_Terminate();
}

int select_device() {
    list_audio_devices();
    int deviceIndex;
    std::cout << "Enter the device number for virtual microphone: ";
    std::cin >> deviceIndex;
    return deviceIndex;
}

int main() {
    using namespace std::chrono;
    int deviceIndex = select_device();

    avformat_network_init();

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (avformat_open_input(&fmt_ctx, RTSP_URL, nullptr, nullptr) < 0) {
        std::cerr << "Failed to open RTSP stream" << std::endl;
        return 1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to retrieve input stream information" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::cerr << "Failed to find an audio stream" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt_ctx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        std::cerr << "Failed to find codec" << std::endl;
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_index]->codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters to codec context" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVChannelLayout out_ch_layout = { .order = AV_CHANNEL_ORDER_NATIVE, .nb_channels = CHANNELS, .u = {.mask = AV_CH_LAYOUT_STEREO } };
    AVChannelLayout in_ch_layout = codec_ctx->ch_layout;

    SwrContext* swr_ctx = nullptr;
    if (swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, OUTPUT_FORMAT, RATE,
        &in_ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate, 0, nullptr) < 0) {
        std::cerr << "Failed to initialize the resampling context" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    if (swr_init(swr_ctx) < 0) {
        std::cerr << "Failed to initialize the resampling context" << std::endl;
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    AudioData audio_data = { swr_ctx, fmt_ctx, codec_ctx, pkt, frame, stream_index };

    Pa_Initialize();
    PaStream* stream = initialize_pa_stream(deviceIndex);
    if (stream == nullptr) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        avformat_network_deinit();
        return 1;
    }

    PaError err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "Failed to start stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(stream);
        Pa_Terminate();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        avformat_network_deinit();
        return 1;
    }

    std::vector<uint8_t> buffer(FRAMES_PER_BUFFER * CHANNELS * av_get_bytes_per_sample(OUTPUT_FORMAT));
    std::vector<uint8_t*> buffer_ptrs(1, buffer.data());

    while (true) {
        auto start_time = high_resolution_clock::now();
        if (av_read_frame(fmt_ctx, pkt) >= 0) {
            if (pkt->stream_index == stream_index) {
                if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, frame->sample_rate) +
                            frame->nb_samples, RATE, frame->sample_rate, AV_ROUND_UP);

                        std::vector<uint8_t> buffer(dst_nb_samples * CHANNELS * av_get_bytes_per_sample(OUTPUT_FORMAT));
                        std::vector<uint8_t*> buffer_ptrs(1, buffer.data());

                        int ret = swr_convert(swr_ctx, buffer_ptrs.data(), dst_nb_samples,
                            (const uint8_t**)frame->data, frame->nb_samples);

                        if (ret < 0) {
                            std::cerr << "Error resampling audio" << std::endl;
                            break;
                        }

                        PaError err = Pa_WriteStream(stream, buffer.data(), ret);
                        if (err != paNoError) {
                            std::cerr << "Failed to write to stream: " << Pa_GetErrorText(err) << std::endl;
                            break;
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }
        auto end_time = high_resolution_clock::now();
        duration<double, std::milli> time_spent = end_time - start_time;
        printf("Time spent in one frame of av_read_frame: %f ms\n", time_spent.count());
    }

    err = Pa_StopStream(stream);
    if (err != paNoError) {
        std::cerr << "Failed to stop stream: " << Pa_GetErrorText(err) << std::endl;
    }
    Pa_CloseStream(stream);
    Pa_Terminate();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    avformat_network_deinit();

    return 0;
}
