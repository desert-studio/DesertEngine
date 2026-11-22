#include <Engine/Core/Serialize/ComponentSerializerKit.hpp>

// Static / instanced / skinned mesh and material-slot blocks (asset-bearing: handle <-> GUID + path).

namespace Desert::Core::Serialize::Builtins
{
    void AddMeshSerializers( std::vector<ComponentSerializer>& out )
    {
        // ---- Static Mesh (asset-bearing: handle <-> path) ----
        {
            ComponentSerializer s;
            s.Key             = "StaticMesh";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::StaticMeshComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::StaticMeshComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                    smc = entity.GetComponent<ECS::StaticMeshComponent>();
                Assets::StaticMeshComponentSer meshSer;

                // MeshHandle and MaterialSlots are the component's REFLECTED fields and are written by reflection,
                // {Guid, Path} each (UE's UPROPERTY StaticMesh / OverrideMaterials); the mirror carries the rest.
                const auto* type = Reflection::ReflectionRegistry::Get().Find( "StaticMeshComponent" );
                if ( type == nullptr )
                {
                    LOG_ERROR( "[ComponentRegistry] StaticMeshComponent is not reflected - its mesh and materials "
                               "cannot be saved" );
                    return { Common::Json::Object{} };
                }
                auto                 resolver = MakeAssetResolver( assetManager );
                Common::Json::Object out      = Reflection::SerializeReflected( *type, &smc, &resolver );
                meshSer.Primitive             = smc.GetPrimitive();

                // Rendering controls: write only non-default values (absent = default on load).
                if ( smc.GetOutlineDraw() )
                    meshSer.OutlineDraw = smc.GetOutlineDraw();
                if ( smc.GetForcedLOD() >= 0 )
                    meshSer.ForcedLOD = smc.GetForcedLOD();
                if ( smc.GetLODBias() != 0 )
                    meshSer.LODBias = smc.GetLODBias();
                if ( !smc.GetCastShadows() )
                    meshSer.CastShadows = smc.GetCastShadows();
                if ( !smc.GetReceiveShadows() )
                    meshSer.ReceiveShadows = smc.GetReceiveShadows();
                if ( !smc.GetCastContactShadow() )
                    meshSer.CastContactShadow = smc.GetCastContactShadow();
                if ( smc.GetTranslucencySortPriority() != 0 )
                    meshSer.TranslucencySortPriority = smc.GetTranslucencySortPriority();
                if ( smc.GetMaxDrawDistance() > 0.0f )
                    meshSer.MaxDrawDistance = smc.GetMaxDrawDistance();
                if ( smc.GetHiddenSubmeshes() != 0 )
                    meshSer.HiddenSubmeshes = smc.GetHiddenSubmeshes();

                // The SOURCE is saved, never the render mesh derived from it (see EditableMesh.hpp).
                if ( smc.GetEditableMesh() )
                    meshSer.EditMesh = Geometry::ToSerialized( *smc.GetEditableMesh() );

                const Common::Json::Value rest = Common::Json::FromStruct( meshSer );
                if ( const auto* fields = std::get_if<Common::Json::Object>( &rest.variant() ) )
                    for ( const auto& [name, value] : *fields )
                        if ( name != "MeshHandle" && name != "MaterialSlots" ) // reflection's keys, above
                            out[name] = value;
                return Common::Json::Value( std::move( out ) );
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                const auto* type = Reflection::ReflectionRegistry::Get().Find( "StaticMeshComponent" );
                if ( type == nullptr )
                {
                    LOG_ERROR( "[ComponentRegistry] StaticMeshComponent is not reflected - its block cannot be "
                               "read" );
                    return;
                }
                auto parsed = ReadBlock<Assets::StaticMeshComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& meshData = parsed.value();

                auto& smc      = entity.AddComponent<ECS::StaticMeshComponent>();
                auto  resolver = MakeAssetResolver( assetManager );
                // MeshHandle / MaterialSlots: the reflected fields, read by reflection ({Guid, Path} each).
                Reflection::DeserializeReflected( *type, &smc, g, issues, &resolver );
                // The rest of the value is built on a copy and stated once through the writer below.
                ECS::StaticMeshDesc desc = smc.GetDesc();
                desc.Primitive           = meshData.Primitive;

                desc.OutlineDraw       = meshData.OutlineDraw.value_or( desc.OutlineDraw );
                desc.ForcedLOD         = meshData.ForcedLOD.value_or( desc.ForcedLOD );
                desc.LODBias           = meshData.LODBias.value_or( desc.LODBias );
                desc.CastShadows       = meshData.CastShadows.value_or( desc.CastShadows );
                desc.ReceiveShadows    = meshData.ReceiveShadows.value_or( desc.ReceiveShadows );
                desc.CastContactShadow = meshData.CastContactShadow.value_or( desc.CastContactShadow );
                desc.TranslucencySortPriority =
                     meshData.TranslucencySortPriority.value_or( desc.TranslucencySortPriority );
                desc.MaxDrawDistance = meshData.MaxDrawDistance.value_or( desc.MaxDrawDistance );
                desc.HiddenSubmeshes = meshData.HiddenSubmeshes.value_or( desc.HiddenSubmeshes );

                if ( meshData.EditMesh )
                {
                    // A refusal DROPS the geometry, and says so with the entity's name: the component still
                    // loads (asset handle, materials, flags), so the entity falls back to what it names
                    // besides the edited mesh rather than the whole scene failing to open.
                    const std::string tag    = entity.HasComponent<ECS::TagComponent>()
                                                    ? entity.GetComponent<ECS::TagComponent>().Tag
                                                    : std::string( "Entity" );
                    auto              loaded = Geometry::DynamicMeshFromSerialized( *meshData.EditMesh, tag );
                    if ( !loaded.IsSuccess() )
                    {
                        LOG_ERROR( "[Scene] entity '{0}': its edited mesh could not be read and was DROPPED: {1}",
                                   tag, loaded.GetError() );
                    }
                    else if ( auto set = ECS::SetEditableMesh(
                                   desc, std::make_shared<const Geometry::DynamicMesh3>( loaded.ExtractValue() ) );
                              !set.IsSuccess() )
                    {
                        LOG_ERROR(
                             "[Scene] entity '{0}': its edited mesh was read but not built, and was DROPPED: {1}",
                             tag, set.GetError() );
                    }
                }
                ECS::SetStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
            };

            out.push_back( std::move( s ) );
        }

