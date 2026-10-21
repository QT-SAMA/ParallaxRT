# ParallaxRT

**Real-time 2D-to-3D video conversion, web playback, and offline export for Windows.**

![Version](https://img.shields.io/badge/version-3.0.0-00bfd0)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6)
![Language](https://img.shields.io/badge/language-C%2B%2B17-00599C)
![Backends](https://img.shields.io/badge/inference-TensorRT%20%7C%20CUDA%20%7C%20DirectML%20%7C%20CPU-76B900)

ParallaxRT uses Depth Anything V2 to estimate depth and synthesize stereoscopic views from ordinary 2D video. Watch through a DirectShow player, stream to a phone or compatible VR player on your local network, or export a converted video for later playback.

## Features

- **Real-time conversion:** DirectShow filter integration for 64-bit PotPlayer and other compatible players.
- **GPU acceleration:** TensorRT and CUDA on compatible NVIDIA GPUs, DirectML on compatible AMD and Intel GPUs, and CPU fallback.
- **Three depth models:** Depth Anything V2 Small, Base, and Large. Small is included; Base and Large download on demand.
- **Stereo output:** Side-by-side and top/bottom layouts, full and half stereo options, anaglyph preview, depth map visualization, and ordinary 2D playback.
- **Adjustable depth:** Disparity, convergence, detail enhancement, temporal smoothing, eye swapping, and processing resolution controls.
- **Built-in web player:** Browse videos on the host PC, switch between ordinary playback and AI conversion, seek, and adjust stereo presentation. The browser remembers the last opened directory.
- **LAN streaming:** QR-code access, HTTP/HLS playback, and DLNA/UPnP discovery for compatible external players.
- **Offline export:** Convert 2D video to stereoscopic MP4 without requiring real-time processing speed, with optional audio preservation.
- **Unified interface:** Cyan–red styling, animated page transitions, hover effects, tooltips, and English/Simplified Chinese interfaces.
- **Managed environments:** Hardware detection, automatic downloads of missing acceleration components, alternate download sources, and integrity verification.
- **Local diagnostics:** Crash reports, hardware details, and runtime logs with an option to open the report folder.
- **ParallaxSYNC:** Optional serial shutter synchronization and microcontroller firmware for compatible hardware setups.

## Download and installation

Download the latest Windows x64 installer from this repository's **Releases** page and run it. The installer registers the DirectShow filter and creates a shortcut for the main application.

The basic installation includes:

- The main application, DirectShow filter, export/streaming tools, and crash reporter.
- The Depth Anything V2 Small ONNX model for offline conversion.
- FFmpeg and ffprobe for video probing, encoding, and streaming.
- Essential application runtime libraries.

Large CUDA, cuDNN, TensorRT, and DirectML packages are downloaded separately as needed. Users do not need to install the CUDA Toolkit or configure a system FFmpeg path.

On startup, ParallaxRT checks the available hardware and missing environments. Allow the required downloads to finish, then restart the application when prompted. Close and reopen any external player using the filter as well.

## Hardware and acceleration

Windows 10/11 **64-bit** is required. A compatible GPU and a recent vendor driver are recommended for real-time conversion.

| Hardware | Automatic acceleration path |
| --- | --- |
| Compatible NVIDIA RTX | TensorRT, then CUDA if TensorRT preparation fails |
| Other supported NVIDIA GPUs | CUDA |
| Compatible AMD / Intel GPUs | DirectML |
| No usable GPU backend | CPU |

The current TensorRT path requires an RTX GPU with compute capability 7.5 or newer and a driver reporting CUDA 12.9 support or later. GPU name alone does not guarantee successful engine construction. DirectML accelerates depth inference; CUDA-specific rendering optimizations require NVIDIA hardware.

Performance depends on the model, video resolution, GPU, available memory, and playback settings. Small is the default starting point. Base and Large require more resources; no fixed frame rate is guaranteed. CPU conversion remains available offline, but may be slower than real time.

### TensorRT first-time preparation

When AI conversion first uses a model without a validated engine cache, ParallaxRT displays a preparation window before continuing playback or export.

- Displays the current stage and elapsed time. The underlying build does not expose a reliable overall percentage.
- Builds in a separate process and validates the engine with a warm-up inference.
- Reuses the engine cache on subsequent sessions with the same configuration.
- Allows cancellation and falls back to another available backend on failure.

The first build may take several minutes. Changing the model, driver, GPU, input dimensions, or cache version can require another build.

## Quick start

### Play through PotPlayer

1. Install the 64-bit version of PotPlayer and ParallaxRT.
2. Open PotPlayer preferences (`F5`) and locate **Filter Control / Filter Priority (Overall)**. Labels may vary by player version or language.
3. Add the registered **ParallaxRT** filter and set it to **Prefer / Always Use**.
4. Open a video and use the ParallaxRT control panel to choose the model, stereo format, and depth settings.

The player must actually load the filter for the controls to affect playback. Select the stereo layout supported by your display or headset; anaglyph viewing requires matching glasses.

### Play on a phone or VR device

1. Open **LAN streaming** in the main application.
2. Select the network adapter and port, then start the service.
3. Connect the playback device to the same local network and scan the QR code or open the displayed address.
4. Browse the PC's folders, select a video, and choose the playback/3D settings in the web player.

The PC performs AI conversion and remains running during playback. Local playback through PotPlayer is not required. Compatible external players can also use the streaming address or DLNA discovery. Allow the service through Windows Firewall if another device cannot connect.

### Export a 3D video

1. Open **Export 3D video**.
2. Choose a source file or use the current PotPlayer source, then select the output location.
3. Select the model, stereo format, processing size, and audio option.
4. Start the export and wait for completion.

Export produces SDR H.264 MP4. Variable-frame-rate input is converted to a fixed frame rate; subtitles, chapters, and HDR metadata are not preserved.

## Models and downloads

Open **Advanced settings → Model and acceleration environment downloads** to manage models and runtimes. Installed entries provide access to their associated folders; missing entries offer downloads.

- Use **Prefer mirrors / alternate sources** when the primary route is slow.
- Use **Custom download sources** to configure matching package URLs. The generated configuration file documents resource IDs and the expected format.
- Packages are checked against fixed sizes and SHA-256 hashes. Custom sources must provide the expected files.
- Interrupted downloads can resume when the server supports it.

The installer is smaller because GPU environments are optional; downloading complete acceleration environments can still require several gigabytes of disk space.

## Settings, cache, and logs

| Data | Location |
| --- | --- |
| Main user settings | `HKEY_CURRENT_USER\Software\ParallaxRT\Filter` |
| Bundled Small model | Installation directory |
| Downloaded models | `<installation directory>\parallaxrt-data\models` |
| Downloaded runtimes | `<installation directory>\parallaxrt-data\runtime` |
| Bundled FFmpeg tools | `<installation directory>\tools\ffmpeg` |
| Custom download sources | `<installation directory>\parallaxrt-data\download-sources.txt` |
| TensorRT engine cache | `%LOCALAPPDATA%\ParallaxRT\EngineCache` |
| Diagnostic logs | `%LOCALAPPDATA%\ParallaxRT\Logs` |

Diagnostic storage is limited to **100 MB total and at most five session folders**. Reports are stored locally and are **not uploaded automatically**. You can send the relevant files to the developer manually.

If TensorRT preparation fails, retain `failed.txt` from the affected engine directory and the corresponding `runtime.log` when reporting the issue. To retry a failed configuration, stop playback, close the application/player, remove that configuration's cache folder, and restart.

## Build from source

### Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload and Windows SDK.
- CMake 3.24 or newer.
- OpenCV (`core`, `imgproc`) and ZLIB, discoverable through vcpkg or another configured package installation.
- ONNX Runtime 1.24.4 Windows x64 headers and base runtime files.
- CUDA Toolkit 12.8 for the CUDA-enabled build.
- The bundled Small model at the repository root and the pinned FFmpeg 8.1.2 essentials resources expected by `packaging/ffmpeg_bundle.cmake`.

`CMakePresets.json` includes Visual Studio x64 and CUDA + DirectML configurations. Adjust its local Visual Studio/vcpkg, CUDA, and dependency paths for your machine before configuring. The presets use an existing `vcpkg_installed` tree rather than automatically installing dependencies.

```powershell
# Configure the full GPU-enabled application.
cmake --preset vs2022-cuda-x64

# Build the main application and its required tools.
cmake --build --preset vs2022-cuda-x64-release --target parallaxrt_app
```

Output is written to `out/build/vs2022-cuda-x64/Release`. Keep the matching executables, engine/filter DLLs, runtime libraries, model, web assets, and tools together when updating an installation.

To create the installer, install **Inno Setup 6**, check the local tool paths in `packaging/build_installer.bat`, and run:

```powershell
.\packaging\build_installer.bat
```

The script builds the Release application, verifies the installation payload, and creates the installer in `dist`.

### Repository layout

```text
src/
  app/           Main application
  core/          Inference backends, rendering, filter, settings, and downloads
  playback/      Processing and playback sessions
  stream/        LAN server and web player
  export/        Offline conversion
  diagnostics/   Logging and crash reporting
packaging/       Installer scripts and resource verification
ParallaxSYNC/    Optional shutter synchronization firmware
tests/           Validation utilities and tests
```

## Reporting issues

Include the application version, Windows version, GPU and driver, selected model/backend, playback or export mode, and steps to reproduce. Attach the relevant local logs and TensorRT failure details when applicable. Review diagnostic files before sharing them, as they can contain local paths and hardware information.

## License and acknowledgments

ParallaxRT source code is distributed under the [MIT License](LICENSE). Third-party models and runtime components retain their own licenses.

- [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2#license): Small uses Apache-2.0; Base and Large use CC-BY-NC-4.0.
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) for inference execution.
- NVIDIA CUDA and TensorRT, and Microsoft DirectML, for GPU acceleration.
- [OpenCV](https://opencv.org/) for image processing.
- [FFmpeg](https://www.ffmpeg.org/legal.html) for media processing. The bundled essentials build identifies its license as GPLv3; its license and source information are included with the tools.

The project's MIT license does not replace the licensing terms of downloaded models or bundled third-party components.
