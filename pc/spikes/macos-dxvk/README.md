# macOS DXVK Native spike

DXVK 3.1.1 builds as an arm64 `libdxvk_d3d9.0.dylib` on macOS 15.8 using SDL3 and Homebrew's Vulkan loader. It creates a MoltenVK device on Apple M1 Max. The fixed-function path gets through device initialization, but the current embedded SM3 shader bytecode is rejected by DXVK at `CreateVertexShader`; therefore this spike does **not** yet prove both requested draw paths or finish 120 frames. The D3D9 rendering loop is present and attempts a fixed-function triangle followed by the shader triangle each frame. The shader token stream still needs correction before claiming success.

## Source and patch

DXVK tag `v3.1.1` was checked out with recursive submodules at commit `b1a1c99ab52b687cf950d62c88bc2fa316b41663`. Sources and build output go under `work/dxvk/` (gitignored); `pc/MACOS-BUILD.md`, step 5, has the clone command. The source changes are captured in [`../../dxvk/dxvk-3.1.1-macos.patch`](../../dxvk/dxvk-3.1.1-macos.patch), offered under DXVK's zlib license.

The patch handles macOS SDL3/Vulkan dynamic loading, pthread naming and executable path, Mach-O exported symbols, macOS-only unavailable Win32 calls, MoltenVK portability enumeration, and optional Vulkan features that prevent D3D9 initialization. It returns `D3DERR_NOTAVAILABLE` for surface `GetDC` on non-Windows.

## Build

From the recursive DXVK checkout:

```sh
# from the repository root, after: git clone --recursive --branch v3.1.1 https://github.com/doitsujin/dxvk.git work/dxvk/dxvk-3.1.1-git
cd work/dxvk/dxvk-3.1.1-git
patch -p1 < ../../../pc/dxvk/dxvk-3.1.1-macos.patch
CPPFLAGS=-I/opt/homebrew/include CXXFLAGS=-I/opt/homebrew/include meson setup build-macos --buildtype=release \
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
  -Dnative_sdl2=disabled -Dnative_sdl3=enabled -Dnative_glfw=disabled
ninja -C build-macos
```

Built output: `build-macos/src/d3d9/libdxvk_d3d9.0.dylib` (arm64). SDL3 is the WSI backend. DXVK Native requires `DXVK_WSI_DRIVER=SDL3`. The patched loader loads `/opt/homebrew/lib/libvulkan.1.dylib`, which is the Homebrew Vulkan loader; `VK_ICD_FILENAMES` selects MoltenVK's ICD manifest.

## Build and run the standalone program

From the repository root (the commands change into the spike folder and point `DXVK` at the DXVK tree):

```sh
cd pc/spikes/macos-dxvk
DXVK=../../../work/dxvk/dxvk-3.1.1-git
clang++ -x c++ -arch arm64 \
  -I$DXVK/src/util \
  -I$DXVK/include/native/directx \
  -I$DXVK/include/native/windows \
  $(pkg-config --cflags sdl3) -o triangle triangle.c $(pkg-config --libs sdl3) -ldl
DXVK_D3D9_DYLIB=$DXVK/build-macos/src/d3d9/libdxvk_d3d9.0.dylib \
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
DXVK_WSI_DRIVER=SDL3 DXVK_LOG_LEVEL=info ./triangle
```

The observed info log found `Apple M1 Max (MoltenVK 0.2.2210)`, initialized the D3D9 device and swapchain, then failed on the embedded shader with `Token is not an operand` / `CreateShaderModule: Shader analysis prepass failed`; `CreateVertexShader` returned `0x8876086c`. After that failure is corrected, this program needs to complete its 120-frame loop and return 0 to satisfy the proof goal.

## MoltenVK findings

DXVK initially skipped the device because it requires portability enumeration. The patch enables `VK_KHR_portability_enumeration` and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` when advertised by the loader.

On Apple M1 Max / MoltenVK 1.4.2 Homebrew bundle (DXVK reports MoltenVK 0.2.2210), DXVK reported these unsupported Vulkan features/extensions:

- Core `geometryShader` and `shaderCullDistance` are unavailable. DXVK now treats these as optional for this D3D9-only build; D3D9 geometry shaders are not part of the API, while user clip-distance behavior may be reduced.
- `VK_EXT_depth_clip_enable` `depthClipEnable` is unavailable. The driver reports `extendedDynamicState3DepthClipEnable`; DXVK disables the former requirement, so non-default depth clipping behavior is a limitation to validate.
- `VK_EXT_robustness2` `robustBufferAccess2` and `nullDescriptor` are unavailable; `robustImageAccess2` is available. DXVK no longer requires the first two. Null descriptor behavior may differ for applications relying on it.
- `VK_EXT_transform_feedback` is not exposed (`transformFeedback=0`, `geometryStreams=0`). This affects D3D9 stream output/ProcessVertices paths; the triangle does not use them.
- DXVK mapped `VK_FORMAT_D24_UNORM_S8_UINT` and `VK_FORMAT_D16_UNORM_S8_UINT` to `VK_FORMAT_D32_SFLOAT_S8_UINT`. This is a depth format substitution, not a failed device creation.
- DXVK warned that graphics pipeline libraries are unsupported and used its legacy binding model. This was non-fatal.

The info log also reports `D3DKMTOpenAdapterFromLuid` unavailable and failed, expected for macOS; DXVK still identified the Apple M1 Max. No broader exhaustive D3D9 format audit was performed beyond the format substitutions printed during device initialization.
