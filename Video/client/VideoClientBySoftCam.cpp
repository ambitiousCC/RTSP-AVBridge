extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <time.h>
}

#include <softcam/softcam.h>
#include <csignal>
#include <cstdio>
#include <vector>
#include <iostream>
#include <chrono>
#include <thread>
#include <windows.h>
#include "resource.h"

// Default parameters
const int WIDTH = 640;
const int HEIGHT = 480;
const int FPS = 30;
const char* DEFAULT_RTSP_URL = "rtsp://192.168.1.33:8554/live";

// Global variable to capture Ctrl+C interrupt signal
bool quit = false;

// Capture Ctrl+C signal
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT) {
        quit = true;
        std::printf("Ctrl+C signal received. Exiting...\n");
        return TRUE;  // Indicates that the signal has been handled
    }
    return FALSE;  // Pass the signal to the next handler
}

// Function: Initialize FFmpeg and open RTSP stream
bool init_ffmpeg(const std::string& rtsp_url, AVFormatContext*& fmt_ctx, AVCodecContext*& codec_ctx, int& video_stream_index) {
    fmt_ctx = nullptr;
    codec_ctx = nullptr;
    video_stream_index = -1;

    // Open RTSP stream
    AVDictionary* options = nullptr;
    // Parameter settings
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "max_delay", "1000000", 0);  // Reduce maximum delay to 1 second
    av_dict_set(&options, "buffer_size", "102400", 0); // Limit buffer size

    if (avformat_open_input(&fmt_ctx, rtsp_url.c_str(), nullptr, &options) < 0) {
        std::printf("Failed to open RTSP stream\n");
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::printf("Failed to retrieve input stream information\n");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Find video stream
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    if (video_stream_index == -1) {
        std::printf("Failed to find a video stream\n");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Get decoder
    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::printf("Failed to find codec\n");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        std::printf("Failed to copy codec parameters to codec context\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::printf("Failed to open codec\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    // Register Ctrl+C signal handler
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::printf("Error: Could not set control handler\n");
        return 1;
    }

    // Command line argument parsing
    std::string rtsp_url = "";
    int width = WIDTH;
    int height = HEIGHT;
    int fps = FPS;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-u" && i + 1 < argc) {
            rtsp_url = argv[++i];
        }
        else if (std::string(argv[i]) == "-w" && i + 1 < argc) {
            width = std::stoi(argv[++i]);
        }
        else if (std::string(argv[i]) == "-h" && i + 1 < argc) {
            height = std::stoi(argv[++i]);
        }
        else if (std::string(argv[i]) == "-f" && i + 1 < argc) {
            fps = std::stoi(argv[++i]);
        }
        else {
            std::printf("Usage: %s [-u rtsp_url] [-w width] [-h height] [-f fps]\n", argv[0]);
            return 1;
        }
    }

    if (rtsp_url.empty()) {
        std::printf("Usage: %s [-u rtsp_url] [-w width] [-h height] [-f fps]\n", argv[0]);
        return 1;
    }

    // Initialize FFmpeg library
    avformat_network_init();

    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    int video_stream_index = -1;

    if (!init_ffmpeg(rtsp_url, fmt_ctx, codec_ctx, video_stream_index)) {
        return 1;
    }

    // Create Softcam instance
    scCamera cam = scCreateCamera(width, height, fps);
    if (!cam) {
        std::printf("Failed to create camera\n");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    std::printf("Softcam is now active.\n");
    scWaitForConnection(cam);

    // Initialize conversion context
    SwsContext* sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        width, height, AV_PIX_FMT_BGR24, SWS_BICUBIC, nullptr, nullptr, nullptr);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    std::vector<uint8_t> buffer(av_image_get_buffer_size(AV_PIX_FMT_BGR24, width, height, 1));
    av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize, buffer.data(), AV_PIX_FMT_BGR24, width, height, 1);

    // Main loop, capture and process video frames
    while (!quit) {
        try {
            // Read video frame
            if (av_read_frame(fmt_ctx, packet) < 0) {
                throw std::runtime_error("Failed to read frame from stream");
            }

            if (packet->stream_index == video_stream_index) {
                if (avcodec_send_packet(codec_ctx, packet) == 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, codec_ctx->height,
                            rgb_frame->data, rgb_frame->linesize);
                        scSendFrame(cam, rgb_frame->data[0]);
                    }
                }
            }

            av_packet_unref(packet);
        }
        catch (const std::exception& e) {
            std::printf("Error: %s\n", e.what());

            // Close existing FFmpeg context
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&fmt_ctx);

            // Wait and attempt to reconnect
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::printf("Attempting to reconnect...\n");

            // Reinitialize FFmpeg and RTSP stream
            while (!quit && !init_ffmpeg(rtsp_url, fmt_ctx, codec_ctx, video_stream_index)) {
                std::printf("Reconnection failed, retrying...\n");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (quit) {
                break;
            }
        }
    }

    // Release resources
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    scDeleteCamera(cam);
    std::printf("Softcam has been shut down.\n");

    return 0;
}
