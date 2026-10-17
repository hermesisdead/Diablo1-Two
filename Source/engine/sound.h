/**
 * @file sound.h
 *
 * Interface of functions setting up the audio pipeline.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "engine/ambience_synth.hpp"
#include "levels/gendung.h"
#include "utils/attributes.h"

#ifndef NOSOUND
#ifdef USE_SDL3
struct MIX_Mixer;
#endif

#include "utils/soundsample.h"
#endif

namespace devilution {

enum _music_id : uint8_t {
	TMUSIC_TOWN,
	TMUSIC_CATHEDRAL,
	TMUSIC_CATACOMBS,
	TMUSIC_CAVES,
	TMUSIC_HELL,
	TMUSIC_NEST,
	TMUSIC_CRYPT,
	TMUSIC_INTRO,
	NUM_MUSIC,
};

struct TSnd {
	uint32_t start_tc;

#ifndef NOSOUND
	SoundSample DSB;

	bool isPlaying()
	{
		return DSB.IsPlaying();
	}
#else
	bool isPlaying()
	{
		return false;
	}
#endif

	~TSnd();
};

extern bool gbSndInited;
#ifndef NOSOUND
#ifdef USE_SDL3
extern MIX_Mixer *CurrentMixer;
#endif
#endif

extern _music_id sgnMusicTrack;

void ClearDuplicateSounds();
void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume);
std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream = false);
std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool stream = false);
void snd_init();
void snd_deinit();
_music_id GetLevelMusic(dungeon_type dungeonType);
void music_stop();
void music_start(_music_id nTrack);
/**
 * @brief Changes the music smoothly: the song playing now fades out while the new one fades in.
 * @param file A song file (campaign music, .ogg or .wav) to play instead of the track, or empty.
 */
void music_crossfade_to(_music_id nTrack, std::string_view file = {});
void sound_disable_music(bool disable);
int sound_get_or_set_music_volume(int volume);
int sound_get_or_set_sound_volume(int volume);
int SoundGetOrSetAudioCuesVolume(int volume);
int sound_get_or_set_ambience_volume(int volume);

/**
 * @brief Keeps the weather ambience playing: call every frame with the wanted track and its
 * level (0..1). Changing track crossfades; level changes are smoothed.
 */
void UpdateAmbience(AmbienceTrack track, float level);
/** @brief Stops the ambience immediately (level change, leaving the game). */
void StopAmbience();
/** @brief A thunder clap at the given level (0..1), on the ambience volume. */
void PlayThunder(float level);
void music_mute();
void music_unmute();
/** @brief Plays a recording once at the sound effects volume (a townsperson's voice line), stopping the last one. */
void PlayVoiceFile(const std::string &path);

/* data */

extern DVL_API_FOR_TEST bool gbMusicOn;
extern DVL_API_FOR_TEST bool gbSoundOn;

} // namespace devilution
