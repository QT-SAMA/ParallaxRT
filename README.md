# ParallaxRT: Real-time Monocular 2D-to-3D Video Conversion Filter

<p align="center">
  <strong>A high-performance, low-latency DirectShow Transform Filter for real-time 2D-to-stereoscopic 3D video conversion via Deep Learning and CUDA DIBR.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%20x64-0078D6.svg" alt="Platform">
  <img src="https://img.shields.io/badge/Language-C%2B%2B17%20%2F%20CUDA-00599C.svg" alt="Language">
  <img src="https://img.shields.io/badge/Framework-DirectShow%20Transform%20Filter-green.svg" alt="DirectShow">
  <img src="https://img.shields.io/badge/CUDA-12.8%2B%20%7C%20cuDNN%209-76B900.svg?logo=nvidia" alt="CUDA">
  <img src="https://img.shields.io/badge/Inference-ONNX%20Runtime%20GPU-blue.svg" alt="ONNX Runtime">
  <img src="https://img.shields.io/badge/Model-Depth%20Anything%20V2-purple.svg" alt="Model">
  <img src="https://img.shields.io/badge/License-MIT-lightgrey.svg" alt="License">
</p>

---

## 1. Overview

**ParallaxRT** is a high-performance DirectShow Transform Filter (`ParallaxRTFilter.ax`) engineered for the Windows multimedia ecosystem, specifically designed for seamless integration with PotPlayer and other DirectShow-compliant media players.

Positioned between the upstream video decoder and the downstream video renderer, ParallaxRT intercepts decoded video frames in real time. By coupling state-of-the-art monocular depth estimation (Depth Anything V2) with a dedicated CUDA DIBR (Depth-Image-Based Rendering) view-synthesis pipeline, it synthesizes left- and right-eye stereoscopic disparity frames on the fly, delivering immersive 3D experiences with minimal latency and zero frame desynchronization.

---

## 2. Key Technical Features

### 2.1 High-Throughput Monocular Depth Inference
- Integrates **Depth Anything V2** foundation models (supporting Small, Base, and Large variants exported to ONNX).
- Leverages the **ONNX Runtime GPU (CUDA Execution Provider)** to maximize utilization of NVIDIA Tensor Cores and CUDA Cores.
- Provides dynamic inference resolution scaling (100%, 75%, 50%, and 25%) to balance computational load and stereoscopic visual fidelity across midrange and high-end GPUs.

### 2.2 End-to-End GPU DIBR Synthesis Pipeline
- Custom-developed CUDA rendering kernels executing bilateral depth upsampling, dynamic relative depth normalization, backward-warping disparity projection, and edge occlusion hole filling (inpainting).
- Eliminates host-device memory round-trips by processing intermediate tensors and surfaces entirely in GPU VRAM, minimizing end-to-end processing latency.

### 2.3 Comprehensive Stereoscopic Output Modes
- **Half-SBS (Side-by-Side)**: Horizontally scaled stereo pairs compatible with 3D TVs, projectors, and VR/MR head-mounted displays.
- **Full-SBS**: Native full-resolution side-by-side output preserving source clarity.
- **Red-Cyan Anaglyph**: Enables stereoscopic depth perception on standard displays using standard passive complementary filters.
- **Depth Map Visualization**: Direct grayscale depth map inspection for diagnostic evaluation.
- **2D Pass-Through**: Zero-overhead hardware bypass allowing real-time comparative inspection without unregistering the filter.

### 2.4 Decoupled Asynchronous Dual-Thread Pipeline
- The DirectShow presentation pipeline and the background AI depth inference worker thread are fully decoupled.
- Features a ring buffer frame cache combined with temporal smoothing heuristics, preventing playback stutter, presentation queue underruns, and audio-video desynchronization.

### 2.5 Hardware Texture Stride & Pitch Auto-Adaptation
- Incorporates dynamic row stride adaptation specifically tailored for downstream Direct3D 11 Video Renderers.
- Automatically handles 128-byte and 64-byte GPU staging texture pitch alignment constraints, preventing diagonal skewing, striping, or color-channel offset on arbitrary non-standard resolutions (e.g., 480p, 540p, 720p, 1080p, 4K, 21:9 ultrawide, and anamorphic aspect ratios).

