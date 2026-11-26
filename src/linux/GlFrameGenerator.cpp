#include "linux/GlFrameGenerator.hpp"

#include <algorithm>

namespace veloplay::linuxos {

namespace {

// The shaders of src/render/FrameGenerator.cpp, in GLSL. The comments there
// explain each step at length; the constants are the same. One set of
// uniforms for all of them, as the one constant buffer on Windows.
#define VELOPLAY_GLSL_FLOW                                                                                             \
    "uniform vec2 lumaSize;      // size of the brightness level that is searched, in its pixels\n"                    \
    "uniform float scale;        // picture pixels per pixel of that level\n"                                          \
    "uniform float stepSize;     // distance between two shifts that are tried, in picture pixels\n"                   \
    "uniform float phase;        // where between the two pictures the new one lies, 0..1\n"                           \
    "uniform float mip;          // which copy of the brightness texture to read\n"                                    \
    "uniform float radius;       // shifts are tried from -radius to +radius steps\n"                                  \
    "uniform float hasPrediction;// 1 if the level above has a result to start from\n"                                 \
    "uniform vec2 frameSize;     // size of the pictures in pixels\n"                                                  \
    "uniform vec2 cells;         // size of the texture of vectors a pass reads, where it reads one\n"

// What the search compares: brightness, and next to it the two colour
// differences. Brightness alone is not enough: a blue nozzle in front of a
// grey wall that is exactly as bright does not exist in it, and on the
// screen that part flickered badly.
const char kSearchShader[] = VELOPLAY_GLSL_HEAD R"(
uniform sampler2D picture;
void main() {
    vec3 colour = textureLod(picture, uv, 0.0).rgb;
    float y = dot(colour, vec3(0.299, 0.587, 0.114));
    result = vec4(y, 0.5 + 0.564 * (colour.b - y), 0.5 + 0.713 * (colour.r - y), 1.0);
}
)";

// One level of the search. For every block: which shift makes the two
// pictures agree best? The shift is applied around the moment in between:
// back by `phase` of it in the earlier picture, forward by the rest in the
// later one. Sixteen samples spread over eight by eight pixels are compared.
// A shift away from what the level above found has to be clearly better to
// win; that keeps flat areas, where everything matches, from wandering.
const char kFlowShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_FLOW R"(
uniform sampler2D lumaA;
uniform sampler2D lumaB;
uniform sampler2D coarse;

float cost(vec2 centre, vec2 shift) {
    vec2 back = -phase * shift / scale;
    vec2 forward = (1.0 - phase) * shift / scale;
    float sum = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            vec2 at = centre + (vec2(float(i), float(j)) - 1.5) * 2.0;
            vec3 a = textureLod(lumaA, (at + back) / lumaSize, mip).rgb;
            vec3 b = textureLod(lumaB, (at + forward) / lumaSize, mip).rgb;
            sum += dot(abs(a - b), vec3(1.0)); // brightness and both colour differences
        }
    }
    return sum / 16.0;
}

void main() {
    vec2 predicted = vec2(0.0);
    if (hasPrediction > 0.5) predicted = textureLod(coarse, uv, 0.0).xy;
    vec2 centre = uv * lumaSize;

    // Standing still is always tried: a scoreboard over a moving background
    // must not be dragged along.
    vec2 best = vec2(0.0);
    float bestCost = cost(centre, best) * 1.02 + 0.00002;

    int reach = int(radius + 0.5);
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            vec2 shift = predicted + vec2(float(dx), float(dy)) * stepSize;
            float penalty = 0.04 * float(abs(dx) + abs(dy));
            float c = cost(centre, shift) * (1.0 + penalty) + penalty * 0.001;
            if (c < bestCost) {
                bestCost = c;
                best = shift;
            }
        }
    }
    result = vec4(best, bestCost, 1.0);
}
)";

// Is this a cut? What tells is the fine structure, the brightness of a place
// minus that of its surroundings. Across real motion the structure found
// along the shift is the same in both pictures; across a flash or a fade it
// is the same at the same place; across a cut it is neither. Two passes
// boil that down to one number: the first looks at eight by eight parts of
// the picture, sixty-four samples each; the second counts the parts where
// the smaller difference is as large as the structure itself.
const char kReduceShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_FLOW R"(
uniform sampler2D source;
uniform sampler2D lumaA;
uniform sampler2D lumaB;

float structure(sampler2D luma, vec2 at) {
    return textureLod(luma, at, 0.0).r - textureLod(luma, at, mip).r;
}

