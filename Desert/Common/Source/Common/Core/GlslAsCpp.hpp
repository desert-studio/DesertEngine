#pragma once

// Brackets an `#include` of shader text (`.glslh`) that is being compiled AS C++.
//
// WHY IT HAS TO EXIST. The house arrangement for shader maths is that one file is compiled twice — as
// GLSL by the shaders, and as C++ by a test reference — so a passing test is a statement about the code
// the GPU runs rather than about a hand-written copy of it. GLSL has no `inline`, so the reference
// headers include the shared text inside an ANONYMOUS namespace to give each translation unit its own
// copy. That gives every function in the file internal linkage, and `-Wunused-function` then reports
// each one the test does not happen to call — while the shader sharing the text calls it every frame.
// Fourteen of `SkyMedium.glslh`'s functions report this way, eight of `CloudGeometry.glslh`'s, five of
// `BRDF.glslh`'s.
//
// WHAT IT DOES NOT COVER, deliberately. The suppression brackets the include and nothing else, so a
// genuinely dead C++ helper written in the reference header itself still reports normally. And it says
// nothing about whether the shader side uses the function: that question is answered by grepping the
// shader tree, and doing so is what found `GeometrySchlickGGX` and `CalculateAttenuation` — two
// superseded functions that no shader called either, now deleted rather than bracketed.
//
// This is the ONLY warning suppression in the repository, and it is a suppression of a false positive
// created by a build arrangement, not of a diagnostic we would rather not answer.

#if defined( __clang__ ) || defined( __GNUC__ )
#define DESERT_GLSL_AS_CPP_BEGIN                                                                                  \
    _Pragma( "GCC diagnostic push" ) _Pragma( "GCC diagnostic ignored \"-Wunused-function\"" )
#define DESERT_GLSL_AS_CPP_END _Pragma( "GCC diagnostic pop" )
#elif defined( _MSC_VER )
// C4505: unreferenced function with internal linkage has been removed — MSVC's spelling of the same
// diagnostic, and it is a /W4 warning, which is the level this workspace now builds at.
#define DESERT_GLSL_AS_CPP_BEGIN _Pragma( "warning( push )" ) _Pragma( "warning( disable : 4505 )" )
#define DESERT_GLSL_AS_CPP_END _Pragma( "warning( pop )" )
#else
#define DESERT_GLSL_AS_CPP_BEGIN
#define DESERT_GLSL_AS_CPP_END
#endif
