#include "linux/GlVideoRenderer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace veloplay::linuxos {

namespace {

// Stage one. I420 comes in three planes, NV12 in two; the colour samples are
// read where they lie, with the linear filter, as on Windows.
const char kConvertShader[] = VELOPLAY_GLSL_HEAD R"(
uniform sampler2D lumaTexture;
uniform sampler2D chromaTexture; // U, or for NV12 U and V together
uniform sampler2D chromaV;       // V, for I420
uniform float planar;
uniform vec4 row0;
uniform vec4 row1;
uniform vec4 row2;
void main() {
    float y = textureLod(lumaTexture, uv, 0.0).r;
    vec2 c = planar > 0.5 ? vec2(textureLod(chromaTexture, uv, 0.0).r, textureLod(chromaV, uv, 0.0).r)
                          : textureLod(chromaTexture, uv, 0.0).rg;
    vec4 yuv = vec4(y, c.x, c.y, 1.0);
    result = vec4(clamp(vec3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv)), 0.0, 1.0), 1.0);
}
)";

// A small copy of the brightness, for telling a repeated picture from a new
// one. Four samples a quarter of a cell apart; each reads four pixels.
const char kProbeShader[] = VELOPLAY_GLSL_HEAD R"(
uniform sampler2D lumaTexture;
void main() {
    vec2 d = vec2(0.25 / 96.0, 0.25 / 54.0);
    float y = textureLod(lumaTexture, uv + vec2(-d.x, -d.y), 0.0).r +
              textureLod(lumaTexture, uv + vec2(d.x, -d.y), 0.0).r +
              textureLod(lumaTexture, uv + vec2(-d.x, d.y), 0.0).r +
              textureLod(lumaTexture, uv + vec2(d.x, d.y), 0.0).r;
    result = vec4(y * 0.25, 0.0, 0.0, 1.0);
}
)";

#define VELOPLAY_GLSL_OUTPUT                                                                                           \
    "uniform vec2 sourceSize;  // size of the texture that is read, in pixels\n"                                       \
    "uniform vec2 origin;      // bottom left corner of the picture in the framebuffer, OpenGL's way\n"                \
    "uniform float sharpness;\n"                                                                                       \
    "uniform float seed;       // deband: the random pattern, the same in every picture\n"                             \
    "uniform float threshold;  // deband: largest step between neighbours that counts as banding\n"                    \
    "uniform float radius;     // deband: how far away neighbours are looked for, in pixels\n"                         \
    "uniform float flipY;      // 1 on the way into the caller's framebuffer\n"

// Stage three, soft: the card's own linear filter.
const char kSoftShader[] = VELOPLAY_GLSL_HEAD R"(
uniform sampler2D source;
void main() {
    result = vec4(textureLod(source, uv, 0.0).rgb, 1.0);
}
)";

// Stage three, sharp: Lanczos with two lobes, sixteen samples. Enlarging
// with it keeps edges crisper than the linear filter. Its side effect is a
// faint echo next to hard edges; limiting the result to what the four
// nearest pixels span removes that.
const char kLanczosShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_OUTPUT R"(
uniform sampler2D source;
float weight(float x) {
    x = abs(x);
    if (x < 0.0001) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return sin(px) * sin(px * 0.5) / (px * px * 0.5);
}
void main() {
    vec2 p = uv * sourceSize - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 lowest = vec3(1.0);
    vec3 highest = vec3(0.0);
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            float w = weight(float(x) - f.x) * weight(float(y) - f.y);
            vec3 c = pointAt(source, (base + vec2(float(x), float(y)) + 0.5) / sourceSize).rgb;
            sum += c * w;
            total += w;
            if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
                lowest = min(lowest, c);
                highest = max(highest, c);
            }
        }
    }
    result = vec4(clamp(sum / total, lowest, highest), 1.0);
}
)";

