#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Editor::ThumbnailSlots
{
    /// ONE MESH SLOT'S MATERIAL FOR A PICTURE, by the rule the scene draws that slot by.
    ///
    /// Reference 0 is an UNASSIGNED slot, not a broken one: the scene draws it with the engine's default
    /// surface material (MeshECSSystem substitutes it for a null slot handle), as UE draws an empty slot
    /// with UMaterial::GetDefaultMaterial — so the picture stages a null handle and gets the same default.
    /// A reference no registered material answers to is a BROKEN link: the scene would substitute the
    /// default for a material the mesh DOES name, so the picture is refused with the slot's index.
    /// `resolve` maps the mesh's external reference to the registered material handle (null = none).
    template <typename Resolve>
    [[nodiscard]] Common::ResultStr<Common::AssetHandle> SlotMaterial( const Common::UUID& reference,
                                                                       std::size_t index, Resolve&& resolve )
    {
        if ( reference.IsNull() )
            return Common::MakeSuccess( Common::AssetHandle( static_cast<uint64_t>( 0 ) ) );
        const Common::AssetHandle internal = resolve( reference );
        if ( internal.IsNull() )
            return Common::MakeFormattedError<Common::AssetHandle>(
                 "slot {} names material {}, and no registered material answers to it — the scene would draw "
                 "that slot with its default material, which is not this mesh's look",
                 index, reference.ToString() );
        return Common::MakeSuccess( internal );
    }
} // namespace Desert::Editor::ThumbnailSlots
