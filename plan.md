# Latency Lab — An End-to-End Low-Latency Video Pipeline

A learning project VR headsets, cloud gaming services, and real-time video systems deliver frames with minimal delay.

The idea is to build a tool that generates video frames, encodes them, sends them over a simulated network, decodes them, and **measures exactly how long every step took**. Every setting will be exposed as a CLI flag so I can flip the switches and instantly see what happens to latency and quality.

---

## The Core Measurement Trick

Each generated frame has a timestamp burned into the pixels — a large readable number in one corner and a small machine-readable pattern (like a barcode or row of pixels acting as binary to show frame count) in another. When the receiver decodes the frame, it reads that timestamp and computes true end-to-end latency:

```
latency = time_now - timestamp_in_pixels
```

This includes encoding, network transfer, decoding, and any buffering.

---

## Architecture

```
┌─────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌─────────┐
│ Generate│ → │  Encode  │ → │  Send    │ → │ Simulate │ → │  Decode  │ → │ Measure │
│ frames  │   │ (tunable)│   │  (UDP)   │   │  network │   │ (tunable)│   │ & show  │
│ + stamp │   │          │   │          │   │ loss/jit │   │          │   │ results │
└─────────┘   └──────────┘   └──────────┘   └──────────┘   └──────────┘   └─────────┘
     │              │              │              │              │              │
     └──────────────┴──────────────┴──────────────┴──────────────┴──────────────┘
                        Per-stage timing logs (every frame, every stage)
```

---

## Why C for This Project

Cause I want to see everything and do it all MANUALLY 0_0

---

## Manual Data Structures I'll Build

Manual data structures to add cause I am a masochist.

#### Ring Buffers

Used for:
- Jitter buffer
- Every inter-stage frame queue

Used to teach:
- Power-of-two sizing and index masking
- Full vs empty detection
- False sharing between producer/consumer threads
- Overwrite policy when full

#### Dynamic Arrays

Used for:
- RTP packet descriptors
- Latency sample logs

Used to teach:
- Growth factors (1.5x vs 2x)
- Amortized cost
- Pre-allocation so `realloc` never fires in the hot path

#### Hash Maps

Used for:
- Sequence-number tracking for FEC
- Retransmission

Used to teach:
- Open addressing vs chaining
- Hashing sequential keys without clustering
- Load factor
- Tombstones
- Never rehash mid-frame

#### Pool Allocators

Used for:
- Frame buffers
- Packet buffers

Used to teach:
- Fixed-size free lists
- Pre-allocation
- Getting `malloc` completely out of the per-frame path

#### Tagged Unions

Used for:
- Pipeline messages: frame vs control vs stats

Used to teach:
- Sum types in C — a struct with a tag enum and a union payload

#### Intrusive Lists

Used for:
- Free lists
- Stage chaining

Used to teach:
- `container_of` style embedding
- Linking without extra allocations

---

## C Crutches

### `goto cleanup` everywhere

Single-exit error handling. Every init function returns 0 or -1 and unwinds only what succeeded, in reverse order. FFmpeg and the Linux-kernel  use this pattern and it beats nested `if` pyramids

```c
int pipeline_init(struct Pipeline *p) {
    p->encoder = avcodec_alloc_context3(codec);
    if (!p->encoder) goto fail;

    p->frame = av_frame_alloc();
    if (!p->frame) goto fail;

    p->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->sock < 0) goto fail;

    return 0;

fail:
    if (p->sock >= 0) close(p->sock);
    av_frame_free(&p->frame);
    avcodec_free_context(&p->encoder);
    return -1;
}
```

### Function-pointer structs for pipeline stages

Each stage is a `struct` with `init`, `process`, and `destroy` function pointers. That gives me polymorphism without vtables, and every dispatch is visible in the code.

### Compiler help

- `_Static_assert` for wire-format sizes and struct layouts
- `restrict` on hot-path pointers so the compiler can optimize copies
- `__attribute__((aligned(64)))` on ring buffer head/tail to avoid false sharing
- `__attribute__((packed))` on RTP headers

### X-macros for repetitive boilerplate

One list of parameters generates the enum, the string names, the CLI parser, and the switch statement. Keeps them from drifting apart as I add codec flags.

### Build flags from day one

`-Wall -Wextra -Werror -std=c11` for normal builds, `-O2` when measuring, `-O0 -g` when debugging. Add sanitizers (`-fsanitize=address,undefined`) early — in C they catch the bugs that would otherwise become mysterious latency spikes.

### Ownership in comments

I'll write down who frees what. In C this is a convention, not a type. A one-line comment above every buffer-passing function ("takes ownership of `pkt`") saves me hours later.

---

## Phase 1 — Raw Frames Over UDP (No Compression)