// Sharpening after the method AMD published as RCAS in FidelityFX Super
// Resolution 1 (MIT licence): each pixel is pushed away from the average of
// its four neighbours, but only as far as the contrast around it allows, so
// edges get crisper without bright or dark fringes. Written from the
// description; see THIRD_PARTY.md. It runs on the way into the caller's
// framebuffer, so the pixel it works on is counted from the picture's top.
const char kSharpenShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_OUTPUT R"(
uniform sampler2D source;
vec3 at(ivec2 p) {
    ivec2 limit = ivec2(sourceSize) - 1;
    return texelFetch(source, clamp(p, ivec2(0), limit), 0).rgb;
}
void main() {
    vec2 q = gl_FragCoord.xy - origin;
    if (flipY > 0.5) q.y = sourceSize.y - q.y;
    ivec2 p = ivec2(q);
    vec3 e = at(p);
    vec3 b = at(p + ivec2(0, -1));
    vec3 d = at(p + ivec2(-1, 0));
    vec3 f = at(p + ivec2(1, 0));
    vec3 h = at(p + ivec2(0, 1));
    vec3 lowest = min(min(b, d), min(f, h));
    vec3 highest = max(max(b, d), max(f, h));
    vec3 roomDown = min(lowest, e) / (4.0 * highest + 0.00001);
    vec3 roomUp = (1.0 - max(highest, e)) / min(4.0 * lowest - 4.0, vec3(-0.00001));
    vec3 lobes = max(-roomDown, roomUp);
    float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * sharpness;
    result = vec4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0), 1.0);
}
)";

// Banding: a stream at 6 to 15 Mbit/s turns smooth skies and gradients
// into visible steps one or two levels apart. Each pixel looks at four
// neighbours at a random distance and angle (different for every pixel,
// the same in every picture). Only where all four lie within a step or two
// of it, which is where the picture is flat apart from the steps, it takes
// their average; anywhere with detail or an edge it keeps its own value.
// Where that moves the pixel, it is dithered by up to one level, so writing
// it back into 8 bits does not cut new steps. Written for VeloPlay after the
// idea known from flash3kyuu and libplacebo; see THIRD_PARTY.md. The pixel
// positions are those of Direct3D (first row the top), so the pattern is the same.
const char kDebandShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_OUTPUT R"(
uniform sampler2D source;
float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float most(vec3 v) { return max(v.r, max(v.g, v.b)); }
void main() {
    vec2 px = gl_FragCoord.xy;
    vec3 c = texelFetch(source, ivec2(px), 0).rgb;
    float reach = (0.2 + 0.8 * hash(px + seed)) * radius;
    float angle = hash(px.yx * 1.37 + seed * 1.73 + 11.0) * 6.2831853;
    vec2 o = vec2(cos(angle), sin(angle)) * reach;
    vec2 inv = 1.0 / sourceSize;
    vec3 a = textureLod(source, (px + o) * inv, 0.0).rgb;
    vec3 b = textureLod(source, (px - o) * inv, 0.0).rgb;
    vec3 d = textureLod(source, (px + vec2(-o.y, o.x)) * inv, 0.0).rgb;
    vec3 e = textureLod(source, (px + vec2(o.y, -o.x)) * inv, 0.0).rgb;
    vec3 average = (a + b + d + e) * 0.25;
    float worst = max(max(most(abs(a - c)), most(abs(b - c))), max(most(abs(d - c)), most(abs(e - c))));
    float drift = most(abs(average - c));
    float w = (1.0 - smoothstep(threshold, threshold * 2.0, worst)) * (1.0 - smoothstep(threshold * 0.5, threshold, drift));
    // Two random values add up to a triangle between minus and plus one level. Only where the average
    // really moves the pixel: a flat area keeps its exact value instead of picking up noise.
    float grain = (hash(px * 1.31 + seed * 3.1) + hash(px * 0.77 + seed * 2.3 + 5.0) - 1.0) / 255.0;
    float moved = clamp(drift * 255.0 * 2.0, 0.0, 1.0);
    result = vec4(clamp(mix(c, average, w) + grain * w * moved, 0.0, 1.0), 1.0);
}
)";

// Picture styles. Colour grading in the picture's own (gamma) values, after the pictures in
// between and before enlarging, so real and computed pictures look alike and every scaler gets
// the styled picture. Black and white stay where they are; nothing here needs the scene.
const char kLookShader[] = VELOPLAY_GLSL_HEAD R"(
uniform sampler2D source;
uniform vec4 shadowTint;    // added in the dark parts, fading out towards the bright ones
uniform vec4 highlightTint; // added in the bright parts
uniform float contrast;     // 0 = none, 1 = a full S-curve
uniform float saturation;   // added to every colour's distance from grey
uniform float vibrance;     // added on top, the more the duller a colour is
uniform float vignette;     // how much darker the corners get
uniform float strength;     // mix between the picture as it comes (0) and the style (1)
void main() {
    vec3 original = texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb;
    vec3 c = original;
    // An S-curve: midtones spread, black and white stay.
    c = mix(c, c * c * (3.0 - 2.0 * c), contrast);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(luma), c, 1.0 + saturation + vibrance * (1.0 - clamp(chroma * 2.0, 0.0, 1.0)));
    float dark = (1.0 - luma) * (1.0 - luma);
    c += shadowTint.rgb * dark + highlightTint.rgb * luma * luma;
    vec2 fromCentre = uv - 0.5;
    c *= 1.0 - vignette * smoothstep(0.12, 0.5, dot(fromCentre, fromCentre));
    result = vec4(mix(original, clamp(c, 0.0, 1.0), strength), 1.0);
}
)";

