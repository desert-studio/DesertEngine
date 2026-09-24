#include "MeshXformOperations.hpp"
#include "ModelingToolTarget.hpp"

#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <Common/Core/Logger.hpp>

#include <fmt/format.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    namespace
    {
        using MeshPtr = std::shared_ptr<const Geometry::FDynamicMesh3>;

        struct Target
        {
            Common::UUID                              Id;
            ECS::Entity                               Entity;
            std::shared_ptr<const Geometry::EditMesh> Mesh; // Bridge::EditMeshView of the entity's mesh
            Geometry::Trs                             Local;
            std::string                               Name;
        };

        Common::ResultStr<std::vector<Target>> SelectedTargets( ::Desert::Core::Scene& scene, XformOperation op,
                                                                bool refuseChildren )
        {
            using Out             = std::vector<Target>;
            const auto& selection = SelectionManager::GetSelection();
            if ( selection.empty() )
                return Common::MakeFormattedError<Out>( "{}: select one or more entities with an editable mesh",
                                                        ToString( op ) );
            Out out;
            for ( const Common::UUID& id : selection )
            {
                auto ref = scene.FindEntityByID( id );
                if ( !ref )
                    return Common::MakeFormattedError<Out>( "{}: entity {} is not in the scene", ToString( op ),
                                                            static_cast<uint64_t>( id ) );
                ECS::Entity e = ref->get();
                if ( !e.HasComponent<ECS::StaticMeshComponent>() || !e.HasComponent<ECS::TransformComponent>() )
                    return Common::MakeFormattedError<Out>( "{}: entity {} has no static mesh or no transform",
                                                            ToString( op ), static_cast<uint64_t>( id ) );
                if ( refuseChildren && e.HasComponent<ECS::RelationshipComponent>() &&
                     !e.GetComponent<ECS::RelationshipComponent>().Children.empty() )
                    return Common::MakeFormattedError<Out>(
                         "{}: entity {} has {} children - their world transforms would move with it; detach them "
                         "first",
                         ToString( op ), static_cast<uint64_t>( id ),
                         e.GetComponent<ECS::RelationshipComponent>().Children.size() );
                auto target = GetToolTargetMesh( e.GetComponent<ECS::StaticMeshComponent>() );
                if ( !target.IsSuccess() )
                    return Common::MakeFormattedError<Out>( "{}: entity {}: {}", ToString( op ),
                                                            static_cast<uint64_t>( id ), target.GetError() );
                auto view = Geometry::Bridge::EditMeshView( target.GetValue().Mesh );
                if ( !view.IsSuccess() )
                    return Common::MakeFormattedError<Out>( "{}: entity {}: {}", ToString( op ),
                                                            static_cast<uint64_t>( id ), view.GetError() );
                const auto& tc = e.GetComponent<ECS::TransformComponent>();
                out.push_back( { id, e, view.ExtractValue(),
                                 Geometry::Trs{ tc.Translation, tc.Rotation, tc.Scale },
                                 e.HasComponent<ECS::TagComponent>() ? e.GetComponent<ECS::TagComponent>().Tag
                                                                     : std::string( "Mesh" ) } );
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        Commands::XformEntityState StateOf( const Common::UUID& id, MeshPtr mesh, const Geometry::Trs& local )
        {
            return { id, std::move( mesh ), local.Translation, local.Rotation, local.Scale, std::nullopt };
        }

        // Merge's material table: the target's slots first, then every other part's slots not already there;
        // part k's material i becomes the index of its slot in the table. A material ID past a part's slots
        // reads as the empty slot, as the renderer draws it.
        std::vector<int> RemapInto( std::vector<Common::AssetHandle>&       table,
                                    const std::vector<Common::AssetHandle>& slots, const Geometry::EditMesh& mesh )
        {
            int top = 0;
            for ( const int t : mesh.TriangleIds() )
                top = std::max( top, mesh.Attributes().GetMaterialId( t ) );
            std::vector<int> remap( std::max( slots.size(), static_cast<size_t>( top ) + 1 ) );
            for ( size_t i = 0; i < remap.size(); ++i )
            {
                const Common::AssetHandle handle = i < slots.size() ? slots[i] : Common::AssetHandle{};
                auto                      it     = std::find( table.begin(), table.end(), handle );
                if ( it == table.end() )
                {
                    table.push_back( handle );
                    it = table.end() - 1;
                }
                remap[i] = static_cast<int>( it - table.begin() );
            }
            return remap;
        }

        void SelectResult( const std::vector<Target>& kept, const std::vector<Common::UUID>& created )
        {
            std::vector<Common::UUID> ids;
            for ( const Target& t : kept )
                ids.push_back( t.Id );
            ids.insert( ids.end(), created.begin(), created.end() );
            SelectionManager::SetSelection( std::move( ids ) );
        }
    } // namespace

    const char* ToString( XformOperation operation )
    {
        switch ( operation )
        {
            case XformOperation::EditPivot:
                return "Edit Pivot";
            case XformOperation::BakeTransform:
                return "Bake Transform";
            case XformOperation::Merge:
                return "Merge";
            case XformOperation::Split:
                return "Split";
            case XformOperation::Pattern:
                return "Pattern";
        }
        return "?";
    }

    XformArgs XformArgsFromModelingState()
    {
        const auto& ms = ModelingState::Get();
        XformArgs   args;
        args.Pivot           = ms.XformPivot;
        args.PivotWorldPoint = ms.XformPivotWorldPoint;
        args.Bake            = ms.XformBake;
        args.Split           = ms.XformSplit;
        args.Pattern         = ms.XformPattern;
        args.PatternSeparate = ms.XformPatternSeparate;
        return args;
    }

    Common::BoolResultStr ApplyXformOperation( ::Desert::Core::Scene& scene, XformOperation op,
                                               const XformArgs& args )
    {
        const char* name = ToString( op );
        const bool  refuseChildren =
             op == XformOperation::EditPivot || op == XformOperation::BakeTransform || op == XformOperation::Merge;
        auto selected = SelectedTargets( scene, op, refuseChildren );
        if ( !selected.IsSuccess() )
            return Common::MakeError<bool>( selected.GetError() );
        const std::vector<Target>& targets = selected.GetValue();

        std::vector<Commands::XformEntityState> changes;
        std::vector<Commands::XformNewEntity>   creates;
        std::vector<Common::UUID>               deletes;
        std::vector<Target>                     kept   = targets;
        auto                                    refuse = [&]( const Target& t, const std::string& why )
        { return Common::MakeFormattedError<bool>( "{} on '{}': {}", name, t.Name, why ); };
        // Every result goes back onto the ported core; the first conversion refused refuses the whole edit
        // below, before anything is applied.
        std::string shareError;
        auto        Share = [&]( Geometry::EditMesh&& mesh ) -> MeshPtr
        {
            auto converted = Geometry::Bridge::FromEditMesh( std::move( mesh ) );
            if ( converted.IsSuccess() )
                return converted.ExtractValue();
            if ( shareError.empty() )
                shareError = converted.GetError();
            return nullptr;
        };

        switch ( op )
        {
            case XformOperation::EditPivot:
                for ( const Target& t : targets )
                {
                    // The world choices go through the WORLD transform: a child's pivot lands on the world point.
                    auto pivot = Geometry::ResolvePivot( *t.Mesh, args.Pivot, t.Entity.GetWorldTransform(),
                                                         args.PivotWorldPoint );
                    if ( !pivot.IsSuccess() )
                        return refuse( t, pivot.GetError() );
                    auto out = Geometry::EditPivot( *t.Mesh, t.Local, pivot.GetValue() );
                    if ( !out.IsSuccess() )
                        return refuse( t, out.GetError() );
                    LOG_INFO( "[Modeling] {0} on '{1}' ({2}): {3}", name, t.Name, Geometry::ToString( args.Pivot ),
                              out.GetValue().Report );
                    const Geometry::Trs local = out.GetValue().Transform;
                    changes.push_back( StateOf( t.Id, Share( std::move( out.ExtractValue().Mesh ) ), local ) );
                }
                break;
            case XformOperation::BakeTransform:
                for ( const Target& t : targets )
                {
                    auto out = Geometry::BakeTransform( *t.Mesh, t.Local, args.Bake );
                    if ( !out.IsSuccess() )
                        return refuse( t, out.GetError() );
                    LOG_INFO( "[Modeling] {0} on '{1}': {2}", name, t.Name, out.GetValue().Report );
                    const Geometry::Trs local = out.GetValue().Transform;
                    changes.push_back( StateOf( t.Id, Share( std::move( out.ExtractValue().Mesh ) ), local ) );
                }
                break;
            case XformOperation::Merge:
            {
                if ( targets.size() < 2 )
                    return Common::MakeFormattedError<bool>( "{}: select at least two entities, not {}", name,
                                                             targets.size() );
                const Target&                    into   = targets.front();
                const glm::mat4                  toInto = glm::inverse( into.Entity.GetWorldTransform() );
                std::vector<Common::AssetHandle> table =
                     into.Entity.GetComponent<ECS::StaticMeshComponent>().MaterialSlots;
                std::vector<Geometry::MergePart> parts;
                for ( const Target& t : targets )
                    parts.push_back(
                         { *t.Mesh, toInto * t.Entity.GetWorldTransform(),
                           RemapInto( table, t.Entity.GetComponent<ECS::StaticMeshComponent>().MaterialSlots,
                                      *t.Mesh ) } );
                auto merged = Geometry::MergeMeshes( parts );
                if ( !merged.IsSuccess() )
                    return refuse( into, merged.GetError() );
                if ( !merged.GetValue().Report.empty() )
                    LOG_WARN( "[Modeling] {0} into '{1}': {2}", name, into.Name, merged.GetValue().Report );
                changes.push_back(
                     StateOf( into.Id, Share( std::move( merged.ExtractValue().Mesh ) ), into.Local ) );
                changes.back().MaterialSlots = std::move( table );
                for ( size_t i = 1; i < targets.size(); ++i )
                    deletes.push_back( targets[i].Id );
                kept.resize( 1 );
                break;
            }
            case XformOperation::Split:
                for ( const Target& t : targets )
                {
                    auto parts = Geometry::SplitMesh( *t.Mesh, args.Split );
                    if ( !parts.IsSuccess() )
                        return refuse( t, parts.GetError() );
                    std::vector<Geometry::EditMesh> meshes = parts.ExtractValue();
                    LOG_INFO( "[Modeling] {0} '{1}' by {2}: {3} parts", name, t.Name,
                              Geometry::ToString( args.Split ), meshes.size() );
                    changes.push_back( StateOf( t.Id, Share( std::move( meshes[0] ) ), t.Local ) );
                    for ( size_t i = 1; i < meshes.size(); ++i )
                        creates.push_back( { t.Id, fmt::format( "{} Part {}", t.Name, i ),
                                             StateOf( t.Id, Share( std::move( meshes[i] ) ), t.Local ) } );
                }
                break;
            case XformOperation::Pattern:
            {
                auto transforms = Geometry::PatternTransforms( args.Pattern );
                if ( !transforms.IsSuccess() )
                    return Common::MakeError<bool>( transforms.GetError() );
                const std::vector<glm::mat4>& copies = transforms.GetValue();
                for ( const Target& t : targets )
                {
                    if ( !args.PatternSeparate )
                    {
                        std::vector<Geometry::MergePart> parts;
                        for ( const glm::mat4& m : copies )
                            parts.push_back( { *t.Mesh, m, {} } );
                        auto merged = Geometry::MergeMeshes( parts );
                        if ( !merged.IsSuccess() )
                            return refuse( t, merged.GetError() );
                        changes.push_back(
                             StateOf( t.Id, Share( std::move( merged.ExtractValue().Mesh ) ), t.Local ) );
                        continue;
                    }
                    // Copy 0 is the identity: the source stays as it is and the others are new entities.
                    for ( size_t i = 1; i < copies.size(); ++i )
                    {
                        auto copy = Geometry::TransformMesh( *t.Mesh, copies[i] );
                        if ( !copy.IsSuccess() )
                            return refuse( t, copy.GetError() );
                        creates.push_back(
                             { t.Id, fmt::format( "{} Pattern {}", t.Name, i ),
                               StateOf( t.Id, Share( std::move( copy.ExtractValue().Mesh ) ), t.Local ) } );
                    }
                }
                LOG_INFO( "[Modeling] {0} ({1}): {2} copies of {3} entities{4}", name,
                          Geometry::ToString( args.Pattern.Shape ), copies.size(), targets.size(),
                          args.PatternSeparate ? ", as new entities" : ", merged" );
                break;
            }
        }

        if ( !shareError.empty() )
            return Common::MakeFormattedError<bool>( "{}: {}", name, shareError );
        auto applied = Commands::ApplyXformEdit( fmt::format( "Modeling: {}", name ), changes, creates, deletes );
        if ( !applied.IsSuccess() )
            return Common::MakeError<bool>( applied.GetError() );
        SelectResult( kept, applied.GetValue() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Core
