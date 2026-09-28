// Ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/MeshModelingTools/Public/CSGMeshesTool.h
// (UCSGMeshesToolProperties, UTrimMeshesToolProperties, the builder's two-input rule) and ModelingComponents
// BaseTools/BaseCreateFromSelectedTool.h (OutputWriteTo, HandleInputs), adapted: no live preview and no Accept -
// one click runs the operation on the scene selection; the operation is RunMeshBoolean (the ported
// FMeshBoolean) with the other mesh baked into the operated one's space instead of FBooleanMeshesOp's world
// frame; the result is written through Commands::ApplyXformEdit as one undo step; Hide Inputs is the entity's
// VisibilityComponent.

#include "MeshBooleanTool.hpp"
#include "ModelingToolTarget.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/MeshBooleanOperation.hpp>

#include <Common/Core/Logger.hpp>

#include <spdlog/fmt/fmt.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    namespace
    {
        using OperandMeshPtr = std::shared_ptr<const Geometry::DynamicMesh3>;

        struct Input
        {
            Common::UUID   Id;
            ECS::Entity    Entity;
            OperandMeshPtr Mesh;
            // What the component holds now (ToolTargetMesh::Committed): kept when the input is only hidden.
            OperandMeshPtr Committed;
            std::string    Name;
        };

        Common::ResultStr<std::array<Input, 2>> SelectedInputs( ::Desert::Core::Scene& scene, const char* name )
        {
            using Out             = std::array<Input, 2>;
            const auto& selection = SelectionManager::GetSelection();
            if ( selection.size() != 2 )
                return Common::MakeFormattedError<Out>(
                     "{}: select exactly two entities with a mesh (A first, then B), not {}", name,
                     selection.size() );
            Out out;
            for ( size_t i = 0; i < 2; ++i )
            {
                const Common::UUID id  = selection[i];
                auto               ref = scene.FindEntityByID( id );
                if ( !ref )
                    return Common::MakeFormattedError<Out>( "{}: entity {} is not in the scene", name,
                                                            static_cast<uint64_t>( id ) );
                const ECS::Entity e = ref->get();
                if ( !e.HasComponent<ECS::StaticMeshComponent>() || !e.HasComponent<ECS::TransformComponent>() )
                    return Common::MakeFormattedError<Out>( "{}: entity {} has no static mesh or no transform",
                                                            name, static_cast<uint64_t>( id ) );
                if ( e.HasComponent<ECS::RelationshipComponent>() &&
                     !e.GetComponent<ECS::RelationshipComponent>().Children.empty() )
                    return Common::MakeFormattedError<Out>(
                         "{}: entity {} has {} children - they are not part of its mesh and would be left behind; "
                         "detach them first",
                         name, static_cast<uint64_t>( id ),
                         e.GetComponent<ECS::RelationshipComponent>().Children.size() );
                auto target = GetToolTargetMesh( e.GetComponent<ECS::StaticMeshComponent>() );
                if ( !target.IsSuccess() )
                    return Common::MakeFormattedError<Out>( "{}: entity {}: {}", name, static_cast<uint64_t>( id ),
                                                            target.GetError() );
                out[i] = { id, e, target.GetValue().Mesh, target.GetValue().Committed,
                           e.HasComponent<ECS::TagComponent>() ? e.GetComponent<ECS::TagComponent>().Tag
                                                               : std::string( "Mesh" ) };
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // The other mesh in the operated one's space (inverse(operated world) * other world): MeshBoolean
        // takes both in one space. A Boolean keeps the other mesh's triangles, so their material IDs are
        // remapped into `table` - the operated entity's slots, then the other's slots not already there.
        Common::ResultStr<OperandMeshPtr> BakeInto( const Input& operated, const Input& other,
                                                    std::vector<Common::AssetHandle>* table, const char* name )
        {
            auto view = Geometry::Bridge::EditMeshView( other.Mesh );
            if ( !view.IsSuccess() )
                return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}': {}", name, other.Name,
                                                                   view.GetError() );
            const glm::mat4 toOperated =
                 glm::inverse( operated.Entity.GetWorldTransform() ) * other.Entity.GetWorldTransform();
            auto moved = Geometry::TransformMesh( *view.GetValue(), toOperated );
            if ( !moved.IsSuccess() )
                return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}' into the space of '{}': {}", name,
                                                                   other.Name, operated.Name, moved.GetError() );
            Geometry::EditMesh mesh = std::move( moved.ExtractValue().Mesh );
            if ( table )
            {
                const auto& slots = other.Entity.GetComponent<ECS::StaticMeshComponent>().MaterialSlots;
                for ( const int t : mesh.TriangleIds() )
                {
                    // A material ID past the slots reads as the empty slot, as the renderer draws it.
                    const int                 id     = mesh.Attributes().GetMaterialId( t );
                    const Common::AssetHandle handle = id >= 0 && static_cast<size_t>( id ) < slots.size()
                                                            ? slots[static_cast<size_t>( id )]
                                                            : Common::AssetHandle{};
                    auto                      it     = std::find( table->begin(), table->end(), handle );
                    if ( it == table->end() )
                        it = table->insert( table->end(), handle );
                    mesh.Attributes().SetMaterialId( t, static_cast<int>( it - table->begin() ) );
                }
            }
            auto shared = Geometry::Bridge::FromEditMesh( std::move( mesh ) );
            if ( !shared.IsSuccess() )
                return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}': {}", name, other.Name,
                                                                   shared.GetError() );
            return Common::MakeSuccess( OperandMeshPtr( shared.ExtractValue() ) );
        }
    } // namespace

    const char* ToString( BooleanTool tool )
    {
        return tool == BooleanTool::Boolean ? "Boolean" : "Trim";
    }

    const char* ToString( CsgOperation operation )
    {
        switch ( operation )
        {
            case CsgOperation::DifferenceAB:
                return "Difference A - B";
            case CsgOperation::DifferenceBA:
                return "Difference B - A";
            case CsgOperation::Intersect:
                return "Intersect";
            case CsgOperation::Union:
                return "Union";
        }
        return "Unknown";
    }

    const char* ToString( TrimTarget target )
    {
        return target == TrimTarget::TrimA ? "Trim A" : "Trim B";
    }

    const char* ToString( TrimSide side )
    {
        return side == TrimSide::RemoveInside ? "Remove Inside" : "Remove Outside";
    }

    const char* ToString( BooleanWriteTo writeTo )
    {
        return writeTo == BooleanWriteTo::NewObject ? "New Object" : "Input";
    }

    const char* ToString( BooleanInputs inputs )
    {
        switch ( inputs )
        {
            case BooleanInputs::Delete:
                return "Delete Inputs";
            case BooleanInputs::Hide:
                return "Hide Inputs";
            case BooleanInputs::Keep:
                return "Keep Inputs";
        }
        return "Unknown";
    }

    BooleanToolArgs BooleanArgsFromModelingState()
    {
        return ModelingState::Get().Boolean;
    }

    Common::BoolResultStr ApplyBooleanTool( ::Desert::Core::Scene& scene, BooleanTool tool,
                                            const BooleanToolArgs& args )
    {
        const char* name   = ToString( tool );
        auto        inputs = SelectedInputs( scene, name );
        if ( !inputs.IsSuccess() )
            return Common::MakeError<bool>( inputs.GetError() );
        const std::array<Input, 2>& ab = inputs.GetValue();

        // Which input is operated on (the result lands in its space) and what MeshBoolean runs.
        const bool bTrim = tool == BooleanTool::Trim;
        const bool bOnB = bTrim ? args.Trimmed == TrimTarget::TrimB : args.Operation == CsgOperation::DifferenceBA;
        const Input&               operated  = ab[bOnB ? 1 : 0];
        const Input&               other     = ab[bOnB ? 0 : 1];
        Geometry::BooleanOperation operation = Geometry::BooleanOperation::Difference;
        if ( bTrim )
            operation = args.Side == TrimSide::RemoveInside ? Geometry::BooleanOperation::TrimInside
                                                            : Geometry::BooleanOperation::TrimOutside;
        else if ( args.Operation == CsgOperation::Intersect )
            operation = Geometry::BooleanOperation::Intersect;
        else if ( args.Operation == CsgOperation::Union )
            operation = Geometry::BooleanOperation::Union;

        // A Trim drops every triangle of the cutter, so the operated entity's slots stay as they are.
        std::vector<Common::AssetHandle> table =
             operated.Entity.GetComponent<ECS::StaticMeshComponent>().MaterialSlots;
        auto cutter = BakeInto( operated, other, bTrim ? nullptr : &table, name );
        if ( !cutter.IsSuccess() )
            return Common::MakeError<bool>( cutter.GetError() );
        auto result = Geometry::RunMeshBoolean( operation, *operated.Mesh, *cutter.GetValue() );
        if ( !result.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{} of '{}' by '{}': {}", name, operated.Name, other.Name,
                                                     result.GetError() );
        OperandMeshPtr mesh = std::move( result.ExtractValue().Mesh );

        const auto&                tc = operated.Entity.GetComponent<ECS::TransformComponent>();
        Commands::XformEntityState state{ operated.Id, mesh,         tc.Translation, tc.Rotation,
                                          tc.Scale,    std::nullopt, std::nullopt };
        if ( !bTrim )
            state.MaterialSlots = std::move( table );
        const std::string detail = bTrim ? fmt::format( "{} {}", ToString( args.Trimmed ), ToString( args.Side ) )
                                         : std::string( ToString( args.Operation ) );
        const std::string label  = fmt::format( "{} {}", name, detail );

        std::vector<Commands::XformEntityState> changes;
        std::vector<Commands::XformNewEntity>   creates;
        std::vector<Common::UUID>               deletes;
        if ( args.WriteTo == BooleanWriteTo::Input )
            changes.push_back( std::move( state ) );
        else
            creates.push_back( { operated.Id, fmt::format( "{} {}", operated.Name, name ), std::move( state ) } );
        for ( const Input& in : ab )
        {
            if ( args.WriteTo == BooleanWriteTo::Input && in.Id == operated.Id )
                continue;
            if ( args.Inputs == BooleanInputs::Delete )
                deletes.push_back( in.Id );
            else if ( args.Inputs == BooleanInputs::Hide )
            {
                const auto& itc = in.Entity.GetComponent<ECS::TransformComponent>();
                // The entity's own mesh, not the tool target: a lifted asset keeps drawing its asset.
                changes.push_back(
                     { in.Id, in.Committed, itc.Translation, itc.Rotation, itc.Scale, std::nullopt, false } );
            }
        }

        auto applied = Commands::ApplyXformEdit( label, changes, creates, deletes );
        if ( !applied.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{}: {}", label, applied.GetError() );
        LOG_INFO( "[Modeling] {0} of '{1}' by '{2}': {3} triangles, written to {4}, {5}", label, operated.Name,
                  other.Name, mesh->TriangleCount(), ToString( args.WriteTo ), ToString( args.Inputs ) );
        SelectionManager::SetSelection( args.WriteTo == BooleanWriteTo::Input
                                             ? std::vector<Common::UUID>{ operated.Id }
                                             : applied.GetValue() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Core
