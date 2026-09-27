#pragma once

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The foliage palette's arithmetic and files, apart from its widgets (FO-UI1). The panel
// (FoliagePanel.cpp) and the palette commands (EditorLayer) both call these, and a suite drives them without
// an editor: what a row states (its cost), what the brush preview promises, what the search keeps, and the
// two files the palette writes (a type's copy, a palette preset) are each one function here.
namespace Desert::Editor::Foliage
{
    /// What one foliage type costs right now — in UE this is only visible in the profiler.
    struct FoliageTypeCost
    {
        size_t   Instances            = 0; ///< every instance the field holds
        size_t   InCullRange          = 0; ///< kept by the type's CullDistance from the view (0 when hidden)
        uint64_t TrianglesPerInstance = 0; ///< the mesh's LOD 0 triangles
        uint64_t Triangles            = 0; ///< InCullRange x TrianglesPerInstance: what the field submits
    };

    /**
     * @brief The field's cost from @p view: every instance, those the renderer's cull-distance rule keeps
     *        (Graphic::KeepsInstanceAtDistance, the same function and per-index fade the geometry pass uses),
     *        and their triangles. The frustum is not applied: the number answers "what does this type cost
     *        around the camera", not "what is on screen this frame".
     * @param hidden the field's entity is hidden (the palette's eye): nothing of it is drawn.
     */
    FoliageTypeCost MeasureFoliageTypeCost( std::span<const glm::mat4>                         instances,
                                            const Assets::Serialization::FoliageFloatInterval& cullDistance,
                                            const glm::vec3& view, uint64_t trianglesPerInstance, bool hidden );

    /// What one dab of the brush would do for one checked type at the cursor (FO-UI1 footprint preview).
    struct FoliageFootprint
    {
        float  Desired  = 0.0f; ///< UE AddInstancesForBrush's target for the disk (FoliageBrushDesiredCount)
        size_t Existing = 0;    ///< instances already inside the brush sphere
        float  Expected = 0.0f; ///< Desired - Existing, never below 0: at most this many land
    };

    /// The brush's promise for a type of @p density at @p centre. The type's own filters (slope, height,
    /// layers) can only lower what lands, so Expected is an upper bound, stated as one.
    FoliageFootprint PreviewFoliageFootprint( float density, float radius, float paintDensity,
                                              std::span<const glm::mat4> instances, const glm::vec3& centre );

    /// The palette's search: every whitespace-separated word of @p filter appears in @p name, ignoring
    /// ASCII case. An empty filter keeps every row.
    bool PaletteNameMatches( std::string_view name, std::string_view filter );

    /**
     * @brief "Save as asset" for a type that is already one: a copy of @p data beside @p source as
     *        `<stem>_Copy.defoliage` (or `_Copy_N`), under a NEW identity — the header is dropped so the
     *        writer mints a GUID; two files stating one GUID would be one asset on two paths.
     * @return the written path.
     */
    Common::ResultStr<std::filesystem::path>
    SaveFoliageTypeCopy( const std::filesystem::path& source, const Assets::Serialization::FoliageTypeData& data );

    /// One type of the palette as a preset records it.
    struct PalettePresetEntry
    {
        std::string          Name;     ///< the row's label
        std::string          MeshPath; ///< the type's mesh, working-dir-relative (the item's Mesh)
        Assets::AssetGuidRef Type;     ///< the `.defoliage`, relative to the assets root
    };

    /**
     * @brief A palette preset ("forest", "meadow") written as a COLLECTION: `<collectionsDir>/<name>/
     *        collection.json`, one item per type carrying its FoliageType record. No new format — a preset
     *        applied is a collection dropped on the palette (FoliagePaintTool::AddCollection), which lists the
     *        recorded types and checks exactly them.
     *
     * Refuses an empty palette, a name that is not a plain folder name, and a folder that already exists
     * (a preset does not overwrite a pack or another preset).
     * @return the manifest path.
     */
    Common::ResultStr<std::filesystem::path> SavePalettePreset( const std::filesystem::path& collectionsDir,
                                                                const std::string&           name,
                                                                const std::vector<PalettePresetEntry>& entries );
} // namespace Desert::Editor::Foliage
