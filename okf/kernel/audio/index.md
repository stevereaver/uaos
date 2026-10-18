---
type: Kernel Subsystem
title: UAOS Audio Subsystem
description: 48 kHz stereo mixer, ring buffer, and pluggable host audio backends for Paula emulation.
tags: [kernel, audio, paula, hda, ac97, pc-speaker]
timestamp: 2026-10-17T16:30:00Z
---

# UAOS Audio Subsystem

The UAOS audio subsystem turns the four Paula audio channels into a playable 48 kHz stereo stream.  It lives in `kernel/audio/` and is designed to be host-backend agnostic: the same mixer drives an Intel HD Audio controller, the Intel ICH AC97 controller, or the legacy PC speaker. They are selected in that order (HDA → AC97 → PC speaker, `audio_select_backend`).

## Files

- `kernel/audio/audio.h` — public API: `AudioBackend` interface, ring buffer, mixer helpers.
- `kernel/audio/audio.c` — 48 kHz mixer and ring buffer.
- `kernel/audio/hda.c` — Intel HD Audio backend (UAOS-140), polled with no IRQs. It matches any class 04/03 controller (MBP4,1 ICH8 8086:284b, QEMU `ich9-intel-hda`/`intel-hda`). Bring-up: BAR0 MMIO, TCSEL clear, bounded GCTL.CRST reset, then CORB/RIRB (RIRBCTL=0x03, RIRBSTS cleared per response) with an Immediate Command (ICOI) fallback. It enumerates the AFG and runs a DFS from every Line-Out/Speaker/HP pin back to a DAC, setting D0, connection select, amps at 0 dB, pin OUT/HP and EAPD. Playback uses a 4-entry BDL on the first output stream (tag 1, 48 kHz/16-bit/2ch), refilled from the ring by polling LPIB in `service()`. Apple SSID 0x106b gets Linux's `ALC889_FIXUP_MBP_VREF` + `ALC882_FIXUP_GPIO1`. QEMU-verified with a WAV capture; audibility on MBP4,1 is still to be checked on metal.
- `kernel/audio/ac97.c` / `kernel/audio/ac97.h` — Intel ICH AC97 backend.
- `kernel/audio/pc_speaker.c` / `kernel/audio/pc_speaker.h` — PC speaker fallback backend.
- `kernel/audio/audio_test.c` / `kernel/audio/audio_test.h` — sine-wave and pattern tests.
- `kernel/shell/cmd_audiotest.c` — `audiotest` command. It prints the active backend, then uses `audio_ring_solo()` (which pauses the Paula mixer's ring writes) to inject a 2 s 1 kHz sine end to end.

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

## Master Volume / Mute (UAOS-315)

`audio_set_volume(0-100)` / `audio_set_mute()` / `audio_mute_toggle()` / `audio_adjust_volume()` expose a master output control that defaults to 50.  Two layers apply it: `audio_ring_write()` scales every frame by `vol/100` (so the Paula mix *and* `audio_ring_solo` injections are covered), and an optional `AudioBackend.set_volume(pct, muted)` hook programs the codec amp.  The HDA hook keeps a list of one output amp per configured pin→DAC path (DAC preferred, deduped for the shared DAC) and issues `SET_AMP_GAIN_MUTE` (payload `0xB000 | mute<<7 | gain`) with gain linear across the amp's step range; the AC97 hook drives PCM-out register 0x18 (5-bit attenuation + mute bit).  The `volume` shell command prints `volume: NN|MUTE (backend X)` and accepts a value or `MUTE`.

Metal debugging (commit `ba024b1`): `GET_SUBSYSTEM_ID` was addressed to codec nid 0 *before* the AFG had been found — real codecs only answer that verb on the function group, so the Apple SSID check never matched and the ALC889 GPIO1 fixup (speaker-amp power on MacBooks) silently never ran.  The read now targets the AFG and falls back to the controller's PCI subsystem vendor (0x2C, `0x106B` = Apple).  `CONFIG_DEFAULT` is read via its proper verb `0xF1C` rather than `GET_PARAMETER(0x1C)`, which real codecs don't answer as a parameter.  `C:hdadump` dumps live per-widget state (power, out-amp readback, pin ctl, EAPD, GPIO data, stream LPIB/CTL) through the verb path for bring-up debugging.  Before this fix, the metal DMA path was already proven healthy: SD4 showed RUN+tag1, CBL `0x4000`, LPIB advancing, and the BDL-pointed PCM buffer contained nonzero 1 kHz sine samples during `audiotest`.

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
