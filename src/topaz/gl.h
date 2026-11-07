// Topaz's native layer: OpenGL as it is used here.
//
// The system's own header declares OpenGL as it was in 1997. Everything later (shaders,
// buffers, framebuffers) is fetched from the driver by name once a context exists; the
// functions used are listed here and nothing else of OpenGL is touched. No loader library.
#pragma once

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#else
// Elsewhere OpenGL comes from a library found when a window first needs one, not one jade is
// linked with, so that jade runs where there is none (G56): the types and the few names of
// OpenGL 1.1 used here are declared below, and its functions are reached through `gl` as the
// later ones are (the gl* names below stand for them).
#define APIENTRY
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLubyte = unsigned char;
using GLbitfield = unsigned int;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ONE 1
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define JADE_GL_TABLE_1_1 1
#endif

#include <cstddef>

namespace jade::topaz {

const GLenum kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;
const GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0, kTexture0 = 0x84C0, kClampToEdge = 0x812F;
const GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
const GLenum kR8 = 0x8229, kRed = 0x1903, kRgba8 = 0x8058, kTriangleStrip = 0x0005;

struct GlApi {
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* UseProgram)(GLuint);
    GLint(APIENTRY* GetUniformLocation)(GLuint, const char*);
    void(APIENTRY* Uniform1i)(GLint, GLint);
    void(APIENTRY* Uniform1f)(GLint, GLfloat);
    void(APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void(APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void(APIENTRY* BindVertexArray)(GLuint);
    void(APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindBuffer)(GLenum, GLuint);
    void(APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void(APIENTRY* EnableVertexAttribArray)(GLuint);
    void(APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void(APIENTRY* VertexAttribDivisor)(GLuint, GLuint);
    void(APIENTRY* DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
    void(APIENTRY* ActiveTexture)(GLenum);
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
#ifdef JADE_GL_TABLE_1_1
    // OpenGL 1.1's, which Windows' own library gives by name and elsewhere are fetched too.
    void(APIENTRY* PixelStorei)(GLenum, GLint);
    void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(APIENTRY* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void(APIENTRY* BindTexture)(GLenum, GLuint);
    void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
    void(APIENTRY* GenTextures)(GLsizei, GLuint*);
    void(APIENTRY* DeleteTextures)(GLsizei, const GLuint*);
    void(APIENTRY* Enable)(GLenum);
    void(APIENTRY* Disable)(GLenum);
    void(APIENTRY* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Clear)(GLbitfield);
    void(APIENTRY* Viewport)(GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* Scissor)(GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    void(APIENTRY* GetIntegerv)(GLenum, GLint*);
    const GLubyte*(APIENTRY* GetString)(GLenum);
#endif
};

extern GlApi gl;

#ifdef JADE_GL_TABLE_1_1
#define glPixelStorei ::jade::topaz::gl.PixelStorei
#define glTexImage2D ::jade::topaz::gl.TexImage2D
#define glTexSubImage2D ::jade::topaz::gl.TexSubImage2D
#define glBindTexture ::jade::topaz::gl.BindTexture
#define glTexParameteri ::jade::topaz::gl.TexParameteri
#define glGenTextures ::jade::topaz::gl.GenTextures
#define glDeleteTextures ::jade::topaz::gl.DeleteTextures
#define glEnable ::jade::topaz::gl.Enable
#define glDisable ::jade::topaz::gl.Disable
#define glClearColor ::jade::topaz::gl.ClearColor
#define glClear ::jade::topaz::gl.Clear
#define glViewport ::jade::topaz::gl.Viewport
#define glScissor ::jade::topaz::gl.Scissor
#define glReadPixels ::jade::topaz::gl.ReadPixels
#define glGetIntegerv ::jade::topaz::gl.GetIntegerv
#define glGetString ::jade::topaz::gl.GetString
#endif

// Fills `gl` through a function that finds an OpenGL function by name (the window system's
// own). False, with the name of the first one missing, if the driver lacks any.
bool loadGl(void* (*find)(const char* name), const char*& missing);

}  // namespace jade::topaz