### 2.6 Low-Overhead Shared Memory IPC Control Panel
- Includes a dedicated native Win32 floating tray control panel communicating with the running filter via shared memory IPC.
- Allows real-time modification of operational parameters—including maximum disparity, zero-parallax convergence plane, inference downsampling scale, and stereoscopic mode—with instantaneous feedback during playback.

### 2.7 Hardware Active Shutter Synchronization (ParallaxSYNC)
- Designed for active shutter 3D systems (120Hz/144Hz displays and DLP-Link projectors).
- Includes an integrated serial COM port transmitter that outputs left/right field synchronization pulses.
- Provides open-source microcontroller firmware (`ParallaxSYNC.ino`) compatible with Arduino, ESP32, and custom IR/RF emitter circuits.

---

## 3. System Architecture

```mermaid
flowchart TD
    subgraph DirectShow_Pipeline [DirectShow Media Pipeline (PotPlayer)]
        Source[Demuxer / Source Filter] --> Decoder[Video Decoder (FFmpeg / LAV)]
        Decoder -- "NV12 / YV12 / RGB32" --> Filter["ParallaxRT Filter (.ax)"]
        Filter -- "Stereo Output (Hardware Pitch Aligned)" --> Renderer[Direct3D 11 Video Renderer]
    end

    subgraph ParallaxRT_Engine [ParallaxRT Compute Engine]
        Filter --> AsyncBuffer[Async Ring Buffer & Temporal Smoothing]
        AsyncBuffer --> WorkerThread[AI Inference Worker Thread]
        WorkerThread -- "Depth Anything V2" --> DepthData[FP32 Depth Map]
        DepthData --> CudaKernel[CUDA DIBR Synthesis Kernels]
        CudaKernel --> StereoFrame[Stereoscopic Frame]
        StereoFrame --> Filter
    end

    subgraph Control_Interface [Control & Hardware Synchronization]
        GUI[ParallaxRT Control Panel] <-->|Shared Memory IPC| Filter
        Filter -.->|RS-232 Serial Pulses| Microcontroller["ParallaxSYNC Controller (Active Glasses)"]
    end
```

---

## 4. System Requirements

| Specification | Minimum Requirement | Recommended Requirement |
| :--- | :--- | :--- |
| **Operating System** | Windows 10 (64-bit) build 1903+ | Windows 11 (64-bit) |
| **Graphics Processing Unit** | NVIDIA GeForce GTX 1060 (6GB) | NVIDIA GeForce RTX 3060 / 4060 or higher |
| **GPU Architecture** | Pascal (Compute 6.1) or higher | Ampere (Compute 8.6) / Ada Lovelace (Compute 8.9) |
| **Driver Version** | NVIDIA Display Driver $\ge$ 550.00 | Latest official Game Ready / Studio driver |
| **CUDA Environment** | CUDA 12.0+ (bundled with installer) | CUDA Toolkit 12.8 / 13.x |
| **Host Application** | PotPlayer (64-bit edition) | PotPlayer (64-bit latest release) |

---

## 5. Deployment and Configuration

### 5.1 Automated Installation
1. Download the latest installer `ParallaxRT_Setup.exe` from the [Releases](../../releases) section.
2. Run the setup wizard. The installer unpacks all necessary CUDA, cuDNN, and runtime dependencies and automatically registers `ParallaxRTFilter.ax` with the Windows COM subsystem.

### 5.2 PotPlayer Configuration
1. Launch **PotPlayer** and press `F5` to open the **Preferences** dialog.
2. In the navigation tree on the left, navigate to: **Filter** $\rightarrow$ **Filter Management**.
3. Click the **Add Registered Filter** button at the bottom right.
4. Select **`ParallaxRT Real-time 3D Filter`** from the list and click **OK**.
5. Select the newly added filter entry in the list and set its priority condition on the right to **`Always Use`**.
6. Click **Apply** and **OK** to save settings.
7. Open any video file. The ParallaxRT tray icon will appear in the Windows taskbar, allowing real-time parameter configuration.

