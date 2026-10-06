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

#include <algorithm>
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

    /// How a ProceduralFoliageTransaction reads and writes one field of the world, whole (every component, as
    /// UE's transaction serialises the touched objects). @p Snapshot is the host's copy of one field.
    template <class Snapshot>
    struct ProceduralFoliageFieldStore
    {
        /// The field as it stands now; nullopt when there is no such field.
        std::function<std::optional<Snapshot>( const Common::UUID& )> Capture;
        /// The field goes (nothing when it is already gone).
        std::function<void( const Common::UUID& )> Destroy;
        /// The field back as captured, under its own UUID; false when it cannot be made.
        std::function<bool( const Snapshot& )> Restore;
    };

    /**
     * @brief One Resimulate as ONE undo step (UE wraps ResimulateProceduralContent in an FScopedTransaction):
     *        what every field it rewrote or removed was before, and what every field it rewrote or created is
     *        after. Undo puts the before back exactly (removed fields return under their UUIDs, created ones
     *        go); Redo puts the after back.
     *
     * The host calls Touch before it rewrites or removes a field and Made after it creates one; Close takes the
     * after state once the resimulation is done.
     */
    template <class Snapshot>
    class ProceduralFoliageTransaction
    {
    public:
        explicit ProceduralFoliageTransaction( ProceduralFoliageFieldStore<Snapshot> store )
             : m_Store( std::move( store ) )
        {
        }

        void Touch( const Common::UUID& field )
        {
            if ( Knows( field ) )
                return;
            m_Ids.push_back( field );
            if ( auto before = m_Store.Capture( field ) )
                m_Before.push_back( { field, std::move( *before ) } );
        }
        void Made( const Common::UUID& field )
        {
            if ( !Knows( field ) )
                m_Ids.push_back( field );
        }
        void Close()
        {
            m_After.clear();
            for ( const auto& id : m_Ids )
                if ( auto after = m_Store.Capture( id ) )
                    m_After.push_back( { id, std::move( *after ) } );
        }

        [[nodiscard]] bool Empty() const
        {
            return m_Ids.empty();
        }
        bool Undo()
        {
            return Put( m_Before );
        }
        bool Redo()
        {
            return Put( m_After );
        }

    private:
        struct Held
        {
            Common::UUID Id;
            Snapshot     State;
        };

        bool Knows( const Common::UUID& field ) const
        {
            return std::find( m_Ids.begin(), m_Ids.end(), field ) != m_Ids.end();
        }
        // Every field the step touched goes, then @p state's fields return: a field absent from @p state is
        // one that did not exist on that side of the step.
        bool Put( const std::vector<Held>& state )
        {
            for ( const auto& id : m_Ids )
                m_Store.Destroy( id );
            bool all = true;
            for ( const auto& held : state )
                all = m_Store.Restore( held.State ) && all;
            return all;
        }

        ProceduralFoliageFieldStore<Snapshot> m_Store;
        std::vector<Common::UUID>             m_Ids;
        std::vector<Held>                     m_Before;
        std::vector<Held>                     m_After;
    };
} // namespace Desert::Editor::Tools
