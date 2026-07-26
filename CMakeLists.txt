# WiCAN J2534 PassThru DLL.
#
# 32-bit is the safe default — most OEM J2534 apps load a 32-bit DLL.
#   MSVC:  cmake -A Win32 -B build && cmake --build build --config Release
#   MinGW: cmake -G "MinGW Makefiles" -DCMAKE_C_FLAGS=-m32 -B build && cmake --build build
#
# Cross-compile from Linux (syntax/link check):
#   x86_64-w64-mingw32-gcc -shared -o wican_j2534.dll wican_j2534.c \
#       wican_j2534.def -lws2_32
cmake_minimum_required(VERSION 3.15)
project(wican_j2534 C)

add_library(wican_j2534 SHARED wican_j2534.c wican_j2534.def)
target_link_libraries(wican_j2534 ws2_32)
set_target_properties(wican_j2534 PROPERTIES PREFIX "" OUTPUT_NAME "wican_j2534")

# J2534 apps import UNDECORATED names (PassThruOpen, not _PassThruOpen@8).
# MSVC honours the .def; MinGW needs --kill-at to strip the @N stdcall
# decoration (verified: all 14 exports undecorated).
if(MINGW)
    target_link_options(wican_j2534 PRIVATE
        -Wl,--enable-stdcall-fixup -Wl,--kill-at)
endif()
