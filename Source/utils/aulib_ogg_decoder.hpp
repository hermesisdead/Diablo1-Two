/**
 * @file utils/aulib_ogg_decoder.hpp
 *
 * Ogg Vorbis decoding for SDL_audiolib (stb_vorbis), for music in campaigns. The whole compressed
 * file is kept in memory (a few megabytes for a song); samples are decoded as they play.
 */
#pragma once

#include <chrono>
#include <memory>

#include <Aulib/Decoder.h>

namespace devilution {

class DecoderOgg final : public Aulib::Decoder {
public:
	DecoderOgg();
	~DecoderOgg() override;

	auto open(SDL_RWops *rwops) -> bool override;
	[[nodiscard]] auto getChannels() const -> int override;
	[[nodiscard]] auto getRate() const -> int override;
	auto rewind() -> bool override;
	[[nodiscard]] auto duration() const -> std::chrono::microseconds override;
	auto seekToTime(std::chrono::microseconds pos) -> bool override;

	/** @brief The data starts like an Ogg file ("OggS"). */
	[[nodiscard]] static bool LooksLikeOgg(SDL_RWops *rwops);

protected:
	auto doDecoding(float buf[], int len, bool &callAgain) -> int override;

private:
	std::unique_ptr<struct DecoderOggPriv> d;
};

} // namespace devilution
