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
  "encoder": [
    {"encoder": "pyrowave", "codec": "pyrowave"},
    {"encoder": "pyrowave", "codec": "pyrowave"},
    {}
  ],
  "application": ["/bin/sh", "-c", "sleep infinity | exec /opt/experiencenet/hellohydra_xr/hellohydra_xr -g Vulkan"]
}
```

Three things here are easy to get wrong, and all three cost time already:

- The key is **`encoder`, singular**. A plural `encoders` is silently ignored
  and the server streams its default codec while looking healthy. Always
  confirm against the `print_encoders` block in the server log.
- The three entries are left eye, right eye, alpha. **PyroWave cannot encode
  the alpha stream**, so leave the third entry `{}` for the default encoder.
- **Bitrate is a client setting.** There is no server-side bitrate key. Set it
  in the headset app; below ~200 Mbit/s defeats the codec.

The `application` entry is piped from `sleep infinity` because hello_xr polls
stdin and exits at once when stdin is EOF under systemd, which presents as the
headset waiting forever for an application.

Confirm the codec actually engaged by reading the server log:

```
INFO [print_encoders] Encoder configuration:
	* pyrowave (pyrowave 8-bit)   size: 960x1024   bitrate: 98.7Mbit/s
	* pyrowave (pyrowave 8-bit)   size: 960x1024   bitrate: 98.7Mbit/s
	* vulkan (h265 8-bit)         size: 960x512
```

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

**Streaming, validated 2026-09-14.** Omarchy server (msi1060, GTX 1060) to a
Quest 2: both eyes pyrowave 8-bit at 98.7 Mbit/s, rendering real content over
WiFi. Merged to this fork's `master`.

Two bugs were fixed to get there, both recorded on issues.experiencenet.com:
the x50 bitrate weight starved the alpha stream (#726), and the compositor
image was not created sampleable, so the encoder emitted a wavelet encode of a
blank image and the headset showed solid green (#727).

Still open:

- **#728**: pyrowave's shader device features (16-bit and 8-bit storage,
  subgroup size control) are not enabled on the compositor device. NVIDIA
  tolerates it; verify before running on an AMD or Intel host.
- No performance measurement yet. msi1060 is on WiFi rather than ethernet, and
  a Quest 2 is below the intended target. For real numbers: wire the server,
  use a Quest 3 on a dedicated 6 GHz AP, and compare frame time and battery
  against the H.265 baseline.

## Debugging a blank or wrong image

Two techniques that did the work here, in order of cost:

1. **Split the eyes between codecs.** Set the left eye to pyrowave and the
   right to `{"encoder": "vulkan", "codec": "h265"}`. One connect then tells
   you whether the fault is the codec path or the rig, because you see both at
   once.
2. **Dump the bitstream.** Start the server with `WIVRN_DUMP_VIDEO=/var/tmp/dump`
   and inspect the entropy of `dump-0.pyro`. Real compressed video uses most of
   the 256 byte values; a broken encode was 99.7% four values in a repeating
   pattern, which proved the encoder rather than the decoder was at fault
   without needing the headset again.
3. **Vulkan validation on the server.** Install `vulkan-validation-layers` and
   set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. This named #727
   outright.
