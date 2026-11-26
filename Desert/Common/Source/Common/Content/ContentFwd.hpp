#pragma once

#include <cstddef>

// The content types by name only (as <iosfwd> is to the streams): for a header that declares a function over
// them and must not compile the envelope or the kind census. ContentKind's enumerators are ContentKinds.hpp's;
// the envelope's structs are AssetEnvelope.hpp's.
namespace Common::Content
{
    enum class ContentKind : std::size_t;

    struct AssetHeader;
    struct AssetHeaderReadContext;
    class IAssetHeaderFormat;
} // namespace Common::Content
