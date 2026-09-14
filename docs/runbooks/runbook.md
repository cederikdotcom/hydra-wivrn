# hydra-wivrn runbook

## What this repository is

hydra-wivrn is a fork of [WiVRn](https://github.com/WiVRn/WiVRn), the OpenXR
wireless PCVR streaming server for Linux. The fork carries one feature on top
of upstream: the PyroWave codec, a wavelet video codec that runs in Vulkan
shaders on both ends. PyroWave trades bandwidth for latency. It needs 200+
Mbit/s of sustained UDP throughput and returns sub-millisecond encode and
decode with no keyframe stalls.

Upstream: https://github.com/WiVRn/WiVRn
Codec: https://github.com/Themaister/pyrowave (MIT, vendored in
`common/pyrowave/`)
Tracked as Hydra issue #388 on issues.experiencenet.com.

## Branches

| Branch | Content |
|---|---|
| `master` | Mirror of upstream master. Do not commit here. |
| `pyrowave` | The working branch: upstream master plus the ported PyroWave integration. Build from this. |
| `proto/pyrowave` | The original 2025 prototype by the WiVRn maintainer, kept for reference. One year stale. Do not build. |

To update: fetch upstream, push `upstream/master` to `master`, then merge
`master` into `pyrowave`.

## Build

Server (Linux host with a Vulkan GPU):

```
cmake -B build-server -GNinja -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_CLIENT=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

Client (Quest, from the repo root with the Android SDK installed):

```
./gradlew assembleStandardRelease
```

Dependencies follow upstream WiVRn exactly. The PyroWave shaders compile at
build time with glslang; `-DWIVRN_OPTIMIZE_SHADERS=ON` also needs
spirv-tools.

### Omarchy / Arch server build

`contrib/arch/PKGBUILD` builds the server as a pacman package
(`makepkg -si` in that directory). For a direct build, install:

```
pacman -S --needed cmake ninja gcc git glslang spirv-tools vulkan-headers \
  vulkan-icd-loader vulkan-tools avahi boost eigen ffmpeg nlohmann-json \
  libpulse pipewire x264 openxr python nasm glib2-devel pkgconf cli11
```

then run the server cmake commands above with `-DWIVRN_USE_NVENC=OFF`
(NVENC needs the CUDA toolchain; x264 and pyrowave do not). Validated
2026-09-14 on the msi1060 test machine (spicy-cactus-76, node-d71b197c,
Omarchy 4.0.2, GTX 1060). Note for Pascal GPUs such as the 1060: the
driver reports no `shaderFloat16`, so PyroWave uses its non-fp16 shader
variants; this costs some GPU time but works.

## Enable the codec

PyroWave is opt-in. Auto-selection keeps choosing nvenc/vaapi/x264. Select it
in the server configuration (`~/.config/wivrn/config.json`):

```json
{
  "encoders": [
    {
      "encoder": "pyrowave",
      "codec": "pyrowave"
    }
  ]
}
```

The bitrate setting is honored, but sizing below ~200 Mbit/s defeats the
codec. There is no alpha-stream support; do not assign pyrowave to the alpha
(third) encoder slot.

## Hardware requirements

- Client: Quest 3 or Quest 3S (XR2 Gen 2, WiFi 6E). Quest 2 is not a target:
  its radio cannot sustain the bitrate and its GPU lacks decode headroom.
- Network: dedicated 6 GHz (WiFi 6E) AP, headset as the only client on the
  channel. Wired backhaul from the AP to the server.
- Client GPU must expose Vulkan 1.1 with 16-bit storage and float16 features.
  The client requests them only when available and falls back to the
  fragment-shader decode path otherwise.

## Troubleshooting

- Client shows H.265 even though pyrowave is configured: the client does not
  advertise pyrowave in `supported_codecs()`; the server must force it via
  the config above. Check the server log for the selected encoder line.
- Sparkling or soft image: bitrate too low or WiFi retries. Confirm the
  headset negotiated a 6 GHz link and raise the bitrate.
- Server refuses to start with pyrowave on alpha: expected. Pyrowave is not
  supported for the alpha channel.
- Build fails in `common/pyrowave/shaders`: glslang missing or too old.
  Install `glslang-tools` (and `spirv-tools` when optimizing shaders).

## Status

The `pyrowave` branch is a port of the maintainer prototype onto 2026
upstream, verified by CI build only. It has not yet streamed to a headset.
Before venue use: build both sides, stream to a Quest 3 on a 6 GHz AP, and
measure decode time and battery draw against the H.265 baseline.
