#pragma once

// THE SURFACE CACHE'S GPU RECORDS, declared once: a card, a card's page in the atlas, a mesh instance's cards, an
// instance table row. Tools/ShaderProtocolGen writes them as Generated/GI/SurfaceCache.glslh, which
// Common/SurfaceCache.glslh includes.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::SurfaceCache
{
    // An instance table row whose instance has no cards this frame (SurfaceCache.hpp, the instance table).
    inline constexpr uint32_t kNoMeshCards = 0xFFFFFFFFu; // Common/GITracing.glslh GI_NO_MESH_CARDS
    inline constexpr uint32_t kNoInstance  = 0xFFFFFFFFu; // Common/GITracing.glslh GI_NO_INSTANCE

    // World -> card: rows of the rotation in xyz, the card origin (world, cm) in w of the three rows.
    using CardWorldToLocalRows = std::array<glm::vec4, 3>;
    // Per CardDirection, the mask of a mesh's cards facing it: [0].xyzw = directions 0..3, [1].xy = 4..5.
    using MeshCardLookupRows = std::array<glm::uvec4, 2>;

    // One card of the scene (UE FLumenCardData, LUMEN_CARD_DATA_STRIDE = 7 float4).
#define DESERT_CARD_PAYLOAD_FIELDS( X )                                                                           \
    X( CardWorldToLocalRows, WorldToLocal, "worldToLocal", {} )                                                   \
    /* xyz = half size in card space (z = depth range); w = texel size (cm) */                                    \
    X( glm::vec4, LocalExtent, "localExtent", glm::vec4( 0.0f ) )                                                 \
    /* x = SizeInPages (x | y << 16), y = PageTableOffset, z = HiResSizeInPages (x | y << 16),                    \
       w = HiResPageTableOffset - the always-resident pages and the streamed high-resolution ones */              \
    X( glm::uvec4, Pages, "pages", glm::uvec4( 0u ) )                                                             \
    /* x = packed: ResLevelToResLevelXYBias x (bits 0-7), y (8-15), direction (16-19), lighting channels (20-23), \
       visible (24), heightfield (25), far field (26) - UE Packed0W, same bits; y = index of the MeshCardsPayload \
       it belongs to; z, w = 0 */                                                                                 \
    X( glm::uvec4, Info, "info", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( CardPayload, "SurfaceCard", Std430, DESERT_CARD_PAYLOAD_FIELDS );
#undef DESERT_CARD_PAYLOAD_FIELDS

    // One page of a card mapped into the atlas (UE FLumenPageTableEntry / LUMEN_CARD_PAGE_DATA_STRIDE).
#define DESERT_CARD_PAGE_PAYLOAD_FIELDS( X )                                                                      \
    /* x = card index, y = mapped (0/1), z = 1 when the card is captured this frame (its lit layers are not its   \
       own yet), w = 0 */                                                                                         \
    X( glm::uvec4, Card, "card", glm::uvec4( 0u ) )                                                               \
    X( glm::vec4, CardUVRect, "cardUVRect", glm::vec4( 0.0f ) )                   /* the page in card UV */       \
    X( glm::vec4, PhysicalAtlasUVRect, "physicalAtlasUVRect", glm::vec4( 0.0f ) ) /* the page in atlas UV */      \
    /* xy = CardUVTexelScale, zw = PhysicalAtlasUVTexelScale */                                                   \
    X( glm::vec4, TexelScale, "texelScale", glm::vec4( 0.0f ) )                                                   \
    /* x = frame of the last direct-lighting update, y = of the last radiosity update, z = radiosity temporal     \
       index, w = direct-lighting temporal index (UE Last*UpdateFrameIndex) */                                    \
    X( glm::uvec4, Updates, "updates", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( CardPagePayload, "SurfaceCardPage", Std430, DESERT_CARD_PAGE_PAYLOAD_FIELDS );
#undef DESERT_CARD_PAGE_PAYLOAD_FIELDS

    // One mesh instance's cards (UE FLumenMeshCardsData, LUMEN_MESH_CARDS_DATA_STRIDE = 6 float4).
#define DESERT_MESH_CARDS_PAYLOAD_FIELDS( X )                                                                     \
    X( CardWorldToLocalRows, WorldToLocal, "worldToLocal", {} ) /* as CardPayload */                              \
    /* x = first CardPayload, y = card count, z = flags (bit 0 heightfield, bit 1 mostly two-sided), w = 0 */     \
    X( glm::uvec4, Cards, "cards", glm::uvec4( 0u ) )                                                             \
    X( MeshCardLookupRows, CardLookup, "cardLookup", {} )                                                         \
    /* rgb = what a hit OUTSIDE card coverage is shaded with (SurfaceCache.hpp): the mean base colour of the      \
       mesh's materials when the instance is registered, replaced by the mean captured card albedo after its      \
       first capture; a = 0 */                                                                                    \
    X( glm::vec4, FallbackAlbedo, "fallbackAlbedo", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( MeshCardsPayload, "MeshCards", Std430, DESERT_MESH_CARDS_PAYLOAD_FIELDS );
#undef DESERT_MESH_CARDS_PAYLOAD_FIELDS

    // One instance table row (Common/SurfaceCacheSampling.glslh `surfaceCacheInstances`).
#define DESERT_INSTANCE_ROW_PAYLOAD_FIELDS( X )                                                                   \
    /* x = this frame's MeshCardsPayload index (the instance's card layout) or kNoMeshCards, y = the first        \
       CardPagePayload of the instance's cards, z = their page count, w = 0 */                                    \
    X( glm::uvec4, Cards, "cards", glm::uvec4( kNoMeshCards, 0u, 0u, 0u ) )                                       \
    X( glm::vec4, BoundsMin, "boundsMin", glm::vec4( 0.0f ) ) /* world box of the instance's cards, cm (xyz) */   \
    X( glm::vec4, BoundsMax, "boundsMax", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( InstanceRowPayload, "SurfaceCacheInstance", Std430, DESERT_INSTANCE_ROW_PAYLOAD_FIELDS );
#undef DESERT_INSTANCE_ROW_PAYLOAD_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kSurfaceCacheStructs[] = {
         &kCardPayloadStruct,
         &kCardPagePayloadStruct,
         &kMeshCardsPayloadStruct,
         &kInstanceRowPayloadStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kSurfaceCacheShaderInclude{
         .Path         = "Generated/GI/SurfaceCache.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/SurfaceCacheLayout.hpp",
         .Structs      = kSurfaceCacheStructs,
    };
} // namespace Desert::Graphic::SurfaceCache
