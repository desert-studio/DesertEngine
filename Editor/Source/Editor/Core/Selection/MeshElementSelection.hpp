#pragma once

#include <Editor/Core/CommandHistory.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/Geometry/DynamicMeshSelection.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    // The Modeling Mode's mesh element selection (UE: the PolygonSelectionMechanic every mesh-editing tool
    // shares): which vertices / edges / triangles / polygroups of the selected entity's editable mesh the next
    // operation works on. One per editor, like ModelingState; the viewport tool (ElementSelectTool) picks
    // and draws, the Modeling panel and the command palette read and change it.
    //
    // Every change of the ID set goes through Commit, which makes it ONE undo step (UE records a selection
    // change as its own transaction), so Undo after a mis-click gives the previous selection back.
    class MeshElementSelection
    {
    public:
        static MeshElementSelection& Get()
        {
            static MeshElementSelection s;
            return s;
        }

        enum class Op
        {
            SelectAll,
            SelectConnected,
            Grow,
            Shrink,
            Invert,
            Clear,
            // UE PolyEdit's Select Edge Loops / Select Edge Rings on an Edge selection: the loops / rings through
            // the selected edges added, at the current level (Geometry::ExpandEdges).
            SelectEdgeLoops,
            SelectEdgeRings,
        };
        [[nodiscard]] static const char* ToString( Op op );

        [[nodiscard]] const Common::UUID& Entity() const
        {
            return m_Entity;
        }
        [[nodiscard]] const Geometry::ElementSelection& Selection() const
        {
            return m_Selection;
        }
        [[nodiscard]] Geometry::ElementMode Mode() const
        {
            return m_Selection.Mode();
        }
        // What a Vertex / Edge pick lands on: group corners and group edges (UE PolyEdit, the default) or every
        // mesh vertex and edge (UE TriEdit). The selection stores mesh IDs either way, so a change of level keeps
        // it and is not an undo step - like switching between UE's two tools.
        [[nodiscard]] Geometry::TopologyLevel Level() const
        {
            return m_Level;
        }
        void SetLevel( Geometry::TopologyLevel level )
        {
            m_Level = level;
        }
        // UE's UPolygonSelectionMechanicProperties bSelectEdgeLoops / bSelectEdgeRings: an Edge pick - the click
        // and the hover highlight that previews it - takes the loops / rings through the picked edge with it
        // (both on: the union of the two, each grown from the picked edge). Tool settings like the level, so not
        // undo steps.
        [[nodiscard]] bool PickEdgeLoops() const
        {
            return m_PickEdgeLoops;
        }
        [[nodiscard]] bool PickEdgeRings() const
        {
            return m_PickEdgeRings;
        }
        void SetPickEdgeLoops( bool on )
        {
            m_PickEdgeLoops = on;
        }
        void SetPickEdgeRings( bool on )
        {
            m_PickEdgeRings = on;
        }
        // The mesh IDs a pick of @p hit selects: HitElements at the current mode and level, grown by the loops /
        // rings the two settings above ask for when the hit is an edge. Empty for a miss or with no tracked mesh.
        [[nodiscard]] std::vector<int> PickedElements( const Geometry::ElementHit& hit ) const;
        // What the last check against an edited mesh dropped, and the running total since the entity was
        // picked - the panel shows both, so an edit that emptied the selection says so.
        [[nodiscard]] const Geometry::PruneReport& LastDropped() const
        {
            return m_LastDropped;
        }
        [[nodiscard]] int TotalDropped() const
        {
            return m_TotalDropped;
        }
        // The mesh the selection's IDs name and its polygroup topology, built together by Track (null when no
        // mesh is tracked). Picking and every selection operation read these two.
        [[nodiscard]] const std::shared_ptr<const Geometry::DynamicMesh3>& Mesh() const
        {
            return m_Mesh;
        }
        [[nodiscard]] const Geometry::GroupTopology* Topology() const
        {
            return m_Topology.get();
        }

        [[nodiscard]] bool HasMesh() const
        {
            return m_Mesh != nullptr;
        }

        // Called by the tool every frame with the entity it edits and that entity's current mesh (null when
        // it has none). A new entity starts an empty selection (not an undo step: nothing was un-selected
        // by the user); a new mesh on the same entity prunes the selection against it and counts the drop.
        void Track( const Common::UUID& entity, std::shared_ptr<const Geometry::DynamicMesh3> mesh );

        // Replaces the selection as one undo step labelled `label`; nothing is recorded when it is unchanged.
        void Commit( Geometry::ElementSelection next, const std::string& label );
        // The same part of the mesh in another mode (ConvertSelection). NOT an undo step: as in UE Modeling the
        // mode is a tool setting; picks, selection ops and mesh edits are the undo steps.
        [[nodiscard]] Common::BoolResultStr SetMode( Geometry::ElementMode mode );
        // Refused, by name, when no editable mesh is being tracked.
        [[nodiscard]] Common::BoolResultStr Apply( Op op );

        // Undo / redo land here: the selection as it was, on the entity it was made on.
        void Restore( const Common::UUID& entity, Geometry::ElementSelection selection );

        // The same selection change as an undo sub-step that is NOT pushed: a mesh operation records it with
        // its mesh edit, so the edit and the selection it leaves are one step (Commands::RecordMeshChange).
        [[nodiscard]] static std::unique_ptr<ICommand> MakeSelectionChange( const Common::UUID&        entity,
                                                                            Geometry::ElementSelection before,
                                                                            Geometry::ElementSelection after,
                                                                            std::string                label );

        // One-shot for the command palette / control channel: pick at the viewport's centre on the next
        // frame, as if clicked there (no modifier). The viewport owns the camera, so only the tool can.
        bool ReqPickCentre = false;
        // With ReqPickCentre: the pick adds to the selection, as a Shift-click does.
        bool ReqPickCentreAdd = false;

    private:
        Common::UUID                              m_Entity = Common::UUID::Null();
        std::shared_ptr<const Geometry::DynamicMesh3> m_Mesh;
        // Built from m_Mesh whenever m_Mesh changes. No other invalidation exists or is needed: the component's
        // mesh is immutable (every edit puts a NEW DynamicMesh3 on the entity), and m_Mesh keeps the one this
        // topology was built from alive, so pointer identity cannot be reused under it.
        std::unique_ptr<const Geometry::GroupTopology>  m_Topology;
        Geometry::ElementSelection                m_Selection{ Geometry::ElementMode::PolyGroup };
        Geometry::TopologyLevel                         m_Level = Geometry::TopologyLevel::Group;
        bool                                      m_PickEdgeLoops = false; // UE's defaults: both off
        bool                                      m_PickEdgeRings = false;
        Geometry::PruneReport                     m_LastDropped;
        int                                       m_TotalDropped = 0;
    };
} // namespace Desert::Editor::Core