void main() {
    if (hasPrediction > 0.5) {
        // Second pass: the share of parts that do not fit.
        float misfits = 0.0;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                vec4 part = pointAt(source, (vec2(float(x), float(y)) + 0.5) / 8.0);
                float differs = min(part.r, part.g);
                misfits += smoothstep(0.5, 0.8, differs / (part.b + 0.0001)) * smoothstep(0.008, 0.016, differs);
            }
        }
        result = vec4(misfits / 64.0, 0.0, 0.0, 1.0);
        return;
    }
    float alongShift = 0.0;
    float inPlace = 0.0;
    float amount = 0.0;
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i) {
            vec2 at = uv + (vec2(float(i), float(j)) - 3.5) / (8.0 * cells);
            vec2 shift = textureLod(source, at, 0.0).xy / frameSize;
            float a = structure(lumaA, at - phase * shift);
            float b = structure(lumaB, at + (1.0 - phase) * shift);
            alongShift += abs(a - b);
            amount += 0.5 * (abs(a) + abs(b));
            inPlace += abs(structure(lumaA, at) - structure(lumaB, at));
        }
    }
    result = vec4(alongShift / 64.0, inPlace / 64.0, amount / 64.0, 1.0);
}
)";

// A cut in one part of the picture only: a page of a game's menu replaced
// by another while everything around it stays. Per part of about forty
// pixels, judged as cuts are, and only where the part had stood still until
// now: where a scene moves, a good share of the parts fit badly, and showing
// a real picture there would tear it into pieces of two moments.
const char kReplacedShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_FLOW R"(
uniform sampler2D source;
uniform sampler2D lumaA;
uniform sampler2D lumaB;
uniform sampler2D stoodBefore; // the count of standing still as it was before this pair of pictures

float structure(sampler2D luma, vec2 at) {
    return textureLod(luma, at, 0.0).r - textureLod(luma, at, mip).r;
}

void main() {
    float alongShift = 0.0;
    float inPlace = 0.0;
    float amount = 0.0;
    float stood = 0.0;
    for (int j = 0; j < 8; ++j) {
        for (int i = 0; i < 8; ++i) {
            vec2 at = uv + (vec2(float(i), float(j)) - 3.5) / (8.0 * cells);
            vec2 shift = textureLod(source, at, 0.0).xy / frameSize;
            float a = structure(lumaA, at - phase * shift);
            float b = structure(lumaB, at + (1.0 - phase) * shift);
            alongShift += abs(a - b);
            amount += 0.5 * (abs(a) + abs(b));
            inPlace += abs(structure(lumaA, at) - structure(lumaB, at));
            stood += pointAt(stoodBefore, at).r;
        }
    }
    float differs = min(alongShift, inPlace) / 64.0;
    float fitsNothing = smoothstep(0.5, 0.8, differs / (amount / 64.0 + 0.0001)) * smoothstep(0.008, 0.016, differs);
    result = vec4(fitsNothing * smoothstep(0.60, 0.85, stood / 64.0), 0.0, 0.0, 1.0);
}
)";

// Keeps count, per pixel, of how long it has stood still, and of how much
// of an edge it is. A pixel that did not change from one picture to the next
// gains a fifth of what is missing (held after about a third of a second at
// 30 pictures a second); one that changed a lot starts over; a small change,
// as the encoder causes around writing on a moving background, only sets it back.
const char kStillShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_FLOW R"(
uniform sampler2D frameA;
uniform sampler2D frameB;
uniform sampler2D before;

float brightness(vec2 at) {
    return dot(pointAt(frameB, at).rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 difference = abs(pointAt(frameA, uv).rgb - pointAt(frameB, uv).rgb);
    float change = max(difference.r, max(difference.g, difference.b));
    vec4 was = pointAt(before, uv);
    float still = was.r + (1.0 - was.r) * 0.20;
    if (change >= 0.10) still = 0.0;
    else if (change >= 0.03) still = was.r * 0.6;

    vec2 pixel = 1.0 / frameSize;
    float here = brightness(uv);
    float contrast = max(max(abs(here - brightness(uv + vec2(pixel.x, 0.0))), abs(here - brightness(uv - vec2(pixel.x, 0.0)))),
                         max(abs(here - brightness(uv + vec2(0.0, pixel.y))), abs(here - brightness(uv - vec2(0.0, pixel.y)))));
    // An edge is remembered for a while: writing can sit on a background that hides it for a moment.
    float edge = max(contrast, was.g * 0.97);
    result = vec4(still, edge, 0.0, 1.0);
}
)";

