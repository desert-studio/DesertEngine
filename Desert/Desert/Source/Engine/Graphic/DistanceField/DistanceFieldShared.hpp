#pragma once

// The distance-field constants the shaders read (Shaders/Common/DistanceFieldShared.glslh), as C++ constants in
// ONE namespace. The .glslh has an include guard, so a second `namespace X { #include }` in the same translation
// unit (a unity batch) declares nothing: every C++ user includes this header and names DistanceField::DFShared.
namespace Desert::Graphic::DistanceField::DFShared
{
#include <Common/DistanceFieldShared.glslh>
} // namespace Desert::Graphic::DistanceField::DFShared
