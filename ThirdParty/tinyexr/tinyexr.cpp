// The ONE implementation TU of tinyexr — syoyo/tinyexr tag v1.0.13, BSD-3 (LICENSE.txt beside this file).
// Vendored unmodified from that tag, sha256:
//   include/tinyexr/tinyexr.h       56e041e8ca7e76748d1730790b11c5f5daa5fe03c18cf371ded1e672bec49dbb
//   include/tinyexr/exr_reader.hh   9e0bfef0487b9c3082165b2a637c75ef91d9fcc4aa55a5f32eb97f77db19f99b
//   include/tinyexr/streamreader.hh 81f2d1502930ca9ca9957e7607c2a7924d3ef2fe4b08b1a4001245dc6d3fefb6
//
// Its deflate goes through stb (stbi_zlib_decode_buffer / stbi_zlib_compress), which every binary that
// compiles this file already links through ThirdParty/stb/stb_image.cpp — so no miniz is vendored.
// Every project that compiles this TU must also link stb_image.cpp.
#define TINYEXR_USE_MINIZ    0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD   0
#define TINYEXR_USE_OPENMP   0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr/tinyexr.h"
