// Ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/MeshModelingTools/Public/CSGMeshesTool.h
// (UCSGMeshesToolProperties, UTrimMeshesToolProperties, the builder's two-input rule) and ModelingComponents
// BaseTools/BaseCreateFromSelectedTool.h (OutputWriteTo, HandleInputs), adapted: the preview session is
// MeshProcessingTool's (UE UMeshOpPreviewWithBackgroundCompute); the operation is RunMeshBoolean (the ported
// FMeshBoolean) in A's object space instead of FBooleanMeshesOp's world frame; Accept writes the result through
// Commands::ApplyXformEdit as one undo step; Hide Inputs is the entity's VisibilityComponent.

#include "MeshBooleanTool.hpp"
#include "ModelingToolTarget.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshXformOperations.hpp>
#include <Engine/Geometry/MeshDeformOperations.hpp>
#include <Engine/Geometry/MeshBooleanOperation.hpp>

#include <Common/Core/Logger.hpp>

#include <spdlog/fmt/fmt.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
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
            if ( !other.Mesh )
                return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}' has no mesh", name, other.Name );
            const glm::mat4 toOperated =
                 glm::inverse( operated.Entity.GetWorldTransform() ) * other.Entity.GetWorldTransform();
            auto moved = Geometry::TransformMesh( *other.Mesh, toOperated );
            if ( !moved.IsSuccess() )
                return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}' into the space of '{}': {}", name,
                                                                   other.Name, operated.Name, moved.GetError() );
            auto mesh = std::make_shared<Geometry::DynamicMesh3>( std::move( moved.ExtractValue().Mesh ) );
            if ( table )
            {
                const auto& slots = other.Entity.GetComponent<ECS::StaticMeshComponent>().GetMaterialSlots();
                auto        mapped =
                     Geometry::MapMaterialIds( *mesh,
                                               [&]( const int id )
                                               {
                                                   // A material ID past the slots reads as the empty slot, as the
                                                   // renderer draws it.
                                                   const Common::AssetHandle handle =
                                                        id >= 0 && static_cast<size_t>( id ) < slots.size()
                                                             ? slots[static_cast<size_t>( id )]
                                                             : Common::AssetHandle{};
                                                   auto it = std::find( table->begin(), table->end(), handle );
                                                   if ( it == table->end() )
                                                       it = table->insert( table->end(), handle );
                                                   return static_cast<int>( it - table->begin() );
                                               } );
                if ( !mapped.IsSuccess() )
                    return Common::MakeFormattedError<OperandMeshPtr>( "{}: '{}': {}", name, other.Name,
                                                                       mapped.GetError() );
            }
            return Common::MakeSuccess( OperandMeshPtr( std::move( mesh ) ) );
        }

        // A mesh whose material IDs index @p table keeps only the slots its triangles use, in table order, its IDs
        // renumbered to them (a Trim of B keeps B's triangles, whose IDs the session mapped into the pair's
        // table). An ID past the table reads as the empty slot, as the renderer draws it.
        Common::ResultStr<std::vector<Common::AssetHandle>>
        KeepUsedSlots( Geometry::DynamicMesh3& mesh, const std::vector<Common::AssetHandle>& table )
        {
            using Out = std::vector<Common::AssetHandle>;
            Out slots;
            for ( const int id : Geometry::DistinctMaterialIds( mesh ) )
            {
                const Common::AssetHandle handle = id >= 0 && static_cast<size_t>( id ) < table.size()
                                                        ? table[static_cast<size_t>( id )]
                                                        : Common::AssetHandle{};
                if ( std::find( slots.begin(), slots.end(), handle ) == slots.end() )
                    slots.push_back( handle );
            }
            auto mapped = Geometry::MapMaterialIds(
                 mesh,
                 [&]( const int id )
                 {
                     const Common::AssetHandle handle = id >= 0 && static_cast<size_t>( id ) < table.size()
                                                             ? table[static_cast<size_t>( id )]
                                                             : Common::AssetHandle{};
                     return static_cast<int>( std::find( slots.begin(), slots.end(), handle ) - slots.begin() );
                 } );
            if ( !mapped.IsSuccess() )
                return Common::MakeError<Out>( mapped.GetError() );
            return Common::MakeSuccess( std::move( slots ) );
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

    namespace
    {
        // Which input is operated on (the result belongs to it) and what MeshBoolean runs.
        bool OperatesOnB( BooleanTool tool, const BooleanToolArgs& args )
        {
            return tool == BooleanTool::Trim ? args.Trimmed == TrimTarget::TrimB
                                             : args.Operation == CsgOperation::DifferenceBA;
        }

        std::string OperationLabel( BooleanTool tool, const BooleanToolArgs& args )
        {
            return tool == BooleanTool::Trim
                        ? fmt::format( "{} {} {}", ToString( tool ), ToString( args.Trimmed ),
                                       ToString( args.Side ) )
                        : fmt::format( "{} {}", ToString( tool ), ToString( args.Operation ) );
        }
    } // namespace

    Common::ResultStr<Geometry::DeformOutcome> RunBooleanTool( BooleanTool tool, const BooleanToolArgs& args,
                                                               const SelectedMeshPair& inputs )
    {
        using Out        = Geometry::DeformOutcome;
        const bool bTrim = tool == BooleanTool::Trim;
        const bool bOnB  = OperatesOnB( tool, args );
        if ( !inputs.MeshA || !inputs.MeshBInA )
            return Common::MakeFormattedError<Out>( "{}: an input has no mesh", ToString( tool ) );
        Geometry::BooleanOperation operation = Geometry::BooleanOperation::Difference;
        if ( bTrim )
            operation = args.Side == TrimSide::RemoveInside ? Geometry::BooleanOperation::TrimInside
                                                            : Geometry::BooleanOperation::TrimOutside;
        else if ( args.Operation == CsgOperation::Intersect )
            operation = Geometry::BooleanOperation::Intersect;
        else if ( args.Operation == CsgOperation::Union )
            operation = Geometry::BooleanOperation::Union;

        const Geometry::DynamicMesh3& target     = bOnB ? *inputs.MeshBInA : *inputs.MeshA;
        const Geometry::DynamicMesh3& cutter     = bOnB ? *inputs.MeshA : *inputs.MeshBInA;
        const std::string&            targetName = bOnB ? inputs.NameB : inputs.NameA;
        const std::string&            cutterName = bOnB ? inputs.NameA : inputs.NameB;
        const std::string             label      = OperationLabel( tool, args );
        auto                          result     = Geometry::RunMeshBoolean( operation, target, cutter );
        if ( !result.IsSuccess() )
            return Common::MakeFormattedError<Out>( "{} of '{}' by '{}': {}", label, targetName, cutterName,
                                                    result.GetError() );
        Out out;
        out.Mesh   = *result.GetValue().Mesh;
        out.Report = fmt::format( "{} of '{}' by '{}': {} triangles", label, targetName, cutterName,
                                  out.Mesh.TriangleCount() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<std::vector<Common::UUID>> CommitBooleanTool( ::Desert::Core::Scene& scene, BooleanTool tool,
                                                                    const BooleanToolArgs&  args,
                                                                    const SelectedMeshPair& inputs,
                                                                    const OperandMeshPtr&   resultInA )
    {
        using Out               = std::vector<Common::UUID>;
        const std::string label = OperationLabel( tool, args );
        const auto        refA  = scene.FindEntityByID( inputs.A );
        const auto        refB  = scene.FindEntityByID( inputs.B );
        if ( !refA || !refB )
            return Common::MakeFormattedError<Out>( "{}: an input is no longer in the scene", label );
        if ( !resultInA )
            return Common::MakeFormattedError<Out>( "{}: no result to write", label );
        const bool         bOnB     = OperatesOnB( tool, args );
        const ECS::Entity  eA       = refA->get();
        const ECS::Entity  eB       = refB->get();
        const ECS::Entity& operated = bOnB ? eB : eA;
        const Common::UUID opId     = bOnB ? inputs.B : inputs.A;
        const std::string& opName   = bOnB ? inputs.NameB : inputs.NameA;

        // The result was computed in A's space, its material IDs indexing the pair's table (A's slots, then B's
        // not among them - UE UCSGMeshesTool's combined material set). B operated takes it in its own space.
        OperandMeshPtr                                  mesh = resultInA;
        std::optional<std::vector<Common::AssetHandle>> slots;
        if ( tool == BooleanTool::Boolean )
            slots = inputs.MaterialSlots; // both meshes' triangles: the whole table
        if ( bOnB )
        {
            auto moved = Geometry::TransformMesh( *resultInA, glm::inverse( eB.GetWorldTransform() ) *
                                                                   eA.GetWorldTransform() );
            if ( !moved.IsSuccess() )
                return Common::MakeFormattedError<Out>( "{}: the result into the space of '{}': {}", label, opName,
                                                        moved.GetError() );
            auto movedMesh = std::make_shared<Geometry::DynamicMesh3>( std::move( moved.ExtractValue().Mesh ) );
            if ( tool == BooleanTool::Trim )
            {
                // A Trim of B keeps only B's triangles: B keeps the slots they use.
                auto kept = KeepUsedSlots( *movedMesh, inputs.MaterialSlots );
                if ( !kept.IsSuccess() )
                    return Common::MakeFormattedError<Out>( "{}: the materials of '{}': {}", label, opName,
                                                            kept.GetError() );
                slots = kept.ExtractValue();
            }
            mesh = std::move( movedMesh );
        }
        // A Trim of A keeps A's triangles with their own IDs (the table starts with A's slots): A's slots stay.

        const auto&                tc = operated.GetComponent<ECS::TransformComponent>();
        Commands::XformEntityState state{ opId,          mesh,         tc.GetTranslation(), tc.GetRotation(),
                                          tc.GetScale(), std::nullopt, std::nullopt };
        state.MaterialSlots = std::move( slots );

        std::vector<Commands::XformEntityState> changes;
        std::vector<Commands::XformNewEntity>   creates;
        std::vector<Common::UUID>               deletes;
        if ( args.WriteTo == BooleanWriteTo::Input )
            changes.push_back( std::move( state ) );
        else
            creates.push_back( { opId, fmt::format( "{} {}", opName, ToString( tool ) ), std::move( state ) } );
        const struct
        {
            Common::UUID   Id;
            ECS::Entity    Entity;
            OperandMeshPtr Committed;
        } both[2] = { { inputs.A, eA, inputs.CommittedA }, { inputs.B, eB, inputs.CommittedB } };
        for ( const auto& in : both )
        {
            if ( args.WriteTo == BooleanWriteTo::Input && in.Id == opId )
                continue;
            if ( args.Inputs == BooleanInputs::Delete )
                deletes.push_back( in.Id );
            else if ( args.Inputs == BooleanInputs::Hide )
            {
                const auto& itc = in.Entity.GetComponent<ECS::TransformComponent>();
                // The entity's own mesh, not the tool target: a lifted asset keeps drawing its asset.
                changes.push_back( { in.Id, in.Committed, itc.GetTranslation(), itc.GetRotation(), itc.GetScale(),
                                     std::nullopt, false } );
            }
        }

        auto applied = Commands::ApplyXformEdit( fmt::format( "Modeling: {}", label ), changes, creates, deletes );
        if ( !applied.IsSuccess() )
            return Common::MakeFormattedError<Out>( "{}: {}", label, applied.GetError() );
        LOG_INFO( "[Modeling] {0} accepted on '{1}': {2} triangles, written to {3}, {4}", label, opName,
                  mesh->TriangleCount(), ToString( args.WriteTo ), ToString( args.Inputs ) );
        Out result = args.WriteTo == BooleanWriteTo::Input ? Out{ opId } : applied.GetValue();
        SelectionManager::SetSelection( result );
        return Common::MakeSuccess( std::move( result ) );
    }

    Common::ResultStr<SelectedMeshPair> SelectedPairInSpaceOfA( ::Desert::Core::Scene& scene, const char* toolName,
                                                                const bool remapMaterials )
    {
        auto inputs = SelectedInputs( scene, toolName );
        if ( !inputs.IsSuccess() )
            return Common::MakeError<SelectedMeshPair>( inputs.GetError() );
        const std::array<Input, 2>& ab = inputs.GetValue();
        if ( !ab[0].Mesh )
            return Common::MakeFormattedError<SelectedMeshPair>( "{}: '{}' has no mesh", toolName, ab[0].Name );
        std::vector<Common::AssetHandle> table;
        if ( remapMaterials )
            table = ab[0].Entity.GetComponent<ECS::StaticMeshComponent>().GetMaterialSlots();
        auto b = BakeInto( ab[0], ab[1], remapMaterials ? &table : nullptr, toolName );
        if ( !b.IsSuccess() )
            return Common::MakeError<SelectedMeshPair>( b.GetError() );
        return Common::MakeSuccess( SelectedMeshPair{ ab[0].Id, ab[1].Id, ab[0].Name, ab[1].Name, ab[0].Mesh,
                                                      b.ExtractValue(), ab[0].Committed, ab[1].Committed,
                                                      std::move( table ) } );
    }
} // namespace Desert::Editor::Core
