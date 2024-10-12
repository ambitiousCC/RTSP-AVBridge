# RTSP-AVBridge

**RTSP-AVBridge** is an open-source project that offers a cross-platform solution for implementing virtual cameras and virtual microphones using RTSP for audio and video transmission.

| Resolution   | Frame Rate | Encoding Side (M3Pro 4-core, whole machine) | Decoding Side (Win32, whole machine) | Visual Latency (Average of three measurements) |
|--------------|------------|---------------------------------------------|--------------------------------------|------------------------------------------------|
| 640x480      | 30         | Frame time: 8~20ms; Max CPU usage: 14.5% (whole machine); Memory: 70MB | Frame time: 30~50ms; Very low CPU usage; 43MB memory | 238ms                                          |
| 1280x720     | 30         | Frame time: 8~20ms; Max CPU usage: 20.9% (whole machine); Memory: 123MB | Frame time: 30~50ms; Very low CPU usage; 45MB memory | 160ms                                          |
| 1760x1328    | 30         | Frame time: 15~25ms; Max CPU usage: 24% (whole machine); Memory: 247MB | Frame time: 30~50ms; Very low CPU usage; 61MB memory | 209ms                                          |
| 1920x1080    | 30         | Frame time: 9~25ms; Max CPU usage: 31% (whole machine); Memory: 225MB | Frame time: 30~50ms; Very low CPU usage; 59MB memory | 172ms                                          |

**Microphone usage was not tested.**

## Features

- Bypasses the signed driver requirement for virtual device construction
- Recognized by popular applications such as Tencent Meeting
- Supports reconnection and retransmission
- Ensures stable transmission of high-definition video and audio

## TODO
* [] Improve Documentation

## Setup

- **With Physical Device (Server)** (e.g., Moonlight)
  1. Download and run the `mediamtx` service
- **Without Physical Device (Client)** (e.g., Sunshine-host)
  1. For virtual camera, download and configure [Softcam](https://github.com/tshino/softcam). You can try examples of Softcam.
  2. For virtual microphone, download and configure PortAudio (optional); **Install Virtual Audio Cable (VAC) software**

## Virtual Camera

> **Implementation:** The virtual camera is constructed using a DirectShow driver, based on Softcam.

### Installation

- **With Physical Device (Server)** (Moonlight)

    ```bash
    gcc video.c -o video -I /usr/local/include -L /usr/local/lib -lavdevice -lavformat -lavcodec -lavutil -lswscale
    
    ./video -u [rtsp_url] [-w width] [-h height] [-f fps]
    ./video
    ```

- **Without Physical Device (Client)** (Sunshine-host)
    - SoftCam

### Running

- **With Physical Device (Server)** (Moonlight)
    - Start the `mediamtx` service
    - Run the binary file
- **Without Physical Device (Client)** (Sunshine-host)
    - Set up RTSP_URL
    - Build the Visual Studio project
    - Run the executable

## Virtual Microphone

> **Implementation:** Real-time audio is transmitted to a virtual speaker, which forwards the audio to the virtual microphone.

### Installation

- **With Physical Device (Server)** (Moonlight)

    ```bash
    gcc audio1.c -o audio1 -I /usr/local/include -L /usr/local/lib -lavdevice -lavformat -lavcodec -lavutil -lswscale -lswresample -lpthread
    
    ./audio
    ```

- **Without Physical Device (Client)** (Sunshine-host)
    - virtual audio cable
    - Connect the RTSP audio stream to a virtual speaker

### Running

- **With Physical Device (Server)** (Moonlight)
    - Start the `mediamtx` service
    - Run the binary file
- **Without Physical Device (Client)** (Sunshine-host)
    - Run the Python script (less stable, low latency)
    - Run the Visual Studio project (more stable, higher latency, need portaudio)

## Related Projects

- [PortAudio](https://www.portaudio.com/)
- [Softcam](https://github.com/tshino/softcam)
- [Mediamtx](https://github.com/bluenviron/mediamtx)

## License

MIT
