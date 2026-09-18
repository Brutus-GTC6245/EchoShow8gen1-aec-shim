# EchoShow8gen1-aec-shim

Acoustic echo cancellation (AEC) for the **Amazon Echo Show 8 (1st gen)** running
**LineageOS 18.1** (device codename `crown`), as an audio-HAL shim — no app changes,
no ROM patch. It gives an always-on ambient voice assistant clean **barge-in**: you
can speak over the device's own playback and be heard.

> ⚠️ **Root + supervised use.** This `LD_PRELOAD`s a native library into the vendor
> audio service on a jailbroken device. It is reversible (`scripts/uninstall.sh`),
> but a bad install can wedge the fragile SPI/FPGA audio path. Only run it on a
> device you can physically power-cycle and re-flash. No warranty.

## Why this device can do real AEC cheaply

AEC needs, every frame, the **near-end** (the mic) and a time-aligned **far-end
reference** (what actually left the speaker). On most phones getting a sample-aligned
reference is the hard part. The Echo Show 8 gen-1 hands it to you in hardware:

- The FPGA capture stream **`pcmC0D22c`** is **6-channel `S24_3LE` @ 16 kHz**.
- **ch0–ch3** are the microphones.
- **ch4–ch5** are a **DAC loopback** — a sample-aligned digital tap of the speaker
  output. That *is* the render reference, in the same buffer as the mic.

So a shim that intercepts the mic read already has both signals, perfectly aligned,
and can cancel the echo before AudioFlinger/`AudioRecord` ever see the audio.

