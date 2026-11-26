#pragma once

#include <string>
#include <vector>

#include "linux/GlFunctions.hpp"

namespace veloplay::linuxos {

/// Computes a picture that lies between two pictures from the console: the
/// OpenGL port of render::FlowFrameGenerator, VeloPlay's own method, with
/// the same shaders and constants. It follows how things move between the
/// two pictures and puts each part of the new picture where it would be at
/// that moment:
///
/// 1. Both pictures are reduced to brightness and two colour differences at
///    half size, with ever smaller copies below that.
/// 2. Starting on the smallest copies, each block looks for the shift at
///    which the two pictures match best, going out from what the level above
///    found. Shifts are tried around the moment in between.
/// 3. Every pixel of the new picture is fetched from both pictures along
///    that shift (cubic filter) and mixed. Where the two disagree it falls
///    back to a plain mix.
///
/// On top of that, as on Windows: a cut shows the nearer real picture; what
/// has stood still for about a third of a second (the game's display) stays
/// where it is; a part that had stood and is replaced (a page of a menu)
/// shows the nearer real picture.
///
/// Fragment shaders only (OpenGL 3.3 has no compute shaders), so it runs on
/// any card with OpenGL 3.3, integrated ones included. NVIDIA's optical flow
/// engine and AMD FRC have no counterpart here.
///
/// Needs the context it was created in to be current for every call, the
/// destructor included; abandon() forgets everything for a context that is gone.
class GlFrameGenerator {
public:
    explicit GlFrameGenerator(const GlFunctions& gl) : gl_(gl) {}
    ~GlFrameGenerator();
    GlFrameGenerator(const GlFrameGenerator&) = delete;
    GlFrameGenerator& operator=(const GlFrameGenerator&) = delete;

    bool create(std::string& error);

    /// Short name for the display.
    [[nodiscard]] const char* name() const { return "VeloPlay Flow"; }
    /// Lets a caller switch off the care for parts that stand still, to see what it does.
    void holdStillParts(bool hold) { holdStill_ = hold; }

    /// A picture from the console went into `slot` (0 or 1): an RGBA texture of `width` x `height`, first
    /// row the top of the picture. Called once per picture, so work that belongs to a picture and not to a
    /// pair is done only once.
    bool prepare(int slot, GLuint picture, int width, int height);

    /// Writes the picture at `phase` (0 = `from`, 1 = `to`) into `target`, a framebuffer the size of the
    /// pictures. Leaves the state as preparePasses() set it, with other bindings changed.
    bool generate(int slotFrom, GLuint from, int slotTo, GLuint to, float phase, GLuint target);

    /// Forgets everything made in the context without freeing it.
    void abandon();

private:
    struct Level {
        int lumaWidth = 0; // size of the brightness copy on this level
        int lumaHeight = 0;
        GlTarget flow;     // one shift per block of four by four of its pixels
    };
    /// The values every shader of the method reads; one set for all of them, as the one constant buffer
    /// on Windows.
    struct Constants {
        float lumaSize[2] = {0.0f, 0.0f};
        float scale = 0.0f;
        float stepSize = 0.0f;
        float phase = 0.0f;
        float mip = 0.0f;
        float radius = 0.0f;
        float hasPrediction = 0.0f;
        float frameSize[2] = {0.0f, 0.0f};
        float cells[2] = {0.0f, 0.0f};
    };
    struct Program {
        GLuint id = 0;
        GLint lumaSize = -1, scale = -1, stepSize = -1, phase = -1, mip = -1, radius = -1, hasPrediction = -1,
              frameSize = -1, cells = -1;
    };

    bool makeProgram(Program& program, const char* name, const char* source, std::initializer_list<const char*> samplers,
                     std::string& error);
    void apply(const Program& program, const Constants& constants);
    bool ensureSize(int width, int height);
    bool makeVectors(GlTarget& target, int width, int height);
    GLuint builtInFlow(int slotFrom, int slotTo, float phase);
    bool finish(int slotFrom, GLuint from, int slotTo, GLuint to, GLuint flow, float phase, GLuint target);
    void pass(const Program& program, GLuint framebuffer, int width, int height, std::initializer_list<GLuint> textures);
    void releaseSized();

    const GlFunctions& gl_;
    bool created_ = false;
    GLuint vertexArray_ = 0;
    GLuint zero_ = 0;
    Program search_;
    Program flow_;
    Program reduce_;
    Program replacedProgram_;
    Program still_;
    Program synthesis_;

    int width_ = 0;
    int height_ = 0;
    GlTarget luma_[2];          // brightness and the two colour differences at half size with all smaller copies
    std::vector<Level> levels_; // index 0 is the finest
    Level refined_;             // the finest level once more, with half steps

    // Cuts: how much of the picture found no fit, boiled down to one number the last pass can read.
    GlTarget misfitCoarse_; // eight by eight
    GlTarget misfit_;       // one by one
    // A page of a menu that is replaced: per part of the picture, whether it had stood and now fits nothing.
    GlTarget replaced_;
    // Standing still: per pixel how long it has been unchanged and how much of an edge it is.
    GlTarget stillCount_[2]; // written in turns: one is read while the other is written
    int stillNow_ = 0;       // the one that holds the newest count
    bool pairIsNew_ = false; // a picture arrived since the count was last brought up to date
    bool holdStill_ = true;
};

} // namespace veloplay::linuxos
