# Local performance fork

This working tree starts at DirectAudio `directaudio-v1.4.0`, commit
`8cc1227bf1b929521e6b2df2509d8b309c4c4f25`, by The412Banner.
The upstream copyright, AUTHORS, NOTICE and LGPL-2.1-or-later license remain intact.

## Changes dated 2026-10-24

The 48 kHz float32 stereo mixer path with inactive software gains processes
contiguous spans of the source ring, using ARM64 NEON to add four samples at a
time. Format conversion, active gains, other channel counts and resampling retain
the original implementation. The ring state, locking and limiter are unchanged.
The fast path performs separate sample additions without fast-math or reassociation.
Source-first additions match the ARM64 baseline's NaN payload selection.

`build-local.sh` builds a native Android ARM64 unixlib against a caller-supplied
configured Wine tree and matching ARM64 `ntdll.so`. It links directly against a
caller-supplied `libwaudio.so` bridge. Its ELF dependency is therefore already
`libwaudio.so`; no post-build dependency string patch is required.
The Wine driver remains named `winedirectaudio.so` and retains its two 37-entry
Wine 11 classic call tables. The bridge is a separate dependency, not vendored here.

## Reproduce

```sh
bash build-local.sh /path/to/android-ndk \
  /path/to/wine-source /path/to/configured-wine-build \
  /path/to/arm64/ntdll.so /path/to/libwaudio.so ./out
```

The script currently targets Linux-hosted NDK, ARM64, Android API 28 and the classic
Wine 11 mmdevapi ABI. The Wine source/build must provide generated headers and
`dlls/mmdevapi/unixlib.h` for that ABI. The two existing PE `.drv` files are not
rebuilt by this script. Rebuilding the complete PE/unixlib matrix still uses the
upstream Wine integration and CI instructions; that matrix has not been validated
for this local fork. The inherited CI workflow still links platform `libaaudio.so`.

Use the Android test device's adb binary to push and run `out/mixer-test`.
It compares both raw voice mixing and complete limiter output against an unchanged
baseline extracted from the pinned upstream commit. `tests/refresh-reference.sh`
regenerates that baseline, including its original license header.

On local test device, 12,016 deterministic cases passed bit-for-bit, including
ring wraps, partial buffers, format/channel fallbacks, gains, resampling and IEEE
edge values. Six interleaved pairs on CPU 7 measured a 192-frame float stereo
voice at median 1025.05 ns versus 87.905 ns; the complete single-voice mix block
(clear, locks and limiter included) measured 1601.255 ns versus 663.785 ns.
These are warm microbenchmarks, not game FPS or whole-process CPU results.

`out/mixer-extended-test` adds 56,904 bit-for-bit/oracle cases: classic and
extensible PCM8/16/24/32 and float32, 1-8 channels, six rates (8-192 kHz), four
gain states, padded frames and 4-byte aligned buffers at different SIMD alignments.
It covers distinct quiet/signaling NaN payloads and subnormals with four ARM64
FPCR flush-to-zero/default-NaN combinations. Guard pages check source/output
boundaries and an independent indexing oracle checks wraps and underrun tails.
Multi-voice tests mix fast and fallback paths with 0, 1, 2, 8 and 64 voices.
`out/mixer-extended-ubsan` runs the same suite with undefined-behavior traps;
both builds passed on the device. No sanitizer runtime installation is needed.

