#include "linux/GlFunctions.hpp"

namespace veloplay::linuxos {

namespace {

// One triangle that covers the viewport; no vertex buffer, the core profile
// only wants a vertex array bound. `uv` is (0, 0) at the first row of the
// framebuffer, which inside the picture stages holds the top of the picture,
// exactly as in Direct3D. The caller's framebuffer has its first row at the
// bottom of the screen, so the pass onto it sets `flipY`.
const char kVertexShader[] = R"(#version 330 core
uniform float flipY;
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = flipY > 0.5 ? vec2(p.x, 1.0 - p.y) : p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

bool compileStage(const GlFunctions& gl, GLenum stage, const char* source, GLuint& shader, std::string& error) {
    shader = gl.CreateShader(stage);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return true;
    GLint length = 0;
    gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(length > 1 ? length : 1), '\0');
    gl.GetShaderInfoLog(shader, length, nullptr, log.data());
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
    error = log;
    gl.DeleteShader(shader);
    shader = 0;
    return false;
}

} // namespace

bool GlFunctions::load(GlProcLoader loader, std::string& error) {
    loaded_ = false;
    if (!loader) {
        error = "no way to find OpenGL functions";
        return false;
    }
#define VELOPLAY_GL_LOAD(type, member)                                                                                 \
    member = reinterpret_cast<type>(loader("gl" #member));                                                             \
    if (!member) {                                                                                                     \
        error = "OpenGL function gl" #member " is missing";                                                            \
        return false;                                                                                                  \
    }
    VELOPLAY_GL_FUNCTIONS(VELOPLAY_GL_LOAD)
#undef VELOPLAY_GL_LOAD
    GLint major = 0;
    GLint minor = 0;
    GetIntegerv(GL_MAJOR_VERSION, &major);
    GetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor < 3)) {
        const auto* version = reinterpret_cast<const char*>(GetString(GL_VERSION));
        error = std::string("OpenGL 3.3 is needed, this context has ") + (version ? version : "an unknown version");
        return false;
    }
    loaded_ = true;
    return true;
}

bool ensureTarget(const GlFunctions& gl, GlTarget& target, int width, int height, GLenum format, int levels) {
    if (target.texture && target.width == width && target.height == height && target.format == format &&
        target.levels == levels) {
        return true;
    }
    releaseTarget(gl, target);
    if (width <= 0 || height <= 0) return false;

    GLenum layout = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;
    if (format == GL_RGBA16F) type = GL_HALF_FLOAT;
    gl.GenTextures(1, &target.texture);
    gl.BindTexture(GL_TEXTURE_2D, target.texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0, layout, type, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    // Without this a texture of one level would be incomplete, and reads from it would give zero.
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    if (levels > 1) gl.GenerateMipmap(GL_TEXTURE_2D); // makes room for the smaller copies
    gl.BindTexture(GL_TEXTURE_2D, 0);

    gl.GenFramebuffers(1, &target.framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        releaseTarget(gl, target);
        return false;
    }
    target.width = width;
    target.height = height;
    target.format = format;
    target.levels = levels;
    return true;
}

void releaseTarget(const GlFunctions& gl, GlTarget& target) {
    if (target.framebuffer) gl.DeleteFramebuffers(1, &target.framebuffer);
    if (target.texture) gl.DeleteTextures(1, &target.texture);
    target = GlTarget{};
}

bool makeProgram(const GlFunctions& gl, const char* name, const char* fragment, std::initializer_list<const char*> samplers,
                 GLuint& program, std::string& error) {
    GLuint vertex = 0;
    GLuint pixel = 0;
    if (!compileStage(gl, GL_VERTEX_SHADER, kVertexShader, vertex, error)) {
        error = std::string("vertex shader: ") + error;
        return false;
    }
    if (!compileStage(gl, GL_FRAGMENT_SHADER, fragment, pixel, error)) {
        error = std::string(name) + ": " + error;
        gl.DeleteShader(vertex);
        return false;
    }
    program = gl.CreateProgram();
    gl.AttachShader(program, vertex);
    gl.AttachShader(program, pixel);
    gl.LinkProgram(program);
    // The program keeps what it needs; the stages go once it is linked.
    gl.DeleteShader(vertex);
    gl.DeleteShader(pixel);
    GLint ok = GL_FALSE;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint length = 0;
        gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(length > 1 ? length : 1), '\0');
        gl.GetProgramInfoLog(program, length, nullptr, log.data());
        while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) log.pop_back();
        error = std::string(name) + " (linking): " + log;
        gl.DeleteProgram(program);
        program = 0;
        return false;
    }
    gl.UseProgram(program);
    int unit = 0;
    for (const char* sampler : samplers) gl.Uniform1i(gl.GetUniformLocation(program, sampler), unit++);
    gl.UseProgram(0);
    return true;
}

void preparePasses(const GlFunctions& gl, int units) {
    gl.Disable(GL_BLEND);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_DITHER);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (int unit = 0; unit < units; ++unit) gl.BindSampler(static_cast<GLuint>(unit), 0);
}

void runPass(const GlFunctions& gl, GLuint vertexArray, GLuint program, GLuint framebuffer, int x, int y, int width,
             int height, std::initializer_list<GLuint> textures, GLuint zero) {
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    gl.Viewport(x, y, width, height);
    gl.UseProgram(program);
    GLenum unit = GL_TEXTURE0;
    for (const GLuint texture : textures) {
        gl.ActiveTexture(unit++);
        gl.BindTexture(GL_TEXTURE_2D, texture ? texture : zero);
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindVertexArray(vertexArray);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

GLuint makeZeroTexture(const GlFunctions& gl) {
    GLuint texture = 0;
    const unsigned char nothing[4] = {0, 0, 0, 0};
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nothing);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

} // namespace veloplay::linuxos
