/**
 * @file ambience.cpp
 *
 * Plays the weather ambience: procedurally synthesized loops (see ambience_synth.cpp),
 * crossfaded when the weather changes, on their own volume slider.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <Aulib/Stream.h>
#include <SDL.h>
#endif

#include "engine/ambience_synth.hpp"
#include "engine/sound.h"
#include "options.h"
#include "utils/log.hpp"
#include "utils/stdcompat/shared_ptr_array.hpp"

namespace devilution {

namespace {

constexpr int SynthRate = 22050;
constexpr size_t TrackCount = static_cast<size_t>(AmbienceTrack::COUNT);

struct WavData {
	ArraySharedPtr<uint8_t> data;
	size_t size = 0;
};

WavData ToWavData(const std::vector<int16_t> &pcm)
{
	const std::vector<uint8_t> wav = MakeWav(pcm, SynthRate);
	WavData result;
	result.data = MakeArraySharedPtr<uint8_t>(wav.size());
	std::memcpy(result.data.get(), wav.data(), wav.size());
	result.size = wav.size();
	return result;
}

/** Tracks are synthesized in the background the first time they are needed, then cached. */
std::array<WavData, TrackCount> readyTracks;
std::array<std::future<WavData>, TrackCount> pendingTracks;

const WavData *GetTrack(AmbienceTrack track)
{
	const size_t i = static_cast<size_t>(track);
	if (i == 0 || i >= TrackCount)
		return nullptr;
	if (readyTracks[i].size != 0)
		return &readyTracks[i];
	if (!pendingTracks[i].valid()) {
		pendingTracks[i] = std::async(std::launch::async, [track]() { return ToWavData(SynthesizeAmbience(track, SynthRate)); });
		return nullptr;
	}
	if (pendingTracks[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return nullptr;
	readyTracks[i] = pendingTracks[i].get();
	return &readyTracks[i];
}

struct Slot {
	AmbienceTrack track = AmbienceTrack::None;
	SoundSample sample;
	bool playing = false;
	float gain = 0; // current fade gain 0..1
};

std::array<Slot, 2> slots;
size_t activeSlot = 0;
float targetLevel = 0;
uint32_t lastTicks = 0;

SoundSample thunderSample;
std::array<WavData, 3> thunderTracks;
uint32_t thunderCount = 0;

/** The ambience slider as a linear gain. */
float UserGain()
{
	const int volume = *GetOptions().Audio.ambienceVolume;
	if (volume <= VOLUME_MIN)
		return 0.0f;
	const float millibel = static_cast<float>(volume) * (10000.0f / static_cast<float>(-VOLUME_MIN));
	return std::pow(10.0f, millibel / 3321.9281f);
}

void SetLinearGain(SoundSample &sample, float gain)
{
	const float mb = gain <= 0.0001f ? -10000.0f : std::max(-10000.0f, 3321.9281f * std::log10(gain));
	sample.SetVolume(static_cast<int>(mb), -10000, 0);
}

void ReleaseSlot(Slot &slot)
{
	if (slot.playing || slot.sample.IsLoaded()) {
		slot.sample.Stop();
		slot.sample.Release();
	}
	slot.playing = false;
	slot.gain = 0;
	slot.track = AmbienceTrack::None;
}

bool StartSlot(Slot &slot)
{
	const WavData *wav = GetTrack(slot.track);
	if (wav == nullptr)
		return false; // still being synthesized; try again next frame
	if (slot.sample.SetChunk(wav->data, wav->size, /*isMp3=*/false) != 0) {
		LogError(LogCategory::Audio, "Ambience: failed to load track {}", static_cast<int>(slot.track));
		slot.track = AmbienceTrack::None;
		return false;
	}
	SetLinearGain(slot.sample, 0.0f);
	if (!slot.sample.Play(/*numIterations=*/0)) {
		slot.sample.Release();
		slot.track = AmbienceTrack::None;
		return false;
	}
	slot.playing = true;
	LogVerbose("Ambience: playing track {}", static_cast<int>(slot.track));
	return true;
}

} // namespace

int sound_get_or_set_ambience_volume(int volume)
{
	if (volume == 1)
		return *GetOptions().Audio.ambienceVolume;
	GetOptions().Audio.ambienceVolume.SetValue(std::clamp(volume, VOLUME_MIN, VOLUME_MAX));
	return *GetOptions().Audio.ambienceVolume;
}

void UpdateAmbience(AmbienceTrack track, float level)
{
	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.25f, static_cast<float>(now - lastTicks) / 1000.0f);
	lastTicks = now;
	if (!gbSndInited)
		return;

	const float user = UserGain();
	if (user <= 0.0f) {
		StopAmbience();
		return;
	}

	if (slots[activeSlot].track != track) {
		// The old track fades out in the other slot while the new one fades in.
		activeSlot ^= 1;
		ReleaseSlot(slots[activeSlot]);
		slots[activeSlot].track = track;
	}
	Slot &active = slots[activeSlot];
	if (active.track != AmbienceTrack::None && !active.playing)
		StartSlot(active);

	targetLevel = std::clamp(level, 0.0f, 1.0f);
	constexpr float FadeInPerSecond = 0.35f;  // ~3 s
	constexpr float FadeOutPerSecond = 0.5f;  // ~2 s
	for (size_t i = 0; i < slots.size(); i++) {
		Slot &slot = slots[i];
		if (!slot.playing)
			continue;
		const float target = i == activeSlot ? targetLevel : 0.0f;
		if (slot.gain < target)
			slot.gain = std::min(target, slot.gain + FadeInPerSecond * dt);
		else
			slot.gain = std::max(target, slot.gain - FadeOutPerSecond * dt);
		if (i != activeSlot && slot.gain <= 0.0f) {
			ReleaseSlot(slot);
			continue;
		}
		SetLinearGain(slot.sample, slot.gain * user);
	}
}

void StopAmbience()
{
	for (Slot &slot : slots)
		ReleaseSlot(slot);
	if (thunderSample.IsLoaded()) {
		thunderSample.Stop();
		thunderSample.Release();
	}
}

void PlayThunder(float level)
{
	if (!gbSndInited || level <= 0.0f)
		return;
	const float user = UserGain();
	if (user <= 0.0f)
		return;
	// A few variations, synthesized on first use (they are short).
	WavData &wav = thunderTracks[thunderCount++ % thunderTracks.size()];
	if (wav.size == 0)
		wav = ToWavData(SynthesizeThunder(SynthRate, thunderCount * 7u + 1u));
	if (thunderSample.IsLoaded()) {
		thunderSample.Stop();
		thunderSample.Release();
	}
	if (thunderSample.SetChunk(wav.data, wav.size, /*isMp3=*/false) != 0)
		return;
	SetLinearGain(thunderSample, std::clamp(level, 0.0f, 1.0f) * 0.8f * user);
	thunderSample.Play(1);
	LogVerbose("Ambience: thunder at {:.2f}", level);
}

} // namespace devilution
