// THE BUILDERS AND THEIR NAMES — UE 5.5's UWorldPartitionHLODsBuilder (and its -DeleteHLODs),
// UWorldPartitionConvertCommandlet, UWorldPartitionResaveActorsBuilder and UWorldPartitionFoliageBuilder, as rows of
// one table. Each builder is the editor's own operation run on the open world, never a second copy of it: HLODs is
// the World Partition panel's Build HLODs (HLODBuild), Convert its Convert to World Partition, FoliageRepartition the
// foliage tool's repartition (FoliagePaintTool::RepartitionFoliage), and every save is SceneFiles::SaveOpenScene.
// The Minimap builder (UWorldPartitionMiniMapBuilder) is a row WP-MAP-VIEW adds here.

#include "WorldPartitionBuilder.hpp"

#include <Editor/Core/BuilderRunOptions.hpp>
#include <Editor/Panels/ViewportPanel/Tools/FoliagePaintTool.hpp>
#include <Editor/Panels/WorldPartition/HLODBuild.hpp>
#include <Editor/Panels/WorldPartition/WorldPartitionPanel.hpp>

#include <Engine/Core/Scene.hpp>
#include <Common/Core/Logger.hpp>

#include <array>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        // UE UWorldPartitionHLODsBuilder: every cell's HLOD of the world SAVED at the scene's file, its landscape
        // tiles baked on the GPU over the next frames (a hidden window renders them as a shown one does).
        class HLODsBuilder final : public IWorldPartitionBuilder
        {
        public:
            Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& /*cell*/ ) override
            {
                auto build = HLODBuild::Start( context.ScenePath, context.Scene->GetSceneName(), context.Assets );
                if ( !build )
                    return Common::MakeError<BuilderStep>( build.GetError() );
                m_Build = std::move( build.GetValue() );
                return Continue( context );
            }

            Common::ResultStr<BuilderStep> Continue( BuilderContext& /*context*/ ) override
            {
                const std::optional<Common::BoolResultStr> done = m_Build->Tick();
                if ( !done )
                    return Common::MakeSuccess( BuilderStep::Pending );
                m_Build.reset();
                if ( !*done )
                    return Common::MakeError<BuilderStep>( done->GetError() );
                return Common::MakeSuccess( BuilderStep::Done );
            }

        private:
            std::unique_ptr<HLODBuild> m_Build;
        };

        // UE UWorldPartitionHLODsBuilder -DeleteHLODs: the world's built HLOD files removed.
        class DeleteHLODsBuilder final : public IWorldPartitionBuilder
        {
        public:
            Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& /*cell*/ ) override
            {
                if ( auto deleted = DeleteBuiltHLODs( context.ScenePath ); !deleted )
                    return Common::MakeError<BuilderStep>( deleted.GetError() );
                return Common::MakeSuccess( BuilderStep::Done );
            }
        };

        // UE UWorldPartitionConvertCommandlet: the level becomes a partitioned world - the panel's Convert to World
        // Partition (its foliage refiled by the new grid) - and is saved, its entities into their own files.
        class ConvertBuilder final : public IWorldPartitionBuilder
        {
        public:
            Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& /*cell*/ ) override
            {
                if ( auto converted = ConvertToWorldPartition( context.Scene ); !converted )
                    return Common::MakeError<BuilderStep>( converted.GetError() );
                if ( auto saved = context.Save(); !saved )
                    return Common::MakeError<BuilderStep>( saved.GetError() );
                return Common::MakeSuccess( BuilderStep::Done );
            }
        };

        // UE UWorldPartitionResaveActorsBuilder: every entity of the world (or of each cell) written again through
        // the current serializer - each `.deent` rewritten when its bytes differ - or, with --only-dirty, only the
        // entities the load itself left out of date with their files.
        class ResaveEntitiesBuilder final : public IWorldPartitionBuilder
        {
        public:
            Common::BoolResultStr PreRun( BuilderContext& context ) override
            {
                if ( !context.Scene->GetWorldPartition() )
                    return Common::MakeError( std::format(
                         "'{}' is not a partitioned world: its entities live in the scene file, there is no .deent to "
                         "resave (run the Convert builder first)",
                         context.ScenePath.string() ) );
                return BOOLSUCCESS;
            }

            Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& /*cell*/ ) override
            {
                // TouchAll is the whole save (EntityPackages): every loaded record serialized, each file written
                // when its bytes differ; the records an iterative run left on disk are listed and not touched.
                if ( !context.Options->OnlyDirty )
                    context.Scene->Packages()->TouchAll();
                if ( auto saved = context.Save(); !saved )
                    return Common::MakeError<BuilderStep>( saved.GetError() );
                return Common::MakeSuccess( BuilderStep::Done );
            }
        };

        // UE UWorldPartitionFoliageBuilder: the foliage of an existing world refiled by its CURRENT grid, one field
        // per type per cell (after the grid's cell size changed), over the entire world, and saved.
        class FoliageRepartitionBuilder final : public IWorldPartitionBuilder
        {
        public:
            Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& /*cell*/ ) override
            {
                if ( auto filed = Tools::FoliagePaintTool::RepartitionFoliage( *context.Scene ); !filed )
                    return Common::MakeError<BuilderStep>( filed.GetError() );
                if ( auto saved = context.Save(); !saved )
                    return Common::MakeError<BuilderStep>( saved.GetError() );
                return Common::MakeSuccess( BuilderStep::Done );
            }
        };

        template <typename Builder>
        std::unique_ptr<IWorldPartitionBuilder> Make()
        {
            return std::make_unique<Builder>();
        }

        constexpr std::array kBuilders = {
             WorldPartitionBuilderRow{ "HLODs", "builds every cell's HLOD of the saved world (landscape baked on the GPU)",
                                       BuilderLoadingMode::Custom, false, false, &Make<HLODsBuilder> },
             WorldPartitionBuilderRow{ "DeleteHLODs", "removes the world's built HLOD files",
                                       BuilderLoadingMode::Custom, false, false, &Make<DeleteHLODsBuilder> },
             WorldPartitionBuilderRow{ "Convert", "converts the level to a partitioned world and saves it",
                                       BuilderLoadingMode::Custom, false, false, &Make<ConvertBuilder> },
             WorldPartitionBuilderRow{ "ResaveEntities",
                                       "rewrites every entity file of the world (--only-dirty: the out-of-date ones)",
                                       BuilderLoadingMode::EntireWorld, true, true, &Make<ResaveEntitiesBuilder> },
             WorldPartitionBuilderRow{ "FoliageRepartition", "refiles the world's foliage fields by its current grid",
                                       BuilderLoadingMode::EntireWorld, false, false, &Make<FoliageRepartitionBuilder> },
        };
    } // namespace

    std::span<const WorldPartitionBuilderRow> WorldPartitionBuilderRows()
    {
        return kBuilders;
    }
} // namespace Desert::Editor
