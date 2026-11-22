#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>

// Landscape root / tile / spline and foliage blocks.

namespace Desert::Core::Serialize::Builtins
{

        // A LANDSCAPE ROOT: the frame through the hand-mapped block, then its target layers as `.delayerinfo`
        // references (LS-12b, SCNE 34) — [{Guid, Path}], the mesh block's shape: the GUID is the identity, the
        // path is for the reader and the refusal. UE: ALandscape's target layers -> ULandscapeLayerInfoObject.
        ComponentSerializer MakeLandscapeRoot()
        {
            ComponentSerializer s = MakeAuthored<ECS::LandscapeComponent>( "Landscape" );
            s.Serialize = []( ECS::Entity e, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&          landscape = e.GetComponent<ECS::LandscapeComponent>();
                Common::Json::Object block     = WriteComponent( landscape );
                // Written even when empty: an undo restores the whole block, and an absent key keeps the
                // current value, so an omitted empty list would leave a just-added layer in place after undo.
                Common::Json::Value::Array layers;
                layers.reserve( landscape.Layers.size() );
                for ( const Assets::AssetHandle& handle : landscape.Layers )
                {
                    const auto layer = assetManager.FindByHandle<Assets::LandscapeLayerInfoAsset>( handle );
                    if ( !layer )
                    {
                        // The reader dropped every GUID it could not resolve, so this is a handle set in memory
                        // that no asset carries; its GUID is not recoverable from the handle.
                        LOG_ERROR( "[Landscape] Entity '{}' names layer info handle {} that no asset carries; the "
                                   "layer is not saved",
                                   e.GetComponent<ECS::TagComponent>().Tag, static_cast<uint64_t>( handle ) );
                        continue;
                    }
                    layers.emplace_back(
                         Common::Json::ObjectBuilder()
                              .Set( "Guid", Common::Content::AssetGuidToText( layer->Guid() ) )
                              .Set( "Path", layer->GetMetadata()
                                                 .Filepath.lexically_normal()
                                                 .lexically_relative(
                                                      Common::Constants::Path::ASSETS_PATH.lexically_normal() )
                                                 .generic_string() )
                              .Build() );
                }
                block["Layers"] = Common::Json::Value( std::move( layers ) );
                return Common::Json::Value( std::move( block ) );
            };
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& block,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                if ( !block.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                auto& landscape = e.HasComponent<ECS::LandscapeComponent>()
                                       ? e.GetComponent<ECS::LandscapeComponent>()
                                       : e.AddComponent<ECS::LandscapeComponent>();
                ReadComponent( block, landscape, issues );
                const auto value = block.Find( "Layers" );
                if ( !value )
                    return; // absent: the list stays as it is (rule 1)
                if ( value->GetKind() != Common::Json::Kind::Array )
                {
                    issues.push_back( { value->Where().ToString(), "an array of {Guid, Path}", "not an array" } );
                    return;
                }
                auto&                            manager = const_cast<Assets::AssetManager&>( assetManager );
                std::vector<Assets::AssetHandle> read;
                std::optional<std::string>       refusal;
                value->ForEachElement(
                     [&]( std::size_t index, const Common::Json::Node& element )
                     {
                         if ( refusal )
                             return;
                         if ( element.GetKind() != Common::Json::Kind::Object || !element.Find( "Guid" ) )
                         {
                             refusal = std::format( "layer {} is not a {{Guid, Path}} reference", index );
                             return;
                         }
                         std::string guidText;
                         std::string path;
                         element.ReadInto( "Guid", guidText, issues );
                         element.ReadInto( "Path", path, issues );
                         const auto guid = Common::Content::AssetGuidFromText( guidText );
                         Assets::Asset<Assets::LandscapeLayerInfoAsset> layer;
                         if ( guid )
                         {
                             layer = manager.FindByHandle<Assets::LandscapeLayerInfoAsset>( Common::UUID(
                                  static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) ) );
                             if ( !layer )
                                 layer = Assets::CreateFromRegistryGuid<Assets::LandscapeLayerInfoAsset>(
                                      manager, guid.GetValue(), Common::Content::ContentKind::LandscapeLayerInfo );
                         }
                         if ( !layer )
                         {
                             // REFUSED, NOT SUBSTITUTED (FO-1's rule for a foliage type): neither a default layer
                             // nor a shorter list — either would re-key the tiles' weight planes to a list nobody
                             // authored. The whole list is refused with the GUID and the path.
                             refusal = std::format( "layer {}: GUID '{}' (path '{}') is not a .delayerinfo this "
                                                    "project has scanned",
                                                    index, guidText, path );
                             return;
                         }
                         const Assets::AssetHandle handle = layer->GetMetadata().Handle;
                         if ( std::find( read.begin(), read.end(), handle ) != read.end() )
                         {
                             refusal = std::format( "layer {} ('{}') is listed twice", index, path );
                             return;
                         }
                         read.push_back( handle );
                     } );
                if ( refusal )
                {
                    // Refused whole: the entity keeps no layers, the tiles keep their weights (reported as
                    // unknown layers and not drawn), and the load reports the GUID and path.
                    issues.push_back( { value->Where().ToString(),
                                        "a list of .delayerinfo the content registry knows", *refusal } );
                    LOG_ERROR( "[Landscape] Entity '{}': the target layer list is refused: {}",
                               e.GetComponent<ECS::TagComponent>().Tag, *refusal );
                    return;
                }
                landscape.Layers = std::move( read );
            };
            return s;
        }

        // A LANDSCAPE TILE: the hand-mapped block, and then the heights its `HeightFile` names.
        //
        // THE LOAD IS HERE, IN THE REGISTRY, AND NOT IN THE SCENE LOADER, because every path that rebuilds
        // an entity from its blocks comes through here — open, Play/Stop, Ctrl+C/Ctrl+V, delete-undo,
        // prefab instancing. A load in SceneSerializer alone would give a pasted tile a file name and no
        // terrain, which is the "middle link drops a property" shape.
        //
        // A tile that already holds heights decoded from THIS file keeps them: an undo restoring the block
        // in place must not replace the terrain in memory with an older copy from disk.
        //
        // A failure is REPORTED, by file and reason, and the tile is left without heights; it does not take
        // the entity or the scene with it.
        //
        // The key is spelled inside, in the MakeAuthored call, because that is the form the WorldPartition
        // census reads the registry's keys by.
        ComponentSerializer MakeLandscapeTile()
        {
            ComponentSerializer s = MakeAuthored<ECS::LandscapeTileComponent>( "LandscapeTile" );
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& block, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                if ( !block.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                if ( !e.HasComponent<ECS::LandscapeTileComponent>() )
                    e.AddComponent<ECS::LandscapeTileComponent>();

                // Through the tile's one writer (a patch), so the shared tile table sees a re-read block.
                ECS::EditLandscapeTile(
                     *e.GetRegistry(), e.GetHandle(),
                     [&block, &issues]( ECS::LandscapeTileComponent& comp )
                     {
                         const std::string before = comp.HeightFile;
                         ReadComponent( block, comp, issues );
                         if ( comp.Heights && comp.HeightFile == before )
                             return;

                         comp.Heights.reset();
                         if ( comp.HeightFile.empty() )
                         {
                             LOG_WARN( "[Landscape] tile ({0}, {1}) names no height file; it has no terrain until "
                                       "one is saved for it.",
                                       comp.TileX, comp.TileZ );
                             return;
                         }
                         // HeightFile is a content key ("assets:..."): resolved through the mounted assets root,
                         // so the same record reads in the editor and in a package (SceneSerializer's
                         // WriteLandscapeTiles).
                         auto loaded = World::Landscape::ReadLandscapeTileByKey( comp.HeightFile );
                         if ( !loaded )
                         {
                             LOG_ERROR( "[Landscape] tile ({0}, {1}) has no terrain: {2}", comp.TileX, comp.TileZ,
                                        loaded.GetError() );
                             return;
                         }
                         comp.Heights = loaded.ExtractValue();
                     } );
            };
            return s;
        }

    ComponentSerializer MakeLandscapeSpline()
    {
        return MakeAuthored<ECS::LandscapeSplineComponent>( "LandscapeSpline" );
    }

    void AddFoliageSerializers( std::vector<ComponentSerializer>& out )
    {

        // ---- Procedural foliage field: the volume that generated it (S1) ----
        // {Owner}: the generating entity's UUID (UE: the instances' ProceduralGuid). A field with this block is
        // rewritten by its volume's Resimulate and never painted into by the brush.
        {
            ComponentSerializer s;
            s.Key       = "ProceduralFoliageField";
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<ECS::ProceduralFoliageFieldComponent>(); };
            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& ) -> Common::Json::Value
            {
                Common::Json::ObjectBuilder out;
                out.Set( "Owner", static_cast<int64_t>( static_cast<uint64_t>(
                                       entity.GetComponent<ECS::ProceduralFoliageFieldComponent>().Owner ) ) );
                return { out.Build() };
            };
            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                if ( !g.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                int64_t owner = 0;
                g.ReadInto( "Owner", owner, issues );
                entity.AddComponent<ECS::ProceduralFoliageFieldComponent>().Owner =
                     Common::UUID( static_cast<uint64_t>( owner ) );
            };
            out.push_back( std::move( s ) );
        }

        // ---- Foliage field: the `.defoliage` it is painted with (FO-1, SCNE 33) ----
        // {FoliageTypeGuid, FoliageTypePath}, the mesh block's shape: the GUID is the identity, the path is for
        // the reader and the refusal. The scatter numbers used to be inline here; SceneMigrator moved them into
        // `.defoliage` files and a block of that shape is refused by the reader below, not read.
        {
            ComponentSerializer s;
            s.Key = "Foliage";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::FoliageComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                 foliage = entity.GetComponent<ECS::FoliageComponent>();
                Common::Json::ObjectBuilder out;
                if ( const auto type = assetManager.FindByHandle<Assets::FoliageTypeAsset>( foliage.FoliageType ) )
                {
                    out.Set( "FoliageTypeGuid", Common::Content::AssetGuidToText( type->Guid() ) );
                    out.Set( "FoliageTypePath",
                             Common::AssetHandle::StableKeyForPath( type->GetMetadata().Filepath ) );
                }
                else if ( foliage.FoliageType )
                {
                    LOG_ERROR( "[Foliage] Entity '{}' names foliage type handle {} that no loaded asset carries; "
                               "its reference is saved empty",
                               entity.GetComponent<ECS::TagComponent>().Tag,
                               static_cast<uint64_t>( foliage.FoliageType ) );
                }
                return { out.Build() };
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                if ( !g.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                std::string guidText;
                std::string path;
                g.ReadInto( "FoliageTypeGuid", guidText, issues );
                g.ReadInto( "FoliageTypePath", path, issues );
                // As SequencePath above: a label, refused by name in the bare form; the GUID binds.
                if ( !path.empty() && !Common::AssetHandle::IsProjectRelativeKey( path ) &&
                     !std::filesystem::path( path ).is_absolute() )
                    issues.push_back( Common::Json::Issue{ "Foliage.FoliageTypePath",
                                                           "a content key (\"assets:<path>\")", path } );

                auto& foliage = entity.AddComponent<ECS::FoliageComponent>();
                if ( guidText.empty() && path.empty() )
                    return; // A field whose type was never chosen: authored so, saved so.

                const auto guid    = Common::Content::AssetGuidFromText( guidText );
                auto&      manager = const_cast<Assets::AssetManager&>( assetManager );
                Assets::Asset<Assets::FoliageTypeAsset> type;
                if ( guid )
                {
                    type = manager.FindByHandle<Assets::FoliageTypeAsset>( Common::UUID(
                         static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) ) );
                    if ( !type )
                        type = Assets::CreateFromRegistryGuid<Assets::FoliageTypeAsset>(
                             manager, guid.GetValue(), Common::Content::ContentKind::FoliageType );
                }
                if ( !type )
                {
                    // REFUSED, NOT SUBSTITUTED: a field painted with defaults would look like the scene's grass
                    // while being nobody's.
                    issues.push_back(
                         Common::Json::Issue{ "Foliage.FoliageTypeGuid", "a .defoliage the content registry knows",
                                              std::format( "GUID '{}', path '{}'", guidText, path ) } );
                    LOG_ERROR(
                         "[Foliage] Entity '{}': foliage type GUID '{}' (path '{}') is not a .defoliage this "
                         "project has scanned; the field keeps no type",
                         entity.GetComponent<ECS::TagComponent>().Tag, guidText, path );
                    return;
                }
                foliage.FoliageType = type->GetMetadata().Handle;
            };

            out.push_back( std::move( s ) );
        }
    }

} // Desert::Core::Serialize::Builtins
