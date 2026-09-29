#pragma once

#include <Engine/Animation/Skeleton.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief The Animation Editor's Skeleton Tree (UE Persona's SSkeletonTree), as data: which bones are rows,
     * in which order, how deep, and which bone a name selects.
     *
     * Split out of the window so the rules are tested without a device: the window only draws these rows.
     * Rows are depth-first with children in bone-index order, so the tree reads like the file.
     *
     * A FILTER SHOWS MATCHES AND THEIR ANCESTORS, and ignores collapse: UE's search keeps the path to every hit
     * visible, because a bare "lowerarm_l" row without its chain says nothing about where the bone is.
     */
    struct SkeletonTreeRow
    {
        uint32_t Bone        = 0;
        uint32_t Depth       = 0;
        bool     HasChildren = false;
        bool     Matches     = true; // false: shown only as an ancestor of a filter match
    };

    [[nodiscard]] inline bool BoneNameMatches( std::string_view name, std::string_view filter )
    {
        if ( filter.empty() )
            return true;
        const auto lower = []( const char c )
        { return static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) ); };
        const auto* const it =
             std::search( name.begin(), name.end(), filter.begin(), filter.end(),
                          [&]( const char a, const char b ) { return lower( a ) == lower( b ); } );
        return it != name.end();
    }

    // `collapsed` is indexed by bone; a missing entry is expanded.
    [[nodiscard]] inline std::vector<SkeletonTreeRow> BuildSkeletonTreeRows( const Animation::Skeleton& skeleton,
                                                                             std::string_view           filter,
                                                                             const std::vector<bool>&   collapsed )
    {
        const auto&                        bones = skeleton.GetBones();
        const auto                         count = static_cast<uint32_t>( bones.size() );
        std::vector<std::vector<uint32_t>> children( count );
        std::vector<uint32_t>              roots;
        for ( uint32_t i = 0; i < count; ++i )
        {
            const uint32_t parent = skeleton.ResolveParent( i );
            if ( parent < count )
                children[parent].push_back( i );
            else
                roots.push_back( i );
        }

        // Visible under a filter: the bone matches, or one of its descendants does.
        std::vector<bool> keep( count, filter.empty() );
        if ( !filter.empty() )
            for ( uint32_t i = 0; i < count; ++i )
                if ( BoneNameMatches( bones[i].Name, filter ) )
                    for ( uint32_t b = i; b < count && !keep[b]; b = skeleton.ResolveParent( b ) )
                        keep[b] = true;

        std::vector<SkeletonTreeRow>               rows;
        std::vector<std::pair<uint32_t, uint32_t>> stack; // (bone, depth)
        for ( const uint32_t root : std::views::reverse( roots ) )
            stack.emplace_back( root, 0u );
        while ( !stack.empty() )
        {
            const auto [bone, depth] = stack.back();
            stack.pop_back();
            if ( !keep[bone] )
                continue;
            rows.push_back(
                 { bone, depth, !children[bone].empty(), BoneNameMatches( bones[bone].Name, filter ) } );
            const bool open = !filter.empty() || bone >= collapsed.size() || !collapsed[bone];
            if ( !open )
                continue;
            for ( const uint32_t child : std::views::reverse( children[bone] ) )
                stack.emplace_back( child, depth + 1 );
        }
        return rows;
    }

    // The bone a palette "Select Bone <name>" means: an exact name, never a nearest match.
    [[nodiscard]] inline std::optional<uint32_t> BoneByName( const Animation::Skeleton& skeleton,
                                                             const std::string&         name )
    {
        return skeleton.FindBoneIndex( name );
    }

    // UE's Details "Bone" / "Reference" rows: a local transform as Location / Rotation (degrees, pitch-yaw-roll
    // Euler of the quaternion) / Scale.
    struct BoneTransformRows
    {
        glm::vec3 Location{ 0.0f };
        glm::vec3 RotationDegrees{ 0.0f };
        glm::vec3 Scale{ 1.0f };
    };

    [[nodiscard]] inline BoneTransformRows DecomposeBoneTransform( const glm::mat4& local )
    {
        glm::vec3 scale;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::quat rotation;
        if ( !glm::decompose( local, scale, rotation, translation, skew, perspective ) )
            return {};
        return { translation, glm::degrees( glm::eulerAngles( rotation ) ), scale };
    }
} // namespace Desert::Editor
