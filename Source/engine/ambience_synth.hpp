/**
 * @file ambience_synth.hpp
 *
 * Procedurally generated ambient loops for the weather system (rain, wind, crickets, cave
 * drips, fire...). Everything is synthesized at runtime, so no extra audio assets are needed.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace devilution {

enum class AmbienceTrack : uint8_t {
	None,
	Breeze,      // soft outdoor wind
	NightForest, // crickets, the odd owl, a light breeze
	Rain,
	Storm,       // heavy rain, gusts and a constant low rumble
	Wind,        // gusty wind with rustling leaves
	ColdWind,    // whistling cold wind with faint ice tinkles
	Drone,       // low dungeon drone
	CaveDrips,   // echoing water drips over a low drone
	Fire,        // roaring, crackling fire
	Hell,        // deep dissonant drone, growls and distant moans
	Spores,      // damp drone with bubbling
	COUNT
};

/** @brief A seamless loop of the track, mono 16-bit PCM at the given sample rate. */
std::vector<int16_t> SynthesizeAmbience(AmbienceTrack track, int sampleRate);

/** @brief One thunder clap with rolling rumble, mono 16-bit PCM. */
std::vector<int16_t> SynthesizeThunder(int sampleRate, uint32_t seed);

/** @brief Wraps mono 16-bit PCM in a WAV container. */
std::vector<uint8_t> MakeWav(const std::vector<int16_t> &samples, int sampleRate);

} // namespace devilution
