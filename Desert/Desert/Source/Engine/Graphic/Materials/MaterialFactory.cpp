#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Engine/Assets/Mapper.hpp>

#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/MaterialPipelineStates.hpp>

#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Graphic
{
    namespace
    {
        // Every `Texture2D` slot the template's manifest declares gets a value on every application: the
        // image the `.demat` names for it, or — for a slot the file does not name, has just stopped naming,
        // whose texture is still being read, or whose handle nobody has — the slot's schema default
        // (`setSlot(name, nullptr)`). THE LOOP IS OVER THE SCHEMA, NOT OVER THE FILE (М9): walking the file
        // visits only the slots it mentions, and this runs again over a LIVE material when the `.demat`
        // changes (AssetHotReload), so a slot the file stopped naming would keep drawing its old texture.
        // One function for every template: a slot a shader adds is bound by name, with no line to add here.
        template <class SetSlot>
        void BindManifestTextures( const Core::Formats::ShaderProgramMeta& meta,
                                   const Assets::SurfaceMaterialAsset& asset, const std::string& shaderName,
                                   SetSlot&& setSlot )
        {
            const auto& data = asset.Data();
            Core::Formats::ForEachMaterialTextureSlot(
                 meta, [&data]( const std::string& name ) { return data.GetTexture( name ); },
                 [&]( const Core::Formats::ShaderParam& param, uint64_t handle )
                 {
                     if ( handle == 0 )
                     {
                         setSlot( param.Name, nullptr ); // an empty slot is an authored decision
                         return;
                     }

                     auto* textures = Runtime::ResourceRegistry::GetTextureService();
                     if ( auto* tex = textures->Get( Common::UUID( handle ) ) )
                     {
                         if ( auto* img = static_cast<Graphic::Image2D*>(
                                   Runtime::ResourceRegistry::GetImageService()->Resolve(
                                        tex->GetImageHandle() ) ) )
                         {
                             setSlot( param.Name, img );
                             return;
                         }
                     }

                     // PENDING IS NOT MISSING (AL1-4): the slot shows its default until the texture lands,
                     // and the texture service rebuilds this material then, bound.
                     setSlot( param.Name, nullptr );
                     if ( textures->Require( Common::UUID( handle ) ).IsPending() )
                     {
                         textures->RebuildWhenReady( Common::UUID( handle ), asset.GetMetadata().Handle );
                         return;
                     }

                     // DC §1.4: a `.demat` names its textures by number and by nothing else, so a reference
                     // that stops resolving produces a surface that is merely untextured — nothing in the log
                     // a search can start from. This is the only place that knows the material, the slot and
                     // the number together.
                     LOG_ERROR( "[Materials] '{0}' names texture handle {1} in its '{2}' slot and no texture "
                                "with that handle is registered, so '{3}' samples that slot's schema default "
                                "instead. A texture's handle is HandleForGuid of its .detex header GUID, which "
                                "the slot names, so this means that texture asset is not in the project "
                                "(deleted, or never imported); re-import it or re-assign the slot.",
                                asset.GetMetadata().Filepath.string(), handle, param.Name, shaderName );
                 } );
        }
    } // namespace

    void MaterialFactory::ApplyPBRAsset( MaterialPBR& material, const Assets::SurfaceMaterialAsset& asset )
    {
        // Build the backend's typed view from the material canon (single protocol -> optimized
        // hot-path struct). No per-parameter setters.
        material.Data() = Assets::PBRSurfaceParams::FromMaterialData( asset.Data() );
        // What the shader reads: the generic row, by name from the asset's params (the schema's defaults
        // for anything the file does not mention) — the same builder DataDrivenMaterial's row comes from.
        material.SetParamRow( MaterialBinder::BuildRow( material.GetMaterialLayout(), asset.Data().Params ) );

        BindManifestTextures( material.GetSchema(), asset, asset.GetShaderName(),
                              [&material]( const std::string& name, Graphic::Image2D* image )
                              {
                                  if ( !image )
                                      material.BindSchemaDefaultTexture( name );
                                  else if ( auto* prop = material.Get<Texture2DProperty>( name ) )
                                      prop->SetImage( image );
                              } );
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

        BindManifestTextures( schema, asset, material.GetShaderName(),
                              [&material]( const std::string& name, Graphic::Image2D* image )
                              { material.SetTexture( name, image ); } );

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

        // ROUTED BY THE TEMPLATE'S HANDLE (its GUID identity), never by its name: the name is display text
        // and renaming a shader must not change which backend draws it.
        const Common::AssetHandle shader     = asset->GetShaderHandle();
        const std::string         shaderName = asset->GetShaderName();
        if ( shader.IsNull() )
        {
            LOG_ERROR( "[MaterialFactory] Material '{}' has no resolved surface template (its \"Shader\" GUID is "
                       "missing or names no loaded shader) — it draws nothing; there is no default template.",
                       asset->GetMetadata().Filepath.generic_string() );
            return nullptr;
        }

        // Template registry (replaces the old closed MaterialType switch). Specialized shaders keep
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
        // The template declaring `Role PBRSurface` (StaticMeshPBR.shader) means THE PBR SURFACE and neither picks
        // a vertex path any more — the path is the parameter above. An asset naming the skinned shader used to be
        // answered with the static class here, which is where defect (1) in MeshVertexPath.hpp was born:
        // MeshRenderer then looked for a skinned parent, found a static one, and dropped the mesh.
        const auto* surface = dynamic_cast<const Assets::SurfaceMaterialAsset*>( asset );
        if ( surface != nullptr && !surface->UsesCustomShader() )
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
        // ON LOAD, not at the first draw (AL1-12, UE's PSO precache): every renderer starts this shader's
        // pipeline compile on a worker from its next frame, so it has usually landed before the mesh is seen.
        MaterialPipelineRequests::Get().Request( shaderName );
        if ( const auto* pbr = dynamic_cast<const Assets::SurfaceMaterialAsset*>( asset ) )
            ApplyShaderAsset( *ddm, *pbr );
        return ddm;
    }

} // namespace Desert::Graphic