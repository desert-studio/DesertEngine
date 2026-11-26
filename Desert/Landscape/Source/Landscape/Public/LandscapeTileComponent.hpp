#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::World::Landscape
{
    class LandscapeTileData;
}

namespace Desert::ECS
{
    // THE LOADED HEIGHTS OF ONE TILE, OWNED BY VALUE, HELD OUT OF LINE (UE: ULandscapeComponent keeps its
    // heightfield behind a pointer — the texture / FLandscapeComponentDerivedData — so the component's
    // header never pulls the data's). Copy is a deep copy and move steals, exactly as the
    // std::optional<LandscapeTileData> it replaces, so the component keeps its value semantics; every member
    // that touches the data is out of line (LandscapeTileComponent.cpp), so a translation unit that only
    // names the component never needs LandscapeData.hpp. One that reads the samples includes it itself.
    class LandscapeTileHeights
    {
    public:
        using Data = World::Landscape::LandscapeTileData;

        LandscapeTileHeights() noexcept = default;
        LandscapeTileHeights( std::nullopt_t ) noexcept;
        LandscapeTileHeights( const LandscapeTileHeights& other );
        LandscapeTileHeights( LandscapeTileHeights&& other ) noexcept = default;
        ~LandscapeTileHeights();

        LandscapeTileHeights& operator=( const LandscapeTileHeights& other );
        LandscapeTileHeights& operator=( LandscapeTileHeights&& other ) noexcept;
        LandscapeTileHeights& operator=( std::nullopt_t ) noexcept;
        LandscapeTileHeights& operator=( Data&& data );
        LandscapeTileHeights& operator=( const Data& data );
        // A template so that overload resolution for the component's own copy/move never instantiates
        // std::optional<LandscapeTileData> with the data incomplete; only a caller holding the optional
        // (and therefore LandscapeData.hpp) reaches it.
        template <typename Optional>
            requires std::is_same_v<std::remove_cvref_t<Optional>, std::optional<Data>>
        LandscapeTileHeights& operator=( Optional&& data )
        {
            if ( data.has_value() )
                return *this = std::move( *data );
            reset();
            return *this;
        }

        bool has_value() const noexcept
        {
            return m_Data != nullptr;
        }
        explicit operator bool() const noexcept
        {
            return m_Data != nullptr;
        }
        void reset() noexcept;

        Data& operator*() noexcept
        {
            return *m_Data;
        }
        const Data& operator*() const noexcept
        {
            return *m_Data;
        }
        Data* operator->() noexcept
        {
            return m_Data.get();
        }
        const Data* operator->() const noexcept
        {
            return m_Data.get();
        }
        Data& value() noexcept
        {
            return *m_Data;
        }
        const Data& value() const noexcept
        {
            return *m_Data;
        }

    private:
        std::unique_ptr<Data> m_Data;
    };

    // ONE TILE OF A LANDSCAPE (UE: ALandscapeStreamingProxy of one component).
    //
    // `Landscape` names the root BY ID AND NOT AS A PARENT, and that is the whole reason this is a field:
    // a parent link is containment, and WorldPartition keeps a composite whole in one cell — every tile of
    // a landscape would be one composite and one cell would grow to hold the entire terrain. As an
    // OBSERVATION (WorldPartitionRules.hpp, kEntityReferences) each tile is its own composite, placed by its
    // own rectangle; a tile whose root is not loaded simply has no frame yet.
    //
    // `HeightFile` is where the heights live — a DLHT blob beside the scene, never inside the .desce: a
    // 64x64 tile is 8 KiB of samples and JSON would carry it as text, eleven times over. The path is
    // written as the scene file spells its other files (relative to the working directory), and it is
    // RE-DERIVED on every save from the destination (LandscapeTileFiles.hpp, LandscapeTileBlobPath), so "Save As"
    // never writes one scene's heights into another's files.
    //
    // `Heights` is the loaded tile — the single source of truth for this tile's terrain (landscape
    // analysis A2). Runtime state: it is not in the scene block, it is what the block's file decodes to.
    // OWNED BY THE COMPONENT ALONE (PointerOwnership's Q1): LandscapeTileHeights is a value — copied deep,
    // never shared — that only keeps the data out of this header. A consumer that must keep the tile across
    // frames (the GPU copy, LS-4) cannot keep an address into an entt pool anyway, and decides its own form then.
    //
    // ONE WRITER after construction: ECS::SetLandscapeTileHeights / ECS::EditLandscapeTile
    // (LandscapeRootOf.hpp), both a registry.patch, so the shared tile table (DrawableLandscapeTiles) is
    // rebuilt on exactly the changes that can move or refuse a tile. Sample values edited in place report
    // through the tile's dirty rectangles instead.
    struct LandscapeTileComponent
    {
        Common::UUID Landscape = Common::UUID::Null(); // the root entity; null = no frame
        int32_t      TileX     = 0;                    // which tile of the root's grid, either side of it
        int32_t      TileZ     = 0;
        std::string  HeightFile;

        LandscapeTileHeights Heights;
    };
} // namespace Desert::ECS
