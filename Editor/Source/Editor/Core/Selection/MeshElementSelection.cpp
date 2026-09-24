#include "MeshElementSelection.hpp"

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Common/Core/Logger.hpp>

#include <utility>

namespace Desert::Editor::Core
{
    namespace
    {
        // A selection change as an undo step. It names the entity by UUID and carries both ID sets by value,
        // so it stays meaningful whatever happened to the mesh in between: a restored ID the mesh no longer
        // has is dropped - and counted - by the tool's next Track, like after any other edit.
        class ElementSelectionCommand final : public ICommand
        {
        public:
            ElementSelectionCommand( const Common::UUID& entity, Geometry::ElementSelection before,
                                     Geometry::ElementSelection after, std::string label )
                 : m_Entity( entity ), m_Before( std::move( before ) ), m_After( std::move( after ) ),
                   m_Label( std::move( label ) )
            {
            }

            bool Undo() override
            {
                MeshElementSelection::Get().Restore( m_Entity, m_Before );
                return true;
            }
            bool Redo() override
            {
                MeshElementSelection::Get().Restore( m_Entity, m_After );
                return true;
            }
            std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            Common::UUID               m_Entity;
            Geometry::ElementSelection m_Before;
            Geometry::ElementSelection m_After;
            std::string                m_Label;
        };
    } // namespace

    const char* MeshElementSelection::ToString( Op op )
    {
        switch ( op )
        {
            case Op::SelectAll:
                return "Select All";
            case Op::SelectConnected:
                return "Select Connected";
            case Op::Grow:
                return "Grow Selection";
            case Op::Shrink:
                return "Shrink Selection";
            case Op::Invert:
                return "Invert Selection";
            case Op::Clear:
                return "Clear Selection";
        }
        return "Unknown";
    }

    void MeshElementSelection::Track( const Common::UUID&                            entity,
                                      std::shared_ptr<const Geometry::FDynamicMesh3> mesh )
    {
        if ( static_cast<uint64_t>( entity ) != static_cast<uint64_t>( m_Entity ) )
        {
            m_Entity   = entity;
            m_Mesh     = std::move( mesh );
            m_Topology = m_Mesh ? std::make_unique<const Geometry::FGroupTopology>( m_Mesh.get(), true ) : nullptr;
            m_Selection.Clear();
            m_LastDropped  = {};
            m_TotalDropped = 0;
            return;
        }
        if ( mesh == m_Mesh )
            return;
        m_Mesh = std::move( mesh );
        m_Topology.reset();
        if ( !m_Mesh )
        {
            m_Selection.Clear();
            return;
        }
        m_Topology = std::make_unique<const Geometry::FGroupTopology>( m_Mesh.get(), true );
        const Geometry::PruneReport dropped = m_Selection.Prune( *m_Mesh );
        if ( dropped.Total() > 0 )
        {
            m_LastDropped = dropped;
            m_TotalDropped += dropped.Total();
            LOG_INFO(
                 "[Mesh Selection] the edited mesh dropped {0} selected {1}(s): {2} gone, {3} now a different "
                 "element",
                 dropped.Total(), Geometry::ToString( m_Selection.Mode() ), dropped.Missing, dropped.Changed );
        }
    }

    void MeshElementSelection::Commit( Geometry::ElementSelection next, const std::string& label )
    {
        if ( next == m_Selection )
            return;
        CommandHistory::Get().PushCommand(
             std::make_unique<ElementSelectionCommand>( m_Entity, m_Selection, next, label ) );
        m_Selection = std::move( next );
    }

    Common::BoolResultStr MeshElementSelection::SetMode( Geometry::ElementMode mode )
    {
        if ( mode == m_Selection.Mode() )
            return Common::MakeSuccess( true );
        if ( !m_Mesh )
        {
            // Nothing to convert: the mode is a preference until a mesh is picked.
            m_Selection = Geometry::ElementSelection( mode );
            return Common::MakeSuccess( true );
        }
        Commit( Geometry::ConvertSelection( *m_Mesh, *m_Topology, m_Selection, mode ),
                std::string( "Mesh Selection: " ) + Geometry::ToString( mode ) + " mode" );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr MeshElementSelection::Apply( Op op )
    {
        if ( !m_Mesh )
            return Common::MakeFormattedError<bool>(
                 "Mesh Selection: {} needs the Select Elements tool on an entity with an editable mesh",
                 ToString( op ) );
        const Geometry::FDynamicMesh3&  mesh     = *m_Mesh;
        const Geometry::FGroupTopology& topology = *m_Topology;
        Geometry::ElementSelection next( m_Selection.Mode() );
        switch ( op )
        {
            case Op::SelectAll:
            {
                // Every live element is a superset of every piece: connected-from-everything is "all".
                Geometry::ElementSelection seed( Geometry::ElementMode::Vertex );
                for ( const int v : mesh.VertexIndicesItr() )
                    if ( auto added = seed.Add( mesh, v ); !added.IsSuccess() )
                        return added;
                next = Geometry::ConvertSelection( mesh, topology, seed, m_Selection.Mode() );
                break;
            }
            case Op::SelectConnected:
                next = Geometry::SelectConnected( mesh, topology, m_Selection );
                break;
            case Op::Grow:
                next = Geometry::GrowSelection( mesh, topology, m_Selection );
                break;
            case Op::Shrink:
                next = Geometry::ShrinkSelection( mesh, topology, m_Selection );
                break;
            case Op::Invert:
                next = Geometry::InvertSelection( mesh, topology, m_Selection );
                break;
            case Op::Clear:
                break;
        }
        Commit( std::move( next ), std::string( "Mesh Selection: " ) + ToString( op ) );
        return Common::MakeSuccess( true );
    }

    std::unique_ptr<ICommand> MeshElementSelection::MakeSelectionChange( const Common::UUID&        entity,
                                                                         Geometry::ElementSelection before,
                                                                         Geometry::ElementSelection after,
                                                                         std::string                label )
    {
        return std::make_unique<ElementSelectionCommand>( entity, std::move( before ), std::move( after ),
                                                          std::move( label ) );
    }

    void MeshElementSelection::Restore( const Common::UUID& entity, Geometry::ElementSelection selection )
    {
        if ( static_cast<uint64_t>( entity ) != static_cast<uint64_t>( m_Entity ) )
        {
            // The tool follows the entity selection; point it back at the entity this selection was made on.
            SelectionManager::SetSelected( entity );
            m_Entity = entity;
            m_Mesh.reset();
            m_Topology.reset();
        }
        m_Selection = std::move( selection );
    }
} // namespace Desert::Editor::Core
