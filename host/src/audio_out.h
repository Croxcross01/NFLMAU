#pragma once
#include <cstdint>
// Speaker output for the game's XAudio render-driver frames.
using AudioConsumedFn = void (*)();
bool AudioOutStart(AudioConsumedFn onConsumed);   // false = no device / disabled (game keeps a timer clock)
bool AudioOutActive();
void AudioOutSubmit(const uint8_t* guestSamples);  // 6 planar channels x 256 big-endian floats
