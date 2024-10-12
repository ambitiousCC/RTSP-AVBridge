# 2-channel 16-bit 16000Hz
# pip install ffmpeg-python pyaudio
# python last_version.py -u rtsp://192.168.1.33:8554/mic
# conda activate W:\projects\Mic-1\venv && W: && cd W:\projects\Mic-1 && python last_version.py
# ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental rtmp://192.168.1.33/mic
# Packaging: pyinstaller --onefile --icon=mic.ico last_version.py
import cv2
import ffmpeg
import pyaudio
import threading
import time
import queue
import argparse

# Global thread variable
STOP_EVENT = threading.Event()
COUNT = 0

def get_host_api_info():
    p = pyaudio.PyAudio()
    host_api_info = {}
    for i in range(p.get_host_api_count()):
        api_info = p.get_host_api_info_by_index(i)
        host_api_info[i] = api_info['name']
    p.terminate()
    return host_api_info

def list_audio_devices():
    p = pyaudio.PyAudio()
    host_api_info = get_host_api_info()
    device_list = []
    for i in range(p.get_device_count()):
        dev = p.get_device_info_by_index(i)
        host_api_name = host_api_info[dev['hostApi']]
        device_list.append((i, dev['name'], dev['maxInputChannels'], host_api_name))
    p.terminate()
    return device_list

def select_device():
    devices = list_audio_devices()
    choice = -1
    for index, (i, name, channels, host_api) in enumerate(devices):
        if (name.find("Ruming Speaker") != -1 and host_api.find("WASAPI") != -1):
            # print(f"{index}:{host_api}") # The API of the speaker and microphone needs to match
            choice = index
            break # WASAPI has the lowest latency, but compatibility is poor. However, our provided virtual microphone does not need to consider this.
    if (choice == -1):
        print("Virtual speaker device not found")
        return -1
    selected_device = devices[choice][0]
    return selected_device

# Capture audio
def capture_audio(rtsp_url, channels, rate, transport_type):
    # Use ffmpeg to capture RTSP stream audio and output it as WAV format
    process = (
        ffmpeg
        .input(rtsp_url, f='rtsp', rtsp_transport=transport_type)
        .output('pipe:', format='wav', acodec='pcm_s16le', ac=channels, ar=rate)
        .run_async(pipe_stdout=True, pipe_stderr=True)
    )
    return process

# Play audio
def play_audio(virtual_output_device_index, audio_format, channels, rate, chunk, rtsp_url, transport_type):

    # Initialize PyAudio
    p = pyaudio.PyAudio()

    # Open the virtual speaker output stream
    try:
        stream = p.open(format=audio_format,
                        channels=channels,
                        rate=rate,
                        output=True,
                        output_device_index=virtual_output_device_index) # The key is to select the virtual speaker
    except OSError:
        print("Unable to open virtual device")
        return

    q = queue.Queue()
    read_thread = threading.Thread(target=read_from_process, args=(q,rtsp_url, channels, rate, chunk, transport_type))
    read_thread.start()

    start_time = time.time()
    timeout = 1 # Timeout duration
    global COUNT
    try:
        while True:
            if STOP_EVENT.is_set():
                break
            try:
                # Check if there is data in the queue, with a timeout of 0.1 seconds
                in_bytes = q.get(timeout=0.1)
                
                if isinstance(in_bytes, Exception):
                    raise in_bytes
                if (COUNT > 0):
                    print("Reconnected successfully ===Time taken===> ")
                COUNT = 0
                # Process the data
                stream.write(in_bytes)
                start_time = time.time()  # Reset the start time
            except queue.Empty:
                # If the queue is empty and the timeout has been exceeded
                if time.time() - start_time > timeout:
                    break
    except KeyboardInterrupt:
        STOP_EVENT.set() # Only the keyboard can trigger this
        print("Keyboard interrupt caught, exiting... Please do not close the window immediately")
    finally:
        if STOP_EVENT.is_set():
            read_thread.join() # Only wait for the thread to end if it's set
        stream.stop_stream()
        stream.close()
        p.terminate()

def read_from_process(q, rtsp_url, channels, rate, chunk, transport_type):
    # Capture audio
    process = capture_audio(rtsp_url, channels, rate, transport_type)

    try:
        while not STOP_EVENT.is_set(): # If the thread has not been interrupted
            in_bytes = process.stdout.read(chunk * channels * 2)
            if not in_bytes:
                break
            q.put(in_bytes)
    except Exception as e:
        q.put(e)

def main(args):
    # Let the user select the virtual speaker device
    virtual_output_device_index = select_device()
    if virtual_output_device_index == -1:
        print("No suitable virtual device found, please install supported virtual devices")
        return
    global COUNT
    try:
        print("Connecting virtual audio")
        while True:
            play_audio(virtual_output_device_index, args.fmt, args.ch, args.rt, args.ck, args.url, args.transport_type)
            if STOP_EVENT.is_set():
                print("You can close the window to exit")
                return # If it is 'break' here, it will hang, suspected to be because the thread has not been closed, or it could be a language-level bug
            else:
                print(f"Network exception, attempting to reconnect... {COUNT} seconds", end='\r')
                COUNT += 1
    except KeyboardInterrupt:
        print("Keyboard interrupt caught, exiting......")
        if not STOP_EVENT.is_set():
            STOP_EVENT.set()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Local microphone device access program")
    parser.add_argument("-u", "--url", type=str, required=True, help="RTSP input stream URL")
    parser.add_argument("-t", "--transport_type", type=str, default="tcp", help="RTSP input stream transport type: udp or tcp")
    parser.add_argument("-f", "--fmt", type=int, default=pyaudio.paInt16, help="Audio format")
    parser.add_argument("-c", "--ch", type=int, default=2, help="Number of audio channels")
    parser.add_argument("-r", "--rt", type=int, default=48000, help="Audio sampling rate")
    parser.add_argument("-k", "--ck", type=int, default=512, help="Audio chunk size")
    
    args = parser.parse_args()
    main(args)
