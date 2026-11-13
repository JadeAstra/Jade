# Turns the standard library's WinterWolf sources into a C++ file holding their text, so that
# ww.exe carries them inside itself.
#   cmake -DOUT=<file.cpp> -DFILES=<a.ww|b.ww|...> -P embed.cmake
cmake_minimum_required(VERSION 3.20)

string(REPLACE "|" ";" files "${FILES}")
set(out "// Generated from src/std/*.ww by cmake/embed.cmake. Do not edit.\n#include <cstddef>\n// For the compiler alone: in a section of its own, which a built program leaves out (D161).\n#if defined(_MSC_VER)\n#pragma const_seg(\".wws\")\n#endif\nnamespace ww {\nextern const unsigned char kStdSource[];\nextern const size_t kStdSourceSize;\nextern const size_t kStdFileEnds[];\nextern const size_t kStdFileCount;\nconst unsigned char kStdSource[] = {\n")
# Where each file's text ends, so that the files can be parsed apart, a few at a time.
set(ends "")
set(offset 0)
list(LENGTH files count)
foreach(file ${files})
  file(READ "${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # A line end after each file, so that the last line of one cannot run into the next.
  string(APPEND out "${bytes}0x0a,\n")
  string(LENGTH "${hex}" digits)
  math(EXPR offset "${offset} + ${digits} / 2 + 1")
  string(APPEND ends "${offset},")
endforeach()
string(APPEND out "0x00};\nconst size_t kStdSourceSize = sizeof(kStdSource) - 1;\n")
string(APPEND out "const size_t kStdFileEnds[] = {${ends}};\nconst size_t kStdFileCount = ${count};\n}  // namespace ww\n")
file(WRITE "${OUT}" "${out}")