struct Look {
    float shadowTint[4];
    float highlightTint[4];
    float contrast;
    float saturation;
    float vibrance;
    float vignette;
};

/// The styles, by number as the settings keep them; the same values as kLooks on Windows. 0 is the
/// picture as it comes and has no entry. Vivid lifts dull colours and the contrast a little; cinematic
/// gives a film look: a firmer curve, cool shadows, warm lights, slightly calmer colours and darker
/// corners (kept gentle, the corners hold a game's map and figures).
constexpr Look kLooks[] = {
    {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, 0.12f, 0.04f, 0.30f, 0.0f},
    {{-0.016f, 0.004f, 0.030f, 0.0f}, {0.034f, 0.012f, -0.024f, 0.0f}, 0.22f, -0.08f, 0.10f, 0.14f},
};

// Steps up to three levels apart count as banding. Real detail differs by more; a 1080p stream's
// steps are one or two levels in brightness, slightly more once turned into red, green and blue.
constexpr float kDebandThreshold = 3.0f / 255.0f;
constexpr float kDebandRadius = 16.0f;

// Texture units the stages read from; the sampler objects on them are taken off before the passes.
constexpr int kUnits = 8;

/// Sets the uniforms of the output block that `program` has; the others are left out by the compiler and
/// their location is -1, which OpenGL ignores.
void setOutput(const GlFunctions& gl, UniformCache& uniforms, GLuint program, float sourceWidth, float sourceHeight,
               float originX, float originY, float sharpness, bool flip) {
    gl.UseProgram(program);
    gl.Uniform2f(uniforms.at(gl, program, "sourceSize"), sourceWidth, sourceHeight);
    gl.Uniform2f(uniforms.at(gl, program, "origin"), originX, originY);
    gl.Uniform1f(uniforms.at(gl, program, "sharpness"), sharpness);
    gl.Uniform1f(uniforms.at(gl, program, "flipY"), flip ? 1.0f : 0.0f);
}

void makePlane(const GlFunctions& gl, GLuint& texture, int width, int height, GLenum format, GLenum layout) {
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0, layout, GL_UNSIGNED_BYTE, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

} // namespace

GLint UniformCache::at(const GlFunctions& gl, GLuint program, const char* name) {
    for (const Entry& entry : entries_) {
        if (entry.program == program && entry.name == name) return entry.location;
    }
    const GLint location = gl.GetUniformLocation(program, name);
    entries_.push_back({program, name, location});
    return location;
}

GlVideoRenderer::~GlVideoRenderer() {
    if (created_) releaseAll();
}

bool GlVideoRenderer::create(GlProcLoader load, std::string& error) {
    if (created_) releaseAll();
    if (!gl_.load(load, error)) return false;
    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;
    gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    created_ = true; // from here on releaseAll() cleans up after a failure
    gl_.GenVertexArrays(1, &vertexArray_);
    zero_ = makeZeroTexture(gl_);
    gl_.GenBuffers(1, &uploadBuffer_);
    if (!makeProgram(gl_, "convert", kConvertShader, {"lumaTexture", "chromaTexture", "chromaV"}, convertProgram_, error) ||
        !makeProgram(gl_, "soft", kSoftShader, {"source"}, softProgram_, error) ||
        !makeProgram(gl_, "lanczos", kLanczosShader, {"source"}, lanczosProgram_, error) ||
        !makeProgram(gl_, "sharpen", kSharpenShader, {"source"}, sharpenProgram_, error) ||
        !makeProgram(gl_, "deband", kDebandShader, {"source"}, debandProgram_, error) ||
        !makeProgram(gl_, "look", kLookShader, {"source"}, lookProgram_, error)) {
        releaseAll();
        return false;
    }
    // The comparison of pictures is an extra; without it a halved game rate goes unnoticed.
    std::string probeError;
    if (!makeProgram(gl_, "probe", kProbeShader, {"lumaTexture"}, probeProgram_, probeError)) probeProgram_ = 0;

    // So is frame generation. If it cannot be set up, pictures are shown as they arrive and the reason is kept.
    auto generator = std::make_unique<GlFrameGenerator>(gl_);
    if (generator->create(generatorError_)) generator_ = std::move(generator);
    restore(drawFramebuffer, readFramebuffer);
    return true;
}

