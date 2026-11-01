# Embed one GLSL file as a C string:  cmake -DIN=<file.glsl> -DOUT=<file.c> -P stringify.cmake
#
# HZM [2026-11-01] Replaces the compiled code/tools/stringify.cpp helper. That helper is built with the TARGET
# compiler and then run on the build machine, which fails whenever the two differ: the Linux CI cross-compiles
# linux-amd64 on an ARM runner ("Exec format error"), so renderergl2 could not be built for Linux at all.
# A CMake script runs on any host. Output matches the old helper on Windows byte for byte:
#   const char *fallbackShader_<name> =
#   "<line>\n"      (one per source line, CR stripped)
#   ;
# Unlike the old helper it also escapes backslashes and double quotes (no shader has either today).

if(NOT IN OR NOT OUT)
	message(FATAL_ERROR "stringify.cmake: pass -DIN=<file.glsl> -DOUT=<file.c>")
endif()

get_filename_component(base "${IN}" NAME_WE)
file(READ "${IN}" text)

string(REPLACE "\r" "" text "${text}")
string(REPLACE "\\" "\\\\" text "${text}")
string(REPLACE "\"" "\\\"" text "${text}")
# getline semantics: a final newline does not start another (empty) line
string(REGEX REPLACE "\n$" "" text "${text}")
string(REPLACE "\n" "\\n\"\n\"" text "${text}")

file(WRITE "${OUT}" "const char *fallbackShader_${base} = \n\"${text}\\n\"\n;\n")
