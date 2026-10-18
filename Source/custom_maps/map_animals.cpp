#include "custom_maps/map_animals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

#include "custom_maps/custom_town.hpp"
#include "custom_maps/map_editor.hpp"
#include "diablo.h"
#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "engine/light_tables.hpp"
#include "engine/load_cel.hpp"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"

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
};

constexpr std::array<Kind, 8> Kinds { {
	{ "cow", "Cow", 1.0f, 0.55f, 5, 0.6f },
	{ "horse", "Horse", 2.0f, 0.9f, 6, 0.55f },
	{ "sheep", "Sheep", 1.8f, 0.6f, 5, 0.7f },
	{ "goat", "Goat", 1.75f, 0.8f, 6, 0.5f },
	{ "pig", "Pig", 1.7f, 0.7f, 5, 0.6f },
	{ "chicken", "Chicken", 1.35f, 1.3f, 4, 0.65f },
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
	{ 236, 232, 220 },
	{ 186, 182, 172 },
	{ 122, 119, 112 },
	{ 62, 60, 56 },
	{ 22, 20, 20 },
	{ 228, 160, 152 },
	{ 168, 100, 94 },
	{ 140, 92, 50 },
	{ 80, 52, 28 },
	{ 196, 158, 106 },
	{ 226, 196, 148 },
	{ 232, 200, 70 },
	{ 214, 120, 36 },
	{ 186, 36, 30 },
	{ 206, 122, 56 },
	{ 150, 80, 30 },
	{ 132, 72, 36 },
	{ 46, 30, 20 },
} };

std::array<uint8_t, ColourCount> ColourIndex {};
std::array<SDL_Color, 256> ColoursForPalette {};
bool ColoursReady = false;

void RefreshColours()
{
	bool same = ColoursReady;
	for (size_t i = 0; same && i < logical_palette.size(); i++)
		same = logical_palette[i].r == ColoursForPalette[i].r && logical_palette[i].g == ColoursForPalette[i].g && logical_palette[i].b == ColoursForPalette[i].b;
	if (same)
		return;
	ColoursForPalette = logical_palette;
	ColoursReady = true;
	for (int c = 1; c < ColourCount; c++) {
		int best = 1, bestDistance = INT32_MAX;
		for (int i = 1; i < 256; i++) {
			const SDL_Color &p = logical_palette[i];
			const int dr = p.r - ColourRgb[c][0], dg = p.g - ColourRgb[c][1], db = p.b - ColourRgb[c][2];
			const int distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
			if (distance < bestDistance) {
				bestDistance = distance;
				best = i;
			}
		}
		ColourIndex[c] = static_cast<uint8_t>(best);
	}
}

/** Shades of one coat: lit top, body, shaded belly, outline. */
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

struct Canvas {
	std::array<uint8_t, CanvasWidth * CanvasHeight> px {};
	float k = 1.0f; // scale from sketch to canvas

	float X(float x) const { return FeetX + (x - SketchFeetX) * k; }
	float Y(float y) const { return GroundY + (y - SketchGroundY) * k; }

	void Put(int x, int y, Colour c)
	{
		if (x >= 0 && y >= 0 && x < CanvasWidth && y < CanvasHeight && c != None)
			px[static_cast<size_t>(y) * CanvasWidth + x] = c;
	}

	/** A sketch pixel: a small block once scaled. */
	void Set(int x, int y, Colour c)
	{
		const int x0 = static_cast<int>(X(static_cast<float>(x))), y0 = static_cast<int>(Y(static_cast<float>(y)));
		const int x1 = static_cast<int>(X(static_cast<float>(x + 1))), y1 = static_cast<int>(Y(static_cast<float>(y + 1)));
		for (int py = y0; py < std::max(y0 + 1, y1); py++) {
			for (int px_ = x0; px_ < std::max(x0 + 1, x1); px_++)
				Put(px_, py, c);
		}
	}

