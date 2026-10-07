# ParallaxRT: Real-time Monocular 2D-to-3D Video Conversion Filter

<p align="center">
  <img src="src/core/app_icon.png" alt="ParallaxRT Logo" width="120" height="120">
</p>

<p align="center">
  <strong>基于深度学习与 CUDA DIBR 的低延迟实时 DirectShow 2D 转 3D 视频滤镜</strong>
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

## 1. 项目概述 (Overview)

ParallaxRT 是一个专为 Windows 平台设计的高性能 DirectShow 转换滤镜（Transform Filter，`ParallaxRTFilter.ax`），主要用于与 PotPlayer 及其他兼容 DirectShow 架构的多媒体播放器集成。

该滤镜在视频解码与视频呈现（Video Renderer）之间介入视频流，利用单目深度估计模型（Depth Anything V2）与高并发 CUDA DIBR（Depth-Image-Based Rendering，基于深度图像的渲染）计算流水线，在播放时实时重构左/右眼立体视差图像，实现常规 2D 视频向立体 3D 视频的低延迟、全画质实时转换。

---

## 2. 核心技术特性 (Technical Features)

### 2.1 高吞吐单目深度估计 (Monocular Depth Inference)
- 集成 **Depth Anything V2** 视觉大模型（支持 Small、Base、Large 规格的 ONNX 模型）。
- 通过 **ONNX Runtime GPU (CUDA Execution Provider)** 加速推理，充分调度 NVIDIA GPU 的 Tensor Core 与 CUDA Core 算力。
- 支持推理解析度下采样档位（100%、75%、50%、25%），以平衡推理耗时与立体保真度，适配主流消费级显卡。

### 2.2 纯 GPU 端 DIBR 渲染流水线 (CUDA DIBR Pipeline)
- 包含双边滤波深度上采样（Bilateral Depth Upsampling）、相对深度动态归一化、视差反向映射投影与边缘遮挡自适应填补（Inpainting）。
- 图像数据在 GPU 显存内流转，降低主机内存（Host）与设备内存（Device）之间的数据往返开销，压低帧内处理延迟。

### 2.3 多制式立体呈现模式 (Stereoscopic Output Modes)
- **Half-SBS (Side-by-Side)**：左右半宽并排输出，适配 3D 电视、投影设备以及 VR / MR 头显。
- **Full-SBS**：全分辨率左右并排输出，保留原生无损解析度。
- **红青互补立体 (Red-Cyan Anaglyph)**：兼容普通显示设备与被动式滤光眼镜。
- **深度图直视 (Depth Visualization)**：实时可视化模型输出的灰度深度数据。
- **2D 直通 (Pass-through)**：零计算旁路直通，无需卸载滤镜即可对比原片。

### 2.4 解耦式异步双线程管道 (Decoupled Asynchronous Threading)
- DirectShow 呈现链路与后台 AI 推理工作线程完全解耦，结合环形帧缓冲与时域平滑算法。
- 有效防止因 AI 计算耗时波动引发的播放器卡顿、严重丢帧与音画不同步问题。

### 2.5 硬件纹理步长自适应 (Direct3D 11 Pitch Adaptation)
- 实现了针对下游 Direct3D 11 Video Renderer 的动态步长（Stride / Row Pitch）反推机制。
- 自动适配 GPU 显存 128 / 64 字节硬件对齐约束，彻底解决 480P、540P、720P、1080P、4K 及各类非标准宽纵比视频中的倾斜条纹拉伸与错位问题。

### 2.6 低开销共享内存进程间通信 (Shared Memory IPC)
- 配套独立的 Win32 原生悬浮托盘控制面板，通过轻量级共享内存（Shared Memory IPC）实现与运行中滤镜的实时通信。
- 支持在播放过程中无缝调节最大视差量（Max Disparity）、零视差收敛平面（Zero Parallax Plane）、推理降采样比例与显示制式，即调即显。

### 2.7 硬件快门眼镜时序同步 (ParallaxSYNC)
- 面向 120Hz/144Hz 快门式 3D 显示系统，内置串口通信模块，向外部硬件控制器实时分发左右场同步脉冲。
- 开源提供配套的单片机固件代码（`ParallaxSYNC.ino`），兼容 Arduino / ESP32 及红外/射频同步电路。

---

## 3. 系统架构 (System Architecture)

```mermaid
flowchart TD
    subgraph DirectShow_Pipeline [DirectShow 播放管道 (PotPlayer)]
        Source[视频源分离器] --> Decoder[视频解码器 (FFmpeg / LAV)]
        Decoder -- "NV12 / YV12 / RGB32" --> Filter["ParallaxRT Filter (.ax)"]
        Filter -- "3D 合成画面 (硬件 Pitch 对齐)" --> Renderer[Direct3D 11 视频渲染器]
    end

    subgraph ParallaxRT_Engine [ParallaxRT 核心计算引擎]
        Filter --> AsyncBuffer[异步帧队列与时域平滑]
        AsyncBuffer --> WorkerThread[AI 推理工作线程]
        WorkerThread -- "Depth Anything V2" --> DepthData[浮点深度图]
        DepthData --> CudaKernel[CUDA DIBR 视差合成核函数]
        CudaKernel --> StereoFrame[立体帧生成]
        StereoFrame --> Filter
    end

    subgraph Control_Interface [外部控制与硬件同步]
        GUI[ParallaxRT 控制面板] <-->|共享内存 IPC| Filter
        Filter -.->|RS-232 串口脉冲| Microcontroller["ParallaxSYNC 控制器 (快门眼镜)"]
    end
```

---

## 4. 运行环境要求 (System Requirements)

