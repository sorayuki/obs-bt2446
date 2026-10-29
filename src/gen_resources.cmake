# Embed the effect into a C source, using nothing but CMake itself.
#
# Some hosts load plugins from <host>/plugins and only
# resolve module data under <host>/data/plugins, so a data directory next to the
# DLL is not guaranteed to exist and the effect has to ship inside the module.
#
# Invoked as a script, so the build needs no extra toolchain (no Python, no
# PowerShell, no shell); `${CMAKE_COMMAND} -P` is enough on every platform:
#
#   cmake -DEFFECT=<file.effect> -DOUTPUT=<header.h> -P gen_resources.cmake
#
# The bytes are emitted as a hex array, which sidesteps C string escaping
# entirely: quotes, backslashes, tabs and non-ASCII all survive verbatim. Every
# real byte of the file is kept, newlines included, so shader compiler errors
# still report the line numbers of data/bt2446.effect.

if(NOT DEFINED EFFECT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "usage: cmake -DEFFECT=<file> -DOUTPUT=<header> -P gen_resources.cmake")
endif()

if(NOT EXISTS "${EFFECT}")
  message(FATAL_ERROR "gen_resources: input not found: ${EFFECT}")
endif()

file(READ "${EFFECT}" _text)

# Normalize line endings so the generated array does not depend on how the file
# was checked out (git autocrlf) and the effect parser only ever sees "\n".
string(REPLACE "\r\n" "\n" _text "${_text}")
string(REPLACE "\r" "\n" _text "${_text}")

string(HEX "${_text}" _hex)
string(TOLOWER "${_hex}" _hex)

# One source line per 16 bytes, then prefix every byte with 0x.
string(REGEX REPLACE "([0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f])" "\\1\n" _hex "${_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _hex "${_hex}")
string(REGEX REPLACE "\n" "\n\t" _hex "${_hex}")

get_filename_component(_name "${EFFECT}" NAME)

set(_header "/* Generated from ${_name} by src/gen_resources.cmake - do not edit. */\n\nstatic const unsigned char BT2446_EFFECT_SOURCE[] = {\n\t${_hex}\n\t0x00, /* NUL terminator */\n};\n")

file(WRITE "${OUTPUT}" "${_header}")
