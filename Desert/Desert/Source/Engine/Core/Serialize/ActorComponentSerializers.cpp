#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>

// Hand-written actor, UI, animation, editor-state and authored blocks.

namespace Desert::Core::Serialize::Builtins
{
    void AddActorSerializers( std::vector<ComponentSerializer>& out )
    {

        // ---- UI Anim (custom: the sequence travels as the TMLN block — the one timeline format — and the
        //      playhead is runtime-only and never written) ----
        {
            ComponentSerializer s;
            s.Key             = "UIAnim";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::UIAnimComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::UIAnimComponent>(); };
            s.Serialize       = []( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
            {
                const auto& d       = e.GetComponent<ECS::UIAnimComponent>().Data;
                auto        written = Animation::Timeline::WriteSequence( d.Sequence );
                if ( !written )
                {
                    // A sequence the writer refuses is named here rather than saved as an empty clip.
                    LOG_ERROR( "UIAnim: the TMLN writer refused the sequence: {}", written.GetError() );
                    return Common::Json::Value{};
                }
                const std::vector<uint8_t> bytes = written.ExtractValue();
                auto block = Common::Json::Read<Common::Json::Value>( Common::TextOf( bytes ) );
                if ( !block )
                {
                    // WriteSequence's own output not reading back as JSON is a broken writer, not a state of the
                    // data; it is named here rather than saved as an empty clip.
                    LOG_ERROR( "UIAnim: the TMLN writer produced text that does not read back: {}",
                               block.GetError() );
                    return Common::Json::Value{};
                }
                Assets::UIAnimComponentSer ser;
                ser.Sequence = block.GetValue();
                ser.Loop     = static_cast<int>( d.Loop );
                ser.AutoPlay = d.AutoPlay;
                return Common::Json::FromStruct( ser );
            };
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& g, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::UIAnimComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& d = parsed.value();
                if ( d.Loop < 0 || d.Loop > static_cast<int>( Animation::Timeline::LoopMode::PingPong ) )
                {
                    issues.push_back( { g.Where().Key( "Loop" ).ToString(),
                                        "a LoopMode (0 Once, 1 Loop, 2 PingPong)", std::format( "{}", d.Loop ) } );
                    return;
                }
                const std::string text     = Common::Json::Write( d.Sequence );
                auto              sequence = Animation::Timeline::ReadSequence( Common::BytesOf( text ) );
                if ( !sequence )
                {
                    issues.push_back(
                         { g.Where().Key( "Sequence" ).ToString(), "a TMLN sequence", sequence.GetError() } );
                    return;
                }
                if ( sequence.GetValue().Host != Animation::Timeline::SequenceHost::UIAnimation )
                {
                    issues.push_back( { g.Where().Key( "Sequence" ).ToString(), "a sequence hosted as UIAnimation",
                                        Animation::Timeline::ToString( sequence.GetValue().Host ) } );
                    return;
                }
                auto& ac         = e.HasComponent<ECS::UIAnimComponent>() ? e.GetComponent<ECS::UIAnimComponent>()
                                                                          : e.AddComponent<ECS::UIAnimComponent>();
                ac.Data.Sequence = sequence.ExtractValue();
                ac.Data.Loop     = static_cast<Animation::Timeline::LoopMode>( d.Loop );
                ac.Data.AutoPlay = d.AutoPlay;
                ac.Data.Playback.reset(); // the range may have changed; the next frame re-creates the player
            };
            out.push_back( std::move( s ) );
        }

