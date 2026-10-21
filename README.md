<p align="center">
  <img src="logo.png" alt="Winlator OctaFX" width="600">
</p>

# Winlator OctaFX

**Winlator OctaFX** is an optimized Windows translation and gaming runtime for Android (ARM64), focusing on low-latency direct display presentation, Vulkan GPU compute frame interpolation, and streamlined Direct3D-to-Vulkan execution.

Based on upstream work by [brunodev85](https://github.com/brunodev85/winlator), [Pipetto-crypto](https://github.com/Pipetto-crypto/winlator), and [StevenMXZ](https://github.com/StevenMXZ/Winlator-Ludashi).

---

## Technical Overview

### 1. Frame Generation Engine (Dual Execution Modes)
Winlator OctaFX provides two distinct frame interpolation backends to increase perceived visual motion smoothness:

* **Mode 1: Adreno GPU Compute (Vulkan Compute Shader)**
  - Compiles and dispatches the `lsfg_interpolate_spv` compute shader directly on Adreno GPU ALUs via Vulkan.
  - Utilizes a dedicated 3-binding descriptor set layout:
    - `binding 0`: `sampler2D` (Previous frame)
    - `binding 1`: `sampler2D` (Current frame)
    - `binding 2`: `image2D` (Output intermediate storage image)
  - Zero-copy swapchain buffer import via Android `AHardwareBuffer` (`VK_ANDROID_external_memory_android_hardware_buffer`).
  - Executes as a standard GPU compute pass without CPU interpolation overhead.

* **Mode 2: OctaFX Motion Engine (Parallel ARM NEON SIMD)**
  - Multi-threaded block-matching motion estimation running across persistent CPU worker threads.
  - Leverages 128-bit ARM NEON vector instructions for luma downsampling and sum of absolute differences (SAD) calculations.
  - **HUD & Crosshair Protection**: Automatically detects static 2D interface elements (crosshairs, health bars, minimaps, subtitle text) and zeroes their motion displacement to prevent UI smearing during camera pans.
  - **Vector Field Smoothing**: Applies a spatial 3x3 median filter across motion blocks to reduce boundary tearing.
  - Also initializes Qualcomm QNN HTP device and context handles as an architectural staging hook for future compiled tensor graph execution.

### 2. DisplayX Direct Surface Presentation & Frame Pacing
* **Hardware Compositor Direct Pass**: Utilizes Android `SurfaceControl` and `ASurfaceTransaction` to deliver buffers directly to `SurfaceFlinger`, bypassing standard Android View hierarchy redraw overhead.
* **Rolling Interval Micro-Pacing**: Calculates dynamic rolling frame intervals ($T$). Synthesized intermediate frames are presented at $T / 2$ followed by the native frame to maintain regular presentation cadence on high-refresh AMOLED panels.
* **Fence Synchronization**: Properly coordinates native release fences to avoid swapchain tearing and buffer race conditions.

### 3. DXVK Async Pipeline Compilation
* Pre-configured with asynchronous pipeline compilation (`dxvk.enableAsync = true`, `dxvk.gpl = true`, `dxvk.gplAsyncCache = true`) directly in container configurations.
* Writes localized `dxvk.conf` directly into container system directories to reduce shader compilation stutter across Direct3D 9, 10, and 11 games.

### 4. Memory Management & Process Defense
* **Proactive Memory Scavenging**: Listens for Android OS memory pressure callbacks (`onTrimMemory` / `onLowMemory`) and invokes `mallopt(M_PURGE)` to promptly return unused heap arenas back to the Linux kernel.
* **Process Priority Optimization**: Configures container process priority and large heap flags (`android:largeHeap="true"`) to reduce unexpected terminations by Android's LowMemoryKiller (`lmkd`) under heavy load.

### 5. Repository Security & Release Integrity
* **Hardened Credential Management**: Keystores and plaintext passwords are not stored in the repository.
* **Clean CI Build Pipeline**: GitHub Actions signs official releases using repository secrets with an automated self-signed fallback for fork and local open-source builds.

---

## Configuration Reference

| Setting | Recommended Value | Notes |
| :--- | :--- | :--- |
| **Graphics Driver** | Custom Turnip (Adreno 7xx) | Open-source Vulkan driver with optimized GMEM tile management |
| **DXVK Version** | DXVK 2.4 / 2.3.1 / 1.10.3-async | Select based on title compatibility |
| **Frame Generation** | Mode 1 (GPU Compute) | Dispatches compute interpolation on GPU ALUs |
| **Box64 / FEX Preset** | Performance / Stability | Select based on translation compatibility |
| **Audio Driver** | AAudio Native / PulseAudio | Low-latency audio streaming |

---

## Building from Source

### Prerequisites
* Android NDK (`29.0.14206865` or r25c+)
* Android SDK (API 28–35)
* JDK 17
* `glslang-tools`, `ninja-build`, `zstd`

### Build Command
```bash
git clone https://github.com/octavia-23/Winlator-OctaFX.git -b 4.1
cd Winlator-OctaFX
./gradlew assembleRelease
```
The compiled APK will be located at `app/build/outputs/apk/release/Winlator-OctaFX-v1.1.0.apk`.

---

## Third-Party Components & Acknowledgments

* **Winlator** by [brunodev85](https://github.com/brunodev85/winlator)
* **Winlator Bionic** by [Pipetto-crypto](https://github.com/Pipetto-crypto/winlator)
* **Winlator Ludashi** by [StevenMXZ](https://github.com/StevenMXZ/Winlator-Ludashi)
* **Wine** by [WineHQ](https://www.winehq.org/)
* **Box86 / Box64** by [ptitSeb](https://github.com/ptitSeb)
* **FEX-Emu** by [FEX-Emu Team](https://github.com/FEX-Emu/FEX)
* **Mesa (Turnip / Zink / VirGL)** by [Mesa3D](https://www.mesa3d.org) & [Danylo Piliaiev](https://blogs.igalia.com/dpiliaiev/)