        // ---- Instanced Static Mesh (UE-style ISM: asset/primitive mesh + N world matrices) ----
        {
            ComponentSerializer s;
            s.Key             = "InstancedStaticMesh";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::InstancedStaticMeshComponentSer>();
            s.Has = []( ECS::Entity e ) { return e.HasComponent<ECS::InstancedStaticMeshComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto& ism = entity.GetComponent<ECS::InstancedStaticMeshComponent>();
                Assets::InstancedStaticMeshComponentSer ser;

                auto resolver = MakeAssetResolver( assetManager );
                if ( ism.GetMeshHandle() )
                {
                    if ( auto p =
                              resolver.ToPath( static_cast<uint64_t>( ism.GetMeshHandle() ), "StaticMeshAsset" );
                         !p.empty() )
                        ser.MeshPath = p;
                    ser.MeshGuid =
                         MeshGuidText( resolver, static_cast<uint64_t>( ism.GetMeshHandle() ), "StaticMeshAsset" );
                }

                if ( !ism.GetMaterialSlots().empty() )
                {
                    ser.MaterialPaths = std::vector<std::string>{};
                    ser.MaterialGuids = std::vector<std::string>{};
                    for ( auto handle : ism.GetMaterialSlots() )
                    {
                        ser.MaterialPaths->push_back(
                             resolver.ToPath( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                        ser.MaterialGuids->push_back(
                             resolver.ToGuid( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                    }
                }
                ser.Primitive = ism.GetPrimitive();
                if ( !ism.GetCastShadows() )
                    ser.CastShadows = ism.GetCastShadows();
                if ( !ism.GetCastContactShadow() )
                    ser.CastContactShadow = ism.GetCastContactShadow();
                if ( ism.GetMaxDrawDistance() > 0.0f )
                    ser.MaxDrawDistance = ism.GetMaxDrawDistance();
                if ( !ism.GetInstanceTransforms().empty() )
                {
                    std::vector<std::array<float, 16>> flat;
                    flat.reserve( ism.GetInstanceTransforms().size() );
                    for ( const auto& m : ism.GetInstanceTransforms() )
                    {
                        std::array<float, 16> a{};
                        std::memcpy( a.data(), &m[0][0], sizeof( float ) * 16 );
                        flat.push_back( a );
                    }
                    ser.InstanceTransforms = std::move( flat );
                }

                return Common::Json::FromStruct( ser );
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::InstancedStaticMeshComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& data = parsed.value();

                ECS::InstancedStaticMeshDesc desc;
                auto                         resolver = MakeAssetResolver( assetManager );

                const uint64_t meshHandle =
                     ResolveMeshRef( resolver, data.MeshGuid, data.MeshPath, "StaticMeshAsset" );
                if ( meshHandle != 0 )
                {
                    desc.MeshHandle = Common::UUID( meshHandle );
                }

                size_t slotCount = 0;
                if ( data.MaterialGuids )
                {
                    slotCount = data.MaterialGuids->size();
                }
                else if ( data.MaterialPaths )
                {
                    slotCount = data.MaterialPaths->size();
                }
                if ( slotCount > 0 )
                {
                    desc.MaterialSlots.reserve( slotCount );
                    for ( size_t i = 0; i < slotCount; ++i )
                    {
                        desc.MaterialSlots.emplace_back(
                             ResolveSlotRef( resolver, data.MaterialGuids, data.MaterialPaths, i ) );
                    }
                }
                desc.Primitive         = data.Primitive;
                desc.CastShadows       = data.CastShadows.value_or( desc.CastShadows );
                desc.CastContactShadow = data.CastContactShadow.value_or( desc.CastContactShadow );
                desc.MaxDrawDistance   = data.MaxDrawDistance.value_or( desc.MaxDrawDistance );
                if ( data.InstanceTransforms.has_value() )
                {
                    desc.InstanceTransforms.reserve( data.InstanceTransforms->size() );
                    for ( const auto& a : *data.InstanceTransforms )
                    {
                        glm::mat4 m( 1.0f );
                        std::memcpy( &m[0][0], a.data(), sizeof( float ) * 16 );
                        desc.InstanceTransforms.push_back( m );
                    }
                }
                ECS::SetInstancedStaticMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
            };

            out.push_back( std::move( s ) );
        }

        // ---- Material (generic data-driven: shader name + param overrides + texture refs as paths) ----
        {
            ComponentSerializer s;
            s.Key             = "Material";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::MaterialComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::MaterialComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                  mc = entity.GetComponent<ECS::MaterialComponent>();
                Assets::MaterialComponentSer ser;
                if ( !mc.GetShaderName().empty() )
                {
                    // BY GUID (SCNE 31): the runtime binds by name, the file names the shader's identity.
                    const std::string context = EntityContext( entity );
                    if ( auto ref = Assets::FindShaderRefByName( assetManager, mc.GetShaderName(),
                                                                 { "shader", "Material.Shader", context } ) )
                        ser.Shader = ref.ExtractValue();
                    else
                        LOG_ERROR( "[Scene] {} - the shader slot is written EMPTY", ref.GetError() );
                }

                if ( !mc.GetParams().empty() )
                {
                    std::vector<Assets::MaterialParamSer> ps;
                    for ( const auto& p : mc.GetParams() )
                        ps.push_back( { p.Name, p.Value } );
                    ser.Params = std::move( ps );
                }

                if ( !mc.GetTextures().empty() )
                {
                    // The handle alone (DC §4.2). This used to write the resolved path AND the handle into
                    // every entry; see MaterialTextureSer for why that pairing is worse than either half.
                    const auto                              resolver = MakeAssetResolver( assetManager );
                    std::vector<Assets::MaterialTextureSer> ts;
                    ts.reserve( mc.GetTextures().size() );
                    for ( const auto& t : mc.GetTextures() )
                        ts.push_back( { t.Name, resolver.ToGuid( t.TextureHandle, "TextureAsset" ),
                                        resolver.ToPath( t.TextureHandle, "TextureAsset" ) } );
                    ser.Textures = std::move( ts );
                }

                return Common::Json::FromStruct( ser );
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::MaterialComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& data = parsed.value();

                // Built as a value and written once, through ECS::SetMaterial (the component's one writer).
                ECS::MaterialDesc mc;
                if ( data.Shader.has_value() )
                {
                    const std::string context = EntityContext( entity );
                    // An override only: a reference naming the StandardSurface template is REFUSED with its path,
                    // not dropped (Assets::FindOverrideShaderNameByRef); the entity keeps no shader, not a guess.
                    auto name = Assets::FindOverrideShaderNameByRef( assetManager, *data.Shader,
                                                                     { "shader", "Material.Shader", context } );
                    if ( !name )
                    {
                        issues.push_back( Common::Json::Issue{
                             "Material.Shader", "an override template's {Guid, Path}", name.GetError() } );
                        LOG_ERROR( "[Scene] {} - the entity draws with no shader until it names one",
                                   name.GetError() );
                    }
                    else
                        mc.ShaderName = name.ExtractValue();
                }

                // Params and Textures are the named shader's and nothing else's. With no shader they were the
                // old slot-0 hand-off buffer, which is gone: a lit mesh's values are its material slots'. The
                // block is REFUSED with its path, not quietly folded into slot 0 (no migration exists, since
                // the corpus carries none).
                const bool hasValues = ( data.Params.has_value() && !data.Params->empty() ) ||
                                       ( data.Textures.has_value() && !data.Textures->empty() );
                if ( mc.ShaderName.empty() && hasValues )
                {
                    issues.push_back( Common::Json::Issue{ "Material.Params", "a Shader that owns them",
                                                           "parameters with no Shader" } );
                    LOG_ERROR(
                         "[Scene] {}: Material.Params/Textures with no Shader - parameters belong to a Shader "
                         "Override, a lit mesh's values are its material slots'; the values are not loaded",
                         EntityContext( entity ) );
                    if ( data.Shader.has_value() )
                        ECS::SetMaterial( *entity.GetRegistry(), entity.GetHandle(), mc );
                    return;
                }

                if ( data.Params.has_value() )
                    for ( const auto& p : *data.Params )
                        mc.Params.push_back( { p.Name, p.Value } );

                if ( data.Textures.has_value() )
                {
                    auto resolver = MakeAssetResolver( assetManager );
                    for ( const auto& t : *data.Textures )
                    {
                        // ONE reader for one written value. The path fallback that used to sit under this
                        // line is gone with the field it read (DC §4.2) — and it was the dangerous half:
                        // a handle that resolves to nothing is a slot that ends up empty and says so,
                        // while a stale path that still resolves puts the WRONG texture on the surface.
                        //
                        // FromGuid names what it could not find, so a miss here is not silent; that is the
                        // §1.4 obligation this reference class acquired when it became the only spelling.
                        mc.Textures.push_back( { t.Name, ResolveGuidRef( resolver, t.Guid, t.Path, "TextureAsset",
                                                                         "material texture '" + t.Name + "'" ) } );
                    }
                }
                ECS::SetMaterial( *entity.GetRegistry(), entity.GetHandle(), mc );
            };

            out.push_back( std::move( s ) );
        }

        // ---- Skinned Mesh (asset-bearing) ----
        {
            ComponentSerializer s;
            s.Key             = "SkinnedMesh";
            s.DeclaredMembers = Common::Json::MemberNames<Assets::SkinnedMeshComponentSer>();
            s.Has             = []( ECS::Entity e ) { return e.HasComponent<ECS::SkinnedMeshComponent>(); };

            s.Serialize = []( ECS::Entity entity, const Assets::AssetManager& assetManager ) -> Common::Json::Value
            {
                const auto&                     smc = entity.GetComponent<ECS::SkinnedMeshComponent>();
                Assets::SkinnedMeshComponentSer meshSer;

                auto resolver = MakeAssetResolver( assetManager );
                if ( smc.GetMeshHandle() )
                {
                    if ( auto p =
                              resolver.ToPath( static_cast<uint64_t>( smc.GetMeshHandle() ), "SkinnedMeshAsset" );
                         !p.empty() )
                        meshSer.MeshPath = p;
                    meshSer.MeshGuid = MeshGuidText( resolver, static_cast<uint64_t>( smc.GetMeshHandle() ),
                                                     "SkinnedMeshAsset" );
                }

                if ( !smc.GetMaterialSlots().empty() )
                {
                    meshSer.MaterialPaths = std::vector<std::string>{};
                    meshSer.MaterialGuids = std::vector<std::string>{};
                    for ( auto handle : smc.GetMaterialSlots() )
                    {
                        meshSer.MaterialPaths->push_back(
                             resolver.ToPath( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                        meshSer.MaterialGuids->push_back(
                             resolver.ToGuid( static_cast<uint64_t>( handle ), "MaterialAsset" ) );
                    }
                }

                // Rendering controls: write only non-default values (absent = default on load), exactly
                // like the static mesh above.
                if ( !smc.GetCastShadows() )
                    meshSer.CastShadows = smc.GetCastShadows();

                return Common::Json::FromStruct( meshSer );
            };

            s.Deserialize = []( ECS::Entity entity, const Common::Json::Node& g,
                                const Assets::AssetManager& assetManager, Common::Json::Issues& issues )
            {
                auto parsed = ReadBlock<Assets::SkinnedMeshComponentSer>( g, issues );
                if ( !parsed.has_value() )
                    return;
                const auto& meshData = parsed.value();

                ECS::SkinnedMeshDesc desc;
                auto                 resolver = MakeAssetResolver( assetManager );

                const uint64_t meshHandle =
                     ResolveMeshRef( resolver, meshData.MeshGuid, meshData.MeshPath, "SkinnedMeshAsset" );
                if ( meshHandle != 0 )
                    desc.MeshHandle = Common::UUID( meshHandle );

                const size_t slotCount = meshData.MaterialGuids
                                              ? meshData.MaterialGuids->size()
                                              : ( meshData.MaterialPaths ? meshData.MaterialPaths->size() : 0 );
                if ( slotCount > 0 )
                {
                    desc.MaterialSlots.reserve( slotCount );
                    for ( size_t i = 0; i < slotCount; ++i )
                    {
                        desc.MaterialSlots.push_back( Common::UUID(
                             ResolveSlotRef( resolver, meshData.MaterialGuids, meshData.MaterialPaths, i ) ) );
                    }
                }

                desc.CastShadows = meshData.CastShadows.value_or( desc.CastShadows );
                ECS::SetSkinnedMesh( *entity.GetRegistry(), entity.GetHandle(), desc );
            };

            out.push_back( std::move( s ) );
        }
    }

} // namespace Desert::Core::Serialize::Builtins