void GlVideoRenderer::releaseAll() {
    if (created_) {
        generator_.reset();
        for (GLuint* program : {&convertProgram_, &probeProgram_, &softProgram_, &lanczosProgram_, &sharpenProgram_,
                                &debandProgram_, &lookProgram_}) {
            if (*program) gl_.DeleteProgram(*program);
        }
        for (GLuint* texture : {&zero_, &luma_.texture, &chromaU_.texture, &chromaV_.texture}) {
            if (*texture) gl_.DeleteTextures(1, texture);
        }
        for (GlTarget* target : {&slots_[0].rgb, &slots_[1].rgb, &generated_, &scaled_, &debanded_, &styled_, &probeTarget_}) {
            releaseTarget(gl_, *target);
        }
        for (ProbeCopy& copy : probeCopies_) {
            if (copy.fence) gl_.DeleteSync(copy.fence);
            if (copy.buffer) gl_.DeleteBuffers(1, &copy.buffer);
        }
        if (uploadBuffer_) gl_.DeleteBuffers(1, &uploadBuffer_);
        if (vertexArray_) gl_.DeleteVertexArrays(1, &vertexArray_);
    }
    abandon();
}

void GlVideoRenderer::reset() { releaseAll(); }

void GlVideoRenderer::abandon() {
    if (generator_) generator_->abandon();
    generator_.reset();
    created_ = false;
    uniforms_.clear();
    vertexArray_ = zero_ = uploadBuffer_ = 0;
    convertProgram_ = probeProgram_ = softProgram_ = lanczosProgram_ = sharpenProgram_ = debandProgram_ = lookProgram_ = 0;
    luma_ = Plane{};
    chromaU_ = Plane{};
    chromaV_ = Plane{};
    layout_ = media::PixelLayout::I420;
    slots_[0] = Slot{};
    slots_[1] = Slot{};
    newest_ = 0;
    nextId_ = 1;
    generated_ = GlTarget{};
    generatedFor_ = {};
    scaled_ = GlTarget{};
    debanded_ = GlTarget{};
    styled_ = GlTarget{};
    debandUsed_ = false;
    styleUsed_ = false;
    generatorError_.clear();
    generationWanted_ = true;
    prepared_[0] = prepared_[1] = false;
    scalerName_ = "";
    lastGenerated_ = false;
    shownWidth_ = shownHeight_ = shownLeft_ = shownTop_ = 0;
    probeTarget_ = GlTarget{};
    for (ProbeCopy& copy : probeCopies_) copy = ProbeCopy{};
    probeBefore_.clear();
    probeBeforePicture_ = 0;
    changes_.clear();
    submitted_ = 0;
    width_ = height_ = 0;
    hasPicture_ = false;
    color_ = {};
    colorSet_ = false;
}

void GlVideoRenderer::restore(GLint drawFramebuffer, GLint readFramebuffer) {
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
    gl_.UseProgram(0);
    gl_.BindVertexArray(0);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl_.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 4);
}

void GlVideoRenderer::clear() {
    hasPicture_ = false;
    lastGenerated_ = false;
    slots_[0].id = 0;
    slots_[1].id = 0;
    prepared_[0] = false;
    prepared_[1] = false;
    generatedFor_ = {};
    for (ProbeCopy& copy : probeCopies_) dropProbe(copy);
    probeBefore_.clear();
    probeBeforePicture_ = 0;
    changes_.clear();
}

std::optional<GlVideoRenderer::Change> GlVideoRenderer::takeChange() {
    if (created_) {
        collect(nullptr);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    if (changes_.empty()) return std::nullopt;
    const Change change = changes_.front();
    changes_.pop_front();
    return change;
}

void GlVideoRenderer::dropProbe(ProbeCopy& copy) {
    if (copy.fence && created_) gl_.DeleteSync(copy.fence);
    copy.fence = nullptr;
    copy.pending = false;
}

void GlVideoRenderer::readProbe(ProbeCopy& copy) {
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, copy.buffer);
    const std::size_t size = static_cast<std::size_t>(kProbeWidth) * kProbeHeight;
    const void* data = gl_.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
    if (data) {
        std::vector<std::uint8_t> now(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
        gl_.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
        if (probeBeforePicture_ + 1 == copy.picture && probeBefore_.size() == now.size()) {
            // A repeat that went through the encoder again differs by a level or two
            // here and there. More than three levels is a real change.
            std::size_t changed = 0;
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < now.size(); ++i) {
                const int difference = std::abs(static_cast<int>(now[i]) - static_cast<int>(probeBefore_[i]));
                if (difference > 3) ++changed;
                sum += static_cast<std::uint64_t>(difference);
            }
            const double count = static_cast<double>(now.size());
            changes_.push_back({copy.picture, static_cast<double>(changed) / count, static_cast<double>(sum) / count});
            if (changes_.size() > 64) changes_.pop_front();
        }
        probeBefore_ = std::move(now);
        probeBeforePicture_ = copy.picture;
    }
    dropProbe(copy);
}

void GlVideoRenderer::collect(const ProbeCopy* mustGo) {
    // The copies are read in the order they were made: a comparison needs the picture right before.
    while (true) {
        ProbeCopy* oldest = nullptr;
        for (ProbeCopy& copy : probeCopies_) {
            if (copy.pending && (!oldest || copy.picture < oldest->picture)) oldest = &copy;
        }
        if (!oldest) return;
        const GLenum state = gl_.ClientWaitSync(oldest->fence, 0, 0); // never waits
        if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) {
            readProbe(*oldest);
        } else if (oldest == mustGo || state == GL_WAIT_FAILED) {
            // Its place is wanted for the picture now, and the card is not done with it: that comparison is lost.
            dropProbe(*oldest);
        } else {
            return;
        }
    }
}

void GlVideoRenderer::probe() {
    if (!probeProgram_ || !luma_.texture || !ensureTarget(gl_, probeTarget_, kProbeWidth, kProbeHeight, GL_RGBA8)) return;
    ProbeCopy& copy = probeCopies_[submitted_ % std::size(probeCopies_)];
    // What this place holds is three pictures old. Read it, with all before it, before it is written again.
    collect(&copy);
    // A refused picture can leave this place newer than another one still on its way; it goes all the same.
    if (copy.pending) dropProbe(copy);

    runPass(gl_, vertexArray_, probeProgram_, probeTarget_.framebuffer, 0, 0, kProbeWidth, kProbeHeight,
            {luma_.texture}, zero_);
    if (!copy.buffer) {
        gl_.GenBuffers(1, &copy.buffer);
        gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, copy.buffer);
        gl_.BufferData(GL_PIXEL_PACK_BUFFER, kProbeWidth * kProbeHeight, nullptr, GL_STREAM_READ);
    }
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, probeTarget_.framebuffer);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, copy.buffer);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    // Into the buffer, not into memory here: the call returns at once and the card fills it when it gets there.
    gl_.ReadPixels(0, 0, kProbeWidth, kProbeHeight, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    copy.fence = gl_.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    copy.picture = submitted_;
    copy.pending = copy.fence != nullptr;
    // Hands the work to the card now rather than at the next swap, so the copy is there a picture or two later.
    gl_.Flush();
}

bool GlVideoRenderer::ensurePlanes(int width, int height, media::PixelLayout layout) {
    if (width == width_ && height == height_ && layout == layout_ && luma_.texture) return true;
    for (GLuint* texture : {&luma_.texture, &chromaU_.texture, &chromaV_.texture}) {
        if (*texture) gl_.DeleteTextures(1, texture);
        *texture = 0;
    }
    if (width != width_ || height != height_) clear();
    width_ = width;
    height_ = height;
    layout_ = layout;

    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    makePlane(gl_, luma_.texture, width, height, GL_R8, GL_RED);
    luma_.width = width;
    luma_.height = height;
    if (layout == media::PixelLayout::Nv12) {
        makePlane(gl_, chromaU_.texture, chromaWidth, chromaHeight, GL_RG8, GL_RG);
    } else {
        makePlane(gl_, chromaU_.texture, chromaWidth, chromaHeight, GL_R8, GL_RED);
        makePlane(gl_, chromaV_.texture, chromaWidth, chromaHeight, GL_R8, GL_RED);
        chromaV_.width = chromaWidth;
        chromaV_.height = chromaHeight;
    }
    chromaU_.width = chromaWidth;
    chromaU_.height = chromaHeight;
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    return luma_.texture != 0 && chromaU_.texture != 0;
}

