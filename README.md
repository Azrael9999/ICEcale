# ICEcale

CLI wrapper that upscales videos with the `realesrgan-x4plus` model and hard caps the output to 1440p. It requires an NVIDIA GPU and depends on `realesrgan-ncnn-vulkan`, `ffmpeg`, and `ffprobe` bundled inside the project folder (no PATH editing required).

## Requirements

- NVIDIA GPU with working drivers (`nvidia-smi` must succeed).
- `ffmpeg` and `ffprobe` binaries placed in the project folder (see below).
- [`realesrgan-ncnn-vulkan`](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) binaries and models placed in the project folder (see below).
- CMake 3.16+ and a C++17 compiler.

## Build

```bash
cmake -S . -B build
cmake --build build
```

The resulting binary is available at `build/icecale`.

### Visual Studio (Windows)

1. Install the Desktop development with C++ workload (MSVC + CMake).
2. Place the required binaries inside the project folder (see placement guidance below).
3. Open the folder as a CMake project in Visual Studio (or run the two CMake commands above in a VS Developer Prompt). The target `icecale` will build under `out/build/<config>/icecale.exe` by default.

## Usage

1. Run the binary:
   ```bash
   ./build/icecale
   ```
2. When prompted, paste the full path to the input video.
3. The output file is automatically written to your **Downloads** folder, named `<input_stem>_upscaled.mp4`.

### What the tool does

1. Verifies an NVIDIA GPU is present and the required commands exist.
2. Uses `ffprobe` to read the input resolution, frame rate, and estimated frame count.
3. Extracts audio (if present) and video frames with `ffmpeg`.
4. Upscales each frame with `realesrgan-ncnn-vulkan` using the `realesrgan-x4plus` model and shows an accurate frame-level progress indicator.
5. Reassembles the video with `ffmpeg`, encoding with `h264_nvenc` and applying a final `scale` filter capped at 2560x1440 (aspect ratio preserved and even dimensions enforced). Audio is remuxed without re-encoding when available; the finished file is saved to your Downloads folder.

The process will abort if no NVIDIA GPU is detected to guarantee GPU-accelerated execution.

### Where to place models and dependencies

- **Directory layout (suggested)**:
  - `third_party/ffmpeg/ffmpeg(.exe)`
  - `third_party/ffmpeg/ffprobe(.exe)`
  - `third_party/realesrgan-ncnn-vulkan/realesrgan-ncnn-vulkan(.exe)`
  - `third_party/realesrgan-ncnn-vulkan/realesrgan-x4plus.param`
  - `third_party/realesrgan-ncnn-vulkan/realesrgan-x4plus.bin`
  - (Optionally) copy the binaries into `bin/` or alongside the built `icecale` executable; the app searches these project-local locations automatically and does **not** rely on `PATH`.
- **ffmpeg / ffprobe**: Keep the binaries in the project tree (e.g., `third_party/ffmpeg`). No PATH edits are needed.
- **Temporary workspace**: Frames and audio are written to your system temp dir under `icecale-work/`. The final output goes wherever you point `output_video`.
