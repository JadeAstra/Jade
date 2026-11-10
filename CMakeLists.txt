cmake_minimum_required(VERSION 3.20)
# The version goes up with each milestone of docs/gui-plan.html: M1 is 0.1.0, M2 is 0.2.0,
# M3.5 is 0.3.5, M4 is 0.4.0, M5 is 0.5.0, M6 is 0.6.0, M6.5 is 0.6.5, M6.6 is 0.6.6, M6.6.2 is
# 0.6.6.2, M6.7 is 0.6.7, M7 is 0.7.0, M8 is 0.8.0.
project(Jade VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C++ runtime into the executable: jade.exe, and the programs that
# `jade build --exe` makes from it, then run on machines without Visual Studio's runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The standard library is written in Jade (src/std/*.jade) and carried inside jade.exe.
file(GLOB JADE_STD CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/std/*.jade")
list(SORT JADE_STD)
string(REPLACE ";" "|" JADE_STD_ARG "${JADE_STD}")
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/std_source.cpp"
  COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/std_source.cpp" "-DFILES=${JADE_STD_ARG}"
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS ${JADE_STD} "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "Packing the standard library"
  VERBATIM)

# The executable in three parts (M6.5 part 5, D161): what every program needs to run, Topaz's
# window with the text engine and the image codecs, and the compiler side with the jade command.
# The last two are compiled into sections of their own (src/section_*.h), which `jade build`
# leaves out of a built program that does not need them.
add_library(jade_core OBJECT
  src/value.cpp
  src/bytecode.cpp
  src/vm.cpp
  src/builtins.cpp
  src/stdlib.cpp
  src/reflect.cpp
  src/threads.cpp
  src/io.cpp
  src/host.cpp
  src/sandbox.cpp
  src/registers.cpp
  src/charset.cpp
  src/grid.cpp
  src/regex.cpp
  src/regex_api.cpp
  src/xml_scan.cpp
  src/codec.cpp
  src/bits_api.cpp
  src/picture.cpp
  src/numeric.cpp
  src/tensor.cpp
  src/webp.cpp
  src/foreign.cpp
  src/worker.cpp
  src/parallel.cpp
  src/run.cpp
)
add_library(jade_window OBJECT
  src/topaz/platform_win.cpp
  src/topaz/gl.cpp
  src/topaz/render.cpp
  src/topaz/api.cpp
  src/text/face.cpp
  src/text/cff.cpp
  src/text/variation.cpp
  src/text/raster.cpp
  src/text/color.cpp
  src/text/bitmaps.cpp
  src/text/system.cpp
  src/text/fonts.cpp
  src/text/engine.cpp
  src/text/shapes.cpp
  src/text/api.cpp
  src/image/png.cpp
)
add_library(jade_compiler OBJECT
  "${CMAKE_BINARY_DIR}/std_source.cpp"
  src/lexer.cpp
  src/parser.cpp
  src/checker.cpp
  src/cache.cpp
  src/compiler.cpp
  src/command.cpp
  src/doc.cpp
)
# The instruction loop (VM::run) runs as fast whatever is linked before it: with Clang, the
# benchmarks' build, every jump target in vm.cpp starts on 32 bytes (and VM::run on 64, in
# vm.cpp). Without this, code growing elsewhere moved numeric by 14% (G42).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set_source_files_properties(src/vm.cpp PROPERTIES COMPILE_OPTIONS "-mllvm;-align-all-nofallthru-blocks=5")
endif()
add_executable(jade src/main.cpp $<TARGET_OBJECTS:jade_core> $<TARGET_OBJECTS:jade_window> $<TARGET_OBJECTS:jade_compiler>)

# The proof that what a built program keeps needs nothing left out of it: the core and the
# window linked without the compiler side, and the core alone, the window's two entry points
# (which the core calls only when the window's section is there) standing in. Either fails to
# link if anything kept names what is left out. Neither is shipped.
add_executable(jade_proof src/proof_main.cpp $<TARGET_OBJECTS:jade_core> $<TARGET_OBJECTS:jade_window>)
add_executable(jade_proof_plain src/proof_main.cpp src/window_absent.cpp $<TARGET_OBJECTS:jade_core>)
foreach(proof jade_proof jade_proof_plain)
  set_target_properties(${proof} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/proof")
endforeach()
foreach(part jade jade_core jade_window jade_compiler jade_proof jade_proof_plain)
  target_compile_definitions(${part} PRIVATE JADE_VERSION="${PROJECT_VERSION}")
endforeach()
if(WIN32)
  foreach(linked jade jade_proof jade_proof_plain)
    target_link_libraries(${linked} PRIVATE opengl32 ws2_32 shell32 ole32)
  endforeach()
  # __HrLoadAllImportsForDll, which the native layer calls whether or not the program
  # delay-loads OpenGL (only jade does, below).
  target_link_libraries(jade_proof PRIVATE delayimp)
endif()

# jade looks for libraries such as Topaz only in the lib folder beside itself, so every
# build puts a copy of lib/ next to the executable.
add_custom_target(jade_lib ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lib" "$<TARGET_FILE_DIR:jade>/lib"
  COMMENT "Copying lib next to jade"
  VERBATIM)

if(MSVC)
  foreach(part jade jade_core jade_window jade_compiler jade_proof jade_proof_plain)
    target_compile_options(${part} PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(${part} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
  # The parser and compiler recurse over nested source; give them room.
  target_link_options(jade PRIVATE /STACK:16777216)
  # OpenGL and the shell are loaded when first used, not when jade starts: loading
  # opengl32.dll costs about 57 ms, which every script paid, window or not. A delay-loaded DLL
  # whose functions take float arguments is loaded whole before any is called
  # (__HrLoadAllImportsForDll): lld-link's stub for a first call can lose the second float
  # argument (src/topaz/platform_win.cpp, WinPlatform::start).
  target_link_libraries(jade PRIVATE delayimp)
  target_link_options(jade PRIVATE /DELAYLOAD:opengl32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:ole32.dll)
else()
  foreach(part jade jade_core jade_window jade_compiler jade_proof jade_proof_plain)
    target_compile_options(${part} PRIVATE -Wall -Wextra)
  endforeach()
endif()

# The check of what a built program leaves out (D161): nothing of it runs as the program starts,
# and every function the proofs need is in jade.exe in a section a built program keeps.
if(MSVC)
  foreach(linked jade jade_proof jade_proof_plain)
    target_link_options(${linked} PRIVATE "/MAP:$<TARGET_FILE_DIR:${linked}>/${linked}.map")
  endforeach()
  add_custom_target(jade_check_sections ALL
    COMMAND $<TARGET_FILE:jade> --check-sections "$<TARGET_FILE_DIR:jade>/jade.map"
            "$<TARGET_FILE_DIR:jade_proof>/jade_proof.map" "$<TARGET_FILE_DIR:jade_proof_plain>/jade_proof_plain.map"
    DEPENDS jade jade_proof jade_proof_plain
    COMMENT "Checking what a built program leaves out"
    VERBATIM)
endif()

# The standard library, compiled once as the build's last step and attached to jade.exe, so
# that no script compiles it or checks it again (D145). Off, jade compiles it at each start.
option(JADE_ATTACH_PRELUDE "Attach the compiled standard library to jade.exe" ON)
if(JADE_ATTACH_PRELUDE)
  add_custom_command(TARGET jade POST_BUILD
    COMMAND $<TARGET_FILE:jade> --attach-prelude $<TARGET_FILE_DIR:jade>/jade-attached.exe
    COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE_DIR:jade>/jade-attached.exe $<TARGET_FILE:jade>
    COMMENT "Attaching the compiled standard library"
    VERBATIM)
endif()