// The new picture. Every pixel is fetched from both pictures along the
// shift found for its place and mixed by how close the moment is to each.
// Where the two fetched colours differ a lot, the shift cannot be trusted
// there; the pixel then turns into a plain mix of the two pictures at that place.
const char kSynthesisShader[] = VELOPLAY_GLSL_HEAD VELOPLAY_GLSL_FLOW R"(
uniform sampler2D frameA;
uniform sampler2D frameB;
uniform sampler2D flow;
uniform sampler2D misfit;   // one value: the share of the picture whose structure fits nothing
uniform sampler2D standing; // per pixel: how long unchanged, how much of an edge
uniform sampler2D replaced; // per part: it had stood and now fits nothing (a page of a menu was replaced)

// A cubic filter with a strong negative lobe (Keys, a = -1) keeps the new
// picture about as crisp as the real ones around it; the linear filter
// blurred it, and that showed as a flicker in sharpness.
float cubicWeight(float d) {
    d = abs(d);
    if (d <= 1.0) return d * d * d - 2.0 * d * d + 1.0;
    if (d < 2.0) return -d * d * d + 5.0 * d * d - 8.0 * d + 4.0;
    return 0.0;
}
vec3 fetch(sampler2D picture, vec2 at) {
    vec2 p = at * frameSize - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            float w = cubicWeight(float(i) - f.x) * cubicWeight(float(j) - f.y);
            sum += pointAt(picture, (base + vec2(float(i), float(j)) + 0.5) / frameSize).rgb * w;
            total += w;
        }
    }
    return clamp(sum / total, 0.0, 1.0);
}

// How firmly a pixel is held: it has stood still for about a third of a second and is an edge.
float held(vec2 at) {
    vec4 s = pointAt(standing, at);
    return smoothstep(0.80, 0.95, s.r) * smoothstep(0.05, 0.15, s.g);
}

void main() {
    // A cut: in about half of the picture or more, nothing fits. The nearer real picture, as it is.
    if (pointAt(misfit, vec2(0.5)).r > 0.45) {
        result = vec4(phase < 0.5 ? pointAt(frameA, uv).rgb : pointAt(frameB, uv).rgb, 1.0);
        return;
    }
    vec2 shift = textureLod(flow, uv, 0.0).xy / frameSize;
    vec3 a = fetch(frameA, uv - phase * shift);
    vec3 b = fetch(frameB, uv + (1.0 - phase) * shift);
    // Judged like the search judges: by brightness and by colour.
    vec3 apart = a - b;
    float brighter = dot(apart, vec3(0.299, 0.587, 0.114));
    float difference = dot(abs(apart), vec3(0.299, 0.587, 0.114)) + 0.564 * abs(apart.b - brighter) +
                       0.713 * abs(apart.r - brighter);
    float doubt = smoothstep(0.10, 0.30, difference);
    // What has stood still stays where it is, and so does the pixel next to it: the soft rim of a letter.
    // (Holding every pixel that is merely the same in both pictures is wrong: the gaps of a ladder that
    // moves are the same too. Only having stood still over several pictures tells.)
    vec2 pixel = 1.0 / frameSize;
    float hold = max(held(uv), max(max(held(uv + vec2(pixel.x, 0.0)), held(uv - vec2(pixel.x, 0.0))),
                                   max(held(uv + vec2(0.0, pixel.y)), held(uv - vec2(0.0, pixel.y)))));
    vec3 plain = mix(textureLod(frameA, uv, 0.0).rgb, textureLod(frameB, uv, 0.0).rgb, phase);
    vec3 computed = mix(mix(a, b, phase), plain, max(doubt, hold));
    // A part that was replaced shows the nearer real picture. Looked up half a part to each side as well:
    // a part that lies half on the replaced page does not say so itself.
    vec2 halfPart = 0.5 / cells;
    float gone = textureLod(replaced, uv, 0.0).r;
    gone = max(gone, max(max(textureLod(replaced, uv + halfPart, 0.0).r, textureLod(replaced, uv - halfPart, 0.0).r),
                         max(textureLod(replaced, uv + vec2(halfPart.x, -halfPart.y), 0.0).r,
                             textureLod(replaced, uv + vec2(-halfPart.x, halfPart.y), 0.0).r)));
    vec3 nearer = phase < 0.5 ? pointAt(frameA, uv).rgb : pointAt(frameB, uv).rgb;
    result = vec4(mix(computed, nearer, smoothstep(0.35, 0.65, gone)), 1.0);
}
)";