        // ---- Text (custom: only the authored fields; the glyph mesh is transient) ----
        {
            ComponentSerializer s;
            s.Key             = "Text";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::TextComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::TextComponent>(); };
            s.Serialize       = []( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
            {
                const auto& tc = e.GetComponent<ECS::TextComponent>();
                // The font is an asset HANDLE in memory and persists as the ROOT-TAGGED KEY its handle is
                // the FNV of — the same form the reflected UIText.Font slot above writes, through the
                // same two functions, so the world-space and UI text routes cannot drift apart.
                const std::string fontKey =
                     ServiceKeyForPath( Runtime::ResourceRegistry::GetFontService()->PathForHandle(
                          static_cast<uint64_t>( tc.Font ) ) );
                Assets::TextComponentSer ser{ tc.Text,     fontKey, tc.Color, tc.Size, tc.EmissiveIntensity,
                                              tc.Billboard };
                return Common::Json::FromStruct( ser );
            };
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& g, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::TextComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& d  = parsed.value();
                auto&       tc = e.HasComponent<ECS::TextComponent>() ? e.GetComponent<ECS::TextComponent>()
                                                                      : e.AddComponent<ECS::TextComponent>();
                tc.Text        = d.Text;
                // Path -> stable handle (registers it so the handle resolves at render time). Empty stays null,
                // which the render path falls back to the default font for.
                tc.Font = Assets::AssetHandle(
                     Runtime::ResourceRegistry::GetFontService()->RegisterFont( ServicePathForKey( d.Font ) ) );
                tc.Color             = d.Color;
                tc.Size              = d.Size;
                tc.EmissiveIntensity = d.EmissiveIntensity;
                tc.Billboard         = d.Billboard;
            };
            out.push_back( std::move( s ) );
        }

        // ---- Animation (manual: playback settings + the `.danimgraph` this entity plays) ----
        {
            ComponentSerializer s;
            s.Key             = "Animation";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::AnimationComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::AnimationComponent>(); };
            s.Serialize = []( ECS::Entity e, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                   ac = e.GetComponent<ECS::AnimationComponent>();
                Assets::AnimationComponentSer ser;
                ser.CurrentClip             = ac.CurrentClip;
                ser.Playing                 = ac.Playing;
                ser.Loop                    = ac.Loop;
                ser.PlaybackSpeed           = ac.PlaybackSpeed;
                ser.UpdateAnimationInEditor = ac.UpdateAnimationInEditor;
                ser.RootMotion              = ac.RootMotion;

                // THE HANDLE, NOT THE GRAPH. `Animation::Graph::Serialize(*ac.Graph)` stood here and put
                // the whole state machine inside the entity; the file is the graph's identity now and the
                // scene only names it. A handle the resolver cannot place writes NOTHING rather than an
                // empty string: absence is how this format says "no state machine".
                if ( ac.GraphAsset )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    if ( auto path = resolver.ToPath( static_cast<uint64_t>( ac.GraphAsset ), "AnimGraphAsset" );
                         !path.empty() )
                    {
                        ser.Graph = std::move( path );
                    }
                }
                // The linked layers, by path like the graph; one the resolver cannot place is dropped with
                // the rest of the unresolvable handles, never written as an empty string.
                if ( !ac.LinkedLayerGraphs.empty() )
                {
                    auto                     resolver = MakeAssetResolver( assetManager );
                    std::vector<std::string> paths;
                    for ( const Assets::AssetHandle linked : ac.LinkedLayerGraphs )
                        if ( auto path = resolver.ToPath( static_cast<uint64_t>( linked ), "AnimGraphAsset" );
                             !path.empty() )
                            paths.push_back( std::move( path ) );
                    if ( !paths.empty() )
                        ser.LinkedLayers = std::move( paths );
                }
                return Common::Json::FromStruct( ser );
            };
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::AnimationComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& d = parsed.value();
                auto& ac = e.HasComponent<ECS::AnimationComponent>() ? e.GetComponent<ECS::AnimationComponent>()
                                                                     : e.AddComponent<ECS::AnimationComponent>();
                ac.CurrentClip             = d.CurrentClip;
                ac.Playing                 = d.Playing;
                ac.Loop                    = d.Loop;
                ac.PlaybackSpeed           = d.PlaybackSpeed;
                ac.UpdateAnimationInEditor = d.UpdateAnimationInEditor;
                ac.RootMotion              = d.RootMotion;

                // ONLY THE HANDLE IS SET HERE. The graph OBJECT is AnimationECSSystem's to hand over
                // (SyncAnimGraph), from the asset, so that every entity naming one file ends up pointing
                // at one object — parsing it here would give each entity a copy and put us back where
                // GraphJson was. FromPath loads the asset, so a graph the scene names and the project
                // cannot produce is reported by the resolver rather than becoming a silently empty slot.
                if ( d.Graph.has_value() && !d.Graph->empty() )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    ac.GraphAsset = Assets::AssetHandle( resolver.FromPath( *d.Graph, "AnimGraphAsset" ) );
                }
                ac.LinkedLayerGraphs.clear();
                if ( d.LinkedLayers.has_value() )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    for ( const std::string& path : *d.LinkedLayers )
                        ac.LinkedLayerGraphs.emplace_back( resolver.FromPath( path, "AnimGraphAsset" ) );
                }
            };
            out.push_back( std::move( s ) );
        }
    }

    void AddUIRenderTextureSerializer( std::vector<ComponentSerializer>& out )
    {
        {
            // BY GUID (SCNE 31), like a material's shader: the runtime field is the path the host opens, the
            // file states `"Scene": {Guid, Path}` - the scene's header GUID, and its key for the reader.
            // Everything else in the block is the reflected data as it was.
            ComponentSerializer s = MakeReflected<ECS::UIRenderTextureComponent, UI::UIRenderTextureData>(
                 "UIRenderTexture", "UIRenderTextureData", &ECS::UIRenderTextureComponent::Data );
            s.Serialize = [reflected =
                                s.Serialize]( ECS::Entity                 entity,
                                              const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const Common::Json::Value block = reflected( entity, assetManager );
                const auto& path = entity.GetComponent<ECS::UIRenderTextureComponent>().Data.ScenePath;
                Common::Json::ObjectBuilder out;
                if ( !path.empty() )
                {
                    const std::string context = EntityContext( entity );
                    if ( auto ref = SceneRefForPath( path, { "scene", "UIRenderTexture.Scene", context } ) )
                        out.Set( "Scene", Common::Json::FromStruct( ref.GetValue() ) );
                    else
                        LOG_ERROR( "[Scene] {} - the scene slot is written EMPTY", ref.GetError() );
                }
                Common::Json::Root( block ).ForEachMember(
                     [&]( std::string_view key, const Common::Json::Node& value )
                     {
                         if ( key != "ScenePath" )
                             out.Set( key, value.Raw() );
                     } );
                return { std::move( out ).Build() };
            };
            s.Deserialize = [reflected = s.Deserialize]( ECS::Entity entity, const Common::Json::Node& g,
                                                         const Assets::AssetManager& assetManager,
                                                         Common::Json::Issues&       issues )
            {
                if ( !g.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                // `ScenePath` is not read: a SCNE 31 file names its scene by GUID, and the version gate
                // refuses an older file before any block is read.
                Common::Json::ObjectBuilder rest;
                g.ForEachMember(
                     [&]( std::string_view key, const Common::Json::Node& value )
                     {
                         if ( key != "Scene" && key != "ScenePath" )
                             rest.Set( key, value.Raw() );
                     } );
                const Common::Json::Value restBlock( std::move( rest ).Build() );
                reflected( entity, Common::Json::Root( restBlock, g.Where() ), assetManager, issues );

                auto& data       = entity.GetComponent<ECS::UIRenderTextureComponent>().Data;
                data.ScenePath   = {};
                const auto scene = g.Find( "Scene" );
                if ( !scene )
                    return; // no scene, which the element draws as the magenta error fill
                // A {Guid, Path} of the wrong shape is an Issue at `...UIRenderTexture.Scene` in the entity's
                // one error line, never an empty GUID that reads as "no scene".
                const auto ref = Assets::ReadAssetGuidRef( *scene, issues );
                if ( !ref )
                    return;
                const std::string context = EntityContext( entity );
                const auto        path    = ScenePathForRef( *ref, { "scene", "UIRenderTexture.Scene", context } );
                if ( path )
                    data.ScenePath = path.GetValue();
                else
                    LOG_ERROR( "[Scene] {} - the element draws the magenta error fill", path.GetError() );
            };
            out.push_back( std::move( s ) );
        }
    }

    void AddEditorAndSequenceSerializers( std::vector<ComponentSerializer>& out )
    {

        // The voxels a Cube Grid blockout was baked from, so the tool reopens on it
        // (CubeGridBlockoutComponent.hpp). No version bump - an added key is what ForeignKeys is for.
        {
            ComponentSerializer s;
            s.Key       = "CubeGridBlockout";
            s.Has       = []( ECS::Entity e ) { return e.HasComponent<ECS::CubeGridBlockoutComponent>(); };
            s.Serialize = []( ECS::Entity e, const Assets::AssetManager& ) -> Common::Json::Value
            { return ECS::WriteCubeGridBlockout( e.GetComponent<ECS::CubeGridBlockoutComponent>() ); };
            s.Deserialize = []( ECS::Entity e, const Common::Json::Node& block, const Assets::AssetManager&,
                                Common::Json::Issues& issues )
            {
                if ( !block.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                if ( auto read = ECS::ReadCubeGridBlockout( block, issues ) )
                {
                    if ( e.HasComponent<ECS::CubeGridBlockoutComponent>() )
                        e.GetComponent<ECS::CubeGridBlockoutComponent>() = std::move( *read );
                    else
                        e.AddComponent<ECS::CubeGridBlockoutComponent>( std::move( *read ) );
                }
            };
            out.push_back( std::move( s ) );
        }

        // ---- Marker components (presence is the state) ----
        out.push_back( MakeMarker<ECS::FolderComponent>( "Folder" ) );
        // The authoring lock. Serialized for the reason Components.hpp gives: a lock that does not
        // survive a reload protects nothing. No version bump — an added key is what ForeignKeys is for.
        out.push_back( MakeMarker<ECS::LockComponent>( "Lock" ) );
        // World Partition's author override (Components.hpp, AlwaysLoadedComponent). Serialized because its
        // whole effect is on the file: the partitioner reads the key, not the ECS. No version bump - an
        // added key is what ForeignKeys is for.
        out.push_back( MakeMarker<ECS::AlwaysLoadedComponent>( "AlwaysLoaded" ) );

        // ---- Single-flag components ----
        // The outliner's eye, for the same reason the lock beside it is here: a hidden object that comes
        // back visible on the next load is a setting the user made and the file never kept, and nothing
        // says so — the entity simply reappears. It was the lock's defect, already live for visibility,
        // in the same panel and one row up. No version bump: an added key is what ForeignKeys is for.
        out.push_back( MakeFlag<ECS::VisibilityComponent>(
             "Visibility", "Visible", &ECS::VisibilityComponent::IsVisible, &ECS::SetVisible ) );

        // ---- Level sequence actor: the `.dseq` it plays + playback settings + binding overrides (I11) ----
        // {SequenceGuid, SequencePath, Loop, AutoPlay, BindingOverrides[{Binding, Entity}]}: the Foliage shape
        // for the reference (GUID is the identity, path for the reader and the refusal).
        {
            ComponentSerializer s;
            s.Key = "LevelSequence";
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::LevelSequenceComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                 actor = entity.GetComponent<ECS::LevelSequenceComponent>();
                Common::Json::ObjectBuilder out;
                if ( const auto sequence =
                          assetManager.FindByHandle<Assets::LevelSequenceAsset>( actor.Sequence ) )
                {
                    out.Set( "SequenceGuid", Common::Content::AssetGuidToText( sequence->Guid() ) );
                    out.Set( "SequencePath",
                             Common::AssetHandle::StableKeyForPath( sequence->GetMetadata().Filepath ) );
                }
                else if ( static_cast<uint64_t>( actor.Sequence ) != 0 )
                {
                    LOG_ERROR(
                         "[LevelSequence] Entity '{}' names sequence handle {} that no loaded asset carries; "
                         "its reference is saved empty",
                         entity.GetComponent<ECS::TagComponent>().Tag, static_cast<uint64_t>( actor.Sequence ) );
                }
                // LoopMode is stored BY NAME ("Once" / "Loop" / "PingPong"): the scene is text a person reads.
                out.Set( "Loop", actor.Loop );
                out.Set( "AutoPlay", actor.AutoPlay );
                out.Set( "PlayRate", actor.PlayRate );
                Common::Json::Value::Array overrides;
                for ( const auto& over : actor.BindingOverrides )
                    overrides.emplace_back(
                         Common::Json::ObjectBuilder()
                              .Set( "Binding", Common::Content::AssetGuidToText( over.Binding.Value ) )
                              .Set( "Entity", over.Entity )
                              .Build() );
                out.Set( "BindingOverrides", Common::Json::Value( std::move( overrides ) ) );
                return { out.Build() };
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                if ( !g.ExpectKind( Common::Json::Kind::Object, issues ) )
                    return;
                std::string guidText;
                std::string path;
                g.ReadInto( "SequenceGuid", guidText, issues );
                g.ReadInto( "SequencePath", path, issues );
                // The path is the reader's label; the GUID is the identity. A label in the bare form is refused
                // by name (PATH1) — the actor still binds by GUID.
                if ( !path.empty() && !Common::AssetHandle::IsProjectRelativeKey( path ) &&
                     !std::filesystem::path( path ).is_absolute() )
                    issues.push_back( Common::Json::Issue{ "LevelSequence.SequencePath",
                                                           "a content key (\"assets:<path>\")", path } );

                auto& actor = entity.AddComponent<ECS::LevelSequenceComponent>();
                g.ReadInto( "Loop", actor.Loop, issues ); // an unknown name is a named issue, the default kept
                g.ReadInto( "AutoPlay", actor.AutoPlay, issues );
                g.ReadInto( "PlayRate", actor.PlayRate, issues );
                if ( const auto overrides = g.Find( "BindingOverrides" ) )
                    overrides->ForEachElement(
                         [&]( std::size_t, const Common::Json::Node& element )
                         {
                             std::string                       bindingText;
                             ECS::LevelSequenceBindingOverride over;
                             element.ReadInto( "Binding", bindingText, issues );
                             element.ReadInto( "Entity", over.Entity, issues );
                             const auto binding = Common::Content::AssetGuidFromText( bindingText );
                             if ( !binding )
                             {
                                 issues.push_back( Common::Json::Issue{ element.Where().ToString(),
                                                                        "a binding GUID (32 hex)", bindingText } );
                                 return;
                             }
                             over.Binding.Value = binding.GetValue();
                             actor.BindingOverrides.push_back( over );
                         } );
                if ( guidText.empty() && path.empty() )
                    return; // an actor whose sequence was never chosen: authored so, saved so

                const auto guid = Common::Content::AssetGuidFromText( guidText );
                // A deserializer is handed the registry as const, and creating the asset it names is a write
                // (the same cast the FoliageType row below and FromPath make).
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                auto& manager = const_cast<Assets::AssetManager&>( assetManager );
                Assets::Asset<Assets::LevelSequenceAsset> sequence;
                if ( guid )
                {
                    sequence = manager.FindByHandle<Assets::LevelSequenceAsset>( Common::UUID(
                         static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) ) );
                    if ( !sequence )
                        sequence = Assets::CreateFromRegistryGuid<Assets::LevelSequenceAsset>(
                             manager, guid.GetValue(), Common::Content::ContentKind::LevelSequence );
                }
                if ( !sequence )
                {
                    // REFUSED, NOT SUBSTITUTED: an actor playing nothing would look authored.
                    issues.push_back(
                         Common::Json::Issue{ "LevelSequence.SequenceGuid", "a .dseq the content registry knows",
                                              std::format( "GUID '{}', path '{}'", guidText, path ) } );
                    LOG_ERROR( "[LevelSequence] Entity '{}': sequence GUID '{}' (path '{}') is not a .dseq this "
                               "project has scanned; the actor plays nothing",
                               entity.GetComponent<ECS::TagComponent>().Tag, guidText, path );
                    return;
                }
                actor.Sequence = sequence->GetMetadata().Handle;
            };

            out.push_back( std::move( s ) );
        }
    }

    void AddAuthoredSerializers( std::vector<ComponentSerializer>& out )
    {

        // ---- Hand-mapped authored blocks (no reflected Data; see AuthoredComponentIO.hpp) ----
        // U13. Five components with a full Details editor and no row in this table: everything the
        // artist typed into them was discarded by the next load, and by every Ctrl+C, every delete-undo,
        // every prefab instancing and every Play/Stop, because all of those are this same registry.
        // Desert/Tests/Engine/ComponentPersistence now derives "is authored in Details" from the editor's
        // own registration source and refuses to let a sixth one exist. No version bump: an added key is
        // what ForeignKeys is for, and no scene in this repository carries these blocks yet — nothing
        // ever wrote one. (Projectile, the fifth, is a reflected block now: ProjectileData.)
        out.push_back( MakeAuthored<ECS::MorphComponent>( "Morph" ) );
        out.push_back( MakeAuthored<ECS::SocketAttachmentComponent>( "SocketAttachment" ) );
    }

} // namespace Desert::Core::Serialize::Builtins
