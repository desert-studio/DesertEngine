#pragma once

// S1: the Resimulate button of a procedural foliage volume (UE UProceduralFoliageComponent::ResimulateProcedural
// Content: GenerateProceduralContent, RemoveProceduralContent, then FoliageEdMode's AddInstances on the desired
// instances). The rule is a pure function of the volume, its types and a host — the world as a trace, the
// fields already in it and three writes — so a suite drives it over an in-memory world with a real landscape;
// the Scene host is FoliagePaintTool::ResimulateProcedural (no test project can compile Scene).

#include <Editor/Panels/ViewportPanel/Tools/FoliageBrush.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>
#include <Engine/World/Foliage/Procedural/ProceduralFoliageVolume.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Editor::Tools
{
    /// The world a volume resimulates into.
    struct ProceduralFoliageHost
    {
        /// The nearest surface on [start, end] that @p filter allows (a refused surface is traced through, as
        /// for the brush); LayerWeight is not read — the type's weight comes from LayerWeightAt.
        std::function<std::optional<FoliageTraceHit>( const glm::vec3& start, const glm::vec3& end,
                                                      const FoliageSurfaceFilter& filter )>
             Trace;
        /// The largest weight of type @p typeIndex's landscape layers at a landscape point; nullopt off any
        /// landscape. Read only for a type that lists layers.
        std::function<std::optional<float>( uint32_t typeIndex, const glm::vec3& point )> LayerWeightAt;
        /// The world's foliage cell size (FO-6); nullopt when the world is not partitioned.
        std::optional<double> CellSize;
        /// Every foliage field in the world, painted or generated, with its type as an index into the volume's
        /// types (UINT32_MAX when the volume does not list it).
        std::vector<World::Foliage::Procedural::ProceduralFoliageExistingField> Existing;
        /// Writes: an existing field (by its index in Existing) takes new instances or goes; a new field of
        /// the volume is made for a fresh type x cell.
        std::function<void( size_t existing, std::vector<glm::mat4> instances )> Rewrite;
        std::function<void( size_t existing )>                                   Remove;
        std::function<Common::BoolResultStr( const World::Foliage::Procedural::ProceduralFoliageTypeField& )>
             Create;
    };

    /// What one resimulation did, for the toast and the tests.
    struct ProceduralFoliageResimulated
    {
        size_t Instances = 0;
        size_t Created   = 0;
        size_t Rewritten = 0;
        size_t Removed   = 0;
    };

    /**
     * @brief Simulates @p volume over @p types, traces every desired instance in the box @p center +- Extent
     *        onto the surfaces the volume allows, places it by its type's rules and files it into @p owner's
     *        fields, leaving every other field untouched.
     *
     * Deterministic: the placement stream is seeded from the volume's RandomSeed, so resimulating an unchanged
     * world rewrites the same instances into the same fields. An error (and no write) when the spawner settings
     * are refused or a type index is out of range.
     */
    [[nodiscard]] Common::ResultStr<ProceduralFoliageResimulated> ResimulateProceduralFoliage(
         const ECS::ProceduralFoliageData& volume, const glm::vec3& center, const Common::UUID& owner,
         std::span<const Assets::Serialization::FoliageTypeData> types, const ProceduralFoliageHost& host );
} // namespace Desert::Editor::Tools