---

## 6. Building from Source

### 6.1 Build Prerequisites
- **Operating System**: Windows 10 / 11 64-bit
- **Compiler**: Microsoft Visual Studio 2022 (MSVC v143 toolset with the "Desktop development with C++" workload)
- **Build System**: CMake 3.24 or higher
- **Compute Platform**: NVIDIA CUDA Toolkit 12.8+ and cuDNN 9+
- **Package Manager**: vcpkg
  ```powershell
  vcpkg install --triplet=x64-windows "opencv4[core,imgproc]:x64-windows"
  ```
- **Inference Engine**: ONNX Runtime GPU (v1.20+) Windows x64 binaries

### 6.2 Compilation Steps
```powershell
# 1. Clone the repository
git clone https://github.com/your-username/ParallaxRT.git
cd ParallaxRT

# 2. Configure CMake build tree (with CUDA enabled)
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
  -DPARALLAXRT_ENABLE_CUDA=ON `
  -DPARALLAXRT_ENABLE_DIRECTML=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# 3. Build Release target
cmake --build build --config Release --target parallaxrt_filter
```
Upon completion, the compiled binary `ParallaxRTFilter.ax` will be located in the `build/Release/` directory.

### 6.3 Manual COM Registration
Execute the following commands in an elevated PowerShell or Command Prompt (Run as Administrator):
```cmd
:: Register filter
regsvr32 "C:\Path\To\ParallaxRTFilter.ax"

:: Unregister filter
regsvr32 /u "C:\Path\To\ParallaxRTFilter.ax"
```

---

## 7. Hardware Shutter Synchronization (ParallaxSYNC)

For active stereoscopic display setups requiring external shutter synchronization:

1. **Firmware Flashing**: Open `ParallaxSYNC/ParallaxSYNC.ino` in the Arduino IDE and upload it to a compatible development board (e.g., Arduino Nano, Pro Micro, or ESP32).
2. **Hardware Interface**: Connect the designated digital output pin to the synchronization input of your active shutter emitter or DLP-Link driver circuit. The pin outputs a square wave where high corresponds to the left field and low corresponds to the right field.
3. **Software Activation**: In the ParallaxRT Control Panel, enable **Hardware Serial Sync** and specify the assigned serial COM port and baud rate (default: 115200 bps).

---

## 8. Repository Layout

```text
ParallaxRT/
├── CMakeLists.txt                 # Root CMake build configuration
├── CMakePresets.json              # CMake configuration presets for Visual Studio
├── LICENSE                        # Open-source license terms (MIT License)
├── README.md                      # Technical documentation
├── ParallaxSYNC/                  # Hardware shutter synchronization module
│   └── ParallaxSYNC.ino           # Arduino synchronization firmware
├── src/
│   ├── core/
│   │   ├── depth_estimator.h/.cpp # ONNX Runtime depth estimation wrapper
│   │   ├── dibr_renderer.h/.cpp   # DIBR CPU fallback and geometric routines
│   │   ├── dibr_renderer_cuda.cu  # High-performance CUDA DIBR compute kernels
│   │   ├── directshow_filter.h/.cpp # DirectShow filter pipeline & stride adaptation
│   │   ├── directshow_exports.cpp # COM class factory and registration exports
│   │   ├── filter_config.h        # Shared memory IPC configuration manager
│   │   └── tray_controller.h      # Floating tray control panel implementation
│   └── app/                       # Standalone preview and batch processing utilities
└── packaging/                     # Inno Setup scripts and installer resources
```

---

## 9. License and Acknowledgments

- **License**: Distributed under the terms of the [MIT License](LICENSE).
- **Depth Model**: Monocular depth estimation is powered by the [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2) architecture.
- **Inference Runtime**: Accelerated via [ONNX Runtime](https://onnxruntime.ai/) and the [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit).
