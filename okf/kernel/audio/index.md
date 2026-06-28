---
type: Kernel Subsystem
title: UAOS Audio Subsystem
description: 48 kHz stereo mixer, ring buffer, and pluggable host audio backends for Paula emulation.
tags: [kernel, audio, paula, ac97, pc-speaker]
timestamp: 2026-07-01T00:00:00Z
---

# UAOS Audio Subsystem

The UAOS audio subsystem turns the four Paula audio channels into a playable 48 kHz stereo stream.  It lives in `kernel/audio/` and is designed to be host-backend agnostic: the same mixer drives either the Intel ICH AC97 controller or the legacy PC speaker.

## Files

- `kernel/audio/audio.h` — public API: `AudioBackend` interface, ring buffer, mixer helpers.
- `kernel/audio/audio.c` — 48 kHz mixer and ring buffer.
- `kernel/audio/ac97.c` / `kernel/audio/ac97.h` — Intel ICH AC97 backend.
- `kernel/audio/pc_speaker.c` / `kernel/audio/pc_speaker.h` — PC speaker fallback backend.
- `kernel/audio/audio_test.c` / `kernel/audio/audio_test.h` — sine-wave and pattern tests.

## Paula Emulation

The chipset emulator (`kernel/chipset/chip_emu.c`) tracks four channels with `AUDxLCH/LCL`, `AUDxLEN`, `AUDxPER`, `AUDxVOL`, and `AUDxDAT`.  `chip_emu_audio_advance()` is called with a number of Amiga master-clock cycles and moves the channel forward:

- `byte_sel` toggles between the high and low byte of the current `AUDxDAT` word.
- When the low byte finishes, the next word is fetched from guest RAM and `AUDxLEN` is decremented.
- When `AUDxLEN` reaches zero, the corresponding `INTREQ` bit (9–12) is set.

`chip_emu_audio_sample_8bit()` returns the currently selected byte as a signed value.

## Mixer

`audio_tick()` is called every 100 Hz PIT tick.  Each tick it produces 480 stereo 16-bit frames and writes them to the ring buffer.

- The mixer maintains a 16.16 fixed-point accumulator of the PAL Amiga master clock (≈3.546895 MHz) and passes the integer delta to `chip_emu_audio_advance()` for each sample.
- Paula samples are 8-bit unsigned; they are converted to signed 16-bit and scaled by `AUDxVOL` (0–64).
- A one-pole LED-style low-pass filter is applied per channel.
- Channels 0 and 2 are panned left; channels 1 and 3 are panned right.
- The final mix is clamped to the `[-32768, 32767]` range.

## Ring Buffer

The mixer is the single producer and the active backend is the single consumer.  A 16384-frame lock-free ring buffer sits between them:

- `audio_ring_free()` / `audio_ring_ready()` report free and ready frames.
- `audio_ring_write()` writes stereo frames from the mixer.
- `audio_ring_read()` reads stereo frames for the backend.

Because the buffer is larger than one AC97 half-buffer, the backend can consume in 2048-sample bursts while the mixer produces 480 frames per tick.

## Backends

### AC97

`audio_backend_ac97` probes PCI for the Intel ICH AC97 controller (class `0x04/0x01/0x00`), enables I/O space and bus mastering, resets the codec, and sets the master and PCM-out volumes to 0 dB.  The sample rate is 48 kHz.

The DMA buffer is 4096 stereo frames (16 KiB) split into two 2048-frame halves.  Two bus-master descriptors alternate so the hardware plays one half while the mixer fills the other.  On each PIT tick, the backend checks the current descriptor index (`CIV`) and refills the descriptor that just finished playing.

### PC Speaker

`audio_backend_pcspk` is the fallback.  It reads the mix output, computes the average sample energy, and updates the PIT channel 2 square-wave frequency once per tick.  It is intentionally low-fidelity.

## Boot-Time Tests

`audio_sine_test()` and `audio_pattern_test()` are called from `uaos_kernel_main()` immediately after `audio_init()`.  They place a sine wave in guest RAM and start Paula channel 0.  Because the tests run before the chip-window page-fault handler is installed, they use direct helpers (`chip_emu_audio_set_channel()` and `chip_emu_audio_set_dmacon()`) instead of writing to the Amiga register window.

## Initialization Order

`audio_init()` runs after the MMU sandbox is active (`UAOS_MMU_Init()`) and before the chipset self-tests.  The mixer and backend are driven by `audio_tick()` from the 100 Hz PIT timer interrupt.