constexpr int kMaxLevels = 6;
constexpr int kUnits = 6;

} // namespace

GlFrameGenerator::~GlFrameGenerator() {
    if (!created_) return;
    releaseSized();
    for (Program* program : {&search_, &flow_, &reduce_, &replacedProgram_, &still_, &synthesis_}) {
        if (program->id) gl_.DeleteProgram(program->id);
    }
    if (zero_) gl_.DeleteTextures(1, &zero_);
    if (vertexArray_) gl_.DeleteVertexArrays(1, &vertexArray_);
}

void GlFrameGenerator::abandon() {
    created_ = false;
    vertexArray_ = 0;
    zero_ = 0;
    for (Program* program : {&search_, &flow_, &reduce_, &replacedProgram_, &still_, &synthesis_}) *program = Program{};
    for (GlTarget& target : luma_) target = GlTarget{};
    levels_.clear();
    refined_ = Level{};
    misfitCoarse_ = GlTarget{};
    misfit_ = GlTarget{};
    replaced_ = GlTarget{};
    stillCount_[0] = GlTarget{};
    stillCount_[1] = GlTarget{};
    width_ = 0;
    height_ = 0;
}

bool GlFrameGenerator::makeProgram(Program& program, const char* name, const char* source,
                                   std::initializer_list<const char*> samplers, std::string& error) {
    if (!linuxos::makeProgram(gl_, name, source, samplers, program.id, error)) return false;
    // Looked up once: asking the driver per pass would make a threaded driver wait for its thread.
    const auto at = [&](const char* uniform) { return gl_.GetUniformLocation(program.id, uniform); };
    program.lumaSize = at("lumaSize");
    program.scale = at("scale");
    program.stepSize = at("stepSize");
    program.phase = at("phase");
    program.mip = at("mip");
    program.radius = at("radius");
    program.hasPrediction = at("hasPrediction");
    program.frameSize = at("frameSize");
    program.cells = at("cells");
    return true;
}

bool GlFrameGenerator::create(std::string& error) {
    created_ = true;
    gl_.GenVertexArrays(1, &vertexArray_);
    zero_ = makeZeroTexture(gl_);
    if (!makeProgram(search_, "search", kSearchShader, {"picture"}, error) ||
        !makeProgram(flow_, "flow", kFlowShader, {"lumaA", "lumaB", "coarse"}, error) ||
        !makeProgram(synthesis_, "synthesis", kSynthesisShader,
                     {"frameA", "frameB", "flow", "misfit", "standing", "replaced"}, error)) {
        return false;
    }
    // Not needed for a picture; without them cuts and standing parts get no special care.
    std::string extraError;
    if (!makeProgram(reduce_, "reduce", kReduceShader, {"source", "lumaA", "lumaB"}, extraError)) reduce_ = Program{};
    if (!makeProgram(still_, "still", kStillShader, {"frameA", "frameB", "before"}, extraError)) still_ = Program{};
    if (!makeProgram(replacedProgram_, "replaced", kReplacedShader, {"source", "lumaA", "lumaB", "stoodBefore"},
                     extraError)) {
        replacedProgram_ = Program{};
    }
    return true;
}

void GlFrameGenerator::apply(const Program& program, const Constants& c) {
    gl_.UseProgram(program.id);
    gl_.Uniform2f(program.lumaSize, c.lumaSize[0], c.lumaSize[1]);
    gl_.Uniform1f(program.scale, c.scale);
    gl_.Uniform1f(program.stepSize, c.stepSize);
    gl_.Uniform1f(program.phase, c.phase);
    gl_.Uniform1f(program.mip, c.mip);
    gl_.Uniform1f(program.radius, c.radius);
    gl_.Uniform1f(program.hasPrediction, c.hasPrediction);
    gl_.Uniform2f(program.frameSize, c.frameSize[0], c.frameSize[1]);
    gl_.Uniform2f(program.cells, c.cells[0], c.cells[1]);
}

void GlFrameGenerator::pass(const Program& program, GLuint framebuffer, int width, int height,
                            std::initializer_list<GLuint> textures) {
    runPass(gl_, vertexArray_, program.id, framebuffer, 0, 0, width, height, textures, zero_);
}

bool GlFrameGenerator::makeVectors(GlTarget& target, int width, int height) {
    // Shifts in picture pixels, with sign and fractions; the third channel keeps how well it matched.
    return ensureTarget(gl_, target, width, height, GL_RGBA16F);
}

