// OpenGL's later functions, fetched by name: see gl.h. And the names of the keys, which
// like this are the same on every system: see platform.h.

#include "gl.h"

#include <string>

#include "platform.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww::jade {

GlApi gl = {};

bool loadGl(void* (*find)(const char* name), const char*& missing) {
    bool complete = true;
    auto fetch = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(find(name));
        if (slot == nullptr && complete) {
            complete = false;
            missing = name;
        }
    };
    fetch(gl.CreateShader, "glCreateShader");
    fetch(gl.ShaderSource, "glShaderSource");
    fetch(gl.CompileShader, "glCompileShader");
    fetch(gl.GetShaderiv, "glGetShaderiv");
    fetch(gl.GetShaderInfoLog, "glGetShaderInfoLog");
    fetch(gl.CreateProgram, "glCreateProgram");
    fetch(gl.AttachShader, "glAttachShader");
    fetch(gl.LinkProgram, "glLinkProgram");
    fetch(gl.GetProgramiv, "glGetProgramiv");
    fetch(gl.UseProgram, "glUseProgram");
    fetch(gl.GetUniformLocation, "glGetUniformLocation");
    fetch(gl.Uniform1i, "glUniform1i");
    fetch(gl.Uniform1f, "glUniform1f");
    fetch(gl.Uniform2f, "glUniform2f");
    fetch(gl.GenVertexArrays, "glGenVertexArrays");
    fetch(gl.BindVertexArray, "glBindVertexArray");
    fetch(gl.GenBuffers, "glGenBuffers");
    fetch(gl.BindBuffer, "glBindBuffer");
    fetch(gl.BufferData, "glBufferData");
    fetch(gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
    fetch(gl.VertexAttribPointer, "glVertexAttribPointer");
    fetch(gl.VertexAttribDivisor, "glVertexAttribDivisor");
    fetch(gl.DrawArraysInstanced, "glDrawArraysInstanced");
    fetch(gl.ActiveTexture, "glActiveTexture");
    fetch(gl.GenFramebuffers, "glGenFramebuffers");
    fetch(gl.BindFramebuffer, "glBindFramebuffer");
    fetch(gl.FramebufferTexture2D, "glFramebufferTexture2D");
    fetch(gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
    fetch(gl.DeleteFramebuffers, "glDeleteFramebuffers");
    fetch(gl.BlendFuncSeparate, "glBlendFuncSeparate");
#ifdef WW_GL_TABLE_1_1
    fetch(gl.PixelStorei, "glPixelStorei");
    fetch(gl.TexImage2D, "glTexImage2D");
    fetch(gl.TexSubImage2D, "glTexSubImage2D");
    fetch(gl.BindTexture, "glBindTexture");
    fetch(gl.TexParameteri, "glTexParameteri");
    fetch(gl.GenTextures, "glGenTextures");
    fetch(gl.DeleteTextures, "glDeleteTextures");
    fetch(gl.Enable, "glEnable");
    fetch(gl.Disable, "glDisable");
    fetch(gl.ClearColor, "glClearColor");
    fetch(gl.Clear, "glClear");
    fetch(gl.Viewport, "glViewport");
    fetch(gl.Scissor, "glScissor");
    fetch(gl.ReadPixels, "glReadPixels");
    fetch(gl.GetIntegerv, "glGetIntegerv");
    fetch(gl.GetString, "glGetString");
#endif
    return complete;
}

namespace {

// In the order of Key.
const char* const kKeyNames[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "digit0", "digit1", "digit2", "digit3", "digit4", "digit5", "digit6", "digit7", "digit8", "digit9",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21",
    "f22", "f23", "f24", "f25",
    "space", "apostrophe", "comma", "minus", "period", "slash", "semicolon", "equal", "leftBracket", "backslash", "rightBracket", "graveAccent",
    "escape", "enter", "tab", "backspace", "insert", "delete", "right", "left", "down", "up", "pageUp", "pageDown", "home", "end",
    "capsLock", "scrollLock", "numLock", "printScreen", "pause", "menu",
    "numpad0", "numpad1", "numpad2", "numpad3", "numpad4", "numpad5", "numpad6", "numpad7", "numpad8", "numpad9",
    "numpadDecimal", "numpadDivide", "numpadMultiply", "numpadSubtract", "numpadAdd", "numpadEnter", "numpadEqual",
    "shiftLeft", "controlLeft", "altLeft", "metaLeft", "shiftRight", "controlRight", "altRight", "metaRight",
    "unknown",
};
const int kKeyCount = static_cast<int>(sizeof kKeyNames / sizeof kKeyNames[0]);
static_assert(sizeof kKeyNames / sizeof kKeyNames[0] == static_cast<size_t>(Key::Unknown) + 1, "a name for every key");

}  // namespace

const char* keyName(Key key) {
    const int index = static_cast<int>(key);
    return index >= 0 && index < kKeyCount ? kKeyNames[index] : "unknown";
}

Key keyNamed(const std::string& name) {
    for (int i = 0; i < kKeyCount; i++)
        if (name == kKeyNames[i]) return static_cast<Key>(i);
    return Key::Unknown;
}

}  // namespace ww::jade
