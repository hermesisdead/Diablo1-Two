#include "engine/render/painterly.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#if !defined(USE_SDL1) && !defined(USE_SDL3)
#include <SDL_opengl.h>
#endif

#include "engine/render/color_overlay.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

namespace devilution {

namespace {

bool Running = false;
std::string Status = "Off";

#if !defined(USE_SDL1) && !defined(USE_SDL3)

// GLSL 1.10: runs on the OpenGL 2.1 context SDL's OpenGL renderer uses.
constexpr const char *VertexSource = R"(#version 110
varying vec2 uv;
void main()
{
	gl_Position = gl_Vertex;
	uv = gl_MultiTexCoord0.xy;
}
)";

// Kuwahara filter: the average of the calmest of the four quadrants around the pixel (4x4
// samples each, spread over `radius` game pixels), then grain that is strongest in mid-tones.
constexpr const char *FragmentSource = R"(#version 110
uniform sampler2D tex;
uniform sampler2D mask;
uniform float useMask;
uniform vec2 maskScale;
uniform vec2 texel;
uniform float radius;
uniform float grain;
uniform float seed;
varying vec2 uv;

float hash(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

void main()
{
	vec3 original = texture2D(tex, uv).rgb;
	vec3 color = original;
	if (radius > 0.05) {
		vec3 m0 = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0), m3 = vec3(0.0);
		vec3 s0 = vec3(0.0), s1 = vec3(0.0), s2 = vec3(0.0), s3 = vec3(0.0);
		float stepSize = radius / 3.0;
		for (int j = 0; j <= 3; j++) {
			for (int i = 0; i <= 3; i++) {
				vec2 o = vec2(float(i), float(j)) * stepSize * texel;
				vec3 c;
				c = texture2D(tex, uv + vec2(-o.x, -o.y)).rgb; m0 += c; s0 += c * c;
				c = texture2D(tex, uv + vec2( o.x, -o.y)).rgb; m1 += c; s1 += c * c;
				c = texture2D(tex, uv + vec2(-o.x,  o.y)).rgb; m2 += c; s2 += c * c;
				c = texture2D(tex, uv + vec2( o.x,  o.y)).rgb; m3 += c; s3 += c * c;
			}
		}
		m0 /= 16.0; m1 /= 16.0; m2 /= 16.0; m3 /= 16.0;
		vec3 v0 = s0 / 16.0 - m0 * m0, v1 = s1 / 16.0 - m1 * m1, v2 = s2 / 16.0 - m2 * m2, v3 = s3 / 16.0 - m3 * m3;
		// Soft choice between the quadrants (calmer ones weigh far more): strokes without blocks.
		float w0 = 1.0 / pow(0.0015 + v0.r + v0.g + v0.b, 4.0);
		float w1 = 1.0 / pow(0.0015 + v1.r + v1.g + v1.b, 4.0);
		float w2 = 1.0 / pow(0.0015 + v2.r + v2.g + v2.b, 4.0);
		float w3 = 1.0 / pow(0.0015 + v3.r + v3.g + v3.b, 4.0);
		color = (m0 * w0 + m1 * w1 + m2 * w2 + m3 * w3) / (w0 + w1 + w2 + w3);
	}
	if (grain > 0.0) {
		float lum = dot(color, vec3(0.299, 0.587, 0.114));
		float n = hash(gl_FragCoord.xy + vec2(seed * 37.0, seed * 91.0)) - 0.5;
		color += n * grain * (0.10 + 0.22 * 4.0 * lum * (1.0 - lum));
	}
	// The interface drawn over the world (labels, panels) stays as it is.
	float world = useMask > 0.5 ? texture2D(mask, uv * maskScale).r : 1.0;
	color = mix(original, color, world);
	gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)";

// OpenGL entry points, loaded through SDL (nothing is linked against OpenGL directly).
struct Gl {
	PFNGLCREATESHADERPROC CreateShader;
	PFNGLSHADERSOURCEPROC ShaderSource;
	PFNGLCOMPILESHADERPROC CompileShader;
	PFNGLGETSHADERIVPROC GetShaderiv;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
	PFNGLDELETESHADERPROC DeleteShader;
	PFNGLCREATEPROGRAMPROC CreateProgram;
	PFNGLATTACHSHADERPROC AttachShader;
	PFNGLLINKPROGRAMPROC LinkProgram;
	PFNGLGETPROGRAMIVPROC GetProgramiv;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
	PFNGLUSEPROGRAMPROC UseProgram;
	PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
	PFNGLUNIFORM1IPROC Uniform1i;
	PFNGLUNIFORM1FPROC Uniform1f;
	PFNGLUNIFORM2FPROC Uniform2f;
	PFNGLACTIVETEXTUREPROC ActiveTexture;
	void(APIENTRY *GetIntegerv)(GLenum, GLint *);
	GLboolean(APIENTRY *IsEnabled)(GLenum);
	void(APIENTRY *Enable)(GLenum);
	void(APIENTRY *Disable)(GLenum);
	void(APIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
	void(APIENTRY *Begin)(GLenum);
	void(APIENTRY *End)();
	void(APIENTRY *TexCoord2f)(GLfloat, GLfloat);
	void(APIENTRY *Vertex2f)(GLfloat, GLfloat);
	void(APIENTRY *TexParameteri)(GLenum, GLenum, GLint);
	void(APIENTRY *GetTexParameteriv)(GLenum, GLenum, GLint *);
};

Gl gl;
SDL_Renderer *builtFor = nullptr;
bool ready = false;
GLuint program = 0;
GLint uTex = -1, uTexel = -1, uRadius = -1, uGrain = -1, uSeed = -1, uMask = -1, uUseMask = -1, uMaskScale = -1;
SDL_Texture *maskTexture = nullptr;
int maskW = 0, maskH = 0;
std::vector<uint32_t> maskPixels;

template <typename T>
bool Load(T &fn, const char *name)
{
	fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
	return fn != nullptr;
}

bool LoadGl()
{
	bool ok = true;
	ok &= Load(gl.CreateShader, "glCreateShader");
	ok &= Load(gl.ShaderSource, "glShaderSource");
	ok &= Load(gl.CompileShader, "glCompileShader");
	ok &= Load(gl.GetShaderiv, "glGetShaderiv");
	ok &= Load(gl.GetShaderInfoLog, "glGetShaderInfoLog");
	ok &= Load(gl.DeleteShader, "glDeleteShader");
	ok &= Load(gl.CreateProgram, "glCreateProgram");
	ok &= Load(gl.AttachShader, "glAttachShader");
	ok &= Load(gl.LinkProgram, "glLinkProgram");
	ok &= Load(gl.GetProgramiv, "glGetProgramiv");
	ok &= Load(gl.GetProgramInfoLog, "glGetProgramInfoLog");
	ok &= Load(gl.UseProgram, "glUseProgram");
	ok &= Load(gl.GetUniformLocation, "glGetUniformLocation");
	ok &= Load(gl.Uniform1i, "glUniform1i");
	ok &= Load(gl.Uniform1f, "glUniform1f");
	ok &= Load(gl.Uniform2f, "glUniform2f");
	ok &= Load(gl.ActiveTexture, "glActiveTexture");
	ok &= Load(gl.GetIntegerv, "glGetIntegerv");
	ok &= Load(gl.IsEnabled, "glIsEnabled");
	ok &= Load(gl.Enable, "glEnable");
	ok &= Load(gl.Disable, "glDisable");
	ok &= Load(gl.Viewport, "glViewport");
	ok &= Load(gl.Begin, "glBegin");
	ok &= Load(gl.End, "glEnd");
	ok &= Load(gl.TexCoord2f, "glTexCoord2f");
	ok &= Load(gl.Vertex2f, "glVertex2f");
	ok &= Load(gl.TexParameteri, "glTexParameteri");
	ok &= Load(gl.GetTexParameteriv, "glGetTexParameteriv");
	return ok;
}

GLuint Compile(GLenum type, const char *source)
{
	const GLuint shader = gl.CreateShader(type);
	gl.ShaderSource(shader, 1, &source, nullptr);
	gl.CompileShader(shader);
	GLint ok = 0;
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == 0) {
		char log[1024] = {};
		gl.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
		LogError("Painterly shader: {}", log);
		gl.DeleteShader(shader);
		return 0;
	}
	return shader;
}

/** Sets up the shader for a renderer (once). */
bool Prepare(SDL_Renderer *renderer)
{
	if (builtFor == renderer)
		return ready;
	builtFor = renderer;
	ready = false;
	SDL_RendererInfo info {};
	if (SDL_GetRendererInfo(renderer, &info) != 0 || info.name == nullptr || std::strcmp(info.name, "opengl") != 0) {
		Status = StrCat("Needs OpenGL (renderer: ", info.name != nullptr ? info.name : "?", ")");
		return false;
	}
	if (!LoadGl()) {
		Status = "OpenGL 2.0 shaders are not available";
		return false;
	}
	const GLuint vs = Compile(GL_VERTEX_SHADER, VertexSource);
	const GLuint fs = Compile(GL_FRAGMENT_SHADER, FragmentSource);
	if (vs == 0 || fs == 0) {
		Status = "The shader did not compile (see the log)";
		return false;
	}
	program = gl.CreateProgram();
	gl.AttachShader(program, vs);
	gl.AttachShader(program, fs);
	gl.LinkProgram(program);
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);
	GLint linked = 0;
	gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked == 0) {
		char log[1024] = {};
		gl.GetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
		LogError("Painterly shader: {}", log);
		Status = "The shader did not link (see the log)";
		return false;
	}
	uTex = gl.GetUniformLocation(program, "tex");
	uTexel = gl.GetUniformLocation(program, "texel");
	uRadius = gl.GetUniformLocation(program, "radius");
	uGrain = gl.GetUniformLocation(program, "grain");
	uSeed = gl.GetUniformLocation(program, "seed");
	uMask = gl.GetUniformLocation(program, "mask");
	uUseMask = gl.GetUniformLocation(program, "useMask");
	uMaskScale = gl.GetUniformLocation(program, "maskScale");
	if (maskTexture != nullptr) {
		SDL_DestroyTexture(maskTexture);
		maskTexture = nullptr;
	}
	ready = true;
	Status = "GPU shader on";
	return true;
}

