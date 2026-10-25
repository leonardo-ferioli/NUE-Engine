// 32-bit Windows x86 only. bx (bgfx) #defines __stdcall to nothing when it sees clang with the MSVC runtime: harmless on x86-64, but on 32-bit x86 it makes every
// Windows API call use the wrong calling convention (undecorated import names, link errors). Force-included (-include) into the bx/bimg/bgfx targets of a
// 32-bit build by cmake/NueGfx.cmake: the keyword is restored right after bx's macros are read.
// Same workaround as src/legacy/d3d9/dll/x86_stdcall_fix.h (legacy-d3d9 session), kept here so that NUE_WITH_GFX does not depend on that folder.
#pragma once
#include <bx/bx.h>
#ifdef __stdcall
#undef __stdcall
#endif
