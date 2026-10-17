#include "utils/aulib_ogg_decoder.hpp"

#include <cstdint>
#include <vector>

#include <SDL.h>

#ifdef USE_SDL1
#include "utils/sdl2_to_1_2_backports.h"
#else
#include "utils/sdl2_backports.h"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../../3rdParty/stb_vorbis/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace devilution {

struct DecoderOggPriv {
	std::vector<unsigned char> data;
	stb_vorbis *vorbis = nullptr;
	stb_vorbis_info info {};
	bool eof = false;
};

DecoderOgg::DecoderOgg()
    : d(std::make_unique<DecoderOggPriv>())
{
}

DecoderOgg::~DecoderOgg()
{
	if (d->vorbis != nullptr)
		stb_vorbis_close(d->vorbis);
}

bool DecoderOgg::LooksLikeOgg(SDL_RWops *rwops)
{
	const Sint64 at = SDL_RWtell(rwops);
	char magic[4] {};
	const bool read = SDL_RWread(rwops, magic, 1, 4) == 4;
	SDL_RWseek(rwops, at, RW_SEEK_SET);
	return read && magic[0] == 'O' && magic[1] == 'g' && magic[2] == 'g' && magic[3] == 'S';
}

auto DecoderOgg::open(SDL_RWops *rwops) -> bool
{
	if (isOpen())
		return true;
	// Read the whole (compressed) file: stb_vorbis decodes from memory.
	d->data.clear();
	unsigned char chunk[16384];
	size_t got;
	while ((got = SDL_RWread(rwops, chunk, 1, sizeof(chunk))) > 0)
		d->data.insert(d->data.end(), chunk, chunk + got);
	int error = 0;
	d->vorbis = stb_vorbis_open_memory(d->data.data(), static_cast<int>(d->data.size()), &error, nullptr);
	if (d->vorbis == nullptr) {
		SDL_SetError("stb_vorbis could not open the Ogg file (error %d).", error);
		d->data.clear();
		return false;
	}
	d->info = stb_vorbis_get_info(d->vorbis);
	d->eof = false;
	setIsOpen(true);
	return true;
}

auto DecoderOgg::getChannels() const -> int
{
	return d->info.channels;
}

auto DecoderOgg::getRate() const -> int
{
	return static_cast<int>(d->info.sample_rate);
}

auto DecoderOgg::rewind() -> bool
{
	if (!isOpen())
		return false;
	stb_vorbis_seek_start(d->vorbis);
	d->eof = false;
	return true;
}

auto DecoderOgg::duration() const -> std::chrono::microseconds
{
	if (!isOpen() || d->info.sample_rate == 0)
		return {};
	const double seconds = static_cast<double>(stb_vorbis_stream_length_in_samples(d->vorbis)) / d->info.sample_rate;
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(seconds));
}

auto DecoderOgg::seekToTime(std::chrono::microseconds pos) -> bool
{
	if (!isOpen())
		return false;
	const auto frame = static_cast<unsigned int>(std::chrono::duration<double>(pos).count() * d->info.sample_rate);
	if (stb_vorbis_seek(d->vorbis, frame) == 0)
		return false;
	d->eof = false;
	return true;
}

auto DecoderOgg::doDecoding(float buf[], int len, bool & /*callAgain*/) -> int
{
	if (d->eof || !isOpen() || d->info.channels <= 0)
		return 0;
	const int channels = d->info.channels;
	int total = 0;
	while (total < len) {
		const int frames = stb_vorbis_get_samples_float_interleaved(d->vorbis, channels, buf + total, len - total);
		if (frames <= 0) {
			d->eof = true;
			break;
		}
		total += frames * channels;
	}
	return total;
}

} // namespace devilution
