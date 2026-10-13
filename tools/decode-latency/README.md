# decode_latency

Measures per-frame hardware decode latency outside Moonlight. The same clip can be run through
several decode APIs on Windows and Linux, and the numbers are directly comparable.

The harness feeds one access unit at a time at a stream-like rate (60 fps by default). For each
frame it records:

- `submit -> frame returned`: from submitting the frame until FFmpeg hands back a decoded frame.
- `frame returned -> gpu done`: the extra wait until the GPU has actually finished writing that
  frame. A returned hwaccel frame can still be in flight.
- `total`: from submit until the GPU has finished. Compare APIs on this one.

How each API is waited on:

| hwaccel | wait |
|---|---|
| d3d11va | copy a 64x64 corner of the decoded slice to staging, then Map |
| d3d12va | the frame's fence |
| amf (`hevc_amf`) | same as d3d11va, from every slice of the surface's texture |
| vulkan | the frame's timeline semaphores (`vkWaitSemaphores`) |
| vaapi | `vaSyncSurface` |
| dxva2, none | no wait (only the returned time is meaningful) |

## Build

Open `CMakeLists.txt` in Qt Creator (it's standalone, not part of moonlight-qt).

**Windows:** set `FFMPEG_DIR` to an FFmpeg 8 shared dev package that was built with AMF, e.g.
BtbN's `ffmpeg-n8.0-latest-win64-gpl-shared-8.0.zip`. The bundled `libs/windows` FFmpeg has
no AMF decoders.

For the AMF wait, also set `AMF_INCLUDE_DIR`. Clone https://github.com/GPUOpen-LibrariesAndSDKs/AMF,
copy `amf/public/include` to `<dir>/AMF`, and set `AMF_INCLUDE_DIR=<dir>`. Without it,
`hevc_amf` still runs, but its gpu column is 0.

Vulkan support turns on automatically if the Vulkan SDK is found.

**Linux:** needs the FFmpeg dev packages (and libva-dev for the VAAPI wait). Vulkan support turns
on if the Vulkan headers are found.

## A clip

Use the same file on both OSes. It should look like a Moonlight stream: 4K, HEVC, no B-frames,
long GOP, at your usual bitrate. Grain drives the bitrate up the way game content does:

```
ffmpeg -f lavfi -i testsrc2=size=3840x2160:rate=60 -vf noise=alls=12:allf=t -t 20 ^
  -c:v hevc_amf -usage ultralowlatency -rc cbr -b:v 80M -maxrate 80M -g 600 -an clip4k.hevc
```

(or on Linux: `-c:v libx265 -b:v 80M -x265-params bframes=0:keyint=600`)

Check the bitrate the harness prints on its `clip` line.

## Run

```
decode_latency --list clip4k.hevc                    # what this FFmpeg can do
decode_latency --hwaccel d3d11va clip4k.hevc
decode_latency --hwaccel d3d12va clip4k.hevc
decode_latency --hwaccel vulkan  clip4k.hevc
decode_latency --hwaccel amf     clip4k.hevc          # hevc_amf, low-latency mode
decode_latency --hwaccel amf -o decoder_mode=regular -o lowlatency=0 clip4k.hevc
decode_latency --hwaccel d3d11va --fps 0 clip4k.hevc  # back to back
```

Linux: `--hwaccel vaapi` and `--hwaccel vulkan`.

`--fps 0` submits each frame as soon as the last one has finished. If that is much faster per
frame than `--fps 60`, the GPU is probably clocking down between frames: a power-management
problem, not an API one.

`--csv frames.csv` writes one row per frame.
