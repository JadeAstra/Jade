# Turns the libraries that come with WinterWolf (lib/, Jade) into a C++ file holding each file's
# path and bytes, so that ww.exe carries them inside itself (2026-11-18): a lone ww.exe runs and
# builds a program that imports Jade. Each file's bytes are kept as they are, and a hash of all of
# them names the folder ww writes them out to (src/command.cpp, builtInLibraries).
#   cmake -DOUT=<file.cpp> -DROOT=<lib folder> -DFILES=<Jade/a.ww|Jade/b.ww|...> -P embed_lib.cmake
cmake_minimum_required(VERSION 3.20)

string(REPLACE "|" ";" files "${FILES}")
list(SORT files)
set(out "// Generated from lib/ by cmake/embed_lib.cmake. Do not edit.\n#include <cstddef>\n// For the compiler alone: in a section of its own, which a built program leaves out (D161).\n#if defined(_MSC_VER)\n#pragma const_seg(\".wws\")\n#endif\nnamespace ww {\nextern const unsigned char kLibSource[];\nextern const size_t kLibFileEnds[];\nextern const char* const kLibFileNames[];\nextern const size_t kLibFileCount;\nextern const char kLibHash[];\nconst unsigned char kLibSource[] = {\n")
set(ends "")
set(names "")
set(hashes "")
set(offset 0)
list(LENGTH files count)
foreach(file ${files})
  file(READ "${ROOT}/${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(APPEND out "${bytes}\n")
  string(LENGTH "${hex}" digits)
  math(EXPR offset "${offset} + ${digits} / 2")
  string(APPEND ends "${offset},")
  string(APPEND names "\"${file}\",")
  file(SHA256 "${ROOT}/${file}" one)
  string(APPEND hashes "${file}:${one};")
endforeach()
string(SHA256 all "${hashes}")
string(APPEND out "0x00};\n")
string(APPEND out "const size_t kLibFileEnds[] = {${ends}};\nconst char* const kLibFileNames[] = {${names}};\nconst size_t kLibFileCount = ${count};\n")
string(APPEND out "const char kLibHash[] = \"${all}\";\n}  // namespace ww\n")
file(WRITE "${OUT}" "${out}")
