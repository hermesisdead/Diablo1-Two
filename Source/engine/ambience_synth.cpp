#include "engine/ambience_synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace devilution {

namespace {

constexpr float Pi = 3.14159265358979f;
constexpr float TwoPi = 2.0f * Pi;

/** Loop length and the crossfade used to make the seam inaudible. */
constexpr float LoopSeconds = 24.0f;
constexpr float SeamSeconds = 2.0f;

class Rng {
public:
	explicit Rng(uint64_t seed)
	    : state_(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL)
	{
	}
	uint32_t Next()
	{
		state_ ^= state_ << 13;
		state_ ^= state_ >> 7;
		state_ ^= state_ << 17;
		return static_cast<uint32_t>(state_ >> 11);
	}
	/** Uniform in [0, 1). */
	float Unit() { return static_cast<float>(Next() & 0xFFFFFF) / 16777216.0f; }
	/** Uniform in [-1, 1). */
	float White() { return Unit() * 2.0f - 1.0f; }
	float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }

private:
	uint64_t state_;
};

struct OnePole {
	float a = 0;
	float y = 0;
	void SetCutoff(float fc, float fs) { a = 1.0f - std::exp(-TwoPi * fc / fs); }
	float Process(float x)
	{
		y += a * (x - y);
		return y;
	}
};

/** RBJ biquad (band-pass with constant peak gain, low-pass, high-pass). */
struct Biquad {
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

	void SetBandPass(float fc, float q, float fs)
	{
		const float w = TwoPi * fc / fs, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
		const float a0 = 1.0f + alpha;
		b0 = alpha / a0;
		b1 = 0;
		b2 = -alpha / a0;
		a1 = -2.0f * c / a0;
		a2 = (1.0f - alpha) / a0;
	}
	void SetLowPass(float fc, float q, float fs)
	{
		const float w = TwoPi * fc / fs, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
		const float a0 = 1.0f + alpha;
		b0 = (1.0f - c) / 2.0f / a0;
		b1 = (1.0f - c) / a0;
		b2 = b0;
		a1 = -2.0f * c / a0;
		a2 = (1.0f - alpha) / a0;
	}
	void SetHighPass(float fc, float q, float fs)
	{
		const float w = TwoPi * fc / fs, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
		const float a0 = 1.0f + alpha;
		b0 = (1.0f + c) / 2.0f / a0;
		b1 = -(1.0f + c) / a0;
		b2 = b0;
		a1 = -2.0f * c / a0;
		a2 = (1.0f - alpha) / a0;
	}
	float Process(float x)
	{
		const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		return y;
	}
};

/** A slowly wandering control value in [0, 1] (smoothed random steps). */
class Wander {
public:
	Wander(Rng &rng, float stepSeconds, float fs)
	    : rng_(rng)
	    , step_(std::max(1, static_cast<int>(stepSeconds * fs)))
	{
		from_ = rng_.Unit();
		to_ = rng_.Unit();
	}
	float Next()
	{
		if (++pos_ >= step_) {
			pos_ = 0;
			from_ = to_;
			to_ = rng_.Unit();
		}
		float t = static_cast<float>(pos_) / static_cast<float>(step_);
		t = t * t * (3.0f - 2.0f * t);
		return from_ + (to_ - from_) * t;
	}

private:
	Rng &rng_;
	int step_;
	int pos_ = 0;
	float from_, to_;
};

using Buffer = std::vector<float>;

size_t SamplesFor(float seconds, int fs) { return static_cast<size_t>(seconds * static_cast<float>(fs)); }

void Mix(Buffer &dst, const Buffer &src, float gain)
{
	for (size_t i = 0; i < dst.size() && i < src.size(); i++)
		dst[i] += src[i] * gain;
}

/** Adds a short event at a sample offset (clipped to the buffer). */
template <typename Fn>
void AddEvent(Buffer &dst, size_t start, size_t length, Fn &&sample)
{
	for (size_t i = 0; i < length && start + i < dst.size(); i++)
		dst[start + i] += sample(i);
}