bool GlVideoRenderer::upload(const media::VideoFrame& frame) {
    struct Part {
        const std::uint8_t* data;
        int stride;  // bytes from one row to the next in the frame
        int width;   // in pixels
        int height;
        int bytes;   // per pixel
        GLuint texture;
        GLenum layout;
        std::size_t offset = 0; // where it goes in the upload buffer
        bool block = false;     // the rows go over as they lie, gaps included
    };
    const int chromaWidth = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    std::vector<Part> parts;
    parts.push_back({frame.planes[0], frame.strides[0], frame.width, frame.height, 1, luma_.texture, GL_RED});
    if (frame.layout == media::PixelLayout::Nv12) {
        parts.push_back({frame.planes[1], frame.strides[1], chromaWidth, chromaHeight, 2, chromaU_.texture, GL_RG});
    } else {
        parts.push_back({frame.planes[1], frame.strides[1], chromaWidth, chromaHeight, 1, chromaU_.texture, GL_RED});
        parts.push_back({frame.planes[2], frame.strides[2], chromaWidth, chromaHeight, 1, chromaV_.texture, GL_RED});
    }

    // Each plane goes into one buffer the card copies from on its own time. Where the frame's row length is a
    // whole number of pixels the plane goes in one piece and OpenGL is told the row length; otherwise row by row.
    std::size_t total = 0;
    for (Part& part : parts) {
        const int row = part.width * part.bytes;
        part.block = part.stride >= row && part.stride % part.bytes == 0;
        part.offset = total;
        total += part.block ? static_cast<std::size_t>(part.height - 1) * static_cast<std::size_t>(part.stride) + row
                            : static_cast<std::size_t>(row) * static_cast<std::size_t>(part.height);
        total = (total + 15) & ~static_cast<std::size_t>(15);
    }

    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, uploadBuffer_);
    // A new store each time: the card may still be copying from the one before, and this way nobody waits for it.
    gl_.BufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(total), nullptr, GL_STREAM_DRAW);
    auto* mapped = static_cast<std::uint8_t*>(gl_.MapBufferRange(
        GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(total), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
    if (mapped) {
        for (const Part& part : parts) {
            const std::size_t row = static_cast<std::size_t>(part.width) * static_cast<std::size_t>(part.bytes);
            if (part.block) {
                std::memcpy(mapped + part.offset, part.data,
                            static_cast<std::size_t>(part.height - 1) * static_cast<std::size_t>(part.stride) + row);
            } else {
                for (int y = 0; y < part.height; ++y) {
                    std::memcpy(mapped + part.offset + static_cast<std::size_t>(y) * row,
                                part.data + static_cast<std::ptrdiff_t>(y) * part.stride, row);
                }
            }
        }
        if (gl_.UnmapBuffer(GL_PIXEL_UNPACK_BUFFER) != GL_TRUE) mapped = nullptr; // the store was lost; upload directly
    }
    if (!mapped) gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    for (const Part& part : parts) {
        gl_.BindTexture(GL_TEXTURE_2D, part.texture);
        if (mapped) {
            gl_.PixelStorei(GL_UNPACK_ROW_LENGTH, part.block ? part.stride / part.bytes : 0);
            gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, part.width, part.height, part.layout, GL_UNSIGNED_BYTE,
                              reinterpret_cast<const void*>(part.offset));
        } else if (part.block) {
            gl_.PixelStorei(GL_UNPACK_ROW_LENGTH, part.stride / part.bytes);
            gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, part.width, part.height, part.layout, GL_UNSIGNED_BYTE, part.data);
        } else {
            gl_.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            for (int y = 0; y < part.height; ++y) {
                gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, y, part.width, 1, part.layout, GL_UNSIGNED_BYTE,
                                  part.data + static_cast<std::ptrdiff_t>(y) * part.stride);
            }
        }
    }
    gl_.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool GlVideoRenderer::submit(const media::VideoFrame& frame, bool keep) {
    ++submitted_;
    if (!created_ || frame.width <= 0 || frame.height <= 0) return false;
    // Pictures in the decoder's memory on a Windows graphics card have no way here.
    if (frame.layout != media::PixelLayout::I420 && frame.layout != media::PixelLayout::Nv12) return false;
    if (!frame.planes[0] || !frame.planes[1] || (frame.layout == media::PixelLayout::I420 && !frame.planes[2])) return false;

    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    gl_.GetIntegerv(GL_VIEWPORT, viewport);
    const auto done = [&](bool ok) {
        restore(drawFramebuffer, readFramebuffer);
        gl_.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        return ok;
    };
    preparePasses(gl_, kUnits);

    if (!ensurePlanes(frame.width, frame.height, frame.layout)) {
        luma_.width = 0; // forces a new attempt with the next picture
        width_ = 0;
        return done(false);
    }
    if (!upload(frame)) return done(false);
    probe();
    if (!keep) return done(true);

    if (!colorSet_ || !(color_ == frame.color)) {
        const media::ColorMatrix matrix = media::yuvToRgb(frame.color);
        gl_.UseProgram(convertProgram_);
        const char* names[3] = {"row0", "row1", "row2"};
        for (int r = 0; r < 3; ++r) {
            const auto& row = matrix.rows[static_cast<std::size_t>(r)];
            gl_.Uniform4f(uniforms_.at(gl_, convertProgram_, names[r]), row[0], row[1], row[2], row[3]);
        }
        color_ = frame.color;
        colorSet_ = true;
    }
    gl_.UseProgram(convertProgram_);
    gl_.Uniform1f(uniforms_.at(gl_, convertProgram_, "planar"), frame.layout == media::PixelLayout::Nv12 ? 0.0f : 1.0f);

    // Stage one: to RGB, into the slot that holds the older of the two pictures.
    const int slot = slots_[newest_].id == 0 ? newest_ : 1 - newest_;
    if (!ensureTarget(gl_, slots_[slot].rgb, width_, height_, GL_RGBA8)) return done(false);
    runPass(gl_, vertexArray_, convertProgram_, slots_[slot].rgb.framebuffer, 0, 0, width_, height_,
            {luma_.texture, chromaU_.texture, chromaV_.texture}, zero_);

    slots_[slot].id = nextId_++;
    newest_ = slot;
    prepared_[slot] = false;
    if (generator_ && generationWanted_) {
        if (generator_->prepare(slot, slots_[slot].rgb.texture, width_, height_)) {
            prepared_[slot] = true;
        } else {
            generatorError_ = "the textures for frame generation could not be created";
            generator_.reset();
        }
    }
    hasPicture_ = true;
    return done(true);
}