#endif

} // namespace

bool PainterlyChosen()
{
	return *GetOptions().Graphics.textures == TextureMode::Painterly;
}

bool PainterlyRunning()
{
	return Running;
}

std::string_view PainterlyStatus()
{
	return Status;
}

void PainterlyPresent(SDL_Renderer *renderer, SDL_Texture *frame)
{
	Running = false;
	if (!PainterlyChosen()) {
		Status = "Off";
		return;
	}
#if defined(USE_SDL1) || defined(USE_SDL3)
	(void)renderer;
	(void)frame;
	Status = "Not available in this build";
#else
	if (renderer == nullptr || frame == nullptr) {
		Status = "Needs Upscale (a renderer) in the graphics settings";
		return;
	}
	if (!Prepare(renderer))
		return;
	const int smoothing = std::clamp(*GetOptions().Graphics.paintSmoothing, 0, 100);
	const int grainLevel = std::clamp(*GetOptions().Graphics.paintGrain, 0, 100);
	if (smoothing == 0 && grainLevel == 0) {
		Status = "GPU shader on (nothing to do: smoothing and grain are 0)";
		return;
	}

	// Where the game view lands in the window, in pixels (the frame is letterboxed and scaled).
	float sx = 1, sy = 1;
	SDL_RenderGetScale(renderer, &sx, &sy);
	SDL_Rect vp {};
	SDL_RenderGetViewport(renderer, &vp);
	int outW = 0, outH = 0;
	SDL_GetRendererOutputSize(renderer, &outW, &outH);
	const int px = static_cast<int>(std::lround(static_cast<float>(vp.x) * sx));
	const int py = static_cast<int>(std::lround(static_cast<float>(vp.y) * sy));
	const int pw = static_cast<int>(std::lround(static_cast<float>(gnScreenWidth) * sx));
	// Only the world: the interface below it (and anything drawn over the world after it) stays crisp.
	const int viewRows = std::clamp<int>(gnViewportHeight, 0, gnScreenHeight);
	const int ph = static_cast<int>(std::lround(static_cast<float>(viewRows) * sy));
	if (pw <= 0 || ph <= 0 || outH <= 0)
		return;

	// Which pixels are the world: the interface drawn over it is left alone.
	bool useMask = ColorOverlayWorldMask(maskPixels, gnScreenWidth, viewRows);
	if (useMask) {
		if (maskTexture == nullptr || maskW != gnScreenWidth || maskH != viewRows) {
			if (maskTexture != nullptr)
				SDL_DestroyTexture(maskTexture);
			maskTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, gnScreenWidth, viewRows);
			maskW = gnScreenWidth;
			maskH = viewRows;
		}
		useMask = maskTexture != nullptr && SDL_UpdateTexture(maskTexture, nullptr, maskPixels.data(), gnScreenWidth * 4) == 0;
	}

	SDL_RenderFlush(renderer); // everything SDL queued so far reaches the window first

	// Remember the state SDL's renderer believes is set, to put it back afterwards.
	GLint oldProgram = 0, oldViewport[4] = {}, oldActive = 0;
	gl.GetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
	gl.GetIntegerv(GL_VIEWPORT, oldViewport);
	gl.GetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
	const GLboolean oldBlend = gl.IsEnabled(GL_BLEND);
	const GLboolean oldScissor = gl.IsEnabled(GL_SCISSOR_TEST);
	gl.ActiveTexture(GL_TEXTURE0);

	float texW = 1, texH = 1;
	if (SDL_GL_BindTexture(frame, &texW, &texH) != 0) {
		Status = StrCat("Could not use the frame: ", SDL_GetError());
		gl.ActiveTexture(static_cast<GLenum>(oldActive));
		return;
	}
	if (texW > 1.5f || texH > 1.5f) {
		// A rectangle texture (no power-of-two-free textures on this GPU): the shader expects 2D.
		SDL_GL_UnbindTexture(frame);
		gl.ActiveTexture(static_cast<GLenum>(oldActive));
		Status = "This GPU lacks non-power-of-two textures";
		return;
	}
	float maskTexW = 1, maskTexH = 1;
	if (useMask) {
		gl.ActiveTexture(GL_TEXTURE1);
		useMask = SDL_GL_BindTexture(maskTexture, &maskTexW, &maskTexH) == 0;
		if (useMask && maskTexW > 1.5f) {
			SDL_GL_UnbindTexture(maskTexture);
			useMask = false;
		}
		gl.ActiveTexture(GL_TEXTURE0);
	}
	// Sample the frame smoothly (the brush strokes blend between game pixels).
	GLint oldMin = 0, oldMag = 0;
	gl.GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &oldMin);
	gl.GetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &oldMag);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	gl.Disable(GL_BLEND);
	gl.Disable(GL_SCISSOR_TEST);
	gl.Viewport(px, outH - py - ph, pw, ph);
	gl.UseProgram(program);
	gl.Uniform1i(uTex, 0);
	gl.Uniform2f(uTexel, texW / static_cast<float>(gnScreenWidth), texH / static_cast<float>(gnScreenHeight));
	// Up to 3.5 game pixels each way at full smoothing.
	gl.Uniform1f(uRadius, 3.5f * static_cast<float>(smoothing) / 100.0f);
	gl.Uniform1f(uGrain, static_cast<float>(grainLevel) / 100.0f);
	gl.Uniform1f(uSeed, static_cast<float>(SDL_GetTicks() / 41 % 997));
	const float bottom = texH * static_cast<float>(viewRows) / static_cast<float>(gnScreenHeight);
	gl.Uniform1i(uMask, 1);
	gl.Uniform1f(uUseMask, useMask ? 1.0f : 0.0f);
	gl.Uniform2f(uMaskScale, maskTexW / texW, maskTexH / bottom);
	gl.Begin(GL_QUADS);
	gl.TexCoord2f(0, bottom);
	gl.Vertex2f(-1, -1);
	gl.TexCoord2f(texW, bottom);
	gl.Vertex2f(1, -1);
	gl.TexCoord2f(texW, 0);
	gl.Vertex2f(1, 1);
	gl.TexCoord2f(0, 0);
	gl.Vertex2f(-1, 1);
	gl.End();

	// Put everything back as SDL left it.
	gl.UseProgram(static_cast<GLuint>(oldProgram));
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, oldMin);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, oldMag);
	SDL_GL_UnbindTexture(frame);
	if (useMask) {
		gl.ActiveTexture(GL_TEXTURE1);
		SDL_GL_UnbindTexture(maskTexture);
	}
	gl.Viewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
	if (oldBlend != GL_FALSE)
		gl.Enable(GL_BLEND);
	if (oldScissor != GL_FALSE)
		gl.Enable(GL_SCISSOR_TEST);
	gl.ActiveTexture(static_cast<GLenum>(oldActive));
	Running = true;
	Status = "GPU shader on";
#endif
}

} // namespace devilution