	/** A shaded oval, lit from above, with an outline. `bumpy` roughens the edge (wool). */
	void Oval(float cx, float cy, float rx, float ry, const Coat &coat, bool bumpy = false)
	{
		cx = X(cx), cy = Y(cy), rx *= k, ry *= k;
		for (int y = static_cast<int>(cy - ry - 2); y <= static_cast<int>(cy + ry + 2); y++) {
			for (int x = static_cast<int>(cx - rx - 2); x <= static_cast<int>(cx + rx + 2); x++) {
				const float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
				float edge = 1.0f;
				if (bumpy)
					edge += 0.12f * std::sin(static_cast<float>(x) * 1.9f / k) * std::cos(static_cast<float>(y) * 2.3f / k);
				const float d = nx * nx + ny * ny;
				if (d > edge)
					continue;
				Colour c = coat.base;
				if (d > edge * 0.78f)
					c = coat.outline;
				else if (ny < -0.35f)
					c = coat.light;
				else if (ny > 0.45f)
					c = coat.dark;
				if (bumpy && c == coat.base && ((x * 7 + y * 13) % 9) == 0)
					c = coat.dark;
				Put(x, y, c);
			}
		}
	}

	/** A line in sketch units, as thick as one scaled sketch pixel. */
	void Line(int sx0, int sy0, int sx1, int sy1, Colour c)
	{
		const int thick = std::max(1, static_cast<int>(k + 0.5f));
		for (int t = 0; t < thick; t++)
			RawLine(static_cast<int>(X(static_cast<float>(sx0))) + t, static_cast<int>(Y(static_cast<float>(sy0))), static_cast<int>(X(static_cast<float>(sx1))) + t, static_cast<int>(Y(static_cast<float>(sy1))), c);
	}

	void RawLine(int x0, int y0, int x1, int y1, Colour c)
	{
		const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
		const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
		int err = dx + dy;
		for (int guard = 0; guard < 256; guard++) {
			Put(x0, y0, c);
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

	/** A leg from the body down to the ground; far legs are drawn darker. */
	void Leg(int x, int top, int swing, Colour near, Colour far, bool isFar, int width = 2)
	{
		const Colour c = isFar ? far : near;
		for (int w = 0; w < width; w++)
			Line(x + w, top, x + w + swing, SketchGroundY, c);
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
	c.Leg(20, 30, -s, Chestnut, DarkBrown, true);
	c.Leg(37, 30, s, Chestnut, DarkBrown, true);
	c.Line(15, 24, 12, 35, Mane); // tail
	c.Line(16, 24, 14, 36, Mane);
	c.Oval(28, 25, 14.0f, 7.0f, HorseHide);
	c.Leg(23, 31, s, Chestnut, DarkBrown, false);
	c.Leg(40, 31, -s, Chestnut, DarkBrown, false);
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

/** Puts a drawn animal on screen, feet at `feet`, through a light table. */
void BlitAnimal(const Surface &out, Point feet, const Canvas &canvas, bool facingRight, const uint8_t *tbl)
{
	RefreshColours();
	for (int y = 0; y < CanvasHeight; y++) {
		for (int x = 0; x < CanvasWidth; x++) {
			const uint8_t c = canvas.px[static_cast<size_t>(y) * CanvasWidth + x];
			if (c == None)
				continue;
			const int sx = facingRight ? x - FeetX : FeetX - x;
			const Point p = feet + Displacement { sx, y - GroundY };
			if (out.InBounds(p))
				*out.at(p.x, p.y) = tbl[ColourIndex[c]];
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
	switch (a.state) {
	case State::Stand:
	case State::Graze:
		a.timer -= dt;
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
	BlitAnimal(out, feet, AnimalCanvas(a.kind, CurrentPose(a)), a.facingRight, tbl);
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
		a.facing = a.facingRight ? Direction::SouthEast : Direction::SouthWest;
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