// --- Building blocks -----------------------------------------------------------------------------

/** Wind: band-passed noise whose colour and loudness wander like gusts. */
Buffer WindLayer(Rng &rng, size_t n, int fs, float lowFc, float highFc, float gustSeconds, float q)
{
	Buffer out(n);
	Biquad bp;
	OnePole lp;
	lp.SetCutoff(1800, static_cast<float>(fs));
	Wander colour(rng, gustSeconds * 1.3f, static_cast<float>(fs));
	Wander gust(rng, gustSeconds, static_cast<float>(fs));
	for (size_t i = 0; i < n; i++) {
		const float c = colour.Next();
		if ((i & 31) == 0)
			bp.SetBandPass(lowFc + (highFc - lowFc) * c, q, static_cast<float>(fs));
		const float g = gust.Next();
		out[i] = bp.Process(lp.Process(rng.White())) * (0.25f + 0.75f * g * g);
	}
	return out;
}

/** Whistling: narrow resonances that glide around, like wind through gaps. */
Buffer WhistleLayer(Rng &rng, size_t n, int fs, float lowFc, float highFc)
{
	Buffer out(n);
	Biquad bp;
	Wander pitch(rng, 2.5f, static_cast<float>(fs));
	Wander level(rng, 1.7f, static_cast<float>(fs));
	for (size_t i = 0; i < n; i++) {
		if ((i & 31) == 0)
			bp.SetBandPass(lowFc + (highFc - lowFc) * pitch.Next(), 22.0f, static_cast<float>(fs));
		const float l = level.Next();
		out[i] = bp.Process(rng.White()) * std::max(0.0f, l - 0.35f) * 2.2f;
	}
	return out;
}

/** Low drone made of slowly breathing, slightly detuned partials. */
Buffer DroneLayer(Rng &rng, size_t n, int fs, float base, const std::vector<float> &ratios)
{
	Buffer out(n);
	struct Partial {
		float freq, amp, phase, lfoFreq, lfoPhase;
	};
	std::vector<Partial> partials;
	for (size_t k = 0; k < ratios.size(); k++) {
		// LFO periods that divide the loop, so the drone itself loops cleanly.
		const float lfo = static_cast<float>(1 + rng.Next() % 3) / LoopSeconds;
		partials.push_back({ base * ratios[k], 1.0f / static_cast<float>(1 + k), rng.Unit() * TwoPi, lfo, rng.Unit() * TwoPi });
	}
	OnePole air;
	air.SetCutoff(260, static_cast<float>(fs));
	for (size_t i = 0; i < n; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(fs);
		float s = 0;
		for (const Partial &p : partials)
			s += std::sin(TwoPi * p.freq * t + p.phase) * p.amp * (0.55f + 0.45f * std::sin(TwoPi * p.lfoFreq * t + p.lfoPhase));
		out[i] = s * 0.5f + air.Process(rng.White()) * 0.6f;
	}
	return out;
}

/** Rain: a hiss bed, a darker body and lots of tiny droplet ticks. */
Buffer RainLayer(Rng &rng, size_t n, int fs, float dropsPerSecond)
{
	Buffer out(n);
	Biquad hp, lp, body;
	hp.SetHighPass(500, 0.7f, static_cast<float>(fs));
	lp.SetLowPass(3800, 0.7f, static_cast<float>(fs));
	body.SetLowPass(900, 0.7f, static_cast<float>(fs));
	Wander swell(rng, 3.0f, static_cast<float>(fs));
	Wander grain(rng, 0.012f, static_cast<float>(fs)); // rain is granular, not a steady hiss
	for (size_t i = 0; i < n; i++) {
		const float w = rng.White();
		const float g = grain.Next();
		out[i] = lp.Process(hp.Process(w)) * 0.26f * (0.55f + 0.45f * g) * (0.8f + 0.2f * swell.Next()) + body.Process(rng.White()) * 0.35f;
	}
	// Droplets hitting leaves, stone and puddles.
	const size_t drops = static_cast<size_t>(dropsPerSecond * static_cast<float>(n) / static_cast<float>(fs));
	for (size_t d = 0; d < drops; d++) {
		const size_t start = rng.Next() % n;
		const float freq = rng.Range(1800.0f, 5200.0f);
		const float amp = rng.Unit() * rng.Unit() * 0.28f;
		const float decay = rng.Range(0.0015f, 0.005f);
		const size_t len = SamplesFor(decay * 6.0f, fs);
		AddEvent(out, start, len, [&](size_t k) {
			const float t = static_cast<float>(k) / static_cast<float>(fs);
			return std::sin(TwoPi * freq * t) * std::exp(-t / decay) * amp;
		});
	}
	// Heavier drops on puddles: low "plops".
	const size_t plops = std::min(drops / 10, static_cast<size_t>(25.0f * static_cast<float>(n) / static_cast<float>(fs)));
	for (size_t d = 0; d < plops; d++) {
		const size_t start = rng.Next() % n;
		const float freq = rng.Range(250.0f, 600.0f);
		const float amp = rng.Unit() * 0.18f;
		const size_t len = SamplesFor(0.06f, fs);
		AddEvent(out, start, len, [&](size_t k) {
			const float t = static_cast<float>(k) / static_cast<float>(fs);
			return std::sin(TwoPi * freq * (1.0f + 2.5f * t) * t) * std::exp(-t / 0.012f) * amp;
		});
	}
	return out;
}

Buffer RumbleLayer(Rng &rng, size_t n, int fs, float cutoff)
{
	Buffer out(n);
	Biquad lp1, lp2;
	lp1.SetLowPass(cutoff, 0.8f, static_cast<float>(fs));
	lp2.SetLowPass(cutoff, 0.8f, static_cast<float>(fs));
	Wander level(rng, 2.2f, static_cast<float>(fs));
	for (size_t i = 0; i < n; i++)
		out[i] = lp2.Process(lp1.Process(rng.White())) * (0.4f + 0.6f * level.Next()) * 3.0f;
	return out;
}

/** Crickets: each one chirps a few pulses of a pure high tone on its own rhythm. */
Buffer CricketsLayer(Rng &rng, size_t n, int fs)
{
	Buffer out(n);
	const int crickets = 4;
	for (int c = 0; c < crickets; c++) {
		const float freq = rng.Range(3600.0f, 4800.0f);
		const float period = rng.Range(0.55f, 1.4f);
		const int pulses = 2 + static_cast<int>(rng.Next() % 3);
		const float pulseLen = rng.Range(0.018f, 0.03f);
		const float pulseGap = pulseLen * rng.Range(1.6f, 2.2f);
		const float amp = rng.Range(0.05f, 0.11f);
		float t0 = rng.Unit() * period;
		// Crickets go quiet for a while now and then.
		const float restStart = rng.Unit() * LoopSeconds;
		const float restLen = rng.Range(2.0f, 6.0f);
		while (t0 < static_cast<float>(n) / static_cast<float>(fs)) {
			const bool resting = t0 > restStart && t0 < restStart + restLen;
			if (!resting) {
				for (int p = 0; p < pulses; p++) {
					const size_t start = SamplesFor(t0 + static_cast<float>(p) * pulseGap, fs);
					const size_t len = SamplesFor(pulseLen, fs);
					AddEvent(out, start, len, [&](size_t k) {
						const float t = static_cast<float>(k) / static_cast<float>(fs);
						const float env = std::sin(Pi * static_cast<float>(k) / static_cast<float>(len));
						return std::sin(TwoPi * freq * t + 0.3f * std::sin(TwoPi * 90.0f * t)) * env * env * amp;
					});
				}
			}
			t0 += period * rng.Range(0.92f, 1.08f);
		}
	}
	return out;
}

/** A distant owl: "hoo... hoo-hoo". */
void AddOwl(Buffer &out, Rng &rng, int fs, float at)
{
	const float base = rng.Range(330.0f, 400.0f);
	const float notes[3][2] = { { 0.0f, 0.45f }, { 0.75f, 0.22f }, { 1.05f, 0.55f } };
	OnePole lp;
	lp.SetCutoff(1200, static_cast<float>(fs));
	for (const auto &note : notes) {
		const size_t start = SamplesFor(at + note[0], fs);
		const size_t len = SamplesFor(note[1], fs);
		const float drop = note[0] > 0.5f ? 0.94f : 1.0f;
		AddEvent(out, start, len, [&](size_t k) {
			const float t = static_cast<float>(k) / static_cast<float>(fs);
			const float u = static_cast<float>(k) / static_cast<float>(len);
			const float env = std::sin(Pi * u) * (1.0f - 0.4f * u);
			const float f = base * drop * (1.0f - 0.05f * u);
			const float s = std::sin(TwoPi * f * t) + 0.25f * std::sin(TwoPi * 2.0f * f * t) + 0.04f * rng.White();
			return lp.Process(s) * env * 0.09f;
		});
	}
}

/** Water drops in a cave: a pitched "plink" that falls in pitch, with a couple of echoes. */
void AddDrip(Buffer &out, Rng &rng, int fs, size_t start)
{
	const float f0 = rng.Range(900.0f, 1700.0f);
	const float amp = rng.Range(0.12f, 0.28f);
	const size_t len = SamplesFor(0.25f, fs);
	const float echoes[3][2] = { { 0.0f, 1.0f }, { 0.19f, 0.32f }, { 0.41f, 0.14f } };
	for (const auto &e : echoes) {
		const size_t s0 = start + SamplesFor(e[0], fs);
		float phase = 0;
		AddEvent(out, s0, len, [&](size_t k) {
			const float t = static_cast<float>(k) / static_cast<float>(fs);
			const float f = f0 * (0.55f + 0.45f * std::exp(-t / 0.018f));
			phase += TwoPi * f / static_cast<float>(fs);
			return std::sin(phase) * std::exp(-t / 0.045f) * amp * e[1];
		});
	}
}

/** Fire: low roar plus crackles and the odd pop. */
Buffer FireLayer(Rng &rng, size_t n, int fs, float cracklesPerSecond)
{
	Buffer out = RumbleLayer(rng, n, fs, 220.0f);
	for (float &s : out)
		s *= 0.6f;
	Biquad hp;
	hp.SetHighPass(1400, 0.7f, static_cast<float>(fs));
	Buffer crackle(n, 0.0f);
	const size_t count = static_cast<size_t>(cracklesPerSecond * static_cast<float>(n) / static_cast<float>(fs));
	for (size_t c = 0; c < count; c++) {
		// Crackles come in little clusters.
		const size_t start = rng.Next() % n;
		const int cluster = 1 + static_cast<int>(rng.Next() % 4);
		for (int k = 0; k < cluster; k++) {
			const size_t s0 = start + SamplesFor(rng.Range(0.0f, 0.08f), fs);
			const float decay = rng.Range(0.0007f, 0.004f);
			const float amp = rng.Unit() * rng.Unit() * 0.9f;
			AddEvent(crackle, s0, SamplesFor(decay * 6.0f, fs), [&](size_t j) {
				const float t = static_cast<float>(j) / static_cast<float>(fs);
				return rng.White() * std::exp(-t / decay) * amp;
			});
		}
	}
	for (size_t i = 0; i < n; i++)
		out[i] += hp.Process(crackle[i]);
	return out;
}

// --- Tracks --------------------------------------------------------------------------------------

Buffer Track(AmbienceTrack track, int fs)
{
	Rng rng(static_cast<uint64_t>(track) * 7919u + 17u);
	const size_t n = SamplesFor(LoopSeconds + SeamSeconds, fs);
	Buffer out(n, 0.0f);
	switch (track) {
	case AmbienceTrack::Breeze:
		Mix(out, WindLayer(rng, n, fs, 220, 650, 3.5f, 0.8f), 1.0f);
		Mix(out, RumbleLayer(rng, n, fs, 90), 0.2f);
		break;
	case AmbienceTrack::NightForest:
		Mix(out, CricketsLayer(rng, n, fs), 1.0f);
		Mix(out, WindLayer(rng, n, fs, 200, 520, 4.0f, 0.8f), 0.35f);
		AddOwl(out, rng, fs, 6.5f);
		AddOwl(out, rng, fs, 17.0f);
		break;
	case AmbienceTrack::Rain:
		Mix(out, RainLayer(rng, n, fs, 380.0f), 1.0f);
		break;
	case AmbienceTrack::Storm:
		Mix(out, RainLayer(rng, n, fs, 650.0f), 1.25f);
		Mix(out, WindLayer(rng, n, fs, 250, 900, 2.2f, 0.7f), 0.8f);
		Mix(out, RumbleLayer(rng, n, fs, 70), 0.45f);
		break;
	case AmbienceTrack::Wind: {
		Mix(out, WindLayer(rng, n, fs, 260, 1100, 1.8f, 0.6f), 1.0f);
		// Leaves rustling with the gusts.
		Buffer rustle(n);
		Biquad hp;
		hp.SetHighPass(2500, 0.7f, static_cast<float>(fs));
		Wander gust(rng, 1.8f, static_cast<float>(fs));
		Wander flutter(rng, 0.06f, static_cast<float>(fs));
		for (size_t i = 0; i < n; i++) {
			const float g = gust.Next();
			rustle[i] = hp.Process(rng.White()) * g * g * g * flutter.Next() * 0.9f;
		}
		Mix(out, rustle, 1.0f);
		break;
	}
	case AmbienceTrack::ColdWind: {
		Mix(out, WindLayer(rng, n, fs, 300, 800, 3.0f, 0.9f), 0.8f);
		Mix(out, WhistleLayer(rng, n, fs, 650, 1250), 0.5f);
		Mix(out, WhistleLayer(rng, n, fs, 1100, 1900), 0.3f);
		// Ice tinkles.
		for (int k = 0; k < 14; k++) {
			const size_t start = rng.Next() % n;
			const float f = rng.Range(2800.0f, 5200.0f);
			const float amp = rng.Range(0.015f, 0.04f);
			AddEvent(out, start, SamplesFor(0.4f, fs), [&](size_t j) {
				const float t = static_cast<float>(j) / static_cast<float>(fs);
				return (std::sin(TwoPi * f * t) + 0.5f * std::sin(TwoPi * f * 2.76f * t)) * std::exp(-t / 0.09f) * amp;
			});
		}
		break;
	}
	case AmbienceTrack::Drone:
		Mix(out, DroneLayer(rng, n, fs, 55.0f, { 1.0f, 1.007f, 1.5f, 2.0f, 2.997f }), 1.0f);
		break;
	case AmbienceTrack::CaveDrips: {
		Mix(out, DroneLayer(rng, n, fs, 49.0f, { 1.0f, 1.5f, 2.003f }), 0.45f);
		const int drips = static_cast<int>(LoopSeconds * 1.6f);
		for (int d = 0; d < drips; d++)
			AddDrip(out, rng, fs, rng.Next() % n);
		// A faint trickle somewhere.
		Biquad bp;
		bp.SetBandPass(2200, 2.0f, static_cast<float>(fs));
		Wander flutter(rng, 0.03f, static_cast<float>(fs));
		for (size_t i = 0; i < n; i++)
			out[i] += bp.Process(rng.White()) * flutter.Next() * 0.05f;
		break;
	}
	case AmbienceTrack::Fire:
		Mix(out, FireLayer(rng, n, fs, 9.0f), 1.0f);
		break;
	case AmbienceTrack::Hell: {
		Mix(out, DroneLayer(rng, n, fs, 36.7f, { 1.0f, 1.5f, 2.0f, 2.12f, 2.83f }), 1.0f);
		// A low growl that pulses.
		Buffer growl = RumbleLayer(rng, n, fs, 140);
		for (size_t i = 0; i < n; i++) {
			const float t = static_cast<float>(i) / static_cast<float>(fs);
			growl[i] *= 0.6f + 0.4f * std::sin(TwoPi * 6.5f * t) * std::sin(TwoPi * (2.0f / LoopSeconds) * t);
		}
		Mix(out, growl, 0.5f);
		Mix(out, FireLayer(rng, n, fs, 3.0f), 0.35f);
		// Distant moans.
		for (const float at : { 5.0f, 15.5f }) {
			const float f0 = rng.Range(170.0f, 230.0f);
			float phase = 0;
			AddEvent(out, SamplesFor(at, fs), SamplesFor(2.6f, fs), [&](size_t j) {
				const float t = static_cast<float>(j) / static_cast<float>(fs);
				const float u = t / 2.6f;
				const float f = f0 * (1.0f - 0.35f * u) * (1.0f + 0.02f * std::sin(TwoPi * 5.0f * t));
				phase += TwoPi * f / static_cast<float>(fs);
				return (std::sin(phase) + 0.3f * std::sin(2.0f * phase)) * std::sin(Pi * u) * 0.07f;
			});
		}
		break;
	}
	case AmbienceTrack::Spores: {
		Mix(out, DroneLayer(rng, n, fs, 46.0f, { 1.0f, 1.26f, 1.5f }), 0.5f);
		const int bubbles = static_cast<int>(LoopSeconds * 3.0f);
		for (int b = 0; b < bubbles; b++) {
			const size_t start = rng.Next() % n;
			const float f0 = rng.Range(160.0f, 420.0f);
			const float amp = rng.Range(0.05f, 0.16f);
			float phase = 0;
			AddEvent(out, start, SamplesFor(0.12f, fs), [&](size_t j) {
				const float t = static_cast<float>(j) / static_cast<float>(fs);
				phase += TwoPi * f0 * (1.0f + 6.0f * t) / static_cast<float>(fs);
				return std::sin(phase) * std::exp(-t / 0.025f) * amp;
			});
		}
		break;
	}
	default:
		break;
	}
	return out;
}

/** Crossfades the extra tail into the head so the loop has no seam, then trims to length. */
Buffer MakeLoop(Buffer in, int fs)
{
	const size_t loop = SamplesFor(LoopSeconds, fs);
	const size_t seam = SamplesFor(SeamSeconds, fs);
	if (in.size() < loop + seam)
		return in;
	for (size_t i = 0; i < seam; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(seam);
		// Equal-power crossfade.
		in[i] = in[i] * std::sin(t * Pi * 0.5f) + in[loop + i] * std::cos(t * Pi * 0.5f);
	}
	in.resize(loop);
	return in;
}

/** Brings a buffer to a target loudness and converts to 16-bit with a soft limiter. */
std::vector<int16_t> ToPcm(const Buffer &in, float targetRms)
{
	double sum = 0;
	for (const float s : in)
		sum += static_cast<double>(s) * s;
	const float rms = in.empty() ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(in.size())));
	const float gain = rms > 1e-6f ? targetRms / rms : 0.0f;
	std::vector<int16_t> out(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		const float s = std::tanh(in[i] * gain * 1.1f) / 1.1f;
		out[i] = static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32000.0f);
	}
	return out;
}

float TargetLoudness(AmbienceTrack track)
{
	switch (track) {
	case AmbienceTrack::Breeze: return 0.10f;
	case AmbienceTrack::NightForest: return 0.07f;
	case AmbienceTrack::Rain: return 0.13f;
	case AmbienceTrack::Storm: return 0.17f;
	case AmbienceTrack::Wind: return 0.13f;
	case AmbienceTrack::ColdWind: return 0.10f;
	case AmbienceTrack::Drone: return 0.10f;
	case AmbienceTrack::CaveDrips: return 0.08f;
	case AmbienceTrack::Fire: return 0.11f;
	case AmbienceTrack::Hell: return 0.13f;
	case AmbienceTrack::Spores: return 0.08f;
	default: return 0.1f;
	}
}

void PutLe(std::vector<uint8_t> &v, uint32_t value, int bytes)
{
	for (int i = 0; i < bytes; i++)
		v.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<int16_t> SynthesizeAmbience(AmbienceTrack track, int sampleRate)
{
	if (track == AmbienceTrack::None || track >= AmbienceTrack::COUNT)
		return {};
	return ToPcm(MakeLoop(Track(track, sampleRate), sampleRate), TargetLoudness(track));
}

std::vector<int16_t> SynthesizeThunder(int fs, uint32_t seed)
{
	Rng rng(seed + 0xBADC0DEu);
	const float seconds = rng.Range(5.0f, 7.5f);
	const size_t n = SamplesFor(seconds, fs);
	Buffer out(n, 0.0f);
	// The crack: a bright burst at the start (quieter for distant strikes).
	const float crack = rng.Range(0.2f, 1.0f);
	Biquad hp;
	hp.SetHighPass(900, 0.7f, static_cast<float>(fs));
	for (size_t i = 0; i < SamplesFor(0.25f, fs); i++) {
		const float t = static_cast<float>(i) / static_cast<float>(fs);
		out[i] += hp.Process(rng.White()) * std::exp(-t / 0.05f) * crack * 0.8f;
	}
	// The rumble: low noise with a few rolling swells.
	Biquad lp1, lp2, lp3;
	lp1.SetLowPass(110, 0.7f, static_cast<float>(fs));
	lp2.SetLowPass(110, 0.7f, static_cast<float>(fs));
	lp3.SetLowPass(320, 0.7f, static_cast<float>(fs));
	const int swells = 3 + static_cast<int>(rng.Next() % 3);
	float centres[6], widths[6], heights[6];
	for (int s = 0; s < swells; s++) {
		centres[s] = rng.Range(0.2f, seconds * 0.55f);
		widths[s] = rng.Range(0.25f, 0.9f);
		heights[s] = rng.Range(0.5f, 1.0f);
	}
	for (size_t i = 0; i < n; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(fs);
		float env = std::min(1.0f, t / 0.12f) * std::exp(-t / (seconds * 0.35f)) * 0.5f;
		for (int s = 0; s < swells; s++) {
			const float d = (t - centres[s]) / widths[s];
			env += heights[s] * std::exp(-d * d);
		}
		env *= std::min(1.0f, (seconds - t) / 0.5f);
		const float w = rng.White();
		out[i] += (lp2.Process(lp1.Process(w)) * 9.0f + lp3.Process(w) * 0.8f) * env;
	}
	return ToPcm(out, 0.15f);
}

std::vector<uint8_t> MakeWav(const std::vector<int16_t> &samples, int sampleRate)
{
	std::vector<uint8_t> wav;
	const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
	wav.reserve(44 + dataBytes);
	wav.insert(wav.end(), { 'R', 'I', 'F', 'F' });
	PutLe(wav, 36 + dataBytes, 4);
	wav.insert(wav.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	PutLe(wav, 16, 4);
	PutLe(wav, 1, 2); // PCM
	PutLe(wav, 1, 2); // mono
	PutLe(wav, static_cast<uint32_t>(sampleRate), 4);
	PutLe(wav, static_cast<uint32_t>(sampleRate) * 2, 4);
	PutLe(wav, 2, 2);
	PutLe(wav, 16, 2);
	wav.insert(wav.end(), { 'd', 'a', 't', 'a' });
	PutLe(wav, dataBytes, 4);
	const size_t offset = wav.size();
	wav.resize(offset + dataBytes);
	std::memcpy(wav.data() + offset, samples.data(), dataBytes);
	return wav;
}

} // namespace devilution
