---
name: build-linux
description: "Build, compile, or debug Vibepollo (a Sunshine/Apollo game-streaming fork) on Linux — locally in this sandbox or when triaging the Arch CI (.github/workflows/ci-archlinux.yml) / PR build failures. Use when the task involves compiling the C++ host on Linux, getting the `sunshine` binary or the Arch `.pkg.tar.zst` to build, populating submodules when `git clone` is blocked, or fixing Linux-only compile/link errors (often Windows-only code leaking into cross-platform files). Triggers: 'build on linux', 'compile sunshine/vibepollo', 'arch CI failing', 'porting to linux/cachyos', 'linker error', 'submodules empty/403'. NOT for the Windows build."
---

# Building Vibepollo on Linux

Vibepollo is a fork of Apollo/**Sunshine** (Moonlight host). It is cross-platform but is
developed/tested almost exclusively on **Windows**, so the Linux branch bit-rots: cross-platform
symbols get trapped in `#ifdef _WIN32`, and `src/platform/linux/*` files go uncompiled. The Arch CI
(`.github/workflows/ci-archlinux.yml`) is the source of truth; a local build makes iteration fast.

## Key facts about the build

- **Heavy submodules** (`third-party/*`): moonlight-common-c (+ nested `enet`), Simple-Web-Server,
  inputtino, libdisplaydevice, libvirtualdisplay, glad (glad2 generator), nanors, wayland/wlr/plasma
  protocols, nv-codec-headers, build-deps.
- **FFmpeg is prebuilt**, downloaded by `cmake/dependencies/ffmpeg.cmake` from `LizardByte/build-deps`
  GitHub *releases* (falls back to `releases/latest` if the build-deps git tag is unavailable).
- **Boost 1.89** required; older system Boost → CMake FetchContent downloads & builds it (slow first time).
- **glad** is glad2: it *generates* the EGL/GL loader into the build tree and exposes a `glad`
  INTERFACE target. Do NOT reference `third-party/glad/src/*.c` as sources.
- All Linux capture backends default **ON** (`X11, WAYLAND, KWIN, PORTAL, DRM, VAAPI, VULKAN, CUDA`),
  so one build serves KDE (KWin/Portal) and Hyprland (wlr/Portal); runtime picks the backend.

## Sandbox gotcha: git-clone of submodules is blocked

The agent proxy only allows git over the scoped repo; cloning `github.com/<other>/<repo>.git` returns
**403**. But `codeload.github.com` tarballs, `raw.githubusercontent.com`, `api.github.com`, and GitHub
**release assets** are reachable. So populate submodules from codeload tarballs at the pinned SHAs.
Use the helper next to this skill:

```bash
bash .claude/skills/build-linux/fetch-submodules-codeload.sh
```

It downloads each Linux-needed submodule at the SHA recorded in the superproject tree (`git ls-tree
HEAD <path>`), and resolves nested submodule SHAs (e.g. `enet`) via the GitHub contents API. Boost &
FFmpeg are fetched by CMake itself from GitHub release assets (allowed), so no extra work there.

## Local build recipe (this Ubuntu sandbox)

```bash
# 1) system deps (root, apt)
apt-get install -y --no-install-recommends build-essential cmake pkg-config nodejs npm \
  libboost-all-dev libssl-dev libopus-dev libva-dev libdrm-dev libwayland-dev wayland-protocols \
  libx11-dev libxfixes-dev libxrandr-dev libxtst-dev libxcb1-dev libxcb-xfixes0-dev \
  libpipewire-0.3-dev libcap-dev libevdev-dev libminiupnpc-dev libnotify-dev \
  libayatana-appindicator3-dev libpulse-dev libnuma-dev libgbm-dev uuid-dev \
  libcurl4-openssl-dev libvulkan-dev glslang-tools python3-jinja2 python3-setuptools

# 2) submodules (codeload workaround)
bash .claude/skills/build-linux/fetch-submodules-codeload.sh

# 3) configure — CUDA OFF locally (no GPU here), VULKAN ON with system headers to mirror CI
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DSUNSHINE_ENABLE_WEBRTC=OFF \
  -DSUNSHINE_ENABLE_X11=ON -DSUNSHINE_ENABLE_WAYLAND=ON -DSUNSHINE_ENABLE_KWIN=ON \
  -DSUNSHINE_ENABLE_PORTAL=ON -DSUNSHINE_ENABLE_DRM=ON -DSUNSHINE_ENABLE_VAAPI=ON \
  -DSUNSHINE_ENABLE_VULKAN=ON -DSUNSHINE_SYSTEM_VULKAN_HEADERS=ON \
  -DSUNSHINE_ENABLE_TRAY=ON -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF -DBUILD_WERROR=OFF

# 4) build with keep-going so ALL compile errors surface in one pass (not one-per-CI-round)
cmake --build build-linux -j"$(nproc)" -- -k
# success => build-linux/sunshine (symlink to sunshine-0.0.0); `./build-linux/sunshine --version` runs.
```

First configure is slow (Boost FetchContent + FFmpeg download). Incremental rebuilds after a one-file
fix are seconds — always prefer the local loop over pushing and waiting ~6 min for CI.

## Local vs CI differences (don't get fooled)

- **gcc 13 (sandbox) vs gcc 15 (Arch CI)**: gcc15 is stricter. Some hard errors (e.g.
  `-Wchanges-meaning`) reproduce on gcc13; some appear only on gcc15. `__notify_impl@GLIBCXX_3.4.35`
  (from `std::jthread`/atomic-notify) exists only on gcc14+, so LTO link failures around it won't
  reproduce locally.
- **CUDA**: ON in CI, OFF locally — `src/platform/linux/cuda.{cpp,cu}` only compile in CI.
- The Arch package uses **LTO + -Werror**; we made `-Werror` opt-in (`_werror`, default off in
  `packaging/linux/Arch/PKGBUILD`). `mem_type_e::vulkan` errors mean you configured `VULKAN=OFF`.

## Recurring Linux-bitrot patterns & fixes

1. **`X was not declared in this scope`** in a common file (`nvhttp.cpp`, `video.cpp`): the symbol is
   defined inside an `#ifdef _WIN32` block but used in cross-platform code. → move the definition out
   of the `_WIN32` block (keep it in the same anonymous namespace).
2. **`unused variable` under `-Werror`**: a snapshot var used only in a `_WIN32` block. → wrap the
   declaration in the same `#ifdef _WIN32`.
3. **`changes meaning of 'X' [-Wchanges-meaning]`** (a real C++ error, not a warning): a struct member
   named the same as its type. → fully-qualify the type (`ns::X member = ns::X::value;`).
4. **`undefined reference … DSO missing from command line`** at link under LTO: usually `std::jthread`
   /`std::atomic::wait/notify` pulling `__notify_impl`. → replace `std::jthread` with `std::thread` +
   explicit `join()` if it only relied on auto-join.
5. **`string_view` → `const char*`**: pass `.data()` (string literals are null-terminated).
6. **Missing `platf::` enum value / member**: add it to the cross-platform header
   (`src/platform/common.h`) or the linux struct.

## CI wiring (overlay model — keep upstream files pristine)

This is a **Linux-only fork** of an upstream that is Windows-focused, so to avoid merge
conflicts on every upstream sync, all Linux CI lives in **one self-contained file we own**:
`.github/workflows/linux.yml` (build the Arch package on push/PR/tag + publish a GitHub release
on tags). Do **NOT** edit upstream's `ci-windows.yml` / `ci-archlinux.yml` — they are byte-identical
to upstream. The only upstream edit is a **minimal one** in `ci.yml`: its `on:` is reduced to
`workflow_dispatch` so it does not auto-run on the fork (→ exactly ONE workflow, `linux.yml`, runs
per push/PR, and Windows never builds). That keeps the conflict surface to just the `on:` block.

`linux.yml` builds with `_run_unit_tests=false` and `_werror=false` (phase 1). To get failure logs
use the GitHub MCP `get_job_logs` (tail ~200 lines and grep for `error:` — the tail alone is just
cleanup noise). The local build (above) is the fast loop; CI is the gcc15+CUDA+LTO truth.

## Phase-2 hardening (when asked)

Re-enable `_werror=true`, build+fix unit tests (`BUILD_TESTS=ON`), fix the `dmabuf_t`/`gbm_device`
ODR warning in `src/platform/linux/wayland.h`, and (nice-to-have) build `libwebrtc` for Linux to
enable `SUNSHINE_ENABLE_WEBRTC`.
