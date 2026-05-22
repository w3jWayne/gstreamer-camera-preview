# Linux Userspace Camera App

A compact C++17 GStreamer userspace camera preview app for Linux and WSL2.
This project was created to learn and explore Linux userspace camera development using GStreamer and V4L2, including USB UVC camera integration, pixel format handling, preview pipelines, and frame capture.

## Architecture

The app selects one of three simple pipelines:

**Fake mode**

```text
videotestsrc
  -> videoconvert
  -> autovideosink
```

**MJPEG mode**

```text
v4l2src
  -> jpegdec
  -> videoconvert
  -> autovideosink
```

**YUYV mode**

```text
v4l2src
  -> videoconvert
  -> autovideosink
```

Notes:

- `v4l2src` reads frames from `/dev/video0`
- `jpegdec` decodes MJPEG compressed frames
- `videoconvert` handles pixel format and color conversion
- `autovideosink` selects a suitable display sink automatically

## Learning Notes

### Why GStreamer is used

GStreamer provides a higher-level multimedia pipeline than raw V4L2 access. It handles element linking, runtime state changes, bus messages, and format negotiation so the app can focus on pipeline selection instead of manual buffer and threading management.

### How the fake pipeline works

The fake pipeline uses `videotestsrc` to generate synthetic frames, `videoconvert` to ensure the frames match what the sink expects, and `autovideosink` to display them. This is useful for testing the preview path without requiring a physical camera.

### How the MJPEG pipeline works

In MJPEG mode, `v4l2src` reads compressed JPEG frames from the camera. The app requests `image/jpeg` caps at the desired resolution and frame rate, then passes the stream through `jpegdec` before `videoconvert` and the display sink.

### How the YUYV pipeline works

In YUYV mode, `v4l2src` produces raw video frames instead of compressed JPEG data. The app requests `video/x-raw` with `YUY2` format, then uses `videoconvert` to adapt the frames to whatever the sink needs before display or file output.

### Why `jpegdec` is needed for MJPEG

MJPEG is compressed JPEG video, so the display pipeline cannot consume it directly as raw frames. `jpegdec` decompresses each frame into raw video that downstream elements can process.

### Why `videoconvert` is needed

`videoconvert` bridges format mismatches between the camera output and the next element in the chain. It handles pixel-format and color-space conversion so the sink or encoder receives data in a compatible layout.

### What the bus/message loop does

The pipeline bus carries runtime messages such as `ERROR` and `EOS` back to the application. The main loop waits for those messages so the app can stop cleanly, report failures, and exit the one-shot capture path when the stream reaches end-of-stream.

### Preview mode vs capture mode

Preview mode builds a continuous pipeline that renders frames to a video sink. Capture mode builds a one-shot pipeline that sends a single frame to `filesink`, then exits after `EOS` is received. The capture path is used for saving still images without changing the preview behavior.

## Tested Environment

- WSL2 Ubuntu
- `usbipd-win`
- Logitech C270 USB webcam
- GStreamer 1.x
- V4L2 (`/dev/video0`)

## Capture Example

Captured using Logitech C270 on WSL2

![Capture Example](docs/capture_example.jpg)

Additional screenshots and examples live in `docs/`.

## Build

```bash
sudo apt install build-essential cmake pkg-config libgstreamer1.0-dev
mkdir build && cd build
cmake ..
make
```

## Run

Fake source:

```bash
./camera_app --source fake
```

Real camera MJPEG:

```bash
./camera_app --source v4l2 --device /dev/video0 --format mjpeg --width 1280 --height 720
```

Real camera YUYV:

```bash
./camera_app --source v4l2 --device /dev/video0 --format yuyv --width 640 --height 480
```

Capture MJPEG frame:

```bash
./camera_app --source v4l2 --device /dev/video0 --format mjpeg --capture frame.jpg
```

Capture YUYV frame:

```bash
// Captured JPEG files are saved using GStreamer multifilesink.

./camera_app --source v4l2 --device /dev/video0 --format yuyv --capture frame.jpg
```

If `autovideosink`, `videotestsrc`, `v4l2src`, `jpegdec`, or `jpegenc` are missing at runtime, install the usual GStreamer plugin packages for your Ubuntu image.

## Notes

MJPEG frame capture uses:

v4l2src -> jpegparse -> multifilesink

`jpegparse` is required to properly parse MJPEG frame boundaries,
while `multifilesink` reliably saves individual camera frames as JPEG files.