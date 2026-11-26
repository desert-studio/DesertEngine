#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>

// Foliage blocks (the landscape's are the Landscape module's: LandscapeComponentSerializers.cpp).

namespace Desert::Core::Serialize::Builtins
{

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

            // The type is the Foliage module's asset: the engine names it only by its content-registry row (the
            // GUID its handle is HandleForGuid of) and has the module announce it (ModuleHooks().AssetTypes).
            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& /*assetManager*/ ) -> Common::Json::Value
            {
                const auto&                 foliage = entity.GetComponent<ECS::FoliageComponent>();
                Common::Json::ObjectBuilder out;
                if ( !foliage.FoliageType )
                    return { out.Build() };
                const auto handle = static_cast<uint64_t>( foliage.FoliageType );
                if ( const auto guid = Assets::ContentRegistry::GuidForHandle( handle ); guid && !guid->IsNull() )
                {
                    out.Set( "FoliageTypeGuid", Common::Content::AssetGuidToText( *guid ) );
                    out.Set( "FoliageTypePath", Assets::ContentRegistry::KeyForHandle( handle ) );
                }
                else
                {
                    LOG_ERROR( "[Foliage] Entity '{}' names foliage type handle {} that no content-registry row "
                               "carries; its reference is saved empty",
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
                uint64_t   type    = 0;
                if ( guid )
                {
                    const auto handle = static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) );
                    for ( const Core::AssetAnnouncer& announcer : Core::ModuleHooks().AssetTypes )
                    {
                        if ( std::string_view( announcer.Type ) != "FoliageTypeAsset" )
                            continue;
                        if ( announcer.IsRecorded( manager, handle ) )
                            type = handle;
                        else
                            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::FoliageType ) )
                                if ( row.Guid && *row.Guid == guid.GetValue() )
                                {
                                    type = announcer.Announce( manager, row.Path );
                                    break;
                                }
                    }
                }
                if ( type == 0 )
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
                foliage.FoliageType = Assets::AssetHandle( type );
            };

            out.push_back( std::move( s ) );
        }
    }

} // namespace Desert::Core::Serialize::Builtins
