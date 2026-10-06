#include "ControlRigDocumentModel.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Desert::Editor
{
    namespace
    {
        using namespace Assets::Serialization;

        /// The undo record: the value before and after, installed through the model. Its EditedObject is
        /// the model, so the window's close drops it (CommandHistory::DropFor) with the payload it names.
        class ControlRigEditCommand final : public ICommand
        {
        public:
            ControlRigEditCommand( ControlRigDocumentModel* model, std::string label, ControlRigData before,
                                   ControlRigData after )
                 : m_Model( model ), m_Label( std::move( label ) ), m_Before( std::move( before ) ),
                   m_After( std::move( after ) )
            {
            }

            bool Undo() override
            {
                m_Model->Restore( m_Before );
                return true;
            }
            bool Redo() override
            {
                m_Model->Restore( m_After );
                return true;
            }
            [[nodiscard]] const void* EditedObject() const override
            {
                return m_Model;
            }
            [[nodiscard]] std::string GetLabel() const override
            {
                return m_Label;
            }

        private:
            ControlRigDocumentModel* m_Model;
            std::string              m_Label;
            ControlRigData           m_Before;
            ControlRigData           m_After;
        };

        ControlElementData* FindControl( ControlRigData& data, const std::string& name )
        {
            const auto it = std::ranges::find( data.Controls, name, &ControlElementData::Name );
            return it == data.Controls.end() ? nullptr : &*it;
        }

        RigGraphNodeData* FindNode( ControlRigData& data, const std::string& name )
        {
            if ( !data.Graph )
                return nullptr;
            const auto it = std::ranges::find( data.Graph->Nodes, name, &RigGraphNodeData::Name );
            return it == data.Graph->Nodes.end() ? nullptr : &*it;
        }

        const Animation::RigPin* FindPin( std::span<const Animation::RigPin> pins, const std::string& name )
        {
            const auto it = std::ranges::find( pins, std::string_view( name ), &Animation::RigPin::Name );
            return it == pins.end() ? nullptr : &*it;
        }

        Common::BoolResultStr KindOf( const RigGraphNodeData& node, Animation::RigNodeKind& out )
        {
            const auto kind = Animation::RigNodeKindFromText( node.Kind );
            if ( !kind )
                return Common::MakeFormattedError<bool>( "graph node '{}' is of unknown kind '{}'", node.Name,
                                                         node.Kind );
            out = *kind;
            return BOOLSUCCESS;
        }

        /// True when @p from is reachable from @p to by following links downstream — i.e. wiring
        /// from -> to would close a loop. Downstream = the nodes whose inputs link to a node.
        bool ClosesCycle( const RigGraphData& graph, const std::string& from, const std::string& to )
        {
            if ( from == to )
                return true;
            std::unordered_map<std::string, std::vector<std::string>> consumers;
            for ( const auto& node : graph.Nodes )
                for ( const auto& input : node.Inputs )
                    if ( input.Link )
                        consumers[input.Link->Node].push_back( node.Name );
            std::vector<std::string>        open{ to };
            std::unordered_set<std::string> seen{ to };
            while ( !open.empty() )
            {
                const std::string at = std::move( open.back() );
                open.pop_back();
                for ( const auto& next : consumers[at] )
                {
                    if ( next == from )
                        return true;
                    if ( seen.insert( next ).second )
                        open.push_back( next );
                }
            }
            return false;
        }

        /// Drops every input link that names @p node, putting the pin back to its identity literal.
        void UnlinkFrom( RigGraphData& graph, const std::string& node )
        {
            for ( auto& consumer : graph.Nodes )
            {
                Animation::RigNodeKind kind{};
                if ( !KindOf( consumer, kind ) )
                    continue;
                const auto& desc = Animation::DescribeRigNode( kind );
                for ( auto& input : consumer.Inputs )
                    if ( input.Link && input.Link->Node == node )
                        if ( const auto* pin = FindPin( desc.Inputs, input.Pin ) )
                            input = DefaultRigInput( input.Pin, pin->Type );
            }
        }

        void EraseNodes( ControlRigData& data, const std::function<bool( const RigGraphNodeData& )>& drop )
        {
            if ( !data.Graph )
                return;
            std::vector<std::string> gone;
            for ( const auto& node : data.Graph->Nodes )
                if ( drop( node ) )
                    gone.push_back( node.Name );
            std::erase_if( data.Graph->Nodes, drop );
            for ( const auto& name : gone )
                UnlinkFrom( *data.Graph, name );
            if ( data.Graph->Nodes.empty() )
                data.Graph.reset(); // the file spells "no forwards solve" by leaving Graph out
        }
    } // namespace

    RigGraphInputData DefaultRigInput( std::string_view pin, Animation::RigValueKind type )
    {
        RigGraphInputData input;
        input.Pin = std::string( pin );
        switch ( type )
        {
            case Animation::RigValueKind::Float:
                input.Float = 0.0f;
                break;
            case Animation::RigValueKind::Vec3:
                input.Vec3 = glm::vec3( 0.0f );
                break;
            case Animation::RigValueKind::Quat:
                input.Quat = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
                break;
            case Animation::RigValueKind::Transform:
                input.Transform = RigTransformData{};
                break;
        }
        return input;
    }

    Common::ResultStr<std::unique_ptr<ControlRigDocumentModel>>
    ControlRigDocumentModel::Open( const std::filesystem::path& path, CommandHistory& history )
    {
        auto loaded = LoadControlRigFile( path );
        if ( !loaded )
            return Common::MakeFormattedError<std::unique_ptr<ControlRigDocumentModel>>( "{}", loaded.GetError() );
        return Common::MakeSuccess(
             std::make_unique<ControlRigDocumentModel>( path, loaded.ExtractValue(), history ) );
    }

    ControlRigDocumentModel::ControlRigDocumentModel( std::filesystem::path path, Data loaded,
                                                      CommandHistory& history )
         : m_Path( std::move( path ) ), m_Data( std::move( loaded ) ), m_Saved( m_Data ), m_History( history )
    {
    }

    ControlRigDocumentModel::~ControlRigDocumentModel()
    {
        m_History.DropFor( this );
    }

    Common::BoolResultStr ControlRigDocumentModel::Validate() const
    {
        return ValidateControlRigData( m_Data );
    }

    Common::BoolResultStr ControlRigDocumentModel::Save()
    {
        if ( const auto written = SaveControlRigFile( m_Path, m_Data ); !written )
            return written;
        m_Saved = m_Data;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ControlRigDocumentModel::Commit( std::string label, Data after )
    {
        if ( after == m_Data )
            return Common::MakeFormattedError<bool>( "{}: nothing changed", label );
        Data before = std::exchange( m_Data, std::move( after ) );
        ++m_Revision;
        m_History.PushCommand(
             std::make_unique<ControlRigEditCommand>( this, std::move( label ), std::move( before ), m_Data ) );
        return BOOLSUCCESS;
    }

    void ControlRigDocumentModel::Restore( const Data& value )
    {
        m_Data = value;
        ++m_Revision;
    }

    // ── Rig elements ───────────────────────────────────────────────────────────────────────────────────

    Common::BoolResultStr ControlRigDocumentModel::AddControl( const std::string& name )
    {
        if ( name.empty() )
            return Common::MakeFormattedError<bool>( "a control needs a name" );
        Data next = m_Data;
        if ( FindControl( next, name ) )
            return Common::MakeFormattedError<bool>( "the rig already has a control named '{}'", name );
        ControlElementData control;
        control.Name = name;
        next.Controls.push_back( std::move( control ) );
        return Commit( std::format( "Add control '{}'", name ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::RemoveControl( const std::string& name )
    {
        Data next = m_Data;
        if ( !FindControl( next, name ) )
            return Common::MakeFormattedError<bool>( "the rig has no control named '{}'", name );
        std::erase_if( next.Controls, [&]( const ControlElementData& c ) { return c.Name == name; } );
        for ( auto& control : next.Controls )
            std::erase_if( control.Parents,
                           [&]( const ControlSpaceData& s ) { return s.Kind == "Control" && s.Target == name; } );
        std::erase_if( next.Drives, [&]( const ControlDriveData& d ) { return d.Control == name; } );
        EraseNodes( next,
                    [&]( const RigGraphNodeData& n )
                    {
                        Animation::RigNodeKind kind{};
                        return KindOf( n, kind ) &&
                               Animation::DescribeRigNode( kind ).Target ==
                                    Animation::RigNodeTargetKind::Control &&
                               n.Target == name;
                    } );
        return Commit( std::format( "Remove control '{}'", name ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::SetControl( const std::string&        name,
                                                               const ControlElementData& value )
    {
        Data  next    = m_Data;
        auto* control = FindControl( next, name );
        if ( !control )
            return Common::MakeFormattedError<bool>( "the rig has no control named '{}'", name );
        if ( value.Name.empty() )
            return Common::MakeFormattedError<bool>( "control '{}' cannot be renamed to nothing", name );
        if ( value.Name != name && FindControl( next, value.Name ) )
            return Common::MakeFormattedError<bool>( "the rig already has a control named '{}'", value.Name );
        *control = value;
        if ( value.Name != name )
        {
            for ( auto& other : next.Controls )
                for ( auto& space : other.Parents )
                    if ( space.Kind == "Control" && space.Target == name )
                        space.Target = value.Name;
            for ( auto& drive : next.Drives )
                if ( drive.Control == name )
                    drive.Control = value.Name;
            if ( next.Graph )
                for ( auto& node : next.Graph->Nodes )
                {
                    Animation::RigNodeKind kind{};
                    if ( KindOf( node, kind ) &&
                         Animation::DescribeRigNode( kind ).Target == Animation::RigNodeTargetKind::Control &&
                         node.Target == name )
                        node.Target = value.Name;
                }
        }
        return Commit( std::format( "Edit control '{}'", value.Name ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::SetDrive( const std::string& control, const std::string& bone )
    {
        Data next = m_Data;
        if ( !FindControl( next, control ) )
            return Common::MakeFormattedError<bool>( "the rig has no control named '{}'", control );
        std::erase_if( next.Drives, [&]( const ControlDriveData& d ) { return d.Control == control; } );
        if ( !bone.empty() )
            next.Drives.push_back( ControlDriveData{ control, bone } );
        return Commit( bone.empty() ? std::format( "Clear drive of '{}'", control )
                                    : std::format( "Drive '{}' with '{}'", bone, control ),
                       std::move( next ) );
    }

    // ── Graph ──────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<std::string> ControlRigDocumentModel::AddNode( Animation::RigNodeKind kind,
                                                                     const std::string&     target )
    {
        const auto& desc = Animation::DescribeRigNode( kind );
        if ( desc.Target == Animation::RigNodeTargetKind::None && !target.empty() )
            return Common::MakeFormattedError<std::string>( "a {} names nothing, yet it was given '{}'", desc.Name,
                                                            target );
        if ( desc.Target != Animation::RigNodeTargetKind::None && target.empty() )
            return Common::MakeFormattedError<std::string>(
                 "a {} needs a {}", desc.Name,
                 desc.Target == Animation::RigNodeTargetKind::Control ? "control" : "bone" );
        Data next = m_Data;
        if ( desc.Target == Animation::RigNodeTargetKind::Control && !FindControl( next, target ) )
            return Common::MakeFormattedError<std::string>( "the rig has no control named '{}'", target );
        if ( !next.Graph )
            next.Graph.emplace();

        std::string name;
        for ( uint32_t i = 1;; ++i )
        {
            name = std::format( "{}_{}", desc.Name, i );
            if ( !FindNode( next, name ) )
                break;
        }
        RigGraphNodeData node;
        node.Name   = name;
        node.Kind   = std::string( desc.Name );
        node.Target = target;
        if ( desc.UsesSpace )
            node.Space = std::string( Animation::ToString( Animation::RigControlSpace::Global ) );
        for ( const auto& pin : desc.Inputs )
            node.Inputs.push_back( DefaultRigInput( pin.Name, pin.Type ) );
        next.Graph->Nodes.push_back( std::move( node ) );
        if ( auto committed = Commit( std::format( "Add {}", name ), std::move( next ) ); !committed )
            return Common::MakeFormattedError<std::string>( "{}", committed.GetError() );
        return Common::MakeSuccess( name );
    }

    Common::BoolResultStr ControlRigDocumentModel::RemoveNode( const std::string& node )
    {
        Data next = m_Data;
        if ( !FindNode( next, node ) )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'", node );
        EraseNodes( next, [&]( const RigGraphNodeData& n ) { return n.Name == node; } );
        return Commit( std::format( "Remove {}", node ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::SetNodeTarget( const std::string& node,
                                                                  const std::string& target )
    {
        Data  next  = m_Data;
        auto* found = FindNode( next, node );
        if ( !found )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'", node );
        Animation::RigNodeKind kind{};
        if ( auto known = KindOf( *found, kind ); !known )
            return known;
        const auto targetKind = Animation::DescribeRigNode( kind ).Target;
        if ( targetKind == Animation::RigNodeTargetKind::None )
            return Common::MakeFormattedError<bool>( "{} ({}) names nothing", node, found->Kind );
        if ( targetKind == Animation::RigNodeTargetKind::Control && !FindControl( next, target ) )
            return Common::MakeFormattedError<bool>( "the rig has no control named '{}'", target );
        found->Target = target;
        return Commit( std::format( "{} targets '{}'", node, target ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::SetNodeSpace( const std::string&         node,
                                                                 Animation::RigControlSpace space )
    {
        Data  next  = m_Data;
        auto* found = FindNode( next, node );
        if ( !found )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'", node );
        Animation::RigNodeKind kind{};
        if ( auto known = KindOf( *found, kind ); !known )
            return known;
        if ( !Animation::DescribeRigNode( kind ).UsesSpace )
            return Common::MakeFormattedError<bool>( "{} ({}) has no control space", node, found->Kind );
        found->Space = std::string( Animation::ToString( space ) );
        return Commit( std::format( "{} in {} space", node, found->Space ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::Connect( const std::string& fromNode,
                                                            const std::string& fromPin, const std::string& toNode,
                                                            const std::string& toPin )
    {
        Data  next = m_Data;
        auto* from = FindNode( next, fromNode );
        auto* to   = FindNode( next, toNode );
        if ( !from || !to )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'",
                                                     from ? toNode : fromNode );
        Animation::RigNodeKind fromKind{};
        Animation::RigNodeKind toKind{};
        if ( auto known = KindOf( *from, fromKind ); !known )
            return known;
        if ( auto known = KindOf( *to, toKind ); !known )
            return known;
        const auto* out = FindPin( Animation::DescribeRigNode( fromKind ).Outputs, fromPin );
        const auto* in  = FindPin( Animation::DescribeRigNode( toKind ).Inputs, toPin );
        if ( !out )
            return Common::MakeFormattedError<bool>( "{} has no output '{}'", fromNode, fromPin );
        if ( !in )
            return Common::MakeFormattedError<bool>( "{} has no input '{}'", toNode, toPin );
        if ( out->Type != in->Type )
            return Common::MakeFormattedError<bool>( "{}.{} is a {} and {}.{} takes a {}", fromNode, fromPin,
                                                     Animation::ToString( out->Type ), toNode, toPin,
                                                     Animation::ToString( in->Type ) );
        if ( ClosesCycle( *next.Graph, fromNode, toNode ) )
            return Common::MakeFormattedError<bool>( "wiring {} into {} closes a loop; the forwards solve is one "
                                                     "pass in order",
                                                     fromNode, toNode );
        RigGraphInputData wired;
        wired.Pin       = toPin;
        wired.Link      = RigLinkData{ fromNode, fromPin };
        const auto slot = std::ranges::find( to->Inputs, toPin, &RigGraphInputData::Pin );
        if ( slot == to->Inputs.end() )
            to->Inputs.push_back( std::move( wired ) );
        else
            *slot = std::move( wired );
        return Commit( std::format( "Wire {}.{} -> {}.{}", fromNode, fromPin, toNode, toPin ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::Disconnect( const std::string& toNode,
                                                               const std::string& toPin )
    {
        Data  next = m_Data;
        auto* to   = FindNode( next, toNode );
        if ( !to )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'", toNode );
        Animation::RigNodeKind kind{};
        if ( auto known = KindOf( *to, kind ); !known )
            return known;
        const auto* pin  = FindPin( Animation::DescribeRigNode( kind ).Inputs, toPin );
        const auto  slot = std::ranges::find( to->Inputs, toPin, &RigGraphInputData::Pin );
        if ( !pin || slot == to->Inputs.end() || !slot->Link )
            return Common::MakeFormattedError<bool>( "{}.{} is not wired", toNode, toPin );
        *slot = DefaultRigInput( toPin, pin->Type );
        return Commit( std::format( "Cut {}.{}", toNode, toPin ), std::move( next ) );
    }

    Common::BoolResultStr ControlRigDocumentModel::SetLiteral( const std::string&       node,
                                                               const RigGraphInputData& literal )
    {
        Data  next  = m_Data;
        auto* found = FindNode( next, node );
        if ( !found )
            return Common::MakeFormattedError<bool>( "the graph has no node named '{}'", node );
        const int payloads = int( literal.Link.has_value() ) + int( literal.Float.has_value() ) +
                             int( literal.Vec3.has_value() ) + int( literal.Quat.has_value() ) +
                             int( literal.Transform.has_value() );
        if ( literal.Link || payloads != 1 )
            return Common::MakeFormattedError<bool>( "{}.{}: a literal carries exactly one value and no link",
                                                     node, literal.Pin );
        const auto slot = std::ranges::find( found->Inputs, literal.Pin, &RigGraphInputData::Pin );
        if ( slot == found->Inputs.end() )
            return Common::MakeFormattedError<bool>( "{} has no input '{}'", node, literal.Pin );
        if ( slot->Link )
            return Common::MakeFormattedError<bool>( "{}.{} is wired; cut the wire before typing a value", node,
                                                     literal.Pin );
        *slot = literal;
        return Commit( std::format( "Set {}.{}", node, literal.Pin ), std::move( next ) );
    }
} // namespace Desert::Editor
