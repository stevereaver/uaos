/*
 * ac97.h — UAOS Intel ICH AC97 host audio backend
 *
 * Exposes the AC97 controller as an AudioBackend.  The backend is selected by
 * the mixer if the PCI AC97 controller is found; otherwise the PC speaker is
 * used as a fallback.
 */

#ifndef UAOS_AC97_H
#define UAOS_AC97_H

#include "audio/audio.h"

extern AudioBackend audio_backend_ac97;

#endif /* UAOS_AC97_H */
