#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// An asset's identity and its one text spelling, apart from the envelope and the text header that state it,
// so a file that only holds or checks a GUID reference does not compile either format.
namespace Common::Content
{
    // 128 bits, as UE's FGuid: an asset keeps this identity across renames and moves, so it is minted
    // once (Generate) and afterwards only ever copied. There is deliberately no constructor from a path.
    struct AssetGuid
    {
        uint64_t Hi = 0;
        uint64_t Lo = 0;

        static AssetGuid Generate();

        bool IsNull() const
        {
            return Hi == 0 && Lo == 0;
        }

        bool operator==( const AssetGuid& ) const = default;
    };

    // 32 lower-case hex digits, Hi then Lo. The inverse refuses anything else, naming what it got.
    std::string          AssetGuidToText( const AssetGuid& guid );
    ResultStr<AssetGuid> AssetGuidFromText( std::string_view text );
} // namespace Common::Content
