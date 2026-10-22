// bx (bgfx) #defines __stdcall to nothing when it sees clang with the MSVC runtime: harmless on x86-64, but on 32-bit x86 it makes every Windows API
// call use the wrong calling convention (undecorated import names, link errors). Force-included into the bx/bimg/bgfx targets of a 32-bit build:
// the keyword is restored right after bx's macros are read.
#pragma once
#include <bx/bx.h>
#ifdef __stdcall
#undef __stdcall
#endif
