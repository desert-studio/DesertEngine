#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Engine/Assets/Mapper.hpp>

#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>

#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    void MaterialFactory::ApplyPBRAsset( MaterialPBR& material, const Assets::SurfaceMaterialAsset& asset )
    {
        // Build the backend's typed view from the material canon (single protocol -> optimized
        // hot-path struct). No per-parameter setters.
        material.Data() = Assets::PBRSurfaceParams::FromMaterialData( asset.Data() );

        // Resolve texture handles to images and (re)bind them to the shader's sampler slots.
        auto resolveImage = []( Assets::AssetHandle handle ) -> Graphic::Image2D*
        {
            auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( handle );
            if ( !tex )
                return nullptr;
            return static_cast<Graphic::Image2D*>(
                Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) );
        };

        auto bindTexture = [&]( const Assets::AssetHandle& handle, const char* shaderName )
        {
            // AN UNSET SLOT IS AN AUTHORED DECISION, AND IT IS NOW EXECUTED RATHER THAN SKIPPED. This
            // used to `return` here, which is correct only for a material being built from scratch: a
            // material is also RE-APPLIED in place (AssetHotReload re-runs the factory over the live
            // object when the `.demat` changes), and then skipping meant the descriptor kept the texture
            // the file had just stopped naming. Binding the schema's default is what makes clearing a
            // slot in the editor reach the picture at all.
            if ( static_cast<uint64_t>( handle ) == 0 )
            {
                material.BindSchemaDefaultTexture( shaderName );
                return;
            }

            if ( auto* img = resolveImage( handle ) )
            {
                if ( auto* prop = material.Get<Texture2DProperty>( shaderName ) )
                    prop->SetImage( img );
                return;
            }

            // The handle names a texture nobody has: the slot falls back to its schema default so the
            // surface at least shows what an EMPTY slot shows, rather than the previous material's map.
            // The error below is what says the difference out loud.
            material.BindSchemaDefaultTexture( shaderName );

            // DC §1.4, and this is THE site the rule was written for. A `.demat` names its textures by
            // number and by nothing else, so a reference that stops resolving produces a surface that is
            // merely untextured — no missing file, no failed load, nothing in the log that a search can
            // start from. This line is the only place that knows all three of: which material, which slot,
            // and which number.
            //
            // It replaces an unconditional LOG_INFO that printed `resolved=true` for every successful bind
            // of every material every time a mesh was built, which is how a real `resolved=false` went
            // unread. A message that fires on success is not a message.
            LOG_ERROR( "[Materials] '{0}' names texture handle {1} in its '{2}' slot and no texture with "
                       "that handle is registered, so the surface draws UNTEXTURED. A texture's handle is "
                       "AssetHandle::FromCookedPath of its source image, so this usually means the image "
                       "was renamed, moved, or cooked before the derivation changed; re-cook it (Assets > "
                       "Rebuild Cooked Assets) and re-assign the slot.",
                       asset.GetMetadata().Filepath.string(), static_cast<uint64_t>( handle ), shaderName );
        };

        bindTexture( material.Data().AlbedoTexture, "u_AlbedoTexture" );
        bindTexture( material.Data().NormalTexture, "u_NormalTexture" );
        bindTexture( material.Data().OpacityTexture, "u_OpacityTexture" );
    }

    void MaterialFactory::ApplyShaderAsset( DataDrivenMaterial& material, const Assets::SurfaceMaterialAsset& asset )
    {
        // Seed schema defaults, then overlay the asset's persisted parameter values.
        material.ApplyDefaults();

        const auto& data = asset.Data();
        for ( const auto& p : data.Params )
            material.SetParamRaw( p.Name, p.Value );

        // `MaterialData::Textures` holds the sampler slots only since MATL 3 (the cloud material's asset
        // slots have a list of their own), but the SHADER SCHEMA still says what each name is: a `Texture2D`
        // sampler, a `TextureCube` (bound by MaterialSkybox, not here), or a non-texture asset reference a
        // different service consumes. Asking the schema is what lets the miss below be an ERROR, not noise.
        const auto& schema   = material.GetSchema();
        const auto  paramFor = [&schema]( const std::string& name ) -> const Core::Formats::ShaderParam*
        {
            for ( const auto& p : schema.Params )
                if ( p.Name == name )
                    return &p;
            return nullptr;
        };

        // THE LOOP IS OVER THE SCHEMA, NOT OVER THE FILE, and that is М9's change. Iterating
        // `data.Textures` visits only the slots a material HAS something to say about, so the two cases
        // that matter most were unreachable from it: a slot the material never mentioned, and a slot the
        // material has just STOPPED mentioning. This function is also re-run over a LIVE material when
        // the `.demat` changes (AssetHotReload), so "not mentioned" had to mean "leave whatever is bound"
        // — the file could lose a texture and the surface would keep drawing it. Every 2D sampler the
        // shader declares is now given a value on every application: the authored one, or the schema's
        // own default.
        for ( const auto& param : schema.Params )
        {
            // A non-texture asset reference (its service reads it out of MaterialData directly) or a cube
            // slot (bound by MaterialSkybox from the environment, not from here). Neither is this loop's.
            if ( !param.IsTexture || param.IsCubeTexture || param.IsAssetRef() )
                continue;

            const uint64_t handle = data.GetTexture( param.Name );
            if ( handle == 0 )
            {
                // An empty slot, whether it was never filled or has just been cleared. Not a failure and
                // not silent in the sense DC §1.4 forbids: the value the sampler gets is the one the
                // SHADER declares for an empty slot, so nothing is being substituted behind anybody.
                material.SetTexture( param.Name, nullptr );
                continue;
            }

            auto* tex = Runtime::ResourceRegistry::GetTextureService()->Get( Common::UUID( handle ) );
            auto* img = tex ? static_cast<Graphic::Image2D*>(
                                   Runtime::ResourceRegistry::GetImageService()->Resolve( tex->GetImageHandle() ) )
                            : nullptr;
            if ( img )
            {
                material.SetTexture( param.Name, img );
                continue;
            }

            // DC §1.4 — the same obligation, and the same wording, as the PBR path above. Both `continue`s
            // this replaces were silent, and the visible result of either was a shader sampling its
            // fallback: a surface that looks authored rather than broken.
            material.SetTexture( param.Name, nullptr );
            LOG_ERROR( "[Materials] '{0}' names texture handle {1} in its '{2}' slot and no texture with "
                       "that handle is registered, so '{3}' samples that slot's schema default instead. A "
                       "texture's handle is HandleForGuid of its .detex header GUID, which the slot names, so "
                       "this means that texture asset is not in the project (deleted, or never imported); "
                       "re-import it or re-assign the slot.",
                       asset.GetMetadata().Filepath.string(), handle, param.Name, material.GetShaderName() );
        }

        // The file's side of the same relation: a name the material carries that the shader no longer
        // declares. It cannot be found by the loop above (which only walks names the shader HAS), and it
        // is the one thing that loop can no longer report.
        if ( !schema.Params.empty() )
        {
            for ( const auto& t : data.Textures )
            {
                if ( t.Guid.empty() || paramFor( t.Name ) )
                    continue;

                LOG_WARN( "[Materials] '{0}' carries a value for '{1}', which the shader '{2}' does not "
                          "declare. The value is ignored — the slot was renamed or removed from the "
                          "shader since this material was authored.",
                          asset.GetMetadata().Filepath.string(), t.Name, material.GetShaderName() );
            }
        }
    }

    std::shared_ptr<Material> MaterialFactory::CreateMaterial( const Assets::MaterialAsset* asset,
                                                               MeshVertexPath path, MeshPass pass )
    {
        if ( !asset )
            return nullptr;

        const std::string shaderName = asset->GetShaderName();

        // Shader-name registry (replaces the old closed MaterialType switch). Specialized shaders keep
        // their optimized C++ material (PBR batches into an SSBO); any other shader is handled generically
        // by DataDrivenMaterial — so a new shader becomes assignable with zero C++.
        //
        // THERE IS DELIBERATELY NO DOMAIN CHECK HERE, and this is the obvious place to want one. A
        // Terrain-domain material in a mesh slot used to draw silently and wrongly, and the tempting fix
        // is to refuse it at birth. That would be wrong: this function does not know its consumer, and
        // every other consumer of a Terrain material is legitimate — the terrain draws with one, the
        // Material Editor loads one to edit its parameters, and the File Explorer registers one for every
        // `.demat` it thumbnails. Refusing here would break the correct uses to stop the incorrect one.
        // The refusal lives where the consumer IS known, in MeshRenderer::DrawGenericMeshes, which asks
        // Core::Formats::DrawnByMeshPath() and names the material it will not draw.
        //
        // "StaticMeshPBR" and "SkinnedMeshPBR" both mean THE PBR SURFACE and neither picks a vertex path
        // any more — the path is the parameter above. An asset naming the skinned shader used to be
        // answered with the static class here, which is where defect (1) in MeshVertexPath.hpp was born:
        // MeshRenderer then looked for a skinned parent, found a static one, and dropped the mesh.
        if ( shaderName.empty() || shaderName == "StaticMeshPBR" || shaderName == "SkinnedMeshPBR" )
        {
            auto pbrMaterial = MaterialPBR::Create( path, pass );
            if ( !pbrMaterial )
                return nullptr; // MaterialPBR::Create already named the pair it could not build
            if ( const auto* pbr = dynamic_cast<const Assets::SurfaceMaterialAsset*>( asset ) )
                ApplyPBRAsset( *pbrMaterial, *pbr );
            return pbrMaterial;
        }

        // A custom DSL surface shader exists only on the static FORWARD cell — it has no skinning stage,
        // no instanced variant and no G-buffer variant (a deferred scene draws it forward over the
        // composite, see MeshRenderer::RenderGenericManual). Name the material, the shader AND the path,
        // because the consequence is visible and misleading: the caller (MeshECSSystem) substitutes its
        // default PBR material, so the mesh draws in plain grey rather than vanishing, and "my character
        // is the wrong colour" is a different search from "my character is missing". The message was
        // checked against a frame — it said "will not draw" and the mesh drew.
        if ( path != MeshVertexPath::Static || pass != MeshPass::Forward )
        {
            LOG_WARN( "[MaterialFactory] Material '{}' uses the custom shader '{}', which exists only on "
                      "(Static x Forward) — there is no ({} x {}) variant of it (DSL surface shaders carry "
                      "no skinning, instancing or G-buffer stage). The mesh asking for it will fall back to "
                      "the default PBR material and render in the wrong colour; assign a PBR material to "
                      "it, or author a ({} x {}) variant of '{}'.",
                      asset->GetMetadata().Filepath.generic_string(), shaderName, MeshVertexPathName( path ),
                      MeshPassName( pass ), MeshVertexPathName( path ), MeshPassName( pass ), shaderName );
            return nullptr;
        }

        auto ddm = std::make_shared<DataDrivenMaterial>( shaderName );
        if ( const auto* pbr = dynamic_cast<const Assets::SurfaceMaterialAsset*>( asset ) )
            ApplyShaderAsset( *ddm, *pbr );
        return ddm;
    }

} // namespace Desert::Graphic