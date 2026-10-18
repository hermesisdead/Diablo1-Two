#include "custom_maps/map_animals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "custom_maps/custom_town.hpp"
#include "custom_maps/map_editor.hpp"
#include "diablo.h"
#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "engine/light_tables.hpp"
#include "engine/load_cel.hpp"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

// --- Kinds ------------------------------------------------------------------------------------

struct Kind {
	std::string_view key;
	std::string_view label;
	float scale;       // drawn size (next to people)
	float speed;       // tiles per second
	int range;         // how far from home it strolls
	float grazeChance; // when it stops: graze/peck/sniff rather than stand
	float spriteScale = 1.0f; // size of its sprite sheet frames in the game
};

constexpr std::array<Kind, 8> Kinds { {
	{ "cow", "Cow", 1.0f, 0.0f, 0, 0.0f }, // the game's cow art has no walk: cows stay put, chew and turn
	{ "horse", "Horse", 2.0f, 0.9f, 6, 0.55f },
	{ "sheep", "Sheep", 1.8f, 0.6f, 5, 0.7f },
	{ "goat", "Goat", 1.75f, 0.8f, 6, 0.5f },
	{ "pig", "Pig", 1.7f, 0.7f, 5, 0.6f },
	{ "chicken", "Chicken", 1.35f, 1.3f, 4, 0.65f, 0.65f },
	{ "dog", "Dog", 1.65f, 1.7f, 8, 0.3f },
	{ "cat", "Cat", 1.45f, 1.1f, 6, 0.4f },
} };

const Kind *FindKind(std::string_view key)
{
	for (const Kind &k : Kinds) {
		if (k.key == key)
			return &k;
	}
	return nullptr;
}

// --- Colours ----------------------------------------------------------------------------------
// Muted, earthy colours like the game's pre-rendered art; shading and grain do the rest.

enum Colour : uint8_t {
	None,
	White,
	LightGrey,
	Grey,
	DarkGrey,
	Black,
	Pink,
	DarkPink,
	Brown,
	DarkBrown,
	Tan,
	LightTan,
	Yellow,
	Orange,
	Red,
	Ginger,
	DarkGinger,
	Chestnut,
	Mane,
	ColourCount,
};

constexpr std::array<std::array<uint8_t, 3>, ColourCount> ColourRgb { {
	{ 0, 0, 0 },
	{ 168, 160, 142 }, // dirty wool, a hen's feathers
	{ 128, 121, 108 },
	{ 88, 83, 75 },
	{ 52, 48, 43 },
	{ 20, 18, 16 },
	{ 146, 106, 92 },  // dusky pig skin
	{ 102, 70, 60 },
	{ 94, 68, 45 },
	{ 56, 39, 26 },
	{ 124, 98, 70 },
	{ 146, 120, 90 },
	{ 148, 126, 62 },
	{ 136, 94, 44 },
	{ 112, 30, 24 },
	{ 116, 86, 60 },   // grey-brown tabby
	{ 66, 48, 34 },
	{ 82, 52, 32 },    // dark bay
	{ 28, 21, 16 },
} };

struct Rgb {
	float r = 0, g = 0, b = 0;
};

Rgb ToRgb(Colour c, float scale = 1.0f)
{
	return { ColourRgb[c][0] * scale, ColourRgb[c][1] * scale, ColourRgb[c][2] * scale };
}

// Nearest palette colour for any RGB, worked out as needed (5 bits a channel) and kept until the palette changes.
std::array<int16_t, 32 * 32 * 32> Nearest;
std::array<SDL_Color, 256> NearestFor {};
bool NearestReady = false;

void RefreshNearest()
{
	bool same = NearestReady;
	for (size_t i = 0; same && i < logical_palette.size(); i++)
		same = logical_palette[i].r == NearestFor[i].r && logical_palette[i].g == NearestFor[i].g && logical_palette[i].b == NearestFor[i].b;
	if (same)
		return;
	NearestFor = logical_palette;
	NearestReady = true;
	Nearest.fill(-1);
}

uint8_t NearestIndex(const Rgb &c)
{
	const int r = std::clamp(static_cast<int>(c.r), 0, 255), g = std::clamp(static_cast<int>(c.g), 0, 255), b = std::clamp(static_cast<int>(c.b), 0, 255);
	int16_t &slot = Nearest[static_cast<size_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))];
	if (slot < 0) {
		const int qr = (r & ~7) + 4, qg = (g & ~7) + 4, qb = (b & ~7) + 4;
		int best = 1, bestDistance = INT32_MAX;
		for (int i = 1; i < 256; i++) {
			const SDL_Color &p = logical_palette[i];
			const int dr = p.r - qr, dg = p.g - qg, db = p.b - qb;
			const int distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
			if (distance < bestDistance) {
				bestDistance = distance;
				best = i;
			}
		}
		slot = static_cast<int16_t>(best);
	}
	return static_cast<uint8_t>(slot);
}

/** A coat: its colour (the other fields are kept for the drawings' readability; shading is lit, not banded). */
struct Coat {
	Colour light, base, dark, outline;
};

constexpr Coat Wool { White, White, LightGrey, Grey };
constexpr Coat Feathers { White, White, LightGrey, Grey };
constexpr Coat PigSkin { Pink, Pink, DarkPink, DarkPink };
constexpr Coat GoatHair { White, LightGrey, Grey, DarkGrey };
constexpr Coat DogFur { Tan, Brown, DarkBrown, DarkBrown };
constexpr Coat CatFur { LightTan, Ginger, DarkGinger, DarkBrown };
constexpr Coat HorseHide { Brown, Chestnut, DarkBrown, Mane };
constexpr Coat SheepFace { Grey, DarkGrey, Black, Black };

// --- Drawn animals ----------------------------------------------------------------------------

// Animals are drawn in design units (feet at 32,44 of a 64x48 sketch), scaled up per kind onto the canvas.
constexpr int SketchFeetX = 32;
constexpr int SketchGroundY = 44;
constexpr int CanvasWidth = 128;
constexpr int CanvasHeight = 100;
constexpr int GroundY = 96; // the canvas row the feet stand on
constexpr int FeetX = 64;

enum Pose : uint8_t {
	Stand,
	Walk0,
	Walk1,
	Walk2,
	Walk3,
	Graze,
	PoseCount,
};

/** Grain in [-1, 1] for a pixel, like the noisy textures of the game's renders. */
float Grain(int x, int y)
{
	uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
}

struct Canvas {
	std::array<Rgb, CanvasWidth * CanvasHeight> px {};
	std::array<bool, CanvasWidth * CanvasHeight> on {};
	float k = 1.0f; // scale from sketch to canvas
	int left = CanvasWidth, right = -1; // drawn extent, for the shadow

	float X(float x) const { return FeetX + (x - SketchFeetX) * k; }
	float Y(float y) const { return GroundY + (y - SketchGroundY) * k; }

	void Put(int x, int y, const Rgb &c)
	{
		if (x < 0 || y < 0 || x >= CanvasWidth || y >= CanvasHeight)
			return;
		const size_t i = static_cast<size_t>(y) * CanvasWidth + x;
		px[i] = c;
		on[i] = true;
		left = std::min(left, x);
		right = std::max(right, x);
	}

	/** A small detail (eye, nostril, comb): a block of flat colour once scaled. */
	void Set(int x, int y, Colour c)
	{
		if (c == None)
			return;
		const int x0 = static_cast<int>(X(static_cast<float>(x))), y0 = static_cast<int>(Y(static_cast<float>(y)));
		const int x1 = static_cast<int>(X(static_cast<float>(x + 1))), y1 = static_cast<int>(Y(static_cast<float>(y + 1)));
		for (int py = y0; py < std::max(y0 + 1, y1); py++) {
			for (int px_ = x0; px_ < std::max(x0 + 1, x1); px_++)
				Put(px_, py, ToRgb(c, 0.9f + 0.08f * Grain(px_, py)));
		}
	}

	/**
	 * A body part lit like a rounded solid: light from the upper left, a dark underside, no outline.
	 * `bumpy` roughens the edge and coarsens the grain (wool).
	 */
	void Oval(float cx, float cy, float rx, float ry, const Coat &coat, bool bumpy = false)
	{
		cx = X(cx), cy = Y(cy), rx *= k, ry *= k;
		const float grain = bumpy ? 0.22f : 0.09f;
		for (int y = static_cast<int>(cy - ry - 2); y <= static_cast<int>(cy + ry + 2); y++) {
			for (int x = static_cast<int>(cx - rx - 2); x <= static_cast<int>(cx + rx + 2); x++) {
				const float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
				float edge = 1.0f;
				if (bumpy)
					edge += 0.12f * std::sin(static_cast<float>(x) * 1.9f / k) * std::cos(static_cast<float>(y) * 2.3f / k);
				const float d = nx * nx + ny * ny;
				if (d > edge)
					continue;
				const float nz = std::sqrt(std::max(0.0f, 1.0f - d / edge));
				const float lambert = std::max(0.0f, -0.48f * nx - 0.62f * ny + 0.62f * nz);
				float light = 0.16f + 0.95f * lambert;
				if (ny > 0.15f)
					light *= 1.0f - 0.5f * std::min(1.0f, (ny - 0.15f) / 0.85f); // the belly in its own shadow
				light *= 1.0f + grain * Grain(x, y);
				Put(x, y, ToRgb(coat.base, light));
			}
		}
	}

	/** A line in sketch units (tails, horns, manes), as thick as one scaled sketch pixel. */
	void Line(int sx0, int sy0, int sx1, int sy1, Colour c)
	{
		const int thick = std::max(1, static_cast<int>(k + 0.5f));
		for (int t = 0; t < thick; t++)
			RawLine(static_cast<int>(X(static_cast<float>(sx0))) + t, static_cast<int>(Y(static_cast<float>(sy0))), static_cast<int>(X(static_cast<float>(sx1))) + t, static_cast<int>(Y(static_cast<float>(sy1))), c,
			    thick > 1 ? 1.0f - 0.45f * static_cast<float>(t) / static_cast<float>(thick - 1) : 0.8f);
	}

	void RawLine(int x0, int y0, int x1, int y1, Colour c, float shade)
	{
		const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
		const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (int guard = 0; guard < 256; guard++) {
			Put(x0, y0, ToRgb(c, shade * (1.0f + 0.1f * Grain(x0, y0))));
			if (x0 == x1 && y0 == y1)
				break;
			const int e2 = 2 * err;
			if (e2 >= dy) {
				err += dy;
				x0 += sx;
			}
			if (e2 <= dx) {
				err += dx;
				y0 += sy;
			}
		}
	}

	/** A leg from the body down to the ground, rounded by shading across it; far legs sit in shadow. */
	void Leg(int x, int top, int swing, Colour near, Colour far, bool isFar, int width = 2)
	{
		const Colour c = isFar ? far : near;
		const float x0 = X(static_cast<float>(x)), y0 = Y(static_cast<float>(top));
		const float x1 = X(static_cast<float>(x + swing)), y1 = Y(static_cast<float>(SketchGroundY));
		const int w = std::max(1, static_cast<int>(width * k + 0.5f));
		const int rows = std::max(1, static_cast<int>(y1 - y0));
		for (int r = 0; r <= rows; r++) {
			const float t = static_cast<float>(r) / static_cast<float>(rows);
			const int lx = static_cast<int>(x0 + (x1 - x0) * t), ly = static_cast<int>(y0 + (y1 - y0) * t);
			for (int i = 0; i < w; i++) {
				float light = (isFar ? 0.5f : 0.95f) - 0.4f * static_cast<float>(i) / static_cast<float>(std::max(1, w - 1));
				light *= 1.0f - 0.25f * t; // darker towards the ground
				Put(lx + i, ly, ToRgb(c, light * (1.0f + 0.08f * Grain(lx + i, ly))));
			}
		}
	}
};

/** Leg swing of a walk frame: the diagonal pairs move together. */
int Swing(Pose pose)
{
	switch (pose) {
	case Walk0: return 2;
	case Walk2: return -2;
	default: return 0;
	}
}

void DrawSheep(Canvas &c, Pose pose)
{
	const int s = Swing(pose);
	c.Leg(25, 39, -s, DarkGrey, Black, true);
	c.Leg(36, 39, s, DarkGrey, Black, true);
	c.Oval(30, 34.5f, 11, 7, Wool, true);
	c.Leg(28, 40, s, DarkGrey, Black, false);
	c.Leg(38, 40, -s, DarkGrey, Black, false);
	const bool graze = pose == Graze;
	const float hx = graze ? 43.5f : 42.5f, hy = graze ? 40.0f : 31.0f;
	c.Oval(hx, hy, 3.5f, 2.8f, SheepFace);
	c.Set(static_cast<int>(hx) - 2, static_cast<int>(hy) - 3, Black); // ear
	c.Set(static_cast<int>(hx) + 1, static_cast<int>(hy) - 1, LightGrey); // eye glint
	c.Set(18, 33, White); // tail
	c.Set(17, 34, LightGrey);
}

void DrawPig(Canvas &c, Pose pose)
{
	const int s = Swing(pose);
	c.Leg(24, 38, -s, DarkPink, DarkPink, true);
	c.Leg(36, 38, s, DarkPink, DarkPink, true);
	c.Oval(30, 34, 11.5f, 6.5f, PigSkin);
	c.Leg(27, 39, s, Pink, DarkPink, false);
	c.Leg(38, 39, -s, Pink, DarkPink, false);
	const bool graze = pose == Graze;
	const float hx = graze ? 43.0f : 42.0f, hy = graze ? 38.0f : 32.0f;
	c.Oval(hx, hy, 4.5f, 4.0f, PigSkin);
	c.Oval(hx + 4.0f, hy + 0.8f, 1.6f, 2.0f, { Pink, DarkPink, DarkPink, DarkPink });
	c.Set(static_cast<int>(hx + 4), static_cast<int>(hy + 1), Black); // nostril
	c.Set(static_cast<int>(hx) + 1, static_cast<int>(hy) - 1, Black); // eye
	c.Line(static_cast<int>(hx) - 2, static_cast<int>(hy) - 4, static_cast<int>(hx), static_cast<int>(hy) - 6, DarkPink); // ear
	c.Line(static_cast<int>(hx) - 1, static_cast<int>(hy) - 4, static_cast<int>(hx) + 1, static_cast<int>(hy) - 5, DarkPink);
	c.Set(18, 32, DarkPink); // curly tail
	c.Set(17, 31, DarkPink);
	c.Set(18, 30, DarkPink);
}

void DrawGoat(Canvas &c, Pose pose)
{
	const int s = Swing(pose);
	c.Leg(25, 37, -s, Grey, DarkGrey, true, 1);
	c.Leg(35, 37, s, Grey, DarkGrey, true, 1);
	c.Oval(30, 33.5f, 9.5f, 5.5f, GoatHair);
	c.Leg(27, 38, s, LightGrey, DarkGrey, false);
	c.Leg(37, 38, -s, LightGrey, DarkGrey, false);
	const bool graze = pose == Graze;
	if (graze) {
		c.Oval(39.5f, 35.5f, 2.5f, 3.5f, GoatHair);
		c.Oval(42.5f, 39.5f, 3.5f, 2.5f, GoatHair);
		c.Line(41, 37, 38, 35, DarkGrey); // horns
		c.Set(44, 39, Black);
		c.Set(45, 42, LightGrey); // beard
	} else {
		c.Oval(38.5f, 29.0f, 2.5f, 4.5f, GoatHair);
		c.Oval(41.5f, 25.0f, 3.6f, 2.6f, GoatHair);
		c.Line(40, 23, 37, 20, DarkGrey); // horns
		c.Line(41, 23, 38, 20, Grey);
		c.Set(42, 24, Black); // eye
		c.Set(43, 28, LightGrey); // beard
		c.Set(43, 29, LightGrey);
		c.Set(39, 24, Grey); // ear
	}
	c.Line(21, 31, 19, 28, LightGrey); // tail up
}

void DrawDog(Canvas &c, Pose pose)
{
	const int s = Swing(pose) * 2;
	c.Leg(25, 35, -s, Brown, DarkBrown, true);
	c.Leg(35, 35, s, Brown, DarkBrown, true);
	c.Oval(30, 33, 9.5f, 4.5f, DogFur);
	c.Leg(27, 36, s, Brown, DarkBrown, false);
	c.Leg(37, 36, -s, Brown, DarkBrown, false);
	const bool sniff = pose == Graze;
	const float hx = sniff ? 42.5f : 41.0f, hy = sniff ? 36.5f : 27.5f;
	c.Oval(hx, hy, 4.0f, 3.5f, DogFur);
	c.Oval(hx + 4.0f, hy + 1.0f, 2.2f, 1.5f, DogFur); // muzzle
	c.Set(static_cast<int>(hx + 6), static_cast<int>(hy + 1), Black); // nose
	c.Set(static_cast<int>(hx) + 1, static_cast<int>(hy) - 1, Black); // eye
	c.Line(static_cast<int>(hx) - 2, static_cast<int>(hy) - 2, static_cast<int>(hx) - 3, static_cast<int>(hy) + 2, DarkBrown); // ear
	c.Line(21, 31, 18, 25 - (pose == Walk1 || pose == Walk3 ? 1 : 0), Brown); // tail up, wagging
	c.Line(22, 31, 19, 25, Tan);
}

void DrawCat(Canvas &c, Pose pose)
{
	if (pose == Graze) { // sitting
		c.Oval(31, 36, 5.0f, 5.5f, CatFur);
		c.Oval(33.5f, 28.5f, 3.4f, 3.0f, CatFur);
		c.Set(31, 25, DarkGinger); // ears
		c.Set(32, 25, DarkGinger);
		c.Set(35, 25, DarkGinger);
		c.Set(36, 25, DarkGinger);
		c.Set(34, 28, Yellow); // eye
		c.Line(26, 41, 22, 39, Ginger); // tail round the feet
		c.Line(28, 41, 31, 42, DarkGinger);
		return;
	}
	const int s = Swing(pose);
	c.Leg(27, 37, -s, Ginger, DarkGinger, true, 1);
	c.Leg(35, 37, s, Ginger, DarkGinger, true, 1);
	c.Oval(31, 35, 7.5f, 3.5f, CatFur);
	c.Leg(29, 38, s, Ginger, DarkGinger, false, 1);
	c.Leg(37, 38, -s, Ginger, DarkGinger, false, 1);
	c.Oval(39.5f, 31.0f, 3.2f, 2.9f, CatFur);
	c.Set(38, 28, DarkGinger); // ears
	c.Set(37, 27, DarkGinger);
	c.Set(41, 28, DarkGinger);
	c.Set(41, 27, DarkGinger);
	c.Set(41, 30, Yellow); // eye
	c.Line(24, 34, 21, 28, Ginger); // tail up
	c.Set(22, 27, DarkGinger);
}

void DrawChicken(Canvas &c, Pose pose)
{
	const bool peck = pose == Graze;
	const int step = (pose == Walk0 || pose == Walk2) ? 1 : 0;
	c.Line(30, 40, 30 - step, SketchGroundY, Orange); // legs
	c.Line(33, 40, 33 + step, SketchGroundY, Orange);
	c.Oval(26.0f, 34.5f, 2.0f, 3.0f, Feathers); // tail
	c.Oval(31.0f, 37.0f, 5.5f, 4.0f, Feathers);
	const float hx = peck ? 37.5f : 35.0f, hy = peck ? 38.5f : 31.5f;
	c.Oval(hx, hy, 2.4f, 2.4f, Feathers);
	c.Set(static_cast<int>(hx) - 1, static_cast<int>(hy) - 3, Red); // comb
	c.Set(static_cast<int>(hx), static_cast<int>(hy) - 3, Red);
	c.Set(static_cast<int>(hx), static_cast<int>(hy) - 4, Red);
	c.Set(static_cast<int>(hx) + 2, static_cast<int>(hy) + 2, Red); // wattle
	c.Set(static_cast<int>(hx) + 3, static_cast<int>(hy), Yellow); // beak
	c.Set(static_cast<int>(hx) + 4, static_cast<int>(hy), Orange);
	c.Set(static_cast<int>(hx) + 1, static_cast<int>(hy) - 1, Black); // eye
}

void DrawHorse(Canvas &c, Pose pose)
{
	const int s = Swing(pose) * 2;
	c.Leg(19, 30, -s, Chestnut, DarkBrown, true, 3);
	c.Leg(36, 30, s, Chestnut, DarkBrown, true, 3);
	c.Line(15, 24, 12, 35, Mane); // tail
	c.Line(16, 24, 14, 36, Mane);
	c.Oval(28, 25, 14.0f, 7.0f, HorseHide);
	c.Leg(22, 31, s, Chestnut, DarkBrown, false, 3);
	c.Leg(39, 31, -s, Chestnut, DarkBrown, false, 3);
	const bool graze = pose == Graze;
	if (graze) {
		c.Oval(43.0f, 29.0f, 3.5f, 6.5f, HorseHide);
		c.Oval(46.0f, 37.0f, 3.0f, 5.0f, HorseHide);
		c.Line(41, 23, 44, 32, Mane);
		c.Set(47, 34, Black);
	} else {
		c.Oval(41.5f, 17.5f, 3.8f, 7.5f, HorseHide);
		c.Oval(47.0f, 11.0f, 5.5f, 2.8f, HorseHide);
		for (int y = 9; y <= 21; y++)
			c.Set(39 - (y - 9) / 4, y, Mane); // mane
		c.Set(48, 9, Black); // eye
		c.Set(44, 7, DarkBrown); // ears
		c.Set(45, 6, DarkBrown);
		c.Set(52, 12, DarkBrown); // nostril
	}
}

const Canvas &AnimalCanvas(int kind, Pose pose)
{
	static std::array<std::array<std::optional<Canvas>, PoseCount>, Kinds.size()> cache;
	std::optional<Canvas> &slot = cache[kind][pose];
	if (!slot) {
		slot.emplace();
		slot->k = Kinds[kind].scale;
		const std::string_view key = Kinds[kind].key;
		if (key == "sheep")
			DrawSheep(*slot, pose);
		else if (key == "pig")
			DrawPig(*slot, pose);
		else if (key == "goat")
			DrawGoat(*slot, pose);
		else if (key == "dog")
			DrawDog(*slot, pose);
		else if (key == "cat")
			DrawCat(*slot, pose);
		else if (key == "chicken")
			DrawChicken(*slot, pose);
		else if (key == "horse")
			DrawHorse(*slot, pose);
	}
	return *slot;
}

/** Puts a drawn animal on screen, feet at `feet`, lit by a tile's light, with a soft shadow on the ground. */
void BlitAnimal(const Surface &out, Point feet, const Canvas &canvas, bool facingRight, int lightLevel)
{
	RefreshNearest();
	lightLevel = std::clamp(lightLevel, 0, static_cast<int>(LightTables.size()) - 1);
	// Shadow: the ground under the body darkened, falling a little away from the light.
	if (canvas.right >= canvas.left) {
		const float rx = static_cast<float>(canvas.right - canvas.left) * 0.42f, ry = rx * 0.3f;
		const int centreX = facingRight ? (canvas.left + canvas.right) / 2 - FeetX : FeetX - (canvas.left + canvas.right) / 2;
		const Point c = feet + Displacement { centreX + 3, 1 };
		for (int y = static_cast<int>(-ry); y <= static_cast<int>(ry); y++) {
			for (int x = static_cast<int>(-rx); x <= static_cast<int>(rx); x++) {
				const float d = (x * x) / (rx * rx) + (y * y) / (ry * ry);
				const Point p = c + Displacement { x, y };
				if (d <= 1.0f && out.InBounds(p) && (d < 0.6f || Grain(p.x, p.y) > (d - 0.6f) * 5.0f - 1.0f))
				{
					// Darken the ground's own colour (town light tables are not darkness ramps).
					const SDL_Color &g = logical_palette[*out.at(p.x, p.y)];
					*out.at(p.x, p.y) = NearestIndex({ g.r * 0.45f, g.g * 0.45f, g.b * 0.45f });
				}
			}
		}
	}
	const uint8_t *tbl = LightTables[lightLevel].data();
	for (int y = 0; y < CanvasHeight; y++) {
		for (int x = 0; x < CanvasWidth; x++) {
			const size_t i = static_cast<size_t>(y) * CanvasWidth + x;
			if (!canvas.on[i])
				continue;
			const int sx = facingRight ? x - FeetX : FeetX - x;
			const Point p = feet + Displacement { sx, y - GroundY };
			if (out.InBounds(p))
				*out.at(p.x, p.y) = tbl[NearestIndex(canvas.px[i])];
		}
	}
}

// --- Sprite sheets ----------------------------------------------------------------------------
// animals/<kind>.anm: "ANM1", directions (8, in Direction order), frames (4 idle then 4 walk),
// then per frame: offset x, offset y (int16, top-left from the feet), width, height (uint16) and
// width * height RGB pixels, 0,0,0 being see-through. Matched to the palette as drawn.

struct SpriteFrame {
	int ox = 0, oy = 0, width = 0, height = 0;
	std::vector<uint8_t> rgb;
};

struct SpriteSet {
	int directions = 0;
	int frames = 0;
	std::vector<SpriteFrame> all; // [direction * frames + frame]
	const SpriteFrame &Get(int direction, int frame) const { return all[static_cast<size_t>(direction) * frames + frame]; }
};

const SpriteSet *AnimalSprites(int kind)
{
	static std::array<std::optional<SpriteSet>, Kinds.size()> sets;
	static std::array<bool, Kinds.size()> tried {};
	if (tried[kind])
		return sets[kind] ? &*sets[kind] : nullptr;
	tried[kind] = true;
	size_t size = 0;
	const std::string path = StrCat("animals\\", Kinds[kind].key, ".anm");
	std::expected<std::unique_ptr<std::byte[]>, std::string> file = LoadFileInMemWithStatus<std::byte>(path.c_str(), &size);
	if (!file.has_value() || size < 6)
		return nullptr; // no sprites for this one: it is drawn instead
	const auto *d = reinterpret_cast<const uint8_t *>(file->get());
	if (std::memcmp(d, "ANM1", 4) != 0)
		return nullptr;
	SpriteSet set;
	set.directions = d[4];
	set.frames = d[5];
	size_t at = 6;
	auto i16 = [&](size_t p) { return static_cast<int16_t>(d[p] | (d[p + 1] << 8)); };
	for (int i = 0; i < set.directions * set.frames; i++) {
		if (at + 8 > size)
			return nullptr;
		SpriteFrame f;
		f.ox = i16(at);
		f.oy = i16(at + 2);
		f.width = static_cast<uint16_t>(i16(at + 4));
		f.height = static_cast<uint16_t>(i16(at + 6));
		at += 8;
		const size_t bytes = static_cast<size_t>(f.width) * f.height * 3;
		if (at + bytes > size)
			return nullptr;
		f.rgb.assign(d + at, d + at + bytes);
		at += bytes;
		set.all.push_back(std::move(f));
	}
	if (set.directions != 8 || set.frames < 1)
		return nullptr;
	sets[kind] = std::move(set);
	return &*sets[kind];
}

/** Draws a sprite frame, feet at `feet`, scaled, lit by a tile's light, over a soft shadow. */
void BlitSprite(const Surface &out, Point feet, const SpriteFrame &f, float scale, int lightLevel)
{
	RefreshNearest();
	lightLevel = std::clamp(lightLevel, 0, static_cast<int>(LightTables.size()) - 1);
	const int width = std::max(1, static_cast<int>(static_cast<float>(f.width) * scale + 0.5f));
	const int height = std::max(1, static_cast<int>(static_cast<float>(f.height) * scale + 0.5f));
	const int left = static_cast<int>(static_cast<float>(f.ox) * scale), top = static_cast<int>(static_cast<float>(f.oy) * scale);
	const float rx = static_cast<float>(width) * 0.42f, ry = rx * 0.32f;
	const Point c = feet + Displacement { left + width / 2 + 2, 0 };
	for (int y = static_cast<int>(-ry); y <= static_cast<int>(ry); y++) {
		for (int x = static_cast<int>(-rx); x <= static_cast<int>(rx); x++) {
			const float dd = (x * x) / (rx * rx) + (y * y) / (ry * ry);
			const Point p = c + Displacement { x, y };
			if (dd <= 1.0f && out.InBounds(p) && (dd < 0.6f || Grain(p.x, p.y) > (dd - 0.6f) * 5.0f - 1.0f)) {
				const SDL_Color &g = logical_palette[*out.at(p.x, p.y)];
				*out.at(p.x, p.y) = NearestIndex({ g.r * 0.45f, g.g * 0.45f, g.b * 0.45f });
			}
		}
	}
	const uint8_t *tbl = LightTables[lightLevel].data();
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			// Average the source pixels this one covers; mostly see-through stays see-through.
			const int sx0 = static_cast<int>(static_cast<float>(x) / scale), sy0 = static_cast<int>(static_cast<float>(y) / scale);
			const int sx1 = std::min(f.width, std::max(sx0 + 1, static_cast<int>(static_cast<float>(x + 1) / scale)));
			const int sy1 = std::min(f.height, std::max(sy0 + 1, static_cast<int>(static_cast<float>(y + 1) / scale)));
			int r = 0, g = 0, b = 0, solid = 0, total = 0;
			for (int sy = sy0; sy < sy1; sy++) {
				for (int sx = sx0; sx < sx1; sx++) {
					const uint8_t *px = &f.rgb[(static_cast<size_t>(sy) * f.width + sx) * 3];
					total++;
					if (px[0] == 0 && px[1] == 0 && px[2] == 0)
						continue;
					r += px[0], g += px[1], b += px[2];
					solid++;
				}
			}
			if (solid == 0 || solid * 2 < total)
				continue;
			const Point p = feet + Displacement { left + x, top + y };
			if (out.InBounds(p))
				*out.at(p.x, p.y) = tbl[NearestIndex({ static_cast<float>(r) / solid, static_cast<float>(g) / solid, static_cast<float>(b) / solid })];
		}
	}
}

OptionalOwnedClxSpriteSheet CowSheet;

std::optional<ClxSpriteList> CowSprites(Direction dir)
{
	if (!CowSheet)
		CowSheet.emplace(LoadCelSheet("towners\\animals\\cow", 128));
	if (!CowSheet)
		return std::nullopt;
	return (*CowSheet)[static_cast<size_t>(dir)];
}

// --- Wandering --------------------------------------------------------------------------------

enum class State : uint8_t {
	Stand,
	Walk,
	Graze,
};

struct Animal {
	int kind;
	Point home;
	Point tile;
	Point next;
	float progress = 0;
	Point goal;
	State state = State::Stand;
	float timer = 0;
	float animTime = 0;
	bool facingRight = true;
	Direction facing = Direction::SouthWest;
	uint32_t rng;

	float Random01()
	{
		rng = rng * 1664525u + 1013904223u;
		return static_cast<float>((rng >> 8) & 0xFFFF) / 65535.0f;
	}
	int RandomBetween(int lo, int hi) { return lo + static_cast<int>(Random01() * static_cast<float>(hi - lo + 1)) % (hi - lo + 1); }
};

std::vector<Animal> Animals;
// The level the animals belong to (they are only shown there).
struct LevelId {
	int levelType = -1;
	int level = -1;
	bool set = false;
	int setLevel = -1;
	bool operator==(const LevelId &) const = default;
};
LevelId AnimalsLevel;
uint32_t LastUpdate = 0;
// Who is where, rebuilt each frame: animal index + 1 per world tile.
std::array<std::array<int16_t, MAXDUNY>, MAXDUNX> Occupied {};

LevelId CurrentLevelId()
{
	return { static_cast<int>(leveltype), static_cast<int>(currlevel), setlevel, static_cast<int>(setlvlnum) };
}

bool AnimalsHere()
{
	if (Animals.empty() || !(AnimalsLevel == CurrentLevelId()))
		return false;
	// Only in hand-made maps: a custom town, or a hand-made set level.
	return (setlevel && setlvlnum == SL_CUSTOM) || (!setlevel && currlevel == 0 && InCustomTown());
}

/** Whether an animal may stand on a tile: open ground, no fence or gate, nobody there. */
bool OpenFor(const Animal &a, int self, Point t)
{
	if (!InDungeonBounds(t) || dPiece[t.x][t.y] == 0 || dFence[t.x][t.y] != 0)
		return false;
	if (!IsTileWalkable(t) || dMonster[t.x][t.y] != 0 || dPlayer[t.x][t.y] != 0)
		return false;
	if (Occupied[t.x][t.y] != 0 && Occupied[t.x][t.y] != self + 1)
		return false;
	const Kind &kind = Kinds[a.kind];
	return a.home.WalkingDistance(t) <= kind.range + 2;
}

/** Diagonal steps must not squeeze between two blocked tiles (fence posts touch at their corners). */
bool CanStep(const Animal &a, int self, Point from, Point to)
{
	if (!OpenFor(a, self, to))
		return false;
	const Displacement d = to - from;
	if (d.deltaX != 0 && d.deltaY != 0) {
		const Point side1 { from.x + d.deltaX, from.y }, side2 { from.x, from.y + d.deltaY };
		auto passable = [&](Point t) { return InDungeonBounds(t) && dFence[t.x][t.y] == 0 && IsTileWalkable(t); };
		if (!passable(side1) || !passable(side2))
			return false;
	}
	return true;
}

void StopFor(Animal &a, float seconds)
{
	const Kind &kind = Kinds[a.kind];
	a.state = a.Random01() < kind.grazeChance ? State::Graze : State::Stand;
	a.timer = seconds;
	a.next = a.tile;
	a.progress = 0;
}

void PickGoal(Animal &a)
{
	const Kind &kind = Kinds[a.kind];
	for (int attempt = 0; attempt < 10; attempt++) {
		const Point goal = a.home + Displacement { a.RandomBetween(-kind.range, kind.range), a.RandomBetween(-kind.range, kind.range) };
		if (goal != a.tile && InDungeonBounds(goal) && dFence[goal.x][goal.y] == 0 && IsTileWalkable(goal)) {
			a.goal = goal;
			a.state = State::Walk;
			return;
		}
	}
	StopFor(a, 2.0f + a.Random01() * 3.0f);
}

/** Next tile towards the goal, or the tile itself when there is no way on. */
Point NextStep(Animal &a, int self)
{
	std::array<Point, 8> options;
	int n = 0;
	for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
		const Point to = a.tile + dir;
		if (CanStep(a, self, a.tile, to))
			options[n++] = to;
	}
	if (n == 0)
		return a.tile;
	auto distance = [&](Point p) { const Displacement d = a.goal - p; return d.deltaX * d.deltaX + d.deltaY * d.deltaY; };
	std::sort(options.begin(), options.begin() + n, [&](Point x, Point y) { return distance(x) < distance(y); });
	if (distance(options[0]) < distance(a.tile))
		return options[0];
	return a.tile; // blocked from getting closer: give up on this goal
}

void UpdateAnimal(Animal &a, int self, float dt)
{
	a.animTime += dt;
	const Kind &kind = Kinds[a.kind];
	if (kind.speed <= 0) {
		// Stays where it was put; now and then turns to face somewhere new.
		a.state = State::Stand;
		a.timer -= dt;
		if (a.timer <= 0) {
			a.facing = static_cast<Direction>(a.RandomBetween(0, 7));
			a.timer = 6.0f + a.Random01() * 10.0f;
		}
		return;
	}
	switch (a.state) {
	case State::Stand:
	case State::Graze:
		a.timer -= dt;
		if (a.timer > 1.5f && a.Random01() < dt * 0.15f)
			a.facing = static_cast<Direction>((static_cast<int>(a.facing) + (a.Random01() < 0.5f ? 1 : 7)) % 8); // looks about
		if (a.timer <= 0) {
			if (a.Random01() < 0.65f)
				PickGoal(a);
			else
				StopFor(a, 2.0f + a.Random01() * 5.0f);
		}
		break;
	case State::Walk:
		if (a.next == a.tile) {
			if (a.tile == a.goal) {
				StopFor(a, 2.0f + a.Random01() * 6.0f);
				break;
			}
			const Point step = NextStep(a, self);
			if (step == a.tile) {
				StopFor(a, 1.0f + a.Random01() * 3.0f);
				break;
			}
			a.next = step;
			a.progress = 0;
			a.facing = GetDirection(a.tile, a.next);
			const Displacement d = a.next - a.tile;
			const int screenX = d.deltaX - d.deltaY; // world x runs down-right, y down-left
			if (screenX != 0)
				a.facingRight = screenX > 0;
			Occupied[a.next.x][a.next.y] = static_cast<int16_t>(self + 1);
		}
		{
			const Displacement d = a.next - a.tile;
			const float length = (d.deltaX != 0 && d.deltaY != 0) ? 1.0f : 0.7f; // a diagonal step is one screen tile across
			a.progress += dt * kind.speed / length;
		}
		if (a.progress >= 1.0f) {
			a.tile = a.next;
			a.progress = 0;
		}
		break;
	}
}

Pose CurrentPose(const Animal &a)
{
	if (a.state == State::Graze)
		return Graze;
	if (a.state == State::Walk && a.next != a.tile)
		return static_cast<Pose>(Walk0 + static_cast<int>(a.animTime * 8.0f * Kinds[a.kind].speed) % 4);
	return Stand;
}

void DrawOne(const Surface &out, const Animal &a, Point feet, int lightTableIndex)
{
	const uint8_t *tbl = LightTables[std::clamp(lightTableIndex, 0, static_cast<int>(LightTables.size()) - 1)].data();
	if (Kinds[a.kind].key == "cow") {
		if (const std::optional<ClxSpriteList> sprites = CowSprites(a.facing); sprites) {
			// The cow chews standing; walking it shows its legs-still frames moving along.
			const int frame = static_cast<int>(a.animTime * 6.6f) % static_cast<int>(sprites->numSprites());
			// Drawn like a towner (the sprite's tile is 32 left of and 16 below its feet). Town cows are
			// anchored for a 2x2 block of tiles, so their hooves stand about 20 above the anchor: shift down.
			const Point position { feet.x - 32 - CalculateSpriteTileCenterX(128), feet.y + 16 + 20 };
			ClxDrawTRN(out, position, (*sprites)[frame], tbl);
		}
		return;
	}
	if (const SpriteSet *sprites = AnimalSprites(a.kind); sprites != nullptr) {
		// Sprite sheets: idle frames first, then walk frames (half each when there are 8).
		const int idleFrames = std::max(1, sprites->frames / 2), walkFrames = std::max(1, sprites->frames - idleFrames);
		const bool walking = a.state == State::Walk && a.next != a.tile;
		const int frame = walking ? idleFrames + static_cast<int>(a.animTime * 9.0f * std::max(0.6f, Kinds[a.kind].speed)) % walkFrames
		                          : static_cast<int>(a.animTime * 3.0f) % idleFrames;
		BlitSprite(out, feet, sprites->Get(static_cast<int>(a.facing) % sprites->directions, frame), Kinds[a.kind].spriteScale, lightTableIndex);
		return;
	}
	BlitAnimal(out, feet, AnimalCanvas(a.kind, CurrentPose(a)), a.facingRight, lightTableIndex);
}

} // namespace

const std::vector<AnimalKindInfo> &AnimalKinds()
{
	static std::vector<AnimalKindInfo> kinds;
	if (kinds.empty()) {
		for (const Kind &k : Kinds)
			kinds.push_back({ k.key, k.label });
	}
	return kinds;
}

std::string_view AnimalLabel(std::string_view key)
{
	const Kind *kind = FindKind(key);
	return kind != nullptr ? kind->label : key;
}

void SetLevelAnimals(const std::vector<MapAnimal> &animals)
{
	Animals.clear();
	uint32_t seed = 0x9E3779B9u ^ SDL_GetTicks();
	for (const MapAnimal &placed : animals) {
		const Kind *kind = FindKind(placed.kind);
		if (kind == nullptr)
			continue;
		Animal a;
		a.kind = static_cast<int>(kind - Kinds.data());
		a.home = placed.tile;
		a.tile = placed.tile;
		a.next = placed.tile;
		a.goal = placed.tile;
		seed = seed * 747796405u + 2891336453u;
		a.rng = seed ^ static_cast<uint32_t>(placed.tile.x) * 73856093u ^ static_cast<uint32_t>(placed.tile.y) * 19349663u;
		a.facingRight = a.Random01() < 0.5f;
		a.facing = static_cast<Direction>(a.RandomBetween(0, 7));
		a.animTime = a.Random01() * 4.0f;
		StopFor(a, a.Random01() * 4.0f);
		Animals.push_back(a);
	}
	AnimalsLevel = CurrentLevelId();
	LastUpdate = SDL_GetTicks();
}

void UpdateAnimals()
{
	const uint32_t now = SDL_GetTicks();
	const float dt = std::min(0.1f, static_cast<float>(now - LastUpdate) / 1000.0f);
	LastUpdate = now;
	if (!AnimalsHere())
		return;
	for (auto &column : Occupied)
		column.fill(0);
	for (size_t i = 0; i < Animals.size(); i++) {
		const Animal &a = Animals[i];
		if (InDungeonBounds(a.tile))
			Occupied[a.tile.x][a.tile.y] = static_cast<int16_t>(i + 1);
		if (Kinds[a.kind].speed <= 0) {
			// A standing cow is big: others walk round it, not through it.
			for (const Displacement d : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
				const Point t = a.tile + d;
				if (InDungeonBounds(t) && Occupied[t.x][t.y] == 0)
					Occupied[t.x][t.y] = static_cast<int16_t>(i + 1);
			}
		}
		if (InDungeonBounds(a.next))
			Occupied[a.next.x][a.next.y] = static_cast<int16_t>(i + 1);
	}
	const bool editing = IsMapEditing();
	for (size_t i = 0; i < Animals.size(); i++) {
		Animal &a = Animals[i];
		if (editing) {
			// While editing, everyone stands where they were put.
			a.tile = a.next = a.goal = a.home;
			a.progress = 0;
			a.state = State::Stand;
			a.animTime += dt;
			continue;
		}
		if (PauseMode != 0)
			continue;
		UpdateAnimal(a, static_cast<int>(i), dt);
	}
}

void DrawAnimals(const Surface &out, Point tilePosition, Point targetBufferPosition, int lightTableIndex)
{
	if (!AnimalsHere())
		return;
	for (const Animal &a : Animals) {
		// Drawn with the tile it is mostly on, nudged by how far along its step it is.
		const Point drawTile = a.progress < 0.5f ? a.tile : a.next;
		if (drawTile != tilePosition)
			continue;
		const float fx = static_cast<float>(a.tile.x) + static_cast<float>(a.next.x - a.tile.x) * a.progress - static_cast<float>(drawTile.x);
		const float fy = static_cast<float>(a.tile.y) + static_cast<float>(a.next.y - a.tile.y) * a.progress - static_cast<float>(drawTile.y);
		const Point feet = targetBufferPosition + Displacement { 32 + static_cast<int>((fx - fy) * 32.0f), -16 + static_cast<int>((fx + fy) * 16.0f) };
		DrawOne(out, a, feet, lightTableIndex);
	}
}

void DrawAnimalPicture(const Surface &out, Point feet, std::string_view key)
{
	const Kind *kind = FindKind(key);
	if (kind == nullptr)
		return;
	Animal a;
	a.kind = static_cast<int>(kind - Kinds.data());
	a.rng = 1;
	a.facing = Direction::SouthWest;
	a.facingRight = false;
	DrawOne(out, a, feet, 0);
}

std::vector<Point> AnimalPositions()
{
	std::vector<Point> positions;
	for (const Animal &a : Animals)
		positions.push_back(a.progress < 0.5f ? a.tile : a.next);
	return positions;
}

} // namespace devilution