void GlFrameGenerator::releaseSized() {
    for (GlTarget& target : luma_) releaseTarget(gl_, target);
    for (Level& level : levels_) releaseTarget(gl_, level.flow);
    levels_.clear();
    releaseTarget(gl_, refined_.flow);
    releaseTarget(gl_, misfitCoarse_);
    releaseTarget(gl_, misfit_);
    releaseTarget(gl_, replaced_);
    releaseTarget(gl_, stillCount_[0]);
    releaseTarget(gl_, stillCount_[1]);
}

bool GlFrameGenerator::ensureSize(int width, int height) {
    if (width == width_ && height == height_ && !levels_.empty()) return true;
    width_ = 0;
    height_ = 0;
    releaseSized();

    const int lumaWidth = std::max(1, width / 2);
    const int lumaHeight = std::max(1, height / 2);
    // Levels go down until the smaller side would drop below eight pixels.
    int count = 1;
    while (count < kMaxLevels && std::min(lumaWidth >> (count - 1), lumaHeight >> (count - 1)) >= 16) ++count;
    for (GlTarget& target : luma_) {
        if (!ensureTarget(gl_, target, lumaWidth, lumaHeight, GL_RGBA8, count)) return false;
    }

    levels_.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        Level& level = levels_[static_cast<std::size_t>(index)];
        level.lumaWidth = std::max(1, lumaWidth >> index);
        level.lumaHeight = std::max(1, lumaHeight >> index);
        if (!makeVectors(level.flow, std::max(1, (level.lumaWidth + 3) / 4), std::max(1, (level.lumaHeight + 3) / 4))) {
            releaseSized();
            return false;
        }
    }
    refined_.lumaWidth = lumaWidth;
    refined_.lumaHeight = lumaHeight;
    if (!makeVectors(refined_.flow, std::max(1, (lumaWidth + 3) / 4), std::max(1, (lumaHeight + 3) / 4))) {
        releaseSized();
        return false;
    }

    // For cuts and for what stands still. If these cannot be made, pictures are computed without that care.
    if (!makeVectors(misfitCoarse_, 8, 8) || !makeVectors(misfit_, 1, 1)) {
        releaseTarget(gl_, misfitCoarse_);
        releaseTarget(gl_, misfit_);
    }
    // Parts of about forty pixels: small enough for a panel of a menu, large enough for sixty-four samples to say something.
    if (!makeVectors(replaced_, std::clamp(width / 40, 8, 64), std::clamp(height / 40, 5, 36))) releaseTarget(gl_, replaced_);
    const float nothing[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (GlTarget& still : stillCount_) {
        if (!ensureTarget(gl_, still, width, height, GL_RGBA8)) {
            releaseTarget(gl_, stillCount_[0]);
            releaseTarget(gl_, stillCount_[1]);
            break;
        }
        // A new texture holds whatever was in that memory; the count starts at nothing.
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, still.framebuffer);
        gl_.ClearBufferfv(GL_COLOR, 0, nothing);
    }
    stillNow_ = 0;
    pairIsNew_ = false;
    width_ = width;
    height_ = height;
    return true;
}

bool GlFrameGenerator::prepare(int slot, GLuint picture, int width, int height) {
    if (!created_ || slot < 0 || slot > 1 || !picture) return false;
    preparePasses(gl_, kUnits);
    if (!ensureSize(width, height)) return false;
    pass(search_, luma_[slot].framebuffer, levels_[0].lumaWidth, levels_[0].lumaHeight, {picture});
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, luma_[slot].texture);
    gl_.GenerateMipmap(GL_TEXTURE_2D);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    pairIsNew_ = true;
    return true;
}

bool GlFrameGenerator::generate(int slotFrom, GLuint from, int slotTo, GLuint to, float phase, GLuint target) {
    if (!created_ || levels_.empty() || slotFrom < 0 || slotFrom > 1 || slotTo < 0 || slotTo > 1 || !from || !to) {
        return false;
    }
    preparePasses(gl_, kUnits);
    phase = std::clamp(phase, 0.0f, 1.0f);
    const GLuint flow = builtInFlow(slotFrom, slotTo, phase);
    return finish(slotFrom, from, slotTo, to, flow, phase, target);
}