GLuint GlVideoRenderer::pictureFor(const media::FrameChoice& choice) {
    lastGenerated_ = false;
    const Slot& newer = slots_[newest_];
    const Slot& older = slots_[1 - newest_];
    if (!choice.valid) return newer.rgb.texture;

    if (choice.step > 0 && choice.steps > 1 && generator_ && older.id != 0 && choice.base == older.id &&
        choice.next == newer.id && prepared_[0] && prepared_[1]) {
        // Stage two. A computed picture is kept until another one is asked for.
        if (!(generatedFor_ == choice) || !generated_.texture) {
            if (!ensureTarget(gl_, generated_, width_, height_, GL_RGBA8)) return newer.rgb.texture;
            const float phase = static_cast<float>(choice.step) / static_cast<float>(choice.steps);
            if (!generator_->generate(1 - newest_, older.rgb.texture, newest_, newer.rgb.texture, phase,
                                      generated_.framebuffer)) {
                return older.rgb.texture;
            }
            generatedFor_ = choice;
        }
        lastGenerated_ = true;
        return generated_.texture;
    }
    if (older.id != 0 && choice.base == older.id) return older.rgb.texture;
    return newer.rgb.texture;
}

void GlVideoRenderer::draw(int targetWidth, int targetHeight, const Output& output) {
    if (!created_ || !hasPicture_ || targetWidth <= 0 || targetHeight <= 0) return;

    // The caller's framebuffer comes back at the end; the stages in between draw elsewhere.
    GLint screen = 0;
    GLint readFramebuffer = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screen);
    gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    gl_.GetIntegerv(GL_VIEWPORT, viewport);
    preparePasses(gl_, kUnits);

    GLuint picture = pictureFor(output.choice);
    debandUsed_ = false;
    if (output.deband && picture && ensureTarget(gl_, debanded_, width_, height_, GL_RGBA8)) {
        // Before enlarging, at the stream's own size: the steps are a few pixels wide there, and
        // whatever enlarges afterwards gets the smooth picture.
        setOutput(gl_, uniforms_, debandProgram_, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 0.0f, 0.0f, false);
        // The same random pattern in every picture. A new one each time made a still picture shimmer:
        // in a model on compressed skies about half the pixels changed by a level from one draw to the next.
        gl_.Uniform1f(uniforms_.at(gl_, debandProgram_, "seed"), 0.0f);
        gl_.Uniform1f(uniforms_.at(gl_, debandProgram_, "threshold"), kDebandThreshold);
        gl_.Uniform1f(uniforms_.at(gl_, debandProgram_, "radius"), kDebandRadius);
        runPass(gl_, vertexArray_, debandProgram_, debanded_.framebuffer, 0, 0, width_, height_, {picture}, zero_);
        picture = debanded_.texture;
        debandUsed_ = true;
    }
    styleUsed_ = false;
    const int look = output.look;
    const float lookStrength = std::clamp(output.lookStrength, 0.0f, 1.0f);
    if (look >= 1 && look <= static_cast<int>(std::size(kLooks)) && lookStrength > 0.0f && picture &&
        ensureTarget(gl_, styled_, width_, height_, GL_RGBA8)) {
        const Look& style = kLooks[look - 1];
        gl_.UseProgram(lookProgram_);
        gl_.Uniform4f(uniforms_.at(gl_, lookProgram_, "shadowTint"), style.shadowTint[0], style.shadowTint[1], style.shadowTint[2],
                      style.shadowTint[3]);
        gl_.Uniform4f(uniforms_.at(gl_, lookProgram_, "highlightTint"), style.highlightTint[0], style.highlightTint[1],
                      style.highlightTint[2], style.highlightTint[3]);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "contrast"), style.contrast);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "saturation"), style.saturation);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "vibrance"), style.vibrance);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "vignette"), style.vignette);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "strength"), lookStrength);
        gl_.Uniform1f(uniforms_.at(gl_, lookProgram_, "flipY"), 0.0f);
        runPass(gl_, vertexArray_, lookProgram_, styled_.framebuffer, 0, 0, width_, height_, {picture}, zero_);
        picture = styled_.texture;
        styleUsed_ = true;
    }

    const float scale = std::min(static_cast<float>(targetWidth) / static_cast<float>(width_),
                                 static_cast<float>(targetHeight) / static_cast<float>(height_));
    // Whole pixels, so the sharpening pass can address its neighbours exactly.
    const int shownWidth = std::max(1, std::min(targetWidth, static_cast<int>(static_cast<float>(width_) * scale + 0.5f)));
    const int shownHeight = std::max(1, std::min(targetHeight, static_cast<int>(static_cast<float>(height_) * scale + 0.5f)));
    shownWidth_ = shownWidth;
    shownHeight_ = shownHeight;
    // Centred as on Windows, counted from the top; OpenGL counts rows from the bottom.
    shownLeft_ = (targetWidth - shownWidth) / 2;
    shownTop_ = (targetHeight - shownHeight) / 2;
    const int bottom = targetHeight - shownTop_ - shownHeight;

    // Lanczos is for enlarging. Shrinking with it would skip pixels; the linear filter does that better.
    const bool lanczos = output.upscale && (shownWidth > width_ || shownHeight > height_);
    const GLuint scaler = lanczos ? lanczosProgram_ : softProgram_;
    const float sharpness = std::clamp(output.sharpness, 0.0f, 1.0f);
    scalerName_ = lanczos ? "Lanczos" : "";

    if (sharpness > 0.0f && ensureTarget(gl_, scaled_, shownWidth, shownHeight, GL_RGBA8)) {
        // Stage three in two steps: to window size first, then sharpen onto the screen.
        setOutput(gl_, uniforms_, scaler, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 0.0f, 0.0f, false);
        runPass(gl_, vertexArray_, scaler, scaled_.framebuffer, 0, 0, shownWidth, shownHeight, {picture}, zero_);
        setOutput(gl_, uniforms_, sharpenProgram_, static_cast<float>(shownWidth), static_cast<float>(shownHeight),
                  static_cast<float>(shownLeft_), static_cast<float>(bottom), sharpness, true);
        runPass(gl_, vertexArray_, sharpenProgram_, static_cast<GLuint>(screen), shownLeft_, bottom, shownWidth,
                shownHeight, {scaled_.texture}, zero_);
    } else {
        setOutput(gl_, uniforms_, scaler, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 0.0f, 0.0f, true);
        runPass(gl_, vertexArray_, scaler, static_cast<GLuint>(screen), shownLeft_, bottom, shownWidth, shownHeight,
                {picture}, zero_);
    }
    restore(screen, readFramebuffer);
    gl_.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

} // namespace veloplay::linuxos
