/*
 * pc_speaker.h — UAOS PC speaker audio backend
 *
 * Fallback backend that converts the 48 kHz stereo ring buffer into a simple
 * square-wave tone on the legacy PC speaker.
 */

#ifndef UAOS_PC_SPEAKER_H
#define UAOS_PC_SPEAKER_H

#include "audio/audio.h"

extern AudioBackend audio_backend_pcspk;

/* Direct control helper (kept for diagnostics). */
void pc_speaker_tone(uint32_t freq_hz);

#endif /* UAOS_PC_SPEAKER_H */