GLuint GlFrameGenerator::builtInFlow(int slotFrom, int slotTo, float phase) {
    Constants constants;
    constants.phase = phase;
    constants.frameSize[0] = static_cast<float>(width_);
    constants.frameSize[1] = static_cast<float>(height_);

    // From the smallest copies to the largest, each level starting from the one above.
    GLuint previous = 0;
    for (int index = static_cast<int>(levels_.size()) - 1; index >= 0; --index) {
        const Level& level = levels_[static_cast<std::size_t>(index)];
        constants.lumaSize[0] = static_cast<float>(level.lumaWidth);
        constants.lumaSize[1] = static_cast<float>(level.lumaHeight);
        constants.scale = static_cast<float>(2 << index); // half size, halved again per level
        constants.stepSize = constants.scale;
        constants.mip = static_cast<float>(index);
        constants.radius = 2.0f;
        constants.hasPrediction = previous ? 1.0f : 0.0f;
        apply(flow_, constants);
        pass(flow_, level.flow.framebuffer, level.flow.width, level.flow.height,
             {luma_[slotFrom].texture, luma_[slotTo].texture, previous});
        previous = level.flow.texture;
    }
    // Once more on the finest level with half steps, which is one picture pixel.
    constants.stepSize = constants.scale * 0.5f;
    constants.radius = 1.0f;
    constants.hasPrediction = 1.0f;
    apply(flow_, constants);
    pass(flow_, refined_.flow.framebuffer, refined_.flow.width, refined_.flow.height,
         {luma_[slotFrom].texture, luma_[slotTo].texture, previous});
    return refined_.flow.texture;
}

bool GlFrameGenerator::finish(int slotFrom, GLuint from, int slotTo, GLuint to, GLuint flow, float phase, GLuint target) {
    Constants constants;
    constants.phase = phase;
    constants.frameSize[0] = static_cast<float>(width_);
    constants.frameSize[1] = static_cast<float>(height_);

    // The count of what stands still moves on once per pair of pictures, not once per picture computed.
    const bool counting = holdStill_ && still_.id && stillCount_[0].texture && stillCount_[1].texture;
    if (counting && pairIsNew_) {
        apply(still_, constants);
        pass(still_, stillCount_[1 - stillNow_].framebuffer, width_, height_, {from, to, stillCount_[stillNow_].texture});
        stillNow_ = 1 - stillNow_;
    }
    pairIsNew_ = false;

    // How much of the picture found no fit.
    const bool judging = reduce_.id && misfit_.texture && misfitCoarse_.texture;
    const float surroundings = static_cast<float>(std::min<std::size_t>(3, levels_.size() - 1));
    if (judging) {
        constants.cells[0] = 8.0f;
        constants.cells[1] = 8.0f;
        constants.hasPrediction = 0.0f;
        // "Surroundings" are the copy three steps down, a sixteenth of the picture's size.
        constants.mip = surroundings;
        apply(reduce_, constants);
        pass(reduce_, misfitCoarse_.framebuffer, 8, 8, {flow, luma_[slotFrom].texture, luma_[slotTo].texture});
        constants.cells[0] = 1.0f;
        constants.cells[1] = 1.0f;
        constants.hasPrediction = 1.0f;
        apply(reduce_, constants);
        pass(reduce_, misfit_.framebuffer, 1, 1, {misfitCoarse_.texture});
    }

    // Which parts were replaced. Needs the count as it was before this pair, which is the other of the two textures.
    const bool parts = counting && replacedProgram_.id && replaced_.texture;
    if (parts) {
        constants.cells[0] = static_cast<float>(replaced_.width);
        constants.cells[1] = static_cast<float>(replaced_.height);
        constants.hasPrediction = 0.0f;
        constants.mip = surroundings;
        apply(replacedProgram_, constants);
        pass(replacedProgram_, replaced_.framebuffer, replaced_.width, replaced_.height,
             {flow, luma_[slotFrom].texture, luma_[slotTo].texture, stillCount_[1 - stillNow_].texture});
    }

    // The last pass looks half a part to each side; without parts the size does not matter.
    constants.cells[0] = parts ? static_cast<float>(replaced_.width) : 1.0f;
    constants.cells[1] = parts ? static_cast<float>(replaced_.height) : 1.0f;
    constants.mip = 0.0f;
    constants.hasPrediction = 0.0f;
    apply(synthesis_, constants);
    // A texture that is missing reads as zero: no cut, nothing held, nothing replaced.
    pass(synthesis_, target, width_, height_,
         {from, to, flow, judging ? misfit_.texture : 0u, counting ? stillCount_[stillNow_].texture : 0u,
          parts ? replaced_.texture : 0u});
    return true;
}

} // namespace veloplay::linuxos