My goal is to feel the fundamental problems and raw performance before adding codecs and why it sucks.

### What I'll build
- A sender that generates:
    - Synthetic frames at a fixed rate (e.g., 60 FPS)
    - Draw a moving pattern
    - Frame number
    - Embedded timestamp on each frame
- Send raw RGB frames as UDP datagrams to a receiver on localhost
    - UDP cuase if a frame gets skipped its fine in the grand scheme where as TCP/connection oriented protocols are too slow
- Receiver reads the timestamp from the decoded pixels and computes latency
- Add configurable network impairment: fixed delay, jitter (random delay variation), and packet loss (drop N% of datagrams)
- Implement a simple **jitter buffer** on the receiver — just a queue that holds frames for a configurable amount of time before displaying. If there is jitter, the buffer tanks it. Like a UPS tanks dirty power for a PC
- Build a **ring buffer** first and use it as the frame queue between generator and sender. It is also the skeleton of my jitter buffer
- Build a **dynamic array** to log latency samples for the percentile calculations later

### Experiments 
- I'll mess with the jitter variable and test 0ms, 5ms, 20ms, etc
- I'll turn on 5% packet loss and see what a dropped frame looks like
- I'll change jitter buffer size from 0 to 10 frames and find the sweet spot between smoothness and delay
    - I'll write an algorithm to automatically, in flight, adjust the buffer size
- I'll calculate the bandwidth: `width × height × 3 bytes × FPS`. For 1920×1080 at 60 FPS that's ~373 Mbps — clearly impossible over Wi-Fi. This motivates compression.

### Learning Goals
- Frame pacing
- UDP basics
- Why packet loss and jitter matter
- How buffering trades latency for smoothness
- Ring buffers and dynamic arrays, and why allocation in the hot path is the enemy

---

## Phase 2 — Software Video Encoding

My goal is to understand codec parameters and their direct effect on latency, bitrate, and quality.

### What I'll build
- Before touching the codec, build a **pool allocator** so frames and packets come from pre-allocated fixed-size buffers instead of `malloc`
- Add H.264 encoding using **libx264** through FFmpeg's C API (`libavcodec`). 
- Expose command-line flags for:
  - `preset` (ultrafast → medium)
  - `tune` (zerolatency vs default)
  - `b-frames` (0, 2, 3)
  - `lookahead` (0, 10, 40)
  - `vbv-bufsize` (small vs large)
  - `gop-size` (how often keyframes appear)
  - `bitrate`
- Add H.265 (x265) and optionally AV1 (SVT-AV1 or libaom) as alternative codecs
- Log encode time per frame and end-to-end latency
- Compute **PSNR** or **SSIM** between the original and received frames (ffmpeg filters or `libvmaf`)

### Experiments 
I'll produce a comparison table like this:

| Config | Avg latency | P99 latency | Bitrate | Quality (PSNR) |
|--------|-------------|-------------|---------|----------------|
| ultrafast, 0 B-frames, no lookahead | 12 ms | 18 ms | 20 Mbps | 32 dB |
| veryfast, 2 B-frames, 10 lookahead | 28 ms | 41 ms | 12 Mbps | 35 dB |

Questions I'll answer: 
- Which setting had the biggest effect on latency? 
- What did B-frames cost? 
- How small can the VBV buffer go before quality collapses?

### Learning Goals
- The concrete, measurable relationship between encoder settings and the latency/bitrate/quality triangle. Core skills
- Buffer pooling, and why per-frame `malloc` shows up as latency spikes

---

## Phase 3 — Hardware Encoding and Decoding

I want to learn the hardware APIs that make real-time VR possible.