This hardware fact was documented by **jxlarrea** in
[`lineageos-echo-show-camera`](https://github.com/jxlarrea/lineageos-echo-show-camera)
(`docs/echo-cancellation.md`), whose `libamznaec` shim this project is modeled on and
credits. See **Credits** below.

## How the shim works

`libamznaec_shim.so` is `LD_PRELOAD`ed into `android.hardware.audio.service`. It
interposes tinyalsa `pcm_open` / `pcm_read` / `pcm_close`. When the blob opens the
6-ch 16 kHz FPGA mic stream, the shim, per 10 ms block:

1. reads mic **ch0** as the near-end and **avg(ch4, ch5)** as the far-end reference,
2. runs **SpeexDSP's linear echo canceller** (`speex_echo_cancellation`),
3. writes the cleaned samples back into ch0 before the blob returns them.

It is **linear-only by default** (no nonlinear residual suppressor), which **preserves
the talker during double-talk** — exactly what you want for wake-word detection and
barge-in. It's self-contained: WebRTC-free, links only the bundled speexdsp + the NDK
`libc`/`liblog`. Measured cost ~10 ms latency, low CPU.

### Two engines, and why Speex is the default here

| Engine | Echo removal | Talker during double-talk | Notes |
|--------|-------------:|---------------------------|-------|
| **Speex linear** (this repo) | ~15–20 dB | **preserved (~0 dB loss)** | best for wake-word / barge-in; builds standalone |
| WebRTC (AEC3 + suppressor) | 38–50 dB | attenuated ~9.5 dB | stronger, but eats the talker; needs the ROM tree |

The WebRTC engine is stronger on paper but its nonlinear suppressor ducks *your* voice
while the device is talking, hurting the very barge-in this is for. The talker-preserving
Speex engine is the default. Building the WebRTC variant needs the LineageOS ROM tree
and the ROM's `libwebrtc_audio_preprocessing.so` — see **Building the WebRTC engine**.

## Verified working

Supervised test on a real unit during story playback, with a wake+command spoken over
it ("Hey Jarvis, actually tell me about dogs"). The `5s:` telemetry lines show the
adaptive filter converging — `out` dropping further below `mic in` as it learns the
room:

```
time       ref (dBFS)   mic in (dBFS)   out (dBFS)     cancellation
12:50:19     -45.2         -49.5          -57.2          ~8 dB
12:50:34     -43.6         -48.1          -62.7          ~15 dB
12:50:49     -45.5         -49.6          -66.2          ~16 dB
12:51:09     -44.5         -49.1          -65.3          ~16 dB
```

`ref ~-45 dBFS` confirms the ch4–5 DAC loopback carries real playback on this unit.
The barge-in produced a clean transcript where the pre-shim attempt (no AEC) returned
an empty one. Wake word fires at 0.99+ during playback.

## Layout

```
src/amznaec_speex_shim.cpp   the shim (interposer + Speex AEC)
tools/cap6.c                 6-ch S24_3LE capture probe (dumps pcmC0D22c to verify the loopback)
tools/play6.c                tinyalsa tone player (drives the DAC to exercise the loopback)
scripts/build.sh             standalone NDK build (no ROM tree)
scripts/install.sh           reversible LD_PRELOAD install over adb (.orig backups)
scripts/uninstall.sh         restore .orig backups, remove shim, reboot clean
prebuilt/libamznaec_shim.so  armv7 build of src/ (stripped) — install without building
third_party/speexdsp/        vendored SpeexDSP sources (BSD) so the build is reproducible
```

## Build

Standalone, no ROM tree — needs Android **NDK r28** (`28.2.13676358` tested):

```bash
export ANDROID_NDK=$HOME/Library/Android/sdk/ndk/28.2.13676358   # or your path
./scripts/build.sh            # -> out/libamznaec_shim.so (armv7, 32-bit)
```

`prebuilt/libamznaec_shim.so` is a stripped build of `src/` if you'd rather not build.

## Install / uninstall (adb, supervised)

```bash
./scripts/install.sh                 # push shim, LD_PRELOAD in the audio rc, reboot
./scripts/install.sh --pga 40 --log 1  # also lower ADC MICPGA 80->40 (doc-recommended)
./scripts/uninstall.sh               # restore .orig backups, remove shim, reboot clean
```

Verify after boot:

```bash
adb logcat -d | grep amznaec
# libamznaec_shim loaded into pid ...
# mic PCM 0:22 opened: 6 ch 16000 Hz period 257; SPEEX linear filter=64ms ... gain=20dB
# 5s: ref -45.2 dBFS, mic in -49.5 dBFS, out -57.2 dBFS (speex linear)
```

> The app's mic is blocked while the device **dozes** (`getInputForAttr permission
> denied`). Keep the display awake when testing capture (`svc power stayon true`).

## Tuning (`persist.vendor.amznaec.*`, read at each PCM open)

| Property | Default | Meaning |
|----------|--------:|---------|
| `enable` | `1` | master on/off |
| `log` | `0` | emit the 5 s `ref/mic in/out` telemetry (set `0` for daily use) |
| `gain_db` | `20` | makeup gain applied to the cleaned mic |
| `hpf` | `1` | ~80 Hz high-pass on the near-end |
| `spx_filter_ms` | `64` | adaptive filter length; raise for a longer echo tail |
| `spx_stereo` | `1` | use ch4 & ch5 as a stereo reference (vs their average) |
| `spx_echo_suppress` | `0` | nonlinear residual suppressor (dB, 0 = linear only) — adds cancellation at some talker cost |
| `spx_echo_suppress_active` | `0` | residual suppressor during double-talk |
| `spx_denoise` | `0` | Speex denoiser |
| `spx_noise_suppress` | `-15` | denoiser strength (dB) when enabled |
| `spx_headroom_db` | `12` | internal headroom before the 16-bit Speex core |

Change live, then reopen the mic (toggle capture) for it to take effect:
`adb shell setprop persist.vendor.amznaec.spx_filter_ms 96`.

## Building the WebRTC engine (38–50 dB) — advanced

The stronger engine links the ROM's `libwebrtc_audio_preprocessing.so` and must be
built inside the **full LineageOS 18.1 `crown` tree** (~150 GB, `repo sync`), using
jxlarrea's `shims/libamznaec/{amznaec_shim.cpp,Android.bp,build.sh}` against
`external/webrtc`. It is not talker-preserving (see the engine table). This repo ships
the standalone Speex path; the WebRTC path is left to the ROM tree by design.

## Credits

- **jxlarrea** — [`lineageos-echo-show-camera`](https://github.com/jxlarrea/lineageos-echo-show-camera)
  (MIT): documented the `pcmC0D22c` DAC-loopback echo reference and the original
  `libamznaec` shim this project is modeled on.
- **Xiph.Org / Jean-Marc Valin** — [SpeexDSP](https://github.com/xiph/speexdsp) (BSD),
  vendored under `third_party/speexdsp/`.

## License

MIT — see [LICENSE](./LICENSE). Vendored SpeexDSP under `third_party/speexdsp/` keeps
its own BSD license (`third_party/speexdsp/COPYING`).