| 项目 | 最低配置 | 推荐配置 |
| :--- | :--- | :--- |
| **操作系统** | Windows 10 (64-bit) 1903+ | Windows 11 (64-bit) |
| **图形处理器** | NVIDIA GeForce GTX 1060 (6GB) | NVIDIA GeForce RTX 3060 / 4060 及以上 |
| **GPU 架构** | Pascal (Compute 6.1) 及以上 | Ampere (Compute 8.6) / Ada Lovelace (Compute 8.9) |
| **驱动版本** | NVIDIA Display Driver $\ge$ 550.00 | 最新版本官方驱动 |
| **CUDA 环境** | CUDA Toolkit 12.0+ (预编译包内置) | CUDA Toolkit 12.8 / 13.x |
| **播放器支持** | PotPlayer (64-bit) | PotPlayer (64-bit 最新正式版) |

---

## 5. 部署与使用指南 (Deployment & Usage)

### 5.1 安装
1. 前往本仓库 [Releases](../../releases) 页面下载最新安装包 `ParallaxRT_Setup.exe`。
2. 运行安装向导完成安装。程序将自动部署运行依赖并将 `ParallaxRTFilter.ax` 注册至 Windows COM 组件库。

### 5.2 PotPlayer 配置步骤
1. 打开 **PotPlayer**，按快捷键 `F5` 打开 **参数选项**。
2. 在左侧树形导航中展开：**滤镜** $\rightarrow$ **滤镜管理**。
3. 点击右下角 **添加注册的滤镜** 按钮。
4. 在滤镜列表中选择 **`ParallaxRT Real-time 3D Filter`**，点击确定。
5. 选中新添加的滤镜条目，在右侧将其使用条件修改为 **`总是使用`**。
6. 点击确定保存配置。
7. 播放任意视频文件，系统托盘区将出现 ParallaxRT 控制面板图标，双击即可唤出参数配置窗口。

---

## 6. 源码编译 (Build from Source)

### 6.1 构建依赖
- **操作系统**：Windows 10 / 11 64-bit
- **编译器**：Microsoft Visual Studio 2022（需安装“使用 C++ 的桌面开发”工作负载及 MSVC v143 工具集）
- **构建系统**：CMake 3.24 或更高版本
- **并行计算平台**：NVIDIA CUDA Toolkit 12.8+ 与 cuDNN 9+
- **包管理器**：vcpkg
  ```powershell
  vcpkg install --triplet=x64-windows "opencv4[core,imgproc]:x64-windows"
  ```
- **推理运行时**：ONNX Runtime GPU (v1.20+) Windows x64 发行包

### 6.2 编译步骤
```powershell
# 1. 获取代码仓库
git clone https://github.com/your-username/ParallaxRT.git
cd ParallaxRT

# 2. 配置 CMake 构建工程 (启用 CUDA 架构)
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
  -DPARALLAXRT_ENABLE_CUDA=ON `
  -DPARALLAXRT_ENABLE_DIRECTML=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# 3. 执行 Release 构建
cmake --build build --config Release --target parallaxrt_filter
```
编译产物 `ParallaxRTFilter.ax` 将输出于 `build/Release/` 目录。

### 6.3 手动注册与反注册
使用管理员权限启动 PowerShell 或命令提示符：
```cmd
:: 注册滤镜
regsvr32 "C:\Path\To\ParallaxRTFilter.ax"

:: 卸载注销滤镜
regsvr32 /u "C:\Path\To\ParallaxRTFilter.ax"
```

---

## 7. 硬件快门同步协议 (ParallaxSYNC Specification)

针对主动式快门 3D 系统，ParallaxRT 提供可配置的硬件同步输出接口：

1. **固件刷写**：使用 Arduino IDE 打开 `ParallaxSYNC/ParallaxSYNC.ino`，烧录至兼容开发板（如 Arduino Nano / Pro Micro / ESP32）。
2. **电路连接**：开发板指定输出引脚输出方波信号，高电平指示左眼场，低电平指示右眼场，可直接驱动红外二极管或快门眼镜同步端口。
3. **软件配置**：在 ParallaxRT 控制面板中勾选“启用硬件串口同步”，选择对应串口端口号与波特率（默认 115200bps）。

---

## 8. 代码结构 (Repository Structure)

```text
ParallaxRT/
├── CMakeLists.txt                 # CMake 工程构建脚本
├── CMakePresets.json              # Visual Studio 预设配置
├── LICENSE                        # 开源许可证文件 (MIT License)
├── README.md                      # 技术说明文档
├── ParallaxSYNC/                  # 硬件快门同步模块
│   └── ParallaxSYNC.ino           # Arduino 同步控制固件
├── src/
│   ├── core/
│   │   ├── depth_estimator.h/.cpp # 基于 ONNX Runtime 的模型推理封装
│   │   ├── dibr_renderer.h/.cpp   # DIBR 视差映射算法与 CPU 后备逻辑
│   │   ├── dibr_renderer_cuda.cu  # DIBR 高性能 CUDA 核函数
│   │   ├── directshow_filter.h/.cpp # DirectShow 转换滤镜主体与步长对齐实现
│   │   ├── directshow_exports.cpp # COM 类工厂与注册表操作函数
│   │   ├── filter_config.h        # 共享内存配置定义与同步管理器
│   │   └── tray_controller.h      # 悬浮控制面板与托盘程序逻辑
│   └── app/                       # 命令行工具与独立预览器实现
└── packaging/                     # Inno Setup 自动化打包与安装程序脚本
```

---

## 9. 许可与引用 (License & Acknowledgements)

- **开源协议**：本项目基于 [MIT License](LICENSE) 授权开源。
- **深度模型**：深度估计模块基于 [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2) 算法实现。
- **推理引擎**：依托于 [ONNX Runtime](https://onnxruntime.ai/) 与 [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)。
