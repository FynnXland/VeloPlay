#pragma once

#include <initializer_list>
#include <string>

#include <GL/glcorearb.h>

namespace veloplay::linuxos {

/// Finds an OpenGL function by name. The window passes SDL_GL_GetProcAddress, wrapped:
/// `[](const char* name) -> void* { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }`.
using GlProcLoader = void* (*)(const char*);

// Every function the picture parts call. They live in a struct and not as
// globals: Dear ImGui's OpenGL backend brings a loader of its own with
// global names like glGenTextures, and the two must not meet.
#define VELOPLAY_GL_FUNCTIONS(X)                                                                                       \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                                                           \
    X(PFNGLATTACHSHADERPROC, AttachShader)                                                                             \
    X(PFNGLBINDBUFFERPROC, BindBuffer)                                                                                 \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                                                       \
    X(PFNGLBINDSAMPLERPROC, BindSampler)                                                                               \
    X(PFNGLBINDTEXTUREPROC, BindTexture)                                                                               \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                                                       \
    X(PFNGLBUFFERDATAPROC, BufferData)                                                                                 \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                                                         \
    X(PFNGLCLEARPROC, Clear)                                                                                           \
    X(PFNGLCLEARBUFFERFVPROC, ClearBufferfv)                                                                           \
    X(PFNGLCLEARCOLORPROC, ClearColor)                                                                                 \
    X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync)                                                                         \
    X(PFNGLCOLORMASKPROC, ColorMask)                                                                                   \
    X(PFNGLCOMPILESHADERPROC, CompileShader)                                                                           \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                                                           \
    X(PFNGLCREATESHADERPROC, CreateShader)                                                                             \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers)                                                                           \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                                                                 \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                                                           \
    X(PFNGLDELETESHADERPROC, DeleteShader)                                                                             \
    X(PFNGLDELETESYNCPROC, DeleteSync)                                                                                 \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures)                                                                         \
    X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                                                                 \
    X(PFNGLDISABLEPROC, Disable)                                                                                       \
    X(PFNGLDRAWARRAYSPROC, DrawArrays)                                                                                 \
    X(PFNGLENABLEPROC, Enable)                                                                                         \
    X(PFNGLFENCESYNCPROC, FenceSync)                                                                                   \
    X(PFNGLFINISHPROC, Finish)                                                                                         \
    X(PFNGLFLUSHPROC, Flush)                                                                                           \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                                                             \
    X(PFNGLGENBUFFERSPROC, GenBuffers)                                                                                 \
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap)                                                                         \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                                                                       \
    X(PFNGLGENTEXTURESPROC, GenTextures)                                                                               \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                                                       \
    X(PFNGLGETERRORPROC, GetError)                                                                                     \
    X(PFNGLGETINTEGERVPROC, GetIntegerv)                                                                               \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                                                                   \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                                                                     \
    X(PFNGLGETSHADERIVPROC, GetShaderiv)                                                                               \
    X(PFNGLGETSTRINGPROC, GetString)                                                                                   \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                                                                 \
    X(PFNGLISENABLEDPROC, IsEnabled)                                                                                   \
    X(PFNGLLINKPROGRAMPROC, LinkProgram)                                                                               \
    X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange)                                                                         \
    X(PFNGLPIXELSTOREIPROC, PixelStorei)                                                                               \
    X(PFNGLREADPIXELSPROC, ReadPixels)                                                                                 \
    X(PFNGLSHADERSOURCEPROC, ShaderSource)                                                                             \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D)                                                                                 \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri)                                                                           \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D)                                                                           \
    X(PFNGLUNIFORM1FPROC, Uniform1f)                                                                                   \
    X(PFNGLUNIFORM1IPROC, Uniform1i)                                                                                   \
    X(PFNGLUNIFORM2FPROC, Uniform2f)                                                                                   \
    X(PFNGLUNIFORM4FPROC, Uniform4f)                                                                                   \
    X(PFNGLUNMAPBUFFERPROC, UnmapBuffer)                                                                               \
    X(PFNGLUSEPROGRAMPROC, UseProgram)                                                                                 \
    X(PFNGLVIEWPORTPROC, Viewport)

// What every fragment stage of the picture starts with. `uv` runs from 0 to 1
// over the viewport. `pointAt` is Direct3D's point sampler with clamping: the
// texel the coordinate falls in, the nearest one at the edge. OpenGL has one
// filter per texture unit, so linear reads go through the texture's own
// filter (textureLod) and point reads through this.
#define VELOPLAY_GLSL_HEAD                                                                                             \
    "#version 330 core\n"                                                                                              \
    "in vec2 uv;\n"                                                                                                    \
    "out vec4 result;\n"                                                                                               \
    "vec4 pointAt(sampler2D t, vec2 at) {\n"                                                                           \
    "    ivec2 size = textureSize(t, 0);\n"                                                                            \
    "    return texelFetch(t, clamp(ivec2(floor(at * vec2(size))), ivec2(0), size - ivec2(1)), 0);\n"                  \
    "}\n"

/// The OpenGL 3.3 core functions VeloPlay's picture parts use. Called as
/// `gl.TexImage2D(...)`. Load once per context.
struct GlFunctions {
#define VELOPLAY_GL_MEMBER(type, name) type name = nullptr;
    VELOPLAY_GL_FUNCTIONS(VELOPLAY_GL_MEMBER)
#undef VELOPLAY_GL_MEMBER

    /// Needs a current context of version 3.3 or later. False, with the reason, if a function is missing
    /// or the context is older.
    bool load(GlProcLoader loader, std::string& error);
    [[nodiscard]] bool loaded() const { return loaded_; }

private:
    bool loaded_ = false;
};

/// A texture with a framebuffer that draws into its first level. Every stage
/// of the picture writes into one of these.
struct GlTarget {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int width = 0;
    int height = 0;
    GLenum format = 0; // internal format
    int levels = 1;
};

/// Makes `target` `width` x `height` of `format` unless it already is. Textures with more than one level read
/// between levels (they are reduced with glGenerateMipmap). Leaves no framebuffer bound.
bool ensureTarget(const GlFunctions& gl, GlTarget& target, int width, int height, GLenum format, int levels = 1);
void releaseTarget(const GlFunctions& gl, GlTarget& target);

/// Compiles the shared vertex stage and `fragment` into a program and points its samplers, in the order
/// given, at texture units 0, 1, 2, ... False with the compiler's message if that fails.
bool makeProgram(const GlFunctions& gl, const char* name, const char* fragment, std::initializer_list<const char*> samplers,
                 GLuint& program, std::string& error);

/// Sets the state every pass of the picture needs and nothing of the caller's survives in: no blending,
/// scissor, depth, stencil, culling, dithering or sRGB conversion, all colour channels written, and no
/// sampler objects on the first `units` texture units (ImGui's backend binds one, and it would replace the
/// filtering the textures carry).
void preparePasses(const GlFunctions& gl, int units);

/// Draws one triangle that covers the viewport `x, y, width, height` of `framebuffer` with `program`,
/// reading `textures` on units 0, 1, ... in that order. A texture of 0 reads as zero, as a missing view
/// does in Direct3D (`zero` is a one-pixel texture holding nothing).
void runPass(const GlFunctions& gl, GLuint vertexArray, GLuint program, GLuint framebuffer, int x, int y, int width,
             int height, std::initializer_list<GLuint> textures, GLuint zero);

/// A texture of one pixel, all four channels zero, that stands in where a stage has nothing to read.
GLuint makeZeroTexture(const GlFunctions& gl);

} // namespace veloplay::linuxos
