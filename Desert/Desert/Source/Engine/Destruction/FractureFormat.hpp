#pragma once

// THE `.dfrac` FILE: a baked fracture (FractureBake.hpp) inside the asset envelope (Common/Content/
// AssetEnvelope.hpp, Kind Fracture, one subsystem version under 'DFRC'), the way a cloud modelling volume
// sits in its envelope (CloudModellingVolume.cpp). The envelope carries the identity (GUID); the payload
// carries everything else, little-endian, with no padding:
//
//   source mesh GUID (Hi, Lo u64), settings (seed, levels, auto-cluster, interior UV scale),
//   interior material ID, interior material GUID (v2), node count, then per node: parent, level, kind, damage
//   threshold, volume, centre of mass, the leaf mesh in the saved mesh form (SavedMeshForm.hpp, the scene's own
//   form), and the hull (vertices, polygons).
//
// The settings are stored so a re-bake reproduces the file: the bake is deterministic by seed. A reader of
// another version, a truncated payload or trailing bytes is refused by name; nothing is guessed.

#include <Engine/Destruction/FractureBake.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace Desert::Destruction
{
    inline constexpr uint32_t    kFractureSubsystemTag  = Common::Content::FourCC( "DFRC" );
    inline constexpr uint32_t    kFractureFormatVersion = 2; // 2: the interior material GUID
    inline constexpr const char* kFractureExtension     = ".dfrac";

    struct FractureData
    {
        Common::Content::AssetGuid Guid;       // the envelope's identity
        Common::Content::AssetGuid SourceMesh; // the static mesh the bake cut
        FractureSettings           Settings;
        int32_t                    InteriorMaterialId = 0;
        /// UE's Fracture Mode "Internal Material": the material the interior slot (InteriorMaterialId) draws
        /// with. Null until one is picked in the Fracture mode; the piece draw (DST-06) binds it to that slot.
        Common::Content::AssetGuid InteriorMaterial;
        std::vector<FractureNode>  Nodes;

        bool operator==( const FractureData& ) const = default;
    };

    /// The payload bytes alone (no envelope, no GUID): what determinism is checked against.
    [[nodiscard]] std::vector<unsigned char>      EncodeFracturePayload( const FractureData& data );
    [[nodiscard]] Common::ResultStr<FractureData> DecodeFracturePayload( const std::vector<unsigned char>& bytes );

    /// The whole `.dfrac` file. Refuses a null GUID: the file IS the asset's identity.
    [[nodiscard]] Common::ResultStr<std::vector<unsigned char>> EncodeFracture( const FractureData& data );
    [[nodiscard]] Common::ResultStr<FractureData> DecodeFracture( const std::vector<unsigned char>& file );

    /// The GUID from the envelope header alone.
    [[nodiscard]] Common::ResultStr<Common::Content::AssetGuid> ReadFractureGuid( std::istream& in );
} // namespace Desert::Destruction