### What I'll build
- Add a hardware encode path. Pick based on my hardware:
  - NVIDIA GPU → **NVENC** (via CUDA Video SDK or FFmpeg's `h264_nvenc` / `hevc_nvenc` / `av1_nvenc`)
  - Intel/AMD → **oneVPL** or **VA-API** (via FFmpeg's `h264_qsv`, `h264_vaapi`)
  - Apple Silicon → **VideoToolbox** (via FFmpeg's `h264_videotoolbox`)
- Tune for low latency: P-only mode, CBR rate control, small VBV buffer, minimal encoder pipeline depth
- Add hardware **decoding** with zero-copy to GPU memory. Use CUDA interop, VA-API/EGL, D3D12 video surfaces, or VideoToolbox pixel buffers depending on platform
- Measure: software vs hardware encode time, software vs hardware decode time, CPU usage

### Experiments 
- I'll compare x264 ultrafast vs NVENC at the same bitrate — latency, quality, CPU load.
- I'll measure how much time the zero-copy path saves compared to `decode → copy to RAM → copy to GPU`
- I'll try encoding 4K and see how much longer software takes relative to hardware. I estimate 4k no?

### What I'll learn
Why hardware acceleration is mandatory for VR, how to drive hardware codecs, and the memory architectures that avoid wasteful copies.

---

## Phase 4 — Realistic Transport (RTP, WebRTC)

I want to understand what real transport layers do beyond sending raw UDP packets.

### What I'll build
- Replace raw UDP with **RTP packetization**. Implement basic RTP headers myself or use a library like `libSRTP`
- Add **RTCP feedback**: receiver reports packet loss and jitter back to sender
- Implement **adaptive bitrate**: if loss > threshold, lower the encode bitrate; if stable for N frames, raise it
- Add **FEC (Forward Error Correction)**: simple XOR parity across groups of packets. Compare against doing nothing and against retransmission
- Build a **hash map** keyed by RTP sequence number so the receiver can tell which packets arrived and which need help. This is where open addressing, load factor, and "never rehash mid-frame" become real constraints
- Optional stretch: build a version using **libwebrtc** or **WebRTC data channels** to see a production-grade stack

### Experiments
- I'll turn on 3% loss and compare: no protection vs FEC vs retransmission. 
    - I'll measure effective latency and visible artifacts.
- I'll ramp network jitter up and down and watch the adaptive bitrate algorithm react.
    - I'll build a randomized jitter profile
- I'll plot bitrate over time alongside the imposed network conditions.

### Learning Goals
- Congestion control, adaptive bitrate, FEC, and the feedback loops that keep streaming stable on real networks
- Hash maps under real-time constraints

---

## Phase 5 — VR-Specific Processing

I want to touch the techniques that matter specifically for near-eye displays.

### What I'll build
- **Foveated / ROI encoding:** simulate a gaze point, generate a region-of-interest map, and pass it to the encoder (x264 supports per-macroblock QP offsets; NVENC has ROI). Measure quality in the fovea vs periphery at the same total bitrate
- **Simplified timewarp (reprojection):** intentionally delay a frame by one frame period, then apply a synthetic "head rotation" warp to the stale frame so it matches the latest pose. Show how this hides latency
- **Lens distortion correction:** apply a radial warp to the decoded frame — the same kind of correction a headset compositor does. Understand what "pre-distortion" means
- **Super-resolution:** encode at half resolution and upscale on the receiver using bicubic or an FSR-style shader. Compare quality and bitrate savings

### Experiments
- At a fixed bitrate, I'll compare uniform encoding vs ROI encoding and look at text/detail in the fovea
- With a deliberate 1-frame delay, I'll show side-by-side: with and without reprojection
- I'll compare half-res + upscale vs full-res in terms of total pipeline latency and quality

### Learning Goals
Foveation, reprojection, and the perceptual tricks that make VR feel responsive even when raw numbers aren't perfect.

---

## Phase 6 — Polish and Analysis

### What I'll build
- A simple terminal dashboard or small GUI showing live latency graph, current bitrate, loss rate, and quality estimate
- A batch-mode "parameter sweep": run 50+ configurations automatically and output CSV results
- Generate plots (matplotlib, gnuplot, or a notebook) showing the relationships I discovered
- Write a short findings report: which settings mattered most, where bottlenecks appeared, what I'd do differently in a real headset

### Learning Goals
How to present engineering results, how to automate measurement, and how to reason about a complex system holistically.

---

## Target Technology Stack

#### C11
Everyone uses it

#### Codec Libraries - FFmpeg/libav + x264/x265/SVT-AV1
Universal, well-documented, exposes all parameters

#### Hardware Codecs - AMD Based
I have AMD hardware so...

#### Networking - BSD(UDP) Sockets & WebRTC
Simple to wtf pipeline

#### Image Processing - GLSL/Vulkan 0_0
Standard

#### Data Structures
Hand rolled cause masochism

#### Build System
Oh god don't make me use CMake (Makefiles to start maybe)

#### Logging & Measurements
Custom embedded timestamps

---

## Where to Learn More

### Open-source projects to study
- **ALVR** — wireless VR streaming (closest reference to this project)
- **Moonlight / Sunshine** — game streaming with excellent low-latency design
- **Monado** — open-source OpenXR runtime with compositor and reprojection
- **FFmpeg source** — especially `libavcodec` for encoder/decoder internals
- **x264 source** — especially `encoder/` for motion estimation and rate control

### Standards and APIs
- ITU-T / ISO/IEC — H.264, H.265, H.266 specifications
- AOMedia — AV1
- IETF — RTP, RTCP, QUIC, WebRTC RFCs
- Khronos — Vulkan Video, OpenXR
- FFmpeg documentation — `ffmpeg-libavcodec`, `ffmpeg-libavformat`
