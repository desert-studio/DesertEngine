#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace Common::Content
{
    // THE COOKED MESH'S 64-BYTE HEADER, DESCRIBED ONCE. Two readers look at it: the engine's mesh loader
    // (MeshBinary.cpp), which reads the whole file, and the content scan (ContentScan.cpp), which reads
    // ONLY these 64 bytes to learn a mesh's box without its body. They sit in different libraries — the
    // scan runs in the packager and the tools, which do not link the engine — so the layout lives here,
    // below both, and neither can drift from the other.
    //
    // Plain fixed-width members and nothing else, pinned by static_asserts, because these sizes ARE the
    // file format: a record that quietly grows by four bytes on one compiler reads every following record
    // shifted, and a mesh made of shifted floats is a mesh that draws — wrongly, silently, and only on the
    // platform that did not write the file.
    struct MeshBinaryFileHeader
    {
        char     Magic[8];
        uint32_t ByteOrder;
        uint32_t Version;
        uint64_t FileSize; // declared; compared against the bytes actually in hand
        uint32_t SectionCount;
        uint32_t Flags;
        uint64_t SkeletonSignature;
        // The mesh's box around its own origin (the union of its submesh boxes), meaningful only under
        // kMeshFlagHasBounds. These 24 bytes were reserved and zero before the flag existed, and no reader
        // ever checked them, so a file written before the flag reads as "states no box", never as a box
        // at the origin — which is why the flag, and not the numbers, is what says a box is present.
        float BoundsMin[3];
        float BoundsMax[3];
    };
    static_assert( sizeof( MeshBinaryFileHeader ) == 64 );
    static_assert( alignof( MeshBinaryFileHeader ) == 8 );

    /// Version 2 (M4) appends the PolyGroups section; see MeshBinary.hpp for the history. The bounds flag
    /// is NOT a version: an older reader ignores an unknown flag bit and a newer one reads an older file
    /// as "states no box", so both directions keep loading.
    inline constexpr uint32_t kMeshBinaryVersion = 2;

    /// "DESTMESH". Eight ASCII bytes, so the sequence on disk is the same whatever the host's word
    /// order — a magic written as an integer would itself need a byte-order rule to be read.
    inline constexpr char kMeshBinaryMagic[8] = { 'D', 'E', 'S', 'T', 'M', 'E', 'S', 'H' };

    /// Reads back as 0x01020304 on a big-endian host, which is the whole point of writing it.
    inline constexpr uint32_t kMeshBinaryByteOrderTag = 0x04030201u;

    inline constexpr uint32_t kMeshFlagIsSkinned            = 1u << 0;
    inline constexpr uint32_t kMeshFlagHasSkeletonSignature = 1u << 1;
    // The header states the mesh's box. A mesh with no submeshes states it too — as an inverted box
    // (min > max) — so "this mesh has no extent" and "this file predates the statement" stay different
    // answers, and only the second one sends a reader to the body.
    inline constexpr uint32_t kMeshFlagHasBounds = 1u << 2;

    /// What a header says about the mesh's box.
    struct MeshHeaderBounds
    {
        bool                              Stated = false; // kMeshFlagHasBounds was set
        std::optional<Common::Math::AABB> Bounds;         // nullopt under Stated: the mesh has no extent
    };

    /// Writes @p bounds (nullopt = no extent) into @p header and sets the flag.
    inline void StateMeshBounds( MeshBinaryFileHeader& header, const std::optional<Common::Math::AABB>& bounds )
    {
        header.Flags |= kMeshFlagHasBounds;
        for ( int axis = 0; axis < 3; ++axis )
        {
            header.BoundsMin[axis] = bounds ? bounds->Min[axis] : std::numeric_limits<float>::max();
            header.BoundsMax[axis] = bounds ? bounds->Max[axis] : std::numeric_limits<float>::lowest();
        }
    }

    /// The box @p bytes' header states, or std::nullopt when @p bytes is not a cooked-mesh header this
    /// host can read (too short, other magic, other byte order) — the loader names those; this only
    /// declines to invent a box from them.
    [[nodiscard]] inline std::optional<MeshHeaderBounds> ReadMeshHeaderBounds( const std::string_view bytes )
    {
        if ( bytes.size() < sizeof( MeshBinaryFileHeader ) )
            return std::nullopt;
        MeshBinaryFileHeader header{};
        std::memcpy( &header, bytes.data(), sizeof( header ) );
        if ( std::memcmp( header.Magic, kMeshBinaryMagic, sizeof( header.Magic ) ) != 0 ||
             header.ByteOrder != kMeshBinaryByteOrderTag )
            return std::nullopt;

        MeshHeaderBounds result;
        result.Stated = ( header.Flags & kMeshFlagHasBounds ) != 0;
        if ( result.Stated && header.BoundsMin[0] <= header.BoundsMax[0] &&
             header.BoundsMin[1] <= header.BoundsMax[1] && header.BoundsMin[2] <= header.BoundsMax[2] )
        {
            Common::Math::AABB box;
            box.Min       = { header.BoundsMin[0], header.BoundsMin[1], header.BoundsMin[2] };
            box.Max       = { header.BoundsMax[0], header.BoundsMax[1], header.BoundsMax[2] };
            result.Bounds = box;
        }
        return result;
    }
} // namespace Common::Content
