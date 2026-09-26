#include <Common/Core/DestructorGuard.hpp>
#include "MaterialEditorPanel.hpp"

#include "MaterialShaderRebuild.hpp"

#include "../Clouds/CloudDocumentOpen.hpp"

#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Import/TextureDnD.hpp>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Editor/Widgets/ThumbnailKey.hpp>
#include <Editor/Widgets/ThumbnailService.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphMedium.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SkyPresets.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/CloudLayout/CloudLayoutService.hpp>
#include <Engine/Runtime/Services/CloudType/CloudTypeService.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Runtime/Services/Skybox/SkyboxService.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Desert::Editor
{
    // Inside Desert::* the unqualified name ImGui resolves to the engine's Desert::ImGui wrapper — alias the
    // real Dear ImGui back in (same trick the other panels use).
    namespace ImGui = ::ImGui;

    namespace
    {
        // The preview renders offscreen at a fixed size. Set ONCE rather than per frame: SceneRenderer's
        // Resize recreates every frame buffer and idles the GPU, so following the ImGui window's size would
        // stall the editor on every drag of the window edge.
        constexpr uint32_t kPreviewRenderSize = 512;

        const char* ShapeName( PreviewViewport::Shape s )
        {
            switch ( s )
            {
                case PreviewViewport::Shape::Sphere:
                    return "Sphere";
                case PreviewViewport::Shape::Cube:
                    return "Cube";
                case PreviewViewport::Shape::Plane:
                    return "Plane";
                case PreviewViewport::Shape::Cylinder:
                    return "Cylinder";
            }
            return "Sphere";
        }

        // What a domain is CALLED in this window. Written out rather than reflected off the enum: this is
        // artist-facing copy that has to read as English beside a material's name, and "Unspecified" is a
        // parser state, not a thing to tell somebody.
        const char* DomainName( ::Desert::Core::Formats::ShaderDomain domain )
        {
            using D = ::Desert::Core::Formats::ShaderDomain;
            switch ( domain )
            {
                case D::Unspecified:
                    return "engine-internal";
                case D::Surface:
                    return "Surface";
                case D::Terrain:
                    return "Terrain";
                case D::Skybox:
                    return "Skybox";
                case D::PostProcess:
                    return "Post Process";
                case D::Volume:
                    return "Volume";
                case D::UI:
                    return "UI";
            }
            return "engine-internal";
        }

        // What a shader of this domain draws INSTEAD, as a clause following the shader's own name. The half
        // of "no preview here" that is actually useful: being told a pane cannot show something is only
        // half an answer while nobody says where to look for it.
        const char* WhatTheDomainDrawsInstead( ::Desert::Core::Formats::ShaderDomain domain )
        {
            using D = ::Desert::Core::Formats::ShaderDomain;
            switch ( domain )
            {
                case D::Terrain:
                    return "draws the scene's terrain, a tessellated patch grid the renderer synthesizes "
                           "itself, so there is no sphere, cube or plane this pane could put it on. Edit "
                           "it here and look at the terrain in the viewport.";
                case D::PostProcess:
                    return "draws over the whole finished frame, not an object this pane could place.";
                case D::Volume:
                    return "is marched as the scene's volumetric cloud layer — a medium filling the sky, "
                           "not an object this pane could put on a sphere. Edit it here and look at the "
                           "clouds in the viewport; the layer picks it up the same frame.";
                case D::UI:
                    return "fills a 2D UI element — a screen-space quad the Render2D batcher rasterizes, "
                           "not an object this pane could place. Drop it into a UI Panel's Material slot "
                           "and look at the canvas; edit the parameters here and they land the same frame.";
                case D::Unspecified:
                case D::Surface:
                case D::Skybox: // no longer a refusal: the pane wraps its cubemap onto a ball (see
                                // PreviewUnavailableReason's own Skybox branch, which answers before
                                // this function is ever asked about that domain)
                    break;
            }
            // Surface never reaches here (it is the one domain the pane CAN draw), so this is the
            // Unspecified case: a shader with no `Domain` line at all, which the engine treats as internal.
            return "declares no material domain, so the engine treats it as internal. No material should be "
                   "pointing at it at all.";
        }

        // The window's display name is the material's file stem, taken ONCE — the title carries the ImGui
        // window id (see DocumentTitle) and a title that changed under a live window would orphan its
        // saved dock entry. The id half is the handle, so the label is free to be a human name without being
        // load-bearing.
        std::string MaterialDocumentName( const Assets::AssetHandle&                   material,
                                          const std::shared_ptr<Assets::AssetManager>& assetManager )
        {
            if ( assetManager )
            {
                if ( auto asset = assetManager->FindByHandle<Assets::SurfaceMaterialAsset>( material ) )
                {
                    const auto path = asset->GetMetadata().Filepath;
                    if ( !path.empty() )
                        return path.stem().string();
                }
            }
            // Named by handle rather than "Material": two unnamed materials must still read as two windows.
            return "Material " + std::to_string( static_cast<uint64_t>( material ) );
        }
    } // namespace

    MaterialEditorPanel::MaterialEditorPanel( const Assets::AssetHandle&                   material,
                                              const std::shared_ptr<Assets::AssetManager>& assetManager )
         : ISubjectDocument( MaterialDocumentName( material, assetManager ),
                             AssetSubject( material, static_cast<uint32_t>( Assets::AssetTypeID::Material ) ) ),
           m_AssetManager( assetManager )
    {
        // Start level with the world: a rebuild that happened before this window existed left nothing here to
        // invalidate, and treating it as pending would drop pipelines that were never built.
        m_SeenRebuildCount = MaterialShaderRebuild::CountFor( DrawnShaderName() );
    }

    // Written out rather than left to the members' reverse-declaration order. The two objects have to go in
    // a stated order — the UIHelper's descriptor sets reference the preview's images — and a teardown order
    // that depends on which line a member happens to be declared on is the shape of defect this engine has
    // paid for in Vulkan lifetimes more than once.
    MaterialEditorPanel::~MaterialEditorPanel()
    try
    {
        // CLOSING WITH EDITS NOBODY ACCEPTED IS A LOSS, AND STAGING IS WHAT MADE IT POSSIBLE. Before this
        // window held three states, an unaccepted edit was already in the scene and could still be saved
        // later; now it dies here with the working copy. The right answer is a question on close, and that
        // lives in EditorLayer's document-close path rather than in a destructor, which cannot refuse.
        // Until it exists this is what stops the loss being SILENT — the values are in the message, so a
        // person who closed the wrong window can put them back by hand.
        if ( const MaterialEdit::DirtyState dirty = Dirty(); dirty.Unapplied || dirty.Unsaved )
        {
            const auto        subject = ResolveSubject();
            const std::string name =
                 subject ? subject->GetMetadata().Filepath.generic_string() : std::string( "<unloaded>" );

            if ( dirty.Unapplied )
            {
                // The values themselves, so somebody who closed the wrong window can put them back by hand.
                // Through the copy's own Save() rather than a serializer of this file's choosing: one
                // material is written one way, and that way already refuses a material running on
                // substituted defaults.
                std::string values = "<could not be serialized>";
                if ( m_WorkingCopy )
                {
                    if ( const auto serialized = m_WorkingCopy->Save(); serialized )
                        values = serialized.GetValue();
                }
                LOG_WARN( "[MaterialEditor] '{}' closed with edits that were never applied — they are GONE. "
                          "What this window was showing: {}",
                          name, values );
            }
            else
            {
                LOG_WARN( "[MaterialEditor] '{}' closed with applied edits that were never saved. The scene "
                          "still has them; the file does not, so they end with this session.",
                          name );
            }
        }

        // The preview FIRST. It owns the entity whose slot resolved to the working copy's runtime
        // material, and ~PreviewViewport idles the device before releasing it — so by the time the copy is
        // released there is nothing left holding an instance of a material about to be graveyarded.
        ReleasePreview();

        if ( m_WorkingCopy )
        {
            if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                materialService->Release( m_WorkingCopy->GetMetadata().Handle );
            m_WorkingCopy.reset();
        }
    }
    DESERT_DESTRUCTOR_GUARD( "~MaterialEditorPanel" )

    std::shared_ptr<Assets::SurfaceMaterialAsset> MaterialEditorPanel::ResolveSubject() const
    {
        if ( !m_AssetManager )
            return nullptr;
        return m_AssetManager->FindByHandle<Assets::SurfaceMaterialAsset>(
             Assets::AssetHandle( Subject().Owner ) );
    }

    std::shared_ptr<Assets::SurfaceMaterialAsset> MaterialEditorPanel::DrawnMaterial() const
    {
        if ( m_WorkingCopy )
            return m_WorkingCopy;
        return ResolveSubject();
    }

    Assets::SurfaceMaterialAsset* MaterialEditorPanel::EnsureWorkingCopy( Assets::SurfaceMaterialAsset& subject )
    {
        if ( m_WorkingCopy )
            return m_WorkingCopy.get();
        if ( !m_WorkingCopyRefusal.empty() )
            return nullptr; // refused once; retrying every frame would log the same line forever

        auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
        if ( !materialService )
        {
            // Not written to m_WorkingCopyRefusal: the service is a startup-order fact, not a property of
            // this material, and a frame this early has no window on screen to read a message anyway.
            return nullptr;
        }

        auto copy = Assets::SurfaceMaterialAsset::CreateWorkingCopy( subject );

        // Lazily: the runtime material is built by the first draw that asks for it, which for a document
        // whose domain has no preview shape is never. RegisterAsset can only refuse on an identity
        // collision, and the copy's identity was just generated — so a refusal here means the generator
        // handed out a live id, which is worth saying rather than swallowing.
        if ( const auto registered = materialService->RegisterAsset( copy ); !registered )
        {
            m_WorkingCopyRefusal =
                 "This material cannot be edited in this session: its working copy was refused by the "
                 "material service (" +
                 registered.GetError() +
                 "). The parameters below are shown as they are and cannot be changed — editing the "
                 "material directly would put the change in every open scene with no way to undo it.";
            LOG_ERROR( "[MaterialEditor] '{}': {}", subject.GetMetadata().Filepath.generic_string(),
                       m_WorkingCopyRefusal );
            return nullptr;
        }

        m_WorkingCopy = std::move( copy );

        // The third state, taken at the moment the document opens. The subject in memory IS what the file
        // held — nothing has edited it yet — so this is a snapshot of the file without reading it.
        MaterialEdit::CopyAuthoredValues( m_OnDisk, subject.Data() );

        LOG_INFO( "[MaterialEditor] '{}' opened with a working copy (handle {}); edits stay in this window "
                  "until Apply.",
                  subject.GetMetadata().Filepath.generic_string(),
                  static_cast<uint64_t>( m_WorkingCopy->GetMetadata().Handle ) );
        return m_WorkingCopy.get();
    }

    MaterialEdit::DirtyState MaterialEditorPanel::Dirty() const
    {
        const auto subject = ResolveSubject();
        if ( !m_WorkingCopy || !subject )
            return {};
        return MaterialEdit::EvaluateDirty( m_WorkingCopy->Data(), subject->Data(), m_OnDisk );
    }

    bool MaterialEditorPanel::HasUnappliedEdits() const
    {
        return Dirty().Unapplied;
    }

    ISubjectDocument::DiskState MaterialEditorPanel::GetDiskState() const
    {
        // UNTRACKED, not Clean, while there is no working copy: without one no snapshot was ever taken, so
        // this document has no evidence about its file and must not claim it is up to date.
        if ( !m_WorkingCopy || !ResolveSubject() )
            return DiskState::Untracked;
        return Dirty().Unsaved ? DiskState::Dirty : DiskState::Clean;
    }

    bool MaterialEditorPanel::ApplyEdits()
    {
        const auto subject = ResolveSubject();
        if ( !m_WorkingCopy || !subject )
            return false;
        if ( !MaterialEdit::EvaluateDirty( m_WorkingCopy->Data(), subject->Data(), m_OnDisk ).Unapplied )
            return false;

        // A SHADER change is not a re-valuing. The runtime material's CLASS follows the shader, so the
        // cached one cannot be handed the new values — it has to be dropped and rebuilt from the asset.
        // Read BEFORE the copy, because afterwards the two names agree by construction.
        const bool shaderChanged = subject->GetShaderName() != m_WorkingCopy->GetShaderName();

        MaterialEdit::CopyAuthoredValues( subject->Data(), m_WorkingCopy->Data() );
        subject->ResolveDependencies( *m_AssetManager );

        if ( shaderChanged )
        {
            if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                materialService->Invalidate( subject->GetMetadata().Handle );
        }
        else
        {
            PublishToRuntime( *subject, subject->Data().IsInstance() );
        }
        return true;
    }

    bool MaterialEditorPanel::DiscardEdits()
    {
        const auto subject = ResolveSubject();
        if ( !m_WorkingCopy || !subject )
            return false;

        const bool shaderChanged = subject->GetShaderName() != m_WorkingCopy->GetShaderName();

        MaterialEdit::CopyAuthoredValues( m_WorkingCopy->Data(), subject->Data() );
        m_WorkingCopy->ResolveDependencies( *m_AssetManager );

        // The same distinction Apply makes, one audience over: the preview's own runtime material is the
        // one that has to be dropped when the shader moved back, and merely re-valued when it did not.
        if ( shaderChanged )
        {
            if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                materialService->Invalidate( m_WorkingCopy->GetMetadata().Handle );
        }
        else
        {
            PublishToRuntime( *m_WorkingCopy, m_WorkingCopy->Data().IsInstance() );
        }
        return true;
    }

    std::string MaterialEditorPanel::DrawnShaderName() const
    {
        auto asset = DrawnMaterial();
        if ( !asset )
            return {};

        // Through the parent for an instance — see the header. SurfaceMaterialAsset::GetShaderName() answers
        // "StaticMeshPBR" for a material that names none, which is right for a base asset and wrong for a
        // child, whose shader is simply somewhere else.
        if ( auto parent = ResolveParent( *asset ) )
            return parent->GetShaderName();
        return asset->GetShaderName();
    }

    std::string MaterialEditorPanel::PreviewUnavailableReason( const std::string& shaderName ) const
    {
        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return "No preview: the shader service is not running, so nothing can be resolved to draw with.";

        auto shader = shaderService->GetByName( shaderName );
        if ( !shader )
            return "No preview: the shader '" + shaderName +
                   "' is not loaded, so there is nothing to draw this material with.";

        // Registered-but-uncompiled is a real state and a deliberate one: ShaderService keeps the NAME of a
        // shader that failed to compile so the material does not quietly fall back to the standard one.
        // MeshRenderer then skips the draw outright, which is why the pane would be empty for a reason that
        // has nothing to do with this material's own values.
        if ( !shader->IsCompiled() )
            return "No preview: the shader '" + shaderName +
                   "' is registered but has no compiled stages, so nothing draws with it. The compile "
                   "error is in the Logs panel, named after this shader.";

        const auto domain = shader->GetProgramMeta().Domain;

        // The cubemap domain previews — as the cubemap wrapped onto an orbitable ball — so its refusals
        // are about the CUBE, not the domain. Three states an empty ball would render identically, told
        // apart here because each asks the artist for a different action.
        if ( domain == ::Desert::Core::Formats::ShaderDomain::Skybox )
        {
            const auto& params = shader->GetProgramMeta().Params;
            const auto  cubeParam =
                 std::find_if( params.begin(), params.end(), []( const auto& p ) { return p.IsCubeTexture; } );
            if ( cubeParam == params.end() )
                return "No preview: '" + shaderName +
                       "' is a Skybox-domain shader with no TextureCube property, so there is no cubemap "
                       "this pane could wrap onto the preview ball. Declare one in the shader's "
                       "Properties block.";

            // The WORKING COPY, like everything else this pane answers from: the message beside an empty
            // ball has to describe the material the ball would be showing, or "nothing is bound" appears
            // over a preview that is drawing the cubemap the artist just dropped.
            const auto asset = DrawnMaterial();
            // Kept alive for the whole read: an instance's textures come from its parent (v1 rule).
            const auto     parent = asset ? ResolveParent( *asset ) : nullptr;
            const uint64_t bound =
                 asset ? ( parent ? parent->Data() : asset->Data() ).GetTexture( cubeParam->Name ) : 0;
            if ( bound == 0 )
                return "No preview yet: nothing is bound to '" + cubeParam->DisplayName +
                       "'. Drop an HDR skybox onto that slot in the parameters beside this pane and the "
                       "ball appears.";

            auto*      skyboxService = Runtime::ResourceRegistry::GetSkyboxService();
            const auto skybox = skyboxService ? skyboxService->Get( Assets::AssetHandle( bound ) ) : nullptr;
            if ( !skybox || !skybox->GetEnvironment() )
                return "No preview: the skybox bound to '" + cubeParam->DisplayName +
                       "' is not loaded (its .hdr may have been deleted or moved). Rebind the slot.";

            return {};
        }

        // The VOLUME domain fills the pane too, and with a sky rather than a shape — so it is no longer a
        // refusal. What it draws is the material's OWN preview world, which is a different thing from the
        // scene's cloud layer; that sentence has not gone away, it has moved UNDER the picture (see
        // PreviewSceneNote and its call site in OnUIRender). A pane that showed a convincing sky while
        // silently dropping "the level's layer is not this one" would be worse than the refusal was.
        if ( domain == ::Desert::Core::Formats::ShaderDomain::Volume )
            return {};

        if ( domain != ::Desert::Core::Formats::ShaderDomain::Surface )
            return std::string( "No preview shape for a " ) + DomainName( domain ) + "-domain material.\n\n'" +
                   shaderName + "' " + WhatTheDomainDrawsInstead( domain ) +
                   "\n\nThe parameters beside it edit the material normally.";

        return {};
    }

    std::string MaterialEditorPanel::PreviewSceneNote() const
    {
        if ( !m_Preview || m_Preview->GetFill() != PreviewViewport::Fill::SkyDome )
            return {};

        // THE REFUSAL, KEPT, UNDER THE THING THAT REPLACED IT. The dome is this material in a preview
        // WORLD; the viewport is this material in a LEVEL, and they are not the same picture — the
        // scene's own layer carries tracing budgets, a region size and a planet radius that live on its
        // component and not in any material. A dome that looked right would otherwise be read as a
        // promise about the level, which it cannot make.
        // AND WHAT IT COSTS TO MOVE, because the sentence that stood here promised the opposite of what
        // the artist sees. "The layer picks up an edit here the same frame" is true of the VALUE and false
        // of the PICTURE, and the difference is seconds: about half of this material's parameters — the
        // ones that decide WHERE the clouds are, rather than how they are lit — are inputs to a baked
        // 256x32x256 field of a few thousand cloud bodies, and moving one starts that bake again. Measured
        // on this machine in Debug, from the edit landing to the new volume being collected: 3.3 s, 4.7 s,
        // 5.3 s, 5.9 s, 8.0 s, 10.9 s across two materials — the spread is the number of bodies, so raising
        // Coverage costs more than lowering it. The lighting and erosion parameters re-bake nothing and
        // move within the ten frames the temporal resolve needs.
        //
        // Said in the panel and not only in a report, because "I moved it and nothing happened" was the
        // owner's report twice, and a promise the window cannot keep is worse than silence — this is the
        // fifth such sentence in a week.
        return "This is a preview sky built from this material alone. The level's cloud layer carries its "
               "own tracing budgets and region size; open a scene with a volumetric cloud component to see "
               "it there - it reads the same values, and shows them on its own next frame.\n"
               "Lighting and detail parameters move the picture within a few frames. The ones that place "
               "the clouds - coverage, the weather and placement numbers, the layout, the cloud types - "
               "rebuild the cloud volume first, which takes SEVERAL SECONDS in a Debug build; the sky keeps "
               "showing the previous one until it is ready.";
    }

    std::optional<::Desert::Core::Formats::ShaderDomain> MaterialEditorPanel::EffectiveDomain() const
    {
        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return std::nullopt;
        const auto shader = shaderService->GetByName( DrawnShaderName() );
        if ( !shader )
            return std::nullopt;
        return shader->GetProgramMeta().Domain;
    }

    Graphic::SampledCube MaterialEditorPanel::ResolveSubjectCubemap() const
    {
        if ( !m_AssetManager )
            return {};
        // The working copy: this closure is what the ball resolves through every frame, so it is the
        // window's live edit that must reach it, not the state the scene is still rendering.
        const auto asset = DrawnMaterial();
        if ( !asset )
            return {};

        // An instance's textures come from its parent (the same v1 rule DrawParameters states: no
        // per-instance texture descriptors yet), so the slot is read off the parent's data.
        const auto  parent = ResolveParent( *asset );
        const auto& owner  = parent ? *parent : *asset;
        const auto& data   = owner.Data();

        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        auto  shader = shaderService != nullptr ? shaderService->GetByName( owner.GetShaderName() ) : nullptr;
        if ( !shader )
            return {};

        // The FIRST cube property is the one the ball wraps. Not a hardcoded sampler name: any
        // Skybox-domain shader an artist writes names its own slot, and the schema is the contract.
        const auto& params = shader->GetProgramMeta().Params;
        const auto  cubeParam =
             std::find_if( params.begin(), params.end(), []( const auto& p ) { return p.IsCubeTexture; } );
        if ( cubeParam == params.end() )
            return {};

        const uint64_t bound = data.GetTexture( cubeParam->Name );
        if ( bound == 0 )
            return {};

        // The slot holds an HDR skybox asset; its service caches the baked environment (radiance +
        // IBL) per asset, so this is a map lookup, not a bake.
        auto*      skyboxService = Runtime::ResourceRegistry::GetSkyboxService();
        const auto skybox        = skyboxService ? skyboxService->Get( Assets::AssetHandle( bound ) ) : nullptr;
        if ( !skybox )
            return {};

        const auto& radiance = skybox->GetEnvironment().RadianceMap;
        if ( !radiance.IsValid() || radiance.ImageType != Runtime::ImageHandle::Type::ImageCube )
            return {};

        // Checked like the two services above it. Not because this one is likelier to be absent — it is the
        // same registry and the same lifetime — but because a third call spelled differently beside two
        // guarded ones reads as a deliberate exception, and the next person has to work out which of the
        // three is wrong.
        auto* imageService = Runtime::ResourceRegistry::GetImageService();
        if ( !imageService )
            return {};

        // A cubemap MATERIAL has no scene look of its own: the asset as authored, identity.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the key/handle names this exact type
        return { static_cast<const Graphic::ImageCube*>( imageService->Resolve( radiance ) ), {} };
    }

    void MaterialEditorPanel::DrawPreviewPlaceholder( float side, const std::string& reason ) const
    {
        // Local (window) coordinates as well as screen ones: the draw list wants screen, and both the text
        // wrap position and the cursor restore below are window-space. Mixing the two puts the wrap column
        // hundreds of pixels off and the message renders as one unbroken line.
        const ImVec2 local  = ImGui::GetCursorPos();
        const ImVec2 screen = ImGui::GetCursorScreenPos();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( screen, ImVec2( screen.x + side, screen.y + side ), IM_COL32( 28, 28, 32, 255 ), 4.0f );
        dl->AddRect( screen, ImVec2( screen.x + side, screen.y + side ), IM_COL32( 68, 68, 76, 255 ), 4.0f );

        constexpr float kPad = 14.0f;
        ImGui::SetCursorPos( ImVec2( local.x + kPad, local.y + kPad ) );
        ImGui::PushTextWrapPos( local.x + side - kPad );
        ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.74f, 0.74f, 0.78f, 1.0f ) );
        ImGui::TextUnformatted( reason.c_str() );
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        // Back to the top-left corner and claim exactly the square an image would have claimed. Without
        // this the parameter column beside the pane moves with the length of the message.
        ImGui::SetCursorPos( local );
        ImGui::Dummy( ImVec2( side, side ) );
    }

    void MaterialEditorPanel::EnsurePreview()
    {
        if ( m_Preview )
            return;

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        // A FRESH VIEWPORT HOLDS NOTHING, even when the identity is unchanged — so this clear is still
        // required and is not made redundant by the derived condition. Forgetting it would leave the new
        // viewport empty while m_Pushed claimed the material was already in it.
        m_Pushed = {};
    }

    void MaterialEditorPanel::ReleasePreview()
    {
        if ( !m_Preview )
            return;

        // ~PreviewViewport idles the device and releases the scene before the renderer, which is what
        // returns the renderer slot. Dropping the UIHelper too: its descriptor sets reference images that
        // belonged to the framebuffers just destroyed.
        m_Preview.reset();
        m_UIHelper.reset();
        m_Pushed = {};
    }

    void MaterialEditorPanel::OnPreUpdate()
    {
        // THE SLOT IS NOT CLAIMED UNTIL THE WINDOW HAS ACTUALLY BEEN DRAWN. The document is created in
        // EditorLayer::ServiceSubjectOpenRequests, which runs earlier in this same OnUpdate — so on the frame a
        // material is opened there is a panel but no window on screen yet, and building a Scene and a
        // SceneRenderer for it then would spend one of the six on something nobody has seen.
        //
        // THE RELEASE-WHEN-HIDDEN BRANCH IS BACK, AND IT IS NOT HERE. It used to live in this function and
        // was removed on the grounds that a document is destroyed rather than hidden — which was true while
        // a document could only be closed. It is not true of a document docked as a tab behind another one,
        // which is open, invisible, and was holding one of the six for as long as the user left it there.
        // The editor drives it now: EditorLayer asks the document to ReleaseRendererSlot() when its window
        // has not been drawn for a while, behind the same device-idle wait a close uses. Written there
        // rather than here because "not drawn" is ImGui's answer and this function runs before the frame.
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        // A material the pane cannot draw does not get a renderer slot. RELEASED rather than merely not
        // created: the shader behind a material is editable under a live window (the Node Graph rewrites
        // its scratch material's shader, and a `.demat` can be reloaded from disk), so a window that
        // already holds a slot has to give it back when its material moves to a domain with no shape —
        // otherwise one of the six is held for ever by a window showing a paragraph of text.
        if ( !m_PreviewUnavailable.empty() )
        {
            ReleasePreview();
            return;
        }

        EnsurePreview();

        // Re-push when WHAT WOULD BE PUSHED differs from what was — subject, shape, or the shader the
        // material resolves to. Deriving the condition is the point: any future field added to
        // PushedIdentity is covered the moment it is added, with no new reset site to remember. See the
        // header for why the shader term is hardening rather than a fix — the live path that keeps the
        // preview correct on a shader switch is MaterialService::Invalidate bumping the version that
        // MeshECSSystem rebuilds from, and that was established by rendering the switch, not by reading.
        //
        // WHAT gets pushed is the domain's answer to "what fills the pane" — the preview's extension
        // point. Surface rides a real primitive through the slot route (the Shape combo is ITS
        // particularity); the cubemap domain brings its own draw, a ball the cubemap is wrapped onto,
        // resolved through a closure so the pane tracks the material's slot with no copy to invalidate.
        // A future Volume domain slots in here with a march, not with a new Shape. The domain term of
        // the identity is the ShaderName term: one names the other.
        // THE HANDLE PUSHED IS THE WORKING COPY'S, NOT THE SUBJECT'S, and that one substitution is the
        // whole of "the ball shows the edit and the level does not". PreviewViewport fills an ordinary
        // StaticMeshComponent slot with it, so this pane still takes the exact route a scene mesh takes —
        // it simply takes it to a different material asset. The preview is therefore no more flattering
        // than it was (Desert/Tests/Editor/MaterialPreviewRoute still holds): it is the same route to the
        // material the subject is ABOUT to become.
        const auto drawn = DrawnMaterial();
        if ( !drawn )
            return;

        if ( const PushedIdentity wanted{ drawn->GetMetadata().Handle, m_Shape, DrawnShaderName(), m_PreviewMesh };
             !( wanted == m_Pushed ) )
        {
            const auto domain = EffectiveDomain();
            if ( domain == ::Desert::Core::Formats::ShaderDomain::Skybox )
                m_Preview->SetCubemapMaterial( [this] { return ResolveSubjectCubemap(); } );
            else if ( domain == ::Desert::Core::Formats::ShaderDomain::Volume )
                m_Preview->SetVolumeMaterial( drawn->GetMetadata().Handle );
            else if ( static_cast<uint64_t>( m_PreviewMesh ) != 0 )
            {
                // THIS MATERIAL IN EVERY SLOT, and the count comes from the mesh rather than from a
                // guess: a mesh with four slots handed one handle would draw three of them with the
                // engine default and read as "the material only works on part of it". The window is about
                // one material, so every slot gets it and the tab says so out loud.
                std::vector<Assets::AssetHandle> slots{ drawn->GetMetadata().Handle };
                if ( auto* meshService = Runtime::ResourceRegistry::GetMeshService() )
                {
                    if ( const auto* mesh = meshService->Get( m_PreviewMesh ) )
                        slots.assign( std::max<std::size_t>( mesh->GetSubmeshes().size(), 1 ),
                                      drawn->GetMetadata().Handle );
                }
                m_Preview->SetMesh( m_PreviewMesh, slots );
            }
            else
                m_Preview->SetMaterial( drawn->GetMetadata().Handle, m_Shape );
            m_Pushed = wanted;
        }

        // The shader behind this material was rebuilt: drop the pipelines THIS renderer cached from the old
        // modules. Without it the window keeps drawing the old shader after a recompile — see the note in
        // MaterialShaderRebuild.hpp.
        const std::string shaderName = DrawnShaderName();
        if ( const uint64_t rebuilds = MaterialShaderRebuild::CountFor( shaderName );
             rebuilds != m_SeenRebuildCount )
        {
            m_SeenRebuildCount = rebuilds;
            if ( auto* shaderService = Runtime::ResourceRegistry::GetShaderService() )
            {
                if ( auto shader = shaderService->GetByName( shaderName ) )
                    m_Preview->InvalidatePipelines( shader.get() );
            }
        }

        m_Preview->Update( kPreviewRenderSize, kPreviewRenderSize );
    }

    void MaterialEditorPanel::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        // Guarded even though the palette only offers these entries while HasPreview() is true: the two
        // are read a frame apart — the entries are built when the command arrives, and the preview can be
        // torn down by OnPreUpdate in between (a material whose shader stopped drawing geometry). A null
        // here is that race, not a caller's mistake, and it is quieter to skip than to crash a session.
        if ( !m_Preview )
            return;

        // Degrees in the table, radians at the setter — a person and a document both read "Front, 35, 20",
        // and everything downstream of SetOrbit is trigonometry.
        m_Preview->SetOrbit( glm::radians( viewpoint.YawDegrees ), glm::radians( viewpoint.PitchDegrees ) );
    }

    void MaterialEditorPanel::DrawToolbar( Assets::SurfaceMaterialAsset* working, bool isInstance )
    {
        // No material combo: this window IS one material. Picking a different one is opening a different
        // document, which is the whole point of the change that deleted the combo.
        //
        // The Shape combo is a SURFACE-domain control, not a preview control: only the surface domain
        // fills the pane with a primitive whose shape is a free choice (a grass card wants a plane). A
        // cubemap material's content IS a ball — offering Cube/Plane there would be two entries that
        // rebuild the preview into the same picture — and the refused domains draw nothing at all. So
        // the combo exists exactly where it means something, instead of being disabled everywhere else.
        if ( EffectiveDomain() == ::Desert::Core::Formats::ShaderDomain::Surface )
        {
            ImGui::SetNextItemWidth( 110.0f );
            if ( ImGui::BeginCombo( "Shape", ShapeName( m_Shape ) ) )
            {
                for ( auto s : { PreviewViewport::Shape::Sphere, PreviewViewport::Shape::Cube,
                                 PreviewViewport::Shape::Cylinder, PreviewViewport::Shape::Plane } )
                {
                    const bool selected = ( s == m_Shape );
                    if ( ImGui::Selectable( ShapeName( s ), selected ) && !selected )
                    {
                        // No re-push flag here on purpose: the shape is PART of the pushed identity, so
                        // changing it announces itself. This site used to carry the only reset that was
                        // remembered; the shader site next door was the one that was not.
                        m_Shape = s;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
        }

        if ( ImGui::Button( "Reset View" ) && m_Preview )
            m_Preview->ResetView();

        if ( !working )
            return;

        // APPLY, DISCARD, SAVE — the three states in the order an artist moves through them, and the two
        // that did not exist before are the point of this window.
        //
        // The scene changes on APPLY and nowhere else. Before this row, it changed while the slider was
        // still moving: the edit went into the asset every scene renders from, so an accidental drag
        // reached every mesh in the level immediately, nothing could put it back, and the next deliberate
        // Save wrote the accident to disk. That is what DISCARD is for, and Discard is not optional — a
        // window that marks itself unsaved while offering no way back tells the artist an edit is
        // reversible and then is not.
        const MaterialEdit::DirtyState dirty = Dirty();

        ImGui::SameLine();
        ImGui::BeginDisabled( !dirty.Unapplied );
        if ( ImGui::Button( "Apply" ) )
            ApplyEdits();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Put these values into every scene that draws this material.\nUntil you do, "
                               "they exist only in this window's preview." );

        ImGui::SameLine();
        ImGui::BeginDisabled( !dirty.Unapplied );
        if ( ImGui::Button( "Discard" ) )
            DiscardEdits();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Throw these edits away and go back to what the scene is showing." );

        // Save came here with the parameters it persists. It used to sit on the Details slot row, where it
        // was the only way to write a `.demat` at all — but Details no longer edits a material, so a save
        // button there would be an action with nothing to save, and this window would be an editor whose
        // edits die with the session.
        ImGui::SameLine();
        if ( ImGui::Button( "Save" ) )
            SaveDocument();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Write the file. Applies first, so the file and the running editor cannot "
                               "disagree." );

        if ( isInstance )
        {
            ImGui::SameLine();
            if ( ImGui::Button( "Reset Overrides" ) )
            {
                // The WORKING copy, like every other edit in this window: dropping every override is a
                // large edit, which makes it the one most worth being able to take back.
                working->Data().Params.clear();
                working->Data().Textures.clear();
                working->Data().CloudAssets.clear();
                PublishToRuntime( *working, /*isInstance=*/true );
            }
            if ( ImGui::IsItemHovered() )
                // ASCII: an em dash here draws as '?' — measured, see WhatTheDomainDrawsInstead.
                //
                // IT SAYS WHAT THE ROW ARROWS DO NOT, and that sentence is the whole reason both controls
                // exist. A row arrow appears only where the value DIFFERS from the parent's; an override
                // that happens to equal the parent's value today is still a pin that will stop tracking it
                // tomorrow, and this button is the only thing that clears those.
                ImGui::SetTooltip( "Drop every override, back to the parent's values.\nIncluding the ones "
                                   "that match the parent already - those carry no row arrow, because "
                                   "nothing on screen would change." );
        }

        // WHICH OF THE THREE STATES THIS WINDOW IS IN, said out loud. Two different "dirty"s, so two
        // different sentences: one is about the scene and one is about the file, and collapsing them into
        // a single "modified" would leave the artist unable to tell whether pressing Save is about to
        // publish something they have not looked at.
        if ( dirty.Unapplied )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1.0f, 0.72f, 0.35f, 1.0f ), "not applied" );
        }
        else if ( dirty.Unsaved )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "applied, not saved" );
        }
    }

    std::shared_ptr<Assets::SurfaceMaterialAsset>
    MaterialEditorPanel::ResolveParent( const Assets::SurfaceMaterialAsset& asset ) const
    {
        if ( !m_AssetManager || !asset.Data().IsInstance() )
            return nullptr;

        // The child references its parent by the parent's STABLE in-file id, not by an asset handle — that
        // is what survives the file being moved — so the service's external->internal map is the only way
        // back to a loaded asset.
        auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
        if ( !materialService )
            return nullptr;

        const auto parentId = asset.Data().InstanceParentId();
        if ( !parentId.has_value() )
        {
            return nullptr;
        }

        const auto parentHandle = materialService->GetAssetHandleByExternal( *parentId );
        if ( parentHandle.IsNull() )
            return nullptr;
        return m_AssetManager->FindByHandle<Assets::SurfaceMaterialAsset>( parentHandle );
    }

    bool MaterialEditorPanel::DrawShaderPicker( Assets::SurfaceMaterialAsset& asset )
    {
        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        if ( !shaderService )
        {
            ImGui::TextDisabled( "Shader: the shader service is not running" );
            return false;
        }

        const std::string current = asset.GetShaderName();

        // THE DOMAIN COMES FROM THE MATERIAL, and everything below follows from it. See the header for
        // what the hardcoded `Surface` filter did to a Terrain material.
        auto currentShader = shaderService->GetByName( current );
        if ( !currentShader )
        {
            // No shader, no domain, and therefore no honest list. Falling back to "well, Surface then"
            // would be the whole defect again: a click would silently rehome a material whose real domain
            // nobody in this process can currently read.
            ImGui::TextDisabled( "Shader" );
            ImGui::TextDisabled( "'%s' is not loaded, so its domain is unknown and no list can be offered",
                                 current.c_str() );
            return false;
        }

        // IsUserAssignable() is asked of the MATERIAL'S OWN shader, not of the candidates: inside one
        // domain every entry is assignable by construction, so asking it there would be a tautology. Here
        // it answers the question that is not — whether this material sits in a domain a user authors
        // materials in at all. A `.demat` with no `Domain` line is already drawing nothing; the list it
        // needs is not "more of the same domain".
        //
        // The CUBEMAP domain (Skybox) is authorable in this editor without being slot-assignable, and
        // the two are different questions: IsUserAssignable() answers "may a mesh/terrain slot draw it"
        // (it may not — the mesh gate refuses it by name, rightly), while this window edits and
        // previews such a material as a first-class document. So Skybox joins the same-domain list
        // here, and ONLY here.
        const auto domain     = currentShader->GetProgramMeta().Domain;
        const bool assignable = currentShader->GetProgramMeta().IsUserAssignable() ||
                                domain == ::Desert::Core::Formats::ShaderDomain::Skybox;

        if ( assignable )
        {
            ImGui::TextDisabled( "Shader  (%s domain)", DomainName( domain ) );
        }
        else
        {
            ImGui::TextDisabled( "Shader" );
            ImGui::TextColored( ImVec4( 1.0f, 0.72f, 0.35f, 1.0f ),
                                "'%s' is not a material shader (%s). Pick a real one below.", current.c_str(),
                                DomainName( domain ) );
        }

        // Sorted, because GetAllNames() walks an unordered_map: without this the same project shows the
        // same shaders in a different order every run, and the entry under the cursor moves between
        // sessions.
        std::vector<std::string> names = shaderService->GetAllNames();
        std::sort( names.begin(), names.end() );

        bool shaderChanged = false;
        ImGui::SetNextItemWidth( -FLT_MIN );
        const bool open    = ImGui::BeginCombo( "##shader", current.c_str() );
        const bool hovered = ImGui::IsItemHovered(); // the combo itself; after EndCombo this is the popup
        if ( open )
        {
            for ( const auto& name : names )
            {
                auto candidate = shaderService->GetByName( name );
                if ( !candidate )
                    continue;

                const auto& meta = candidate->GetProgramMeta();
                // Same domain when the material has one; every assignable domain when it does not, which
                // is the only way out of a material pointing at an engine shader.
                const bool offer = assignable ? ( meta.Domain == domain ) : meta.IsUserAssignable();
                if ( !offer && name != current )
                    continue;

                // The current entry is listed even when it would not otherwise qualify. A combo that
                // cannot reproduce the value it is displaying is the defect this function was rewritten
                // for, and a shader that fails to compile keeps its name precisely so it stays visible.
                const bool selected = ( name == current );
                const std::string label    = candidate->IsCompiled() ? name : name + "  (does not compile)";

                if ( ImGui::Selectable( label.c_str(), selected ) && !selected )
                {
                    // Params always belong to a shader's schema — a switch clears them; the
                    // schema editor reseeds defaults on the next draw.
                    // By GUID (MATL 4): the picked shader's header GUID and stable path, resolved back to the
                    // name GetShaderName answers.
                    if ( m_AssetManager == nullptr )
                    {
                        LOG_ERROR( "[MaterialEditor] cannot state shader '{}': no asset manager", name );
                    }
                    else if ( const auto stated = Assets::SurfaceMaterialAsset::StateShaderByName(
                                   asset.Data(), *m_AssetManager, name );
                              !stated )
                    {
                        LOG_ERROR( "[MaterialEditor] cannot state shader: {}", stated.GetError() );
                    }
                    else
                    {
                        asset.Data().Params.clear();
                        asset.Data().Textures.clear();
                        asset.Data().CloudAssets.clear();
                        asset.ResolveDependencies( *m_AssetManager );
                        shaderChanged = true;
                    }
                }
                if ( selected )
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if ( hovered )
        {
            if ( assignable )
                ImGui::SetTooltip( "%s-domain shaders only.\nA material's domain decides how the renderer "
                                   "draws it at all; a shader from another domain would leave these "
                                   "parameters naming uniform fields it does not have, and nothing further "
                                   "down rejects that.",
                                   DomainName( domain ) );
            else
                ImGui::SetTooltip( "Every shader a material may use.\n'%s' is not one of them, so this list "
                                   "is the repair rather than the usual same-domain choice.",
                                   current.c_str() );
        }
        return shaderChanged;
    }

    // The parameter table's two columns, in one place because the table is now opened once per GROUP and a
    // second copy of the column widths would let two groups drift into different layouts down one window.
    static bool BeginParameterTable( const char* id )
    {
        if ( !ImGui::BeginTable( id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings ) )
            return false;
        ImGui::TableSetupColumn( "label", ImGuiTableColumnFlags_WidthStretch, 0.45f );
        ImGui::TableSetupColumn( "control", ImGuiTableColumnFlags_WidthStretch, 0.55f );
        return true;
    }

    // ONE GROUP'S HEADING. Returns whether the group's rows should be drawn.
    //
    // NUMBERED FROM THE PLAN, not from a table here: "00 · Cloud Types" is the group's position in the
    // shader file's own declaration order, which is the order the sky is built in. Epic number their cloud
    // material's groups the same way and for the same reason ("00 - Cloud Layout", "01 - Cloud Shape").
    //
    // DEFAULT OPEN, ALWAYS. A group closed on first sight hides parameters that were visible in this window
    // yesterday, and "the settings are gone" is the complaint that produces — the panel would be hiding its
    // own content to look tidy. ImGui remembers a header a person closed, per window, which is the editor's
    // existing convention and not a sixth place settings are stored (K8/K9/K10).
    // WHAT THIS GROUP'S EDITS COST TO SEE, appended to whatever the heading already said.
    //
    // IT IS THE ONE FACT ABOUT A CLOUD PARAMETER THAT CANNOT BE READ OFF THE CONTROL. Twenty of that
    // material's thirty-four values are inputs to a CPU bake of a 256x32x256 volume, so moving one of them
    // costs SECONDS during which the sky goes on showing the previous volume, while the other fourteen
    // answer in the frame that is drawn next. Drawn with no mark the two are indistinguishable, and the
    // owner reported the layer as "not updating" twice before anybody named the difference (task O8).
    //
    // FOLDED FROM THE MEMBERS by MaterialEdit::FoldGroupTiming and never from the group's NAME — a category
    // called "Weather" in some other shader owes this window nothing. A group whose members disagree says so
    // and defers to its rows rather than claiming an average, and a group nobody classified says NOTHING,
    // which is the honest answer and not the same as claiming "immediate".
    static std::string WithTimingSuffix( const std::string& heading, MaterialEdit::GroupTiming timing )
    {
        const char* phrase = MaterialEdit::GroupTimingPhrase( timing );
        return *phrase == '\0' ? heading : heading + "  \xE2\x80\x94  " + phrase;
    }

    static bool DrawParameterGroupHeader( const MaterialEdit::ParameterGroup& group )
    {
        if ( group.Kind == MaterialEdit::ParameterGroupKind::Inputs )
        {
            // THE MATERIAL'S DEPENDENCIES, GATHERED — what the owner asked to be able to see and choose
            // without opening each file. Unnumbered on purpose: the numbers belong to the shader author's
            // own stages, and this group is not one of them, it is every asset reference the schema
            // declares, pulled to the front.
            const std::string label = WithTimingSuffix( std::string( MaterialEdit::kInputsGroupName ) + " (" +
                                                             std::to_string( group.Params.size() ) + ")",
                                                        group.Timing ) +
                                      "##param_group_inputs";
            const bool open = ImGui::CollapsingHeader( label.c_str(), ImGuiTreeNodeFlags_DefaultOpen );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetTooltip( "The other documents this material is built from - cloud types and "
                                   "layouts. Each row opens its own editor; the sky is what they make "
                                   "together." );
            }
            return open;
        }

        if ( !group.Ordinal.has_value() )
        {
            // THE UNCATEGORISED GROUP, SAID OUT LOUD. These params declare no Category in the shader, and
            // that is a fact about the shader rather than a gap in this panel. Six shipped shaders have
            // them. Drawn under a heading that names the situation instead of being folded into the group
            // above — which would put a parameter under a title that does not describe it — or left in an
            // unnamed block, which reads as a rendering accident.
            const std::string label = WithTimingSuffix( "Uncategorised", group.Timing ) + "##param_group_none";
            const bool        open  = ImGui::CollapsingHeader( label.c_str(), ImGuiTreeNodeFlags_DefaultOpen );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetTooltip( "These parameters declare no Category in the shader's Properties block, "
                                   "so the file says nothing about where they belong. Give them one and they "
                                   "join the numbered groups above." );
            }
            return open;
        }

        char ordinal[8];
        std::snprintf( ordinal, sizeof( ordinal ), "%02zu", *group.Ordinal );
        const std::string label =
             WithTimingSuffix( std::string( ordinal ) + " \xC2\xB7 " + group.Category, group.Timing ) +
             "##param_group_" + group.Category;
        return ImGui::CollapsingHeader( label.c_str(), ImGuiTreeNodeFlags_DefaultOpen );
    }

    bool MaterialEditorPanel::DrawParameters( Assets::SurfaceMaterialAsset& asset,
                                              const Assets::MaterialData* parentData, bool isInstance )
    {
        // ONE RESOLUTION OF THE SCHEMA, shared with the property census and with a `set` arriving on the
        // control channel — see Schema(). Three resolutions would be three lists of parameter names.
        const ::Desert::Core::Formats::ShaderProgramMeta* meta = Schema();
        if ( !meta )
        {
            ImGui::TextDisabled( "Shader '%s' is not loaded", DrawnShaderName().c_str() );
            return false;
        }

        const auto& schema = *meta;
        if ( schema.Params.empty() )
        {
            ImGui::TextDisabled( "This shader exposes no parameters" );
            return false;
        }

        auto& data    = asset.Data();
        bool  changed = false;

        // Editing a value writes it into the material asset and nothing else: no recompile, no pipeline
        // rebuild. A parameter is a uniform-buffer field, so the cost of dragging this slider is the memcpy
        // MeshRenderer already does every frame — which is why parameters update LIVE while a change to the
        // graph's topology is debounced. Measured: a shader that has to be rebuilt costs ~123 ms per SPIR-V
        // module on this machine, and a Surface graph emits three.
        // Two columns, label cell then control cell. Drawing the label as ImGui's own trailing label instead
        // put it on top of the value: a colour row came out reading "A255e255 255 255", which is what
        // looking at the panel found and reading the code did not.

        // THE ORDER THE ROWS ARE DRAWN IN, AND IT IS NOT schema.Params. The plan groups the parameters by
        // the `Category` their author wrote — a field that had been filled in for a long time and read by
        // nothing, so the cloud material's thirty-four parameters arrived here as one undivided list.
        //
        // THE SAME PLAN FEEDS MaterialEdit::DescribeProperties, which is what `properties` on the control
        // channel answers with. One order, consumed twice — not two orders that agree by inspection. That
        // relation is the point: a client counting to the ninth entry and a person counting to the ninth row
        // have to arrive at the same parameter, and they only do while both walk this.
        const std::vector<MaterialEdit::ParameterGroup> groups = MaterialEdit::PlanParameterGroups( schema );

        // A shader that categorised NOTHING has exactly one group, with no name, and is drawn as the single
        // flat table this panel drew before groups existed — Terrain, Skybox, Unlit, TextSDF and the two
        // MatProbes are all in that state. "No categories in this shader" and "one group that happens to be
        // called nothing" must not produce the same picture, so the flat case keeps its old picture exactly.
        const bool grouped = MaterialEdit::HasNamedGroups( groups );

        std::vector<std::size_t> drawOrder;   // indices into schema.Params
        std::vector<std::size_t> groupOfSlot; // parallel: which group each drawn row belongs to
        drawOrder.reserve( schema.Params.size() );
        groupOfSlot.reserve( schema.Params.size() );
        for ( std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex )
        {
            for ( const std::size_t paramIndex : groups[groupIndex].Params )
            {
                drawOrder.push_back( paramIndex );
                groupOfSlot.push_back( groupIndex );
            }
        }

        bool        tableOpen  = false;
        bool        groupShown = true;
        std::size_t drawnGroup = groups.size(); // no group headed yet; the first row always opens one

        for ( std::size_t slot = 0; slot < drawOrder.size(); ++slot )
        {
            // A GROUP BOUNDARY CLOSES THE TABLE AND OPENS A NEW ONE around the heading, rather than drawing
            // the heading as a row inside it. A table row cannot span both columns without fighting the
            // column widths, and a heading sitting in the label column reads as a parameter whose value cell
            // somebody forgot to fill in.
            if ( grouped && groupOfSlot[slot] != drawnGroup )
            {
                if ( tableOpen )
                {
                    ImGui::EndTable();
                    tableOpen = false;
                }
                drawnGroup = groupOfSlot[slot];
                groupShown = DrawParameterGroupHeader( groups[drawnGroup] );
            }
            if ( !groupShown )
                continue;

            if ( !tableOpen )
            {
                // ONE TABLE ID PER GROUP. Reusing a single id for every group would put several tables under
                // one ImGui identity in one window, which is the collision that silently merges two widgets
                // into one — the same hazard SubjectId::ToString refuses a digest for.
                const std::string tableId = "##material_params_" + std::to_string( drawnGroup );
                if ( !BeginParameterTable( tableId.c_str() ) )
                    return changed;
                tableOpen = true;
            }

            const auto& p = schema.Params[drawOrder[slot]];

            using W  = ::Desert::Core::Formats::ShaderParamWidget;
            using VT = ::Desert::Core::Formats::ShaderValueType;

            const char*       label    = p.DisplayName.empty() ? p.Name.c_str() : p.DisplayName.c_str();
            const std::string hiddenId = "##mp_" + p.Name; // control id; the label lives in its own cell

            // Instance mode: a row with its own entry in the child IS an override — mark it.
            const bool overridden = isInstance && !p.IsTexture && data.FindParam( p.Name ) != nullptr;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if ( overridden )
                ImGui::TextColored( ImVec4( 1.0f, 0.85f, 0.4f, 1.0f ), "%s *", label );
            else
                ImGui::TextUnformatted( label );
            // The schema's own Tooltip attribute, on the LABEL: the cloud material carries the calibrated
            // tooltips its component fields used to, and a parameter whose meaning the panel cannot show
            // is a parameter the artist reads the shader file to use.
            //
            // AND THE ROW'S OWN TIMING UNDER IT. The heading carries the group's answer, but a group whose
            // members disagree cannot speak for them — and a person reading ONE control should not have to
            // scroll up to learn whether moving it costs a frame or fourteen seconds. Drawn only when the
            // schema states one: an unclassified parameter gets no sentence rather than a guessed one.
            const char* timingPhrase = MaterialEdit::ParamTimingPhrase( p.Timing );
            if ( ( !p.Tooltip.empty() || *timingPhrase != '\0' ) && ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos( ImGui::GetFontSize() * 30.0f );
                if ( !p.Tooltip.empty() )
                    ImGui::TextUnformatted( p.Tooltip.c_str() );
                if ( *timingPhrase != '\0' )
                {
                    if ( !p.Tooltip.empty() )
                        ImGui::Spacing();
                    ImGui::TextDisabled( "%s", timingPhrase );
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            // RESET TO WHAT THIS ROW INHERITS — the shader's declared default on a base material, the
            // parent chain's value on an instance. Which of the two, and why the two are not one thing, is
            // MaterialEdit::ResetOfferedFor; this site only draws what it is told.
            //
            // ALWAYS VISIBLE WHEN OFFERED, NEVER ON HOVER, and that is a decision rather than a shortcut.
            // Details' row furniture was laid out from a hover flag, and pressing its reset arrow made the
            // row report itself unhovered on the next frame, which moved the arrow one button-width out
            // from under the press (555e8681). Nothing here is positioned from hover, so that trap cannot
            // reach this control — and a condition that is true or false for the whole drag is also what
            // keeps the arrow from flickering while a neighbouring slider is dragged past it.
            if ( const MaterialEdit::RowReset reset =
                      MaterialEdit::ResetOfferedFor( data, parentData, p, isInstance );
                 reset != MaterialEdit::RowReset::None )
            {
                // Right-aligned in the LABEL cell, as in Details — the value column is the widget's, and a
                // button sharing it would move every slider's right edge by a button width on the rows that
                // happen to differ from their default.
                const float buttonWidth = ImGui::GetFrameHeight();
                ImGui::SameLine();
                if ( const float avail = ImGui::GetContentRegionAvail().x; avail > buttonWidth )
                    ImGui::SetCursorPosX( ImGui::GetCursorPosX() + avail - buttonWidth );

                ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
                ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetSelectedColor() );
                // The id carries the parameter name: every row draws the same glyph, and ImGui keys a
                // button on its label, so one id would make all thirty-three rows the same button.
                if ( ImGui::SmallButton( ( ICON_MDI_BACKUP_RESTORE + hiddenId + "_reset" ).c_str() ) )
                    ResetParam( p );
                ImGui::PopStyleColor( 2 );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", MaterialEdit::ResetTooltip( reset ) );
            }

            ImGui::TableNextColumn();
            ImGui::PushItemWidth( -FLT_MIN );

            // Non-texture asset references (CloudType / CloudLayout / the authored Medium) — the schema
            // declares the asset class, the value rides the same name->handle map the textures use. The
            // first two are the rows the Details panel drew while these were component fields, moved here
            // with the fields (O1); the third is a Volume shader graph. All three share the same three
            // properties: there is nothing to thumbnail, the file is authored in its own window, and an
            // empty slot MEANS something good rather than something missing.
            if ( p.IsAssetRef() )
            {
                if ( isInstance )
                {
                    // Same v1 restriction as textures, same reason: asset references travel in the
                    // Textures map, which instances take whole from the parent.
                    ImGui::TextDisabled( "from parent material" );
                    ImGui::PopItemWidth();
                    continue;
                }
                if ( DrawCloudAssetRef( data, p, hiddenId ) )
                    changed = true;
                ImGui::PopItemWidth();
                continue;
            }

            if ( p.IsTexture )
            {
                if ( isInstance )
                {
                    // v1: textures always come from the parent (per-instance texture overrides
                    // need their own descriptor sets — the batched SSBO path can't carry them).
                    ImGui::TextDisabled( "from parent material" );
                    ImGui::PopItemWidth();
                    continue;
                }

                // A CUBE slot takes an HDR skybox asset, not a 2D texture — different asset type,
                // different drop payload, and a picker of its own (the same trio the Skybox component
                // widget offers), so it is a separate row rather than a flag on the 2D one.
                if ( p.IsCubeTexture )
                {
                    std::string disp = "<drop or pick HDR skybox>";
                    if ( const uint64_t h = data.GetTexture( p.Name ); h != 0 && m_AssetManager )
                    {
                        if ( auto sky = m_AssetManager->FindByHandle<Assets::SkyboxAsset>( Common::UUID( h ) ) )
                            disp = sky->GetMetadata().Filepath.filename().string();
                        else
                            disp = "<missing skybox>";
                    }

                    auto bindSkybox = [&]( const Assets::AssetHandle& handle )
                    {
                        // The service caches the baked environment per asset; everything preloaded is
                        // already registered, so this only fires for a skybox created mid-session.
                        auto* svc = Runtime::ResourceRegistry::GetSkyboxService();
                        if ( svc && !svc->Get( handle ) && m_AssetManager )
                        {
                            if ( auto a = m_AssetManager->FindByHandle<Assets::SkyboxAsset>( handle ) )
                            {
                                Graphic::Renderer::GetInstance().WaitDeviceIdle();
                                svc->Register( a );
                            }
                        }
                        // By the panorama's header GUID (MATL 3): a skybox that states none cannot be named.
                        const auto sky = m_AssetManager
                                              ? m_AssetManager->FindByHandle<Assets::SkyboxAsset>( handle )
                                              : nullptr;
                        if ( !sky || sky->Guid().IsNull() )
                        {
                            LOG_ERROR( "[MaterialEditor] skybox {} states no header GUID, so the '{}' slot cannot "
                                       "name it and is left as it was",
                                       static_cast<uint64_t>( handle ), p.Name );
                            return;
                        }
                        data.SetTexture( p.Name, sky->Guid(),
                                         Common::AssetHandle::StableKeyForPath( sky->GetMetadata().Filepath ) );
                        changed = true;
                    };

                    if ( ImGui::Button( ( disp + hiddenId ).c_str(), ImVec2( -FLT_MIN, 0.0f ) ) )
                        ImGui::OpenPopup( ( "cube_selector" + hiddenId ).c_str() );
                    if ( ImGui::BeginDragDropTarget() )
                    {
                        const char* types[] = { ::Desert::Editor::DragPayloads::SkyboxAsset,
                                                ::Desert::Editor::DragPayloads::AssetFile };
                        for ( const char* t : types )
                        {
                            if ( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload( t ) )
                            {
                                const std::string path( static_cast<const char*>( pl->Data ) );
                                if ( m_AssetManager )
                                {
                                    if ( auto a = m_AssetManager->FindByPath<Assets::SkyboxAsset>( path ) )
                                        bindSkybox( a->GetMetadata().Handle );
                                }
                                break;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if ( ImGui::BeginPopup( ( "cube_selector" + hiddenId ).c_str() ) )
                    {
                        if ( m_AssetManager )
                        {
                            const auto skyboxes = m_AssetManager->FindAllByType<Assets::SkyboxAsset>();
                            for ( const auto& [handle, sky] : skyboxes )
                            {
                                const std::string name = sky->GetMetadata().Filepath.filename().string();
                                if ( ImGui::Selectable( name.c_str(), static_cast<uint64_t>( handle ) ==
                                                                           data.GetTexture( p.Name ) ) )
                                    bindSkybox( handle );
                            }
                            if ( skyboxes.empty() )
                                ImGui::TextDisabled( "No HDR skyboxes in this project"
                                                     " (drop a .hdr under Assets/Textures/HDR)" );
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopItemWidth();
                    continue;
                }

                // A NAME FOR THE EMPTY STATE THAT IS NOT AN INSTRUCTION TO DRAG, because dragging is no
                // longer the only way in — see the picker below.
                std::string disp = "<none - pick or drop a texture>";
                if ( const uint64_t h = data.GetTexture( p.Name ); h != 0 && m_AssetManager )
                {
                    if ( auto tex = m_AssetManager->FindByHandle<Assets::TextureAsset>( Common::UUID( h ) ) )
                    {
                        const auto& src  = tex->GetSourcePath();
                        const auto  path = !src.empty() ? src : tex->GetMetadata().Filepath.string();
                        disp             = std::filesystem::path( path ).filename().string();
                    }
                }

                // THE BUTTON DOES SOMETHING NOW. Its result used to be DISCARDED — the row drew a control
                // that looks pressable, said "<drop texture>", and answered a click with nothing, while
                // the CUBE slot forty lines above opened a picker on exactly the same gesture. An artist
                // who clicks and waits has no way to tell that apart from a broken editor, and "I cannot
                // add a texture in the material editor" is what it gets reported as. Same popup, same
                // shape, same reason as the cube slot's (DC §1.3: a control that does nothing may not
                // ship).
                //
                // THE "None" ENTRY EXISTS NOW, and the paragraph that used to stand here explaining why it
                // could not is gone with the defect it described. It said, correctly at the time, that a
                // texture could not be UNBOUND: SetTexture refused a null image, ApplyShaderAsset skipped
                // handle 0, so clearing emptied the `.demat` while the material kept sampling the old
                // texture — a control that changes the document and not the picture. M9 gave
                // ShaderParam::DefaultTexture its first reader, so handle 0 now means the schema's own
                // default and the entry is a real operation rather than a lie. See the popup below.
                if ( ImGui::Button( ( disp + hiddenId ).c_str(), ImVec2( -FLT_MIN, 0.0f ) ) )
                    ImGui::OpenPopup( ( "texture_selector" + hiddenId ).c_str() );
                if ( ImGui::BeginDragDropTarget() )
                {
                    if ( const ImGuiPayload* pl =
                              ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::TextureAsset ) )
                    {
                        const std::string path( static_cast<const char*>( pl->Data ),
                                                pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                        if ( m_AssetManager )
                        {
                            const auto resolved =
                                 ::Desert::Editor::TextureDnD::ResolveOrImport( *m_AssetManager, path );
                            if ( const auto tex =
                                      static_cast<uint64_t>( resolved ) != 0
                                           ? m_AssetManager->FindByHandle<Assets::TextureAsset>( resolved )
                                           : nullptr;
                                 tex && !tex->Guid().IsNull() )
                            {
                                data.SetTexture(
                                     p.Name, tex->Guid(),
                                     Common::AssetHandle::StableKeyForPath( tex->GetMetadata().Filepath ) );
                                changed = true;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if ( ImGui::BeginPopup( ( "texture_selector" + hiddenId ).c_str() ) )
                {
                    if ( m_AssetManager )
                    {
                        const auto     textures = m_AssetManager->FindAllByType<Assets::TextureAsset>();
                        const uint64_t bound    = data.GetTexture( p.Name );

                        // UNBINDING, WHICH WAS NOT EXPRESSIBLE UNTIL M9. This entry was deliberately absent
                        // while clearing a slot emptied the .demat and left the material sampling the OLD
                        // texture — an entry that hides a defect is worse than a missing one (§1.3). It
                        // exists now because ShaderParam::DefaultTexture finally has a reader
                        // (Material::BindSchemaDefaultTexture), so handle 0 means "the schema's own default"
                        // rather than "nothing happened". Measured on a live editor with the counterfactual
                        // built: under the old behaviour clearing moved 0.4 % of floor pixels, max delta
                        // 4/255 — the checker was still there; with the reader, 100 % and 68/255.
                        if ( ImGui::Selectable( "None", bound == 0 ) )
                        {
                            data.SetTexture( p.Name, {}, {} );
                            changed = true;
                        }
                        if ( bound == 0 )
                            ImGui::SetItemDefaultFocus();
                        ImGui::Separator();

                        for ( const auto& [handle, texture] : textures )
                        {
                            // The SOURCE path when there is one, exactly as the button label above resolves
                            // it: a cooked asset's own filepath is a hash nobody recognises, and a list of
                            // those is a list of nothing.
                            const auto& source = texture->GetSourcePath();
                            const auto  path = !source.empty() ? source : texture->GetMetadata().Filepath.string();
                            const std::string name = std::filesystem::path( path ).filename().string();
                            if ( ImGui::Selectable( name.c_str(), static_cast<uint64_t>( handle ) == bound ) &&
                                 !texture->Guid().IsNull() )
                            {
                                data.SetTexture(
                                     p.Name, texture->Guid(),
                                     Common::AssetHandle::StableKeyForPath( texture->GetMetadata().Filepath ) );
                                changed = true;
                            }
                            if ( static_cast<uint64_t>( handle ) == bound )
                                ImGui::SetItemDefaultFocus();
                        }
                        if ( textures.empty() )
                        {
                            // SAID, not an empty popup that reads as a broken menu. The list is what the
                            // project has imported, so "there is nothing here" is an instruction.
                            ImGui::TextDisabled( "No textures imported yet - drop an image into\n"
                                                 "Resources/Assets/Textures, or drag one onto this slot." );
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopItemWidth();
                continue;
            }

            // Seed with: child override -> parent's effective value (instance mode) -> schema default.
            // Through MaterialEdit::EffectiveParamValue, which is also what the property census reads, so a
            // client that asks what this document is showing gets the number that is on the screen.
            glm::vec4 value  = MaterialEdit::EffectiveParamValue( data, parentData, p );
            bool      edited = false;

            if ( p.Widget == W::Color )
            {
                edited = ( p.Type == VT::Float3 ) ? ImGui::ColorEdit3( hiddenId.c_str(), &value.x )
                                                  : ImGui::ColorEdit4( hiddenId.c_str(), &value.x );
            }
            else if ( p.Type == VT::Int )
            {
                // An Int schema param stores in vec4.x like everything else; the widget is what keeps an
                // artist from authoring 2.37 octaves and wondering which sky that is. Before this branch
                // an Int drew as a float drag — the cloud schema is the first to carry Int params.
                int iv = static_cast<int>( value.x );
                if ( p.Min.has_value() && p.Max.has_value() )
                    edited = ImGui::SliderInt( hiddenId.c_str(), &iv, static_cast<int>( *p.Min ),
                                               static_cast<int>( *p.Max ) );
                else
                    edited = ImGui::DragInt( hiddenId.c_str(), &iv );
                if ( edited )
                    value.x = static_cast<float>( iv );
            }
            else if ( p.Type == VT::Bool )
            {
                bool bv = value.x > 0.5f;
                edited  = ImGui::Checkbox( hiddenId.c_str(), &bv );
                if ( edited )
                    value.x = bv ? 1.0f : 0.0f;
            }
            else
            {
                const int comps = ( p.Type == VT::Float2 )   ? 2
                                  : ( p.Type == VT::Float3 ) ? 3
                                  : ( p.Type == VT::Float4 ) ? 4
                                                             : 1;
                if ( p.Min.has_value() && p.Max.has_value() )
                {
                    float mn = *p.Min;
                    float mx = *p.Max;
                    edited =
                         ImGui::SliderScalarN( hiddenId.c_str(), ImGuiDataType_Float, &value.x, comps, &mn, &mx );
                }
                else
                {
                    edited = ImGui::DragScalarN( hiddenId.c_str(), ImGuiDataType_Float, &value.x, comps, 0.01f );
                }
            }

            if ( edited )
            {
                // THROUGH WriteParam, the one write — the same function a `set` on the control channel
                // reaches. `changed` is deliberately NOT raised here: WriteParam has already published, and
                // the caller's publish exists for the texture and asset-reference rows above, which write
                // MaterialData::Textures directly and have no schema-checked setter to go through.
                WriteParam( p, value );
            }
            ImGui::PopItemWidth();
        }

        // Conditional because the last group may have been collapsed, and because a schema whose every
        // group is collapsed opens no table at all. An unconditional EndTable there is a mismatched pair.
        if ( tableOpen )
            ImGui::EndTable();
        return changed;
    }

    bool MaterialEditorPanel::DrawCloudAssetRef( Assets::MaterialData&                       data,
                                                 const ::Desert::Core::Formats::ShaderParam& p,
                                                 const std::string&                          hiddenId )
    {
        bool       changed  = false;
        const bool isType   = p.AssetKind == "CloudTypeAsset";
        const bool isLayout = p.AssetKind == "CloudLayoutAsset";
        // Every cloud slot, the Medium shader included (MATL 4), names its asset by header GUID.
        const uint64_t handle = data.GetCloudAsset( p.Name );

        // ---- THE AUTHORED MEDIUM ---------------------------------------------------------------------
        //
        // A reference to a Volume-domain shader graph — what a cloud IS at a point in space. It is drawn
        // by its own block rather than threaded through the two above because almost nothing is shared:
        // the candidates come from the shader tree instead of an asset type, "empty" means the ENGINE's
        // medium rather than a built-in asset, and there is no document to open (see the note at the
        // missing Edit button).
        if ( p.AssetKind == "ShaderAsset" )
        {
            // Every registered `.shader` that declares a Medium block. Enumerated only while the combo is
            // OPEN, because it parses shader source and there are hundreds of shaders.
            std::string preview = "Engine default";
            if ( handle != 0 )
            {
                preview = "(missing)";
                if ( m_AssetManager )
                {
                    if ( auto shader =
                              m_AssetManager->FindByHandle<Assets::ShaderAsset>( Common::UUID( handle ) ) )
                        preview = shader->GetMetadata().Filepath.stem().string();
                }
            }

            if ( ImGui::BeginCombo( hiddenId.c_str(), preview.c_str() ) )
            {
                if ( ImGui::Selectable( "Engine default", handle == 0 ) && handle != 0 )
                {
                    data.SetCloudAsset( p.Name, {}, {} );
                    changed = true;
                }
                if ( m_AssetManager )
                {
                    for ( const auto& [h, shader] : m_AssetManager->FindAllByType<Assets::ShaderAsset>() )
                    {
                        if ( !shader )
                            continue;
                        // Parsed rather than guessed from the path: "is this a medium" is a property of
                        // the FILE, and a folder convention would let a shader in the wrong directory be
                        // offered for a slot that cannot use it.
                        const auto parsed =
                             ::Desert::Core::Preprocess::DShaderParser::Parse( shader->GetShaderContent() );

                        // TWO SKIPS AND THEY ARE NOT THE SAME EVENT, which is what GpuWriteCensus went
                        // red about and it was right. "This shader is not a medium" is a FILTER — most
                        // shaders are not, and saying so about each of them would be noise. "This shader
                        // would not parse" is a REFUSAL: the file is broken, and a broken file that
                        // simply vanishes from a list looks exactly like a file the artist never saved.
                        // It is shown, greyed and unpickable, the same way the material shader picker
                        // shows one that does not compile.
                        const auto name = shader->GetMetadata().Filepath.stem().string();
                        if ( !parsed.IsSuccess() )
                        {
                            ImGui::TextDisabled( "%s  (does not parse)", name.c_str() );
                            continue;
                        }
                        if ( !parsed.GetValue().Meta.IsMediumProgram() )
                            continue;

                        const bool selected = ( static_cast<uint64_t>( h ) == handle );
                        const auto label    = name;
                        if ( ImGui::Selectable( label.c_str(), selected ) )
                        {
                            const auto& file = shader->GetMetadata().Filepath;
                            data.SetCloudAsset( p.Name, Assets::ReadShaderHeaderGuid( file ),
                                                Common::AssetHandle::StableKeyForPath( file ) );
                            changed = true;
                        }
                        if ( selected )
                            ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // NO "EDIT" BUTTON, AND IT IS A MEASURED ABSENCE RATHER THAN AN OVERSIGHT. What this slot
            // holds is the COMPILED artifact of a graph; the document an artist would want to open is the
            // `.dgraph` it was compiled from, and a `.dgraph` carries no identity at all — no GUID, no
            // back-reference — so the owner cannot be resolved from here (task У7-2's measurement, and
            // task У14 is where that would be fixed). Offering a button that opened the generated
            // `.shader` as text would answer a different question than the one being asked.
            return changed;
        }

        if ( !isType && !isLayout )
        {
            // A kind this panel has no row for is a schema ahead of this binary — said instead of drawn
            // wrong, and the value the material stores is left exactly as it is.
            ImGui::TextDisabled( "unknown asset kind '%s'", p.AssetKind.c_str() );
            return false;
        }

        // "Default"/"None" mean something in both slots — the built-in congestus, the procedural sky —
        // so the empty state is named rather than left reading as unfilled. Same rows the Details panel
        // drew while these were component fields; moved here with them (O1).
        std::string preview = isType ? "Default (cumulus congestus)" : "None (procedural weather)";
        if ( handle != 0 )
        {
            preview = "(missing)";
            if ( m_AssetManager )
            {
                if ( isType )
                {
                    if ( auto type =
                              m_AssetManager->FindByHandle<Assets::CloudTypeAsset>( Common::UUID( handle ) ) )
                        preview = type->GetDisplayName();
                }
                else if ( auto painting =
                               m_AssetManager->FindByHandle<Assets::CloudLayoutAsset>( Common::UUID( handle ) ) )
                {
                    preview = painting->GetMetadata().Filepath.filename().string();
                }
            }
        }

        // ROOM FOR THE EDIT BUTTON, TAKEN BEFORE THE COMBO IS DRAWN. The caller pushes an item width of
        // -FLT_MIN, so the combo fills the cell to its right edge and anything SameLine'd after it lands
        // outside — drawn, clipped, and invisible, which is how the first version of this button shipped
        // into a frame that showed nothing. The width is narrowed only when there IS a button.
        const bool  canOpenDocument = ( handle != 0 && m_AssetManager != nullptr );
        const float editButtonWidth = canOpenDocument ? ImGui::CalcTextSize( "Edit" ).x +
                                                             ImGui::GetStyle().FramePadding.x * 2.0f +
                                                             ImGui::GetStyle().ItemSpacing.x
                                                      : 0.0f;
        if ( canOpenDocument )
            ImGui::SetNextItemWidth( std::max( ImGui::GetContentRegionAvail().x - editButtonWidth, 1.0f ) );

        if ( ImGui::BeginCombo( hiddenId.c_str(), preview.c_str() ) )
        {
            const char* emptyLabel = isType ? "Default (cumulus congestus)" : "None (procedural weather)";
            if ( ImGui::Selectable( emptyLabel, handle == 0 ) && handle != 0 )
            {
                data.SetCloudAsset( p.Name, {}, {} );
                changed = true;
            }
            if ( m_AssetManager && isType )
            {
                for ( const auto& [h, type] : m_AssetManager->FindAllByType<Assets::CloudTypeAsset>() )
                {
                    const bool selected = ( static_cast<uint64_t>( h ) == handle );
                    if ( ImGui::Selectable( type->GetDisplayName().c_str(), selected ) && !type->Guid().IsNull() )
                    {
                        data.SetCloudAsset(
                             p.Name, type->Guid(),
                             Common::AssetHandle::StableKeyForPath( type->GetMetadata().Filepath ) );
                        changed = true;
                    }
                    if ( selected )
                        ImGui::SetItemDefaultFocus();
                }
            }
            else if ( m_AssetManager && isLayout )
            {
                for ( const auto& [h, painting] : m_AssetManager->FindAllByType<Assets::CloudLayoutAsset>() )
                {
                    const bool selected = ( static_cast<uint64_t>( h ) == handle );
                    if ( ImGui::Selectable( painting->GetMetadata().Filepath.filename().string().c_str(),
                                            selected ) &&
                         !painting->Guid().IsNull() )
                    {
                        data.SetCloudAsset(
                             p.Name, painting->Guid(),
                             Common::AssetHandle::StableKeyForPath( painting->GetMetadata().Filepath ) );
                        changed = true;
                    }
                    if ( selected )
                        ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // OPEN WHAT IS IN THE SLOT. A picture becomes part of the sky by being imported INTO a layout, and
        // until this button the only way to reach that document from here was to go and find the file in
        // the Content Browser — which is why "I cannot add a texture to a cloud material" was the report.
        // The slot now names its own next step instead of being a dead end.
        if ( canOpenDocument )
        {
            ImGui::SameLine();
            if ( ImGui::Button( ( "Edit" + hiddenId + "_open" ).c_str() ) )
            {
                if ( isType )
                {
                    if ( auto asset =
                              m_AssetManager->FindByHandle<Assets::CloudTypeAsset>( Common::UUID( handle ) ) )
                        QueueCloudSubjectOpen( asset->GetMetadata().Handle, Assets::CloudTypeAsset::GetTypeID() );
                }
                else if ( auto asset =
                               m_AssetManager->FindByHandle<Assets::CloudLayoutAsset>( Common::UUID( handle ) ) )
                {
                    QueueCloudSubjectOpen( asset->GetMetadata().Handle, Assets::CloudLayoutAsset::GetTypeID() );
                }
            }
            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetTooltip( isType ? "Open this cloud type in its own document."
                                          : "Open this layout in its own document - where a picture is "
                                            "imported into it, painted on, and exported." );
            }
        }

        if ( ImGui::BeginDragDropTarget() )
        {
            // BOTH PAYLOADS, and accepting only the generic one was a live defect here exactly as it was in
            // CloudLayoutPanel's image slots. FileExplorerPanel::EmitAssetDragSource types the payload by
            // FileType, and every image is FileType::Texture — so the browser emits TEXTURE_ASSET for a
            // `.png` and falls back to AssetFile only for what it has no specific type for (which is what
            // `.dclayout` and `.decloudtype` are). An id that does not match fails SILENTLY in ImGui:
            // nothing logs, nothing draws, the drop simply does nothing. Accepting the texture payload is
            // what lets the refusal below exist at all — a drop this target never sees cannot be answered.
            const ImGuiPayload* pl = nullptr;
            for ( const char* accepted :
                  { ::Desert::Editor::DragPayloads::AssetFile, ::Desert::Editor::DragPayloads::TextureAsset } )
            {
                pl = ImGui::AcceptDragDropPayload( accepted );
                if ( pl )
                    break;
            }

            if ( pl )
            {
                const std::string path( static_cast<const char*>( pl->Data ),
                                        pl->DataSize > 0 ? pl->DataSize - 1 : 0 );
                // The extension is checked HERE because the Content Browser emits one generic AssetFile
                // payload for every type it has no icon for — without it this slot would bind a dropped
                // .dcnv to a file that can never parse as what the slot means.
                const char* wantedExt = isType ? Assets::kCloudTypeExtension : Assets::kCloudLayoutExtension;
                if ( m_AssetManager && !path.empty() && std::filesystem::path( path ).extension() != wantedExt )
                {
                    // SAID, NOT SWALLOWED (DC §1.4). A drop that lands on the right slot and does nothing is
                    // indistinguishable from a broken editor, and the artist's next move is to try it again
                    // harder. The sentence names what arrived, what this slot takes, and the step that
                    // actually gets a picture into the sky — because for a picture there IS one and it is
                    // two clicks away, not a refusal to be argued with.
                    m_DropRefusal.Param   = p.Name;
                    m_DropRefusal.Message = MaterialEdit::WhyThatCannotGoInThisSlot(
                         path, isType, Assets::kCloudTypeExtension, Assets::kCloudLayoutExtension );
                    LOG_WARN( "[MaterialEditor] '{}' was dropped on the '{}' slot and was not bound: {}", path,
                              p.Name, m_DropRefusal.Message );
                }
                else if ( m_AssetManager && !path.empty() )
                {
                    if ( isType )
                    {
                        auto type = m_AssetManager->FindByPath<Assets::CloudTypeAsset>( path );
                        if ( !type )
                            type = m_AssetManager->CreateAsset<Assets::CloudTypeAsset>(
                                 Assets::AssetPriority::Medium, path );
                        if ( type && type->IsReadyForUse() && !type->Guid().IsNull() )
                        {
                            if ( const auto registered =
                                      Runtime::ResourceRegistry::GetCloudTypeService()->Register( type );
                                 !registered )
                                LOG_ERROR( "[Clouds] Dropped cloud type '{}' could not be registered: {}", path,
                                           registered.GetError() );
                            data.SetCloudAsset(
                                 p.Name, type->Guid(),
                                 Common::AssetHandle::StableKeyForPath( type->GetMetadata().Filepath ) );
                            changed = true;
                        }
                    }
                    else
                    {
                        auto painting = m_AssetManager->FindByPath<Assets::CloudLayoutAsset>( path );
                        if ( !painting )
                            painting = m_AssetManager->CreateAsset<Assets::CloudLayoutAsset>(
                                 Assets::AssetPriority::Medium, path );
                        if ( painting && painting->IsReadyForUse() && !painting->Guid().IsNull() )
                        {
                            if ( const auto registered =
                                      Runtime::ResourceRegistry::GetCloudLayoutService()->Register( painting );
                                 !registered )
                                LOG_ERROR( "[Clouds] Dropped cloud layout '{}' could not be registered: {}", path,
                                           registered.GetError() );
                            data.SetCloudAsset(
                                 p.Name, painting->Guid(),
                                 Common::AssetHandle::StableKeyForPath( painting->GetMetadata().Filepath ) );
                            changed = true;
                        }
                    }

                    // A FILE OF THE RIGHT KIND THAT WOULD NOT LOAD used to leave the slot untouched and say
                    // nothing, which looks exactly like the payload mismatch above and has a completely
                    // different cause. Both are now answered; only the answers differ.
                    if ( !changed )
                    {
                        m_DropRefusal.Param = p.Name;
                        m_DropRefusal.Message =
                             "'" + std::filesystem::path( path ).filename().string() +
                             "' is the right kind of file for this slot but would not load. The load error "
                             "above says why; the slot was left as it was.";
                        LOG_ERROR( "[MaterialEditor] '{}' was dropped on the '{}' slot and would not load, so "
                                   "the slot keeps what it had.",
                                   path, p.Name );
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // WHY THE LAST DROP ON THIS ROW DID NOTHING, under the row it was aimed at. In the panel and not
        // only in the log, because the log is a different window and the artist is looking here — and
        // because "I dropped it and nothing happened" is precisely the report this exists to answer.
        // Cleared by any successful bind on this row, so it cannot outlive the confusion it explains.
        if ( changed && m_DropRefusal.Param == p.Name )
            m_DropRefusal = {};

        if ( m_DropRefusal.Param == p.Name && !m_DropRefusal.Message.empty() )
        {
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ) );
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextUnformatted( m_DropRefusal.Message.c_str() );
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        return changed;
    }

    void MaterialEditorPanel::DrawPreviewSceneTab()
    {
        if ( !m_Preview )
            return;

        // THE WIDGET'S OWN COPY, edited in place. There is no push call and no Apply: PreviewViewport
        // writes whatever it finds here onto its entities on the next frame it records, so a control
        // added below is live the moment it is drawn and cannot be forgotten by a plumbing step.
        PreviewViewport::SceneSetup& setup = m_Preview->Setup();
        const bool                   dome  = ( m_Preview->GetFill() == PreviewViewport::Fill::SkyDome );

        ImGui::TextDisabled( "Settings of the scene this material is shown in. They belong to this window "
                             "and are not saved with the material." );
        ImGui::Separator();

        if ( ImGui::CollapsingHeader( "Environment", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // The SHARED preset table, walked rather than listed here — adding a preset is one row in
            // Graphic::kSkyPresets and it appears in this combo, in Details, and in the thumbnails.
            ImGui::SetNextItemWidth( -FLT_MIN );
            if ( ImGui::BeginCombo( "##sky_preset", Graphic::SkyPresetName( setup.Sky ) ) )
            {
                for ( const auto& entry : Graphic::kSkyPresets )
                {
                    const bool selected = ( entry.Id == setup.Sky );
                    if ( ImGui::Selectable( entry.Name, selected ) )
                        setup.Sky = entry.Id;
                    if ( selected )
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Sky Intensity", &setup.SkyIntensity, 0.0f, 4.0f );

            // THE GRADE, and it is a row because a preview that grades differently from the level is a
            // preview an artist cannot tune against. Same range as the level's own Exposure in Scene
            // Settings, so the number typed here is the number typed there.
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Exposure", &setup.Exposure, 0.0f, 10.0f, "%.2f" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "The Exposure this pane grades through - the same field a level carries "
                                   "in Scene Settings. A cloud material opens at 0.26, which is what 50 of "
                                   "the 51 cloud scenes in this project author; set it to your level's "
                                   "value if that level disagrees." );

            // ROTATION IS NOT OFFERED, and its absence is a decision rather than an omission. For a
            // preset sky the environment's rotation IS the sun's azimuth — the row below — so a second
            // control would be two knobs on one number; and for an HDR environment the engine has no
            // rotation at all, on the sky, the skybox or in any shader. See the report.
        }

        if ( ImGui::CollapsingHeader( "Light", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            ImGui::TextDisabled( "Hold L over the preview and drag to move the sun." );

            // ONE SUN. These two angles are the key light AND the sun baked into the sky; there is no
            // second pair, because a material lit from the left under a sun visibly on the right is a
            // preview an artist would trust and should not.
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Sun Bearing", &setup.SunYawDegrees, -180.0f, 180.0f, "%.0f deg" );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Sun Elevation", &setup.SunPitchDegrees, 1.0f, 89.0f, "%.0f deg" );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Intensity", &setup.LightIntensity, 0.0f, 30.0f );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::ColorEdit3( "Light Colour", &setup.LightColor.x );
        }

        if ( ImGui::CollapsingHeader( dome ? "Ground" : "Floor", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // THE GROUND IS NOT OPTIONAL UNDER A DOME. A cloud deck's shadow and the light it throws down
            // are part of what the material looks like, so the rows are shown as the fixed facts they are
            // rather than as controls that would be refused on click.
            if ( dome )
            {
                ImGui::TextDisabled( "The ground is part of the picture here: the deck's own shadow, and "
                                     "the light it throws down, are part of what this material looks "
                                     "like." );
            }
            else
            {
                ImGui::Checkbox( "Show Floor", &setup.ShowFloor );
                ImGui::BeginDisabled( !setup.ShowFloor );
                ImGui::Checkbox( "Receive Shadow", &setup.FloorReceivesShadow );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "The cast shadow is the point of the floor, and the only part of it "
                                       "that is not free: one 1024 cascade over 10 m, about 20 MB for this "
                                       "window." );
                ImGui::SetNextItemWidth( -FLT_MIN );
                ImGui::SliderFloat( "Size (cm)", &setup.FloorSize, 100.0f, 2000.0f, "%.0f" );
                ImGui::EndDisabled();
            }

            // Offered in BOTH, because the surface it colours exists in both — the dome's ground is the
            // same entity. A field the widget applies and only one mode lets you reach is the dead-setting
            // shape wearing a layout.
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::ColorEdit3( dome ? "Ground Colour" : "Floor Colour", &setup.FloorColour.x );
            // Not in the dome: an authoring grid drawn across a 20 km ground under a cloudscape is
            // scenery from a different picture, and the row would be offering it as if it were useful.
            if ( !dome )
                ImGui::Checkbox( "Show Grid", &setup.ShowGrid );
        }

        // The dome's budget, and only where there is a dome. These are the two fields that decide what an
        // open cloud-material window costs every frame, so they are shown rather than hidden in a constant.
        if ( dome && ImGui::CollapsingHeader( "Cloud Tracing", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            // WHAT THIS PANE IS NOT, WITH THE NUMBERS, and the line it replaces named two of the six.
            // "This preview's own budget. The scene's layer keeps its component's." was true and it let a
            // person believe the rest of the frame was the level's. It is not: the lens is more than twice
            // as wide, the march is cheaper and the occlusion tier is the cheap one. A divergence a person
            // cannot see is a divergence they will author against — which is the whole of the complaint
            // this text exists to answer, so it is written here rather than in a report nobody opens.
            //
            // WHAT IS DELIBERATELY ABSENT FROM THE LIST: the bake grid. It used to belong on it and no
            // longer does — Graphic::CloudBakeSideForSpecies raises this pane's grid whenever the cheap one
            // could not express a type's placement cell, so the field baked here is the field the level
            // bakes. That one was not a budget, it was a different sky.
            ImGui::TextDisabled( "This pane is not the level's camera and not the level's budget:" );
            ImGui::BulletText( "96 deg of vertical field against the 45 every camera in this project\n"
                               "authors: whatever fills a level's frame covers a third of this one" );
            ImGui::BulletText( "the march and the give-up point below, against a layer's 256 and 0.005" );
            ImGui::BulletText( "cloud quality forced to Low - the sun ray takes 16 samples, not 32" );
            ImGui::BulletText( "no bloom, no lens flare, no height fog, and the wind is stopped" );
            ImGui::TextDisabled( "The grade and the sun are yours, on the two headers above." );

            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderInt( "Max Steps", &setup.CloudMaxSteps, 8, 256 );
            ImGui::SetNextItemWidth( -FLT_MIN );
            ImGui::SliderFloat( "Stop Transmittance", &setup.CloudStopTransmittance, 0.001f, 0.2f, "%.3f" );
        }

        // The preview mesh, which is a SURFACE question: a dome has no shape and a cubemap ball's shape
        // is not a choice. The Shape combo itself stays on the toolbar where it has always been; what is
        // here is the one thing that combo cannot express.
        if ( m_Preview->GetFill() == PreviewViewport::Fill::Object &&
             ImGui::CollapsingHeader( "Preview Mesh", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            ImGui::TextDisabled( "Shape is on the toolbar. A mesh dropped here replaces it." );
            const std::string label =
                 m_PreviewMeshName.empty() ? std::string( "<drop a static mesh>" ) : m_PreviewMeshName;
            ImGui::Button( ( label + "##preview_mesh" ).c_str(), ImVec2( -FLT_MIN, 0.0f ) );
            if ( ImGui::BeginDragDropTarget() )
            {
                const char* types[] = { ::Desert::Editor::DragPayloads::MeshAsset,
                                        ::Desert::Editor::DragPayloads::AssetFile };
                for ( const char* t : types )
                {
                    if ( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload( t ) )
                    {
                        const std::string path( static_cast<const char*>( pl->Data ) );
                        if ( m_AssetManager )
                        {
                            if ( auto mesh = m_AssetManager->FindByPath<Assets::StaticMeshAsset>( path ) )
                            {
                                m_PreviewMesh     = mesh->GetMetadata().Handle;
                                m_PreviewMeshName = mesh->GetMetadata().Filepath.filename().string();
                            }
                        }
                        break;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::BeginDisabled( static_cast<uint64_t>( m_PreviewMesh ) == 0 );
            if ( ImGui::Button( "Back to the primitive" ) )
            {
                m_PreviewMesh = Assets::AssetHandle( static_cast<uint64_t>( 0 ) );
                m_PreviewMeshName.clear();
            }
            ImGui::EndDisabled();
            if ( static_cast<uint64_t>( m_PreviewMesh ) != 0 )
            {
                // SAID, NOT DISCOVERED. A mesh with several material slots is shown with THIS material in
                // every one of them: the window is about one material, and filling slot 0 while the rest
                // kept the mesh's authored materials would make the pane a picture of somebody else's
                // work with a corner of yours in it.
                ImGui::TextDisabled( "This material fills every slot of that mesh." );
            }
        }
    }

    void MaterialEditorPanel::PublishToRuntime( Assets::SurfaceMaterialAsset& asset, bool isInstance )
    {
        auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
        if ( !materialService )
            return;

        // ONE MECHANISM, TWO AUDIENCES, CHOSEN BY THE ASSET HANDED IN. This window's preview target is an
        // ordinary StaticMeshComponent holding a material in a SLOT, exactly like a mesh in the level, so
        // the two are reached by the same wire and cannot diverge in HOW an edit lands — which is what the
        // preview-route test guards.
        //
        // What they no longer share is WHICH material. Called with the working copy this moves the ball
        // and nothing else; called with the subject (only from ApplyEdits) it moves the level. The
        // previous version of this function was called with the subject from the edit sites themselves,
        // and the note here argued that one wire to both audiences was the only way the preview and the
        // scene could be guaranteed to agree. It conflated two questions: whether the preview shows what
        // the material WILL be, and whether the scene shows edits nobody has accepted. Agreement is a
        // property of the shared working copy — identity of the two audiences is stronger than agreement,
        // and it cost the artist every way back.
        // WHO CACHES THESE VALUES decides whether the SCENE has to be told, and the answer is a rule with
        // a name: MaterialEdit::PublishOwesTheGlobalStamp. It is a rule and not an `if` here because the
        // decision is the whole of this fix and nothing in this file can be reached by a test.
        const bool owesTheStamp = MaterialEdit::PublishOwesTheGlobalStamp(
             isInstance, /*isSubject=*/asset.GetMetadata().Handle == Assets::AssetHandle( Subject().Owner ) );

        // A BASE material's values live in the runtime Material, re-valued here. The runtime Material is
        // built ONCE and cached by the service; rebuilding an instance of it would faithfully reproduce the
        // old values, so Apply* is what pushes the new ones in — and it is also what re-binds textures,
        // which is why binding a texture shows on the mesh without a reload.
        //
        // EVERY built variant, one per vertex path. A `.demat` is a surface and the renderer builds one
        // material per path from it, so an edit that reached only the static one would show on the crate
        // and not on the character wearing the same material — the exact divergence the per-path material
        // classes used to make unavoidable.
        //
        // An INSTANCE has no runtime Material of its own to re-value; its overrides are baked into cached
        // MaterialInstances, and dropping those is the stamp's job below.
        if ( !isInstance )
        {
            for ( auto* runtime : materialService->GetBuiltVariants( asset.GetMetadata().Handle ) )
            {
                if ( auto* pbr = dynamic_cast<Graphic::MaterialPBR*>( runtime ) )
                    Graphic::MaterialFactory::ApplyPBRAsset( *pbr, asset );
                else if ( auto* ddm = dynamic_cast<Graphic::DataDrivenMaterial*>( runtime ) )
                    Graphic::MaterialFactory::ApplyShaderAsset( *ddm, asset );
            }
        }

        // ONE BUMP SITE IN THE WHOLE WINDOW, and it is behind the rule. A second one would be a second
        // opinion about who has to be told, and the two would differ on the day one of them is updated.
        if ( owesTheStamp )
            materialService->BumpInvalidationVersion();
    }

    bool MaterialEditorPanel::SaveDocument()
    {
        const auto subject = ResolveSubject();
        if ( !subject )
        {
            LOG_ERROR( "[MaterialEditor] nothing was saved: the material this window edits is no longer "
                       "loaded, so there is no asset to write." );
            return false;
        }

        // APPLY FIRST — see the header. The file must never hold values the running editor is not already
        // rendering.
        ApplyEdits();

        if ( !SaveSubject( *subject ) )
            return false;

        // The on-disk snapshot moves ONLY on a write that happened. Moving it beside the attempt would
        // make a document whose file could not be written report itself clean, and the artist would close
        // it without a question and lose the edit — which is the same class of silent loss as the missing
        // Discard, one state further along.
        MaterialEdit::CopyAuthoredValues( m_OnDisk, subject->Data() );
        return true;
    }

    bool MaterialEditorPanel::SaveSubject( Assets::SurfaceMaterialAsset& asset )
    {
        const auto path       = asset.GetMetadata().Filepath;
        const auto serialized = asset.Save();
        if ( !serialized )
        {
            LOG_ERROR( "[MaterialEditor] {}", serialized.GetError() );
            return false;
        }
        const std::string& text = serialized.GetValue();

        // The write primitive answers now, so the size-on-disk cross-check that used to stand here is
        // gone with it: it existed only because the old primitive reported nothing, it could not tell a
        // failed write from a torn one, and it read the file back on every save to find out something
        // the write itself knew.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text ); !written )
        {
            LOG_ERROR( "[MaterialEditor] '{}' was not written: {} — this material's edits are still only "
                       "in memory.",
                       path.generic_string(), written.GetError() );
            return false;
        }

        // Drop ONLY this material's rendered thumbnail so every panel showing it re-renders with the new
        // look immediately. Deleted rather than left to the browser's modtime check: that check ignores a
        // source newer by less than three seconds (coarse filesystem timestamps otherwise report a PNG as
        // stale the moment it is written), so a material saved shortly after its thumbnail was captured
        // would keep showing the old one for the rest of the session.
        std::error_code   ec;
        const std::string png = ThumbnailKey::DiskPath( path.generic_string() );
        std::filesystem::remove( png, ec );
        ThumbnailService::Get().Invalidate( path.generic_string() );
        return true;
    }

    const ::Desert::Core::Formats::ShaderProgramMeta* MaterialEditorPanel::Schema() const
    {
        auto* shaderService = Runtime::ResourceRegistry::GetShaderService();
        auto  shader        = shaderService != nullptr ? shaderService->GetByName( DrawnShaderName() ) : nullptr;
        if ( !shader )
            return nullptr;

        const auto& own = shader->GetProgramMeta();

        // ---- THE AUTHORED MEDIUM'S OWN PROPERTIES, AS ROWS OF THIS WINDOW -----------------------------
        //
        // A cloud material has TWO schemas and one file. The shipped one is CloudRaymarch's Properties
        // block, mirrored field for field onto Graphic::CloudMaterialValues; the second belongs to
        // whatever Volume graph the Medium slot names, and its names were invented by the artist who drew
        // that graph. Merging them HERE — and only here, for the drawing — is what gets the second one a
        // row, a reset, an instance override and a control-channel `set` without a second implementation
        // of any of those.
        //
        // THE NAMES ARE PREFIXED (Core::kCloudMediumOverridePrefix) AND THAT IS THE WHOLE SAFETY. Every
        // write below lands in MaterialData under ShaderParam::Name, and the renderer reads the shipped
        // schema out of that same map by name — so an unprefixed medium property called `Coverage` would
        // silently retune the layer's bake. A GLSL identifier cannot contain the prefix's dot, so the two
        // key spaces are disjoint by construction and no check has to be maintained to keep them so.
        const auto drawn = DrawnMaterial();
        if ( !drawn || own.Domain != ::Desert::Core::Formats::ShaderDomain::Volume )
            return &own;

        const uint64_t mediumHandle = drawn->Data().GetCloudAsset( Graphic::kCloudMediumSlotName );
        const auto*    mediumSchema = mediumHandle != 0 && shaderService
                                           ? shaderService->MediumSchemaOf( Assets::AssetHandle( mediumHandle ) )
                                           : nullptr;
        if ( !mediumSchema || mediumSchema->empty() )
            return &own;

        m_MergedSchema = own;
        m_MergedSchema.Params.reserve( own.Params.size() + mediumSchema->size() );
        for ( const ::Desert::Core::Formats::ShaderParam& p : *mediumSchema )
        {
            ::Desert::Core::Formats::ShaderParam row = p;
            row.Name                                 = ::Desert::Core::CloudMediumOverrideKey( p.Name );
            // The DISPLAY name stays the artist's own: the prefix is a key-space fact and putting it in
            // front of every label would be twelve characters of noise on every row of the group.
            m_MergedSchema.Params.push_back( std::move( row ) );
        }
        return &m_MergedSchema;
    }

    bool MaterialEditorPanel::WriteParam( const ::Desert::Core::Formats::ShaderParam& p, const glm::vec4& value )
    {
        // THE DRAWN material, which is the working copy once there is one. Not the subject: the whole point
        // of this document is that a value lands where only the pane beside it can see it, and the scene
        // waits for Apply. Resolved here rather than passed in, so no caller can hand in the other one.
        const auto drawn = DrawnMaterial();
        if ( !drawn || !m_WorkingCopy )
            return false;

        drawn->Data().SetParam( p.Name, value );
        PublishToRuntime( *drawn, drawn->Data().IsInstance() );
        return true;
    }

    bool MaterialEditorPanel::ResetParam( const ::Desert::Core::Formats::ShaderParam& p )
    {
        // THE DRAWN material, resolved here for the reason WriteParam resolves it here: the working copy is
        // the only thing this window may edit, and no caller gets to hand in the subject instead.
        const auto drawn = DrawnMaterial();
        if ( !drawn || !m_WorkingCopy )
            return false;

        if ( !drawn->Data().RemoveParam( p.Name ) )
            return false;

        // The publish is what makes the ball show the inherited value again, and it is the SAME publish an
        // ordinary edit makes. For a base material MaterialFactory::ApplyShaderAsset seeds every schema
        // default before overlaying what the asset stores, so a removed entry genuinely reverts on the GPU
        // rather than leaving the last written value behind; for an instance the global stamp drops the
        // cached MaterialInstances and they are rebuilt from the chain, which is where the parent's value
        // comes back from.
        PublishToRuntime( *drawn, drawn->Data().IsInstance() );
        return true;
    }

    std::vector<EditableProperty> MaterialEditorPanel::EditableProperties() const
    {
        const ::Desert::Core::Formats::ShaderProgramMeta* schema = Schema();
        const auto                                        drawn  = DrawnMaterial();
        if ( !schema || !drawn )
            return {};

        // The same three inputs DrawParameters draws its rows from, so the census and the table are one
        // walk of one declaration seeded by one rule.
        const auto parent = ResolveParent( *drawn );
        return MaterialEdit::DescribeProperties( *schema, drawn->Data(), parent ? &parent->Data() : nullptr,
                                                 drawn->Data().IsInstance() );
    }

    Common::BoolResultStr MaterialEditorPanel::SetEditableProperty( const std::string&        name,
                                                                    const std::vector<float>& value )
    {
        // EVERY REFUSAL BELOW WAS A SILENT NO-OP IN THE FLAG THIS REPLACES. `--material-step` logged one of
        // them and swallowed the rest; a value written into a material nothing reads renders as "the
        // preview did not move", which is indistinguishable from the feature being broken.
        const auto drawn = DrawnMaterial();
        if ( !drawn )
        {
            return Common::MakeError<bool>(
                 "the material this window edits is no longer loaded, so there is nothing to write into." );
        }

        if ( !m_WorkingCopy )
        {
            // The read-only mode the window itself is in, said in the same words it prints. Writing the
            // subject instead would put the value in every open scene with no way back, which is the one
            // thing this document exists to prevent.
            return Common::MakeError<bool>(
                 m_WorkingCopyRefusal.empty()
                      ? std::string( "this document has no working copy yet; it is made on the first frame the "
                                     "window draws. Ask 'state' and retry once it is open." )
                      : m_WorkingCopyRefusal );
        }

        const ::Desert::Core::Formats::ShaderProgramMeta* schema = Schema();
        if ( !schema )
        {
            return Common::MakeFormattedError<bool>(
                 "shader '{}' is not loaded, so this document has no declaration to check '{}' against. A "
                 "name checked against nothing is a name written and never read.",
                 DrawnShaderName(), name );
        }

        std::string                                 refusal;
        const ::Desert::Core::Formats::ShaderParam* p =
             MaterialEdit::FindSettableParam( *schema, name, drawn->Data().IsInstance(), refusal );
        if ( !p )
            return Common::MakeError<bool>( refusal );

        // THE COMPONENT COUNT IS PART OF THE PROPERTY'S IDENTITY. Three numbers for a float is a caller who
        // meant a different property, and padding them into a vec4 would write a value that looks accepted.
        const int wanted = MaterialEdit::ComponentsOf( p->Type );
        if ( static_cast<int>( value.size() ) != wanted )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' is a {} and takes {} number(s); {} were sent. Ask 'properties' for the shape of each "
                 "one.",
                 name, MaterialEdit::TypeNameOf( *p ), wanted, value.size() );
        }

        // THE SCHEMA'S OWN CLAMP, REFUSED RATHER THAN APPLIED. The widget cannot leave the range at all, so
        // a channel that clamped silently would accept a request the mouse cannot make and answer with a
        // number the caller did not send -- and the caller would read its own value back out of its own
        // request, not out of the editor.
        if ( p->Min.has_value() && p->Max.has_value() )
        {
            for ( const float component : value )
            {
                if ( component < *p->Min || component > *p->Max )
                {
                    return Common::MakeFormattedError<bool>(
                         "'{}' is declared range({}, {}) and {} is outside it. The window's own control "
                         "cannot leave that range either.",
                         name, *p->Min, *p->Max, component );
                }
            }
        }

        glm::vec4 written( 0.0f );
        for ( int i = 0; i < wanted; ++i )
            written[i] = value[static_cast<std::size_t>( i )];

        // WHAT THE SCENE WAS TOLD, READ ACROSS THE WRITE. MaterialService's stamp is what every mesh
        // component in every open scene watches; a write into the working copy must not move it (see
        // PublishToRuntime). Sampled here rather than asserted, because the honest form of "this costs the
        // level nothing" is a number a reader can check against the next line of the log.
        auto*          materialService = Runtime::ResourceRegistry::GetMaterialService();
        const uint32_t stampBefore     = materialService ? materialService->GetInvalidationVersion() : 0u;

        // THE SAME WRITE THE SLIDER MAKES. Not a copy of it: a capture taken down a route nobody uses
        // proves nothing about the route they do.
        if ( !WriteParam( *p, written ) )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' was accepted by the schema but the write found no working copy to land in; that is a "
                 "defect in this document, not in the request.",
                 name );
        }

        const uint32_t stampAfter = materialService ? materialService->GetInvalidationVersion() : 0u;

        LOG_INFO( "[MaterialEditor] control channel set '{}' = ({}, {}, {}, {}) in the working copy of '{}'. "
                  "The scene still shows the applied state; Apply is what moves it. Scene material stamp "
                  "{} -> {} ({}).",
                  name, written.x, written.y, written.z, written.w, drawn->GetMetadata().Filepath.generic_string(),
                  stampBefore, stampAfter,
                  stampBefore == stampAfter
                       ? "unmoved, so no entity in any scene rebuilds its cached material instances"
                       : "moved: every mesh in every open scene will rebuild its cached material instances" );
        return Common::MakeSuccess( true );
    }

    void MaterialEditorPanel::OnUIRender()
    {
        // Resolved ONCE per frame and handed to everything below. The subject is a handle, not a pointer, so
        // the asset can go away under an open window (deleted on disk, project reloaded); every part of the
        // window then has to agree about that, and re-looking it up per section is how two halves of one
        // window come to disagree.
        const auto subject = ResolveSubject();

        // THE WORKING COPY IS MADE HERE, on the first frame the window really draws, and not in the
        // constructor: the document is created while asset open requests are serviced, where the subject
        // may not have finished loading and where a window nobody has seen would already be holding a
        // second registered material.
        Assets::SurfaceMaterialAsset* working = subject ? EnsureWorkingCopy( *subject ) : nullptr;

        // EVERYTHING BELOW READS THE WORKING COPY. The parameter table edits it, the pane draws it, and
        // the domain decisions follow it — so this window is a picture of what the material will be, while
        // the scene stays a picture of what it is.
        const auto drawn      = DrawnMaterial();
        auto       parent     = drawn ? ResolveParent( *drawn ) : nullptr;
        const bool isInstance = drawn && drawn->Data().IsInstance();

        // Decided HERE, once, for both the pane below and next frame's OnPreUpdate — which is the only
        // place allowed to build or destroy the renderer. Resolving it twice is how the window would come
        // to hold a slot while telling the artist it has no preview, or the reverse.
        m_PreviewUnavailable = drawn ? PreviewUnavailableReason( DrawnShaderName() )
                                     : std::string( "No preview: this material is no longer loaded." );

        DrawToolbar( working, isInstance );
        ImGui::Separator();

        const float  kParamColumnW = 300.0f;
        const ImVec2 avail         = ImGui::GetContentRegionAvail();

        // THE NOTE IS RESERVED FOR, NOT HOPED FOR. The pane is fitted to the SHORTER of the two axes, so
        // a window that is taller than it is wide sizes it from the width and there is room underneath —
        // but a wide, short window sizes it from the height, and the sentence was then laid out past the
        // bottom edge and simply never appeared. A caveat that is only visible at some window shapes is
        // the same as no caveat.
        // AND MEASURED, NOT COUNTED. The reservation used to be a literal three lines, which is the same
        // defect one step further in: a note of four lines is laid out past the reserved area and its last
        // line simply never appears. That is not hypothetical — it happened the moment this note grew to
        // say what a cloud edit costs, and the sentence that went missing was the one the note was
        // lengthened for. ImGui can measure the wrapped text, so nothing here has to agree with the string.
        //
        // TWO PASSES, because the two quantities define each other: the pane's side is what the note wraps
        // at, and the note's height is what the pane's side is left over from. The first pass measures at
        // the WIDEST the pane could be (the width-limited side), which is the SMALLEST the note can be;
        // the second measures at the side that follows, which can only be narrower and so can only need
        // more height. Two steps therefore land on a reservation that is never short — the direction that
        // matters, since being short is what hides the text.
        const std::string note         = PreviewSceneNote();
        const float       widthLimited = avail.x - kParamColumnW;

        const auto measureNote = [&note]( float wrapWidth ) -> float
        {
            if ( note.empty() )
                return 0.0f;
            const ImVec2 size = ImGui::CalcTextSize( note.c_str(), nullptr, false, std::max( wrapWidth, 1.0f ) );
            return size.y + ImGui::GetStyle().ItemSpacing.y;
        };

        const float firstPassSide =
             std::max( 64.0f, std::min( widthLimited, avail.y - measureNote( widthLimited ) ) );
        const float noteHeight = measureNote( firstPassSide );
        const float imageSide  = std::max( 64.0f, std::min( widthLimited, avail.y - noteHeight ) );

        // The pane and the sentence under it are ONE column, so the note cannot end up beside the picture
        // it is about when the window is narrow.
        ImGui::BeginGroup();

        // The image is last frame's render; recording this frame's happens in OnPreUpdate. Rendering from
        // inside the ImGui pass destroys descriptor pools whose sets are bound to the command buffer being
        // recorded — the editor has been bitten by exactly that.
        if ( m_PreviewUnavailable.empty() && m_Preview && m_UIHelper && m_Preview->HasContent() )
        {
            m_Preview->Draw( *m_UIHelper, ImVec2( imageSide, imageSide ) );
        }
        else
        {
            // The empty reason is the one frame between the window first drawing and OnPreUpdate building
            // its viewport. Said out loud rather than left as a blank rectangle: "starting" and "there is
            // nothing to show you" look identical, and only one of them is worth waiting for.
            DrawPreviewPlaceholder( imageSide, m_PreviewUnavailable.empty()
                                                    ? std::string( "Starting the preview..." )
                                                    : m_PreviewUnavailable );
        }

        // WHATEVER FILLS THE PANE, THIS LINE STAYS UNDER IT. Empty for every domain that has nothing to
        // qualify; the Volume domain's dome is the first thing in this window that shows a convincing
        // picture of something that is NOT what the level will do, and the difference has to be readable
        // without opening a scene to find out.
        if ( !note.empty() )
        {
            ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + imageSide );
            ImGui::TextDisabled( "%s", note.c_str() );
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::BeginGroup();
        if ( !drawn )
        {
            ImGui::TextDisabled( "This material is no longer loaded" );
        }
        else
        {
            // A window with no working copy is READ-ONLY, and says why. Editing the subject directly
            // instead would silently restore the behaviour this document exists to prevent — every mesh in
            // the level changing under a drag, with no way back — and the artist would have no way to tell
            // which of the two modes they were in.
            const bool readOnly = ( working == nullptr );
            if ( readOnly && !m_WorkingCopyRefusal.empty() )
            {
                ImGui::PushTextWrapPos( 0.0f );
                ImGui::TextColored( ImVec4( 1.0f, 0.72f, 0.35f, 1.0f ), "%s", m_WorkingCopyRefusal.c_str() );
                ImGui::PopTextWrapPos();
                ImGui::Separator();
            }
            ImGui::BeginDisabled( readOnly );

            // Instance parenting is stated where the overrides are edited, and only here: a starred row in
            // the table below means "this child overrides the parent", which is unreadable without knowing
            // there is a parent at all.
            if ( isInstance )
            {
                const std::string parentName =
                     parent ? std::filesystem::path( parent->GetMetadata().Filepath ).stem().string()
                            : std::string( "<missing parent>" );
                ImGui::TextDisabled( "Instance of %s", parentName.c_str() );
                ImGui::Separator();
            }
            else
            {
                // The shader lives INSIDE the material (Unity's model, and ours). An instance never gets a
                // picker: it always renders with its parent chain's shader. The caption is the picker's
                // own, because it names the domain the list is filtered to and that is not knowable here.
                if ( DrawShaderPicker( *drawn ) )
                {
                    // A different shader means a different runtime material CLASS — the cached one cannot be
                    // re-valued, it has to be dropped so the next Get rebuilds it from the asset. Invalidate
                    // bumps the stamp itself, so every mesh rebuilds its instances with it.
                    //
                    // The DRAWN material's handle: while the shader is only chosen and not yet applied, the
                    // material that has to be rebuilt is the working copy. The subject's own rebuild is
                    // ApplyEdits's business, and it does the same thing there for the same reason.
                    if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                        materialService->Invalidate( drawn->GetMetadata().Handle );
                }
                ImGui::Separator();
            }

            // TWO TABS, BECAUSE THERE ARE TWO SUBJECTS. Everything to the left of this line edits the
            // MATERIAL and is saved with it; the second tab edits the SCENE the material is being shown
            // in, which is a property of this window and of nothing else. Collapsing them into one column
            // of fields is how an artist comes to believe the floor and the sun travel with the asset.
            if ( ImGui::BeginTabBar( "##material_tabs", ImGuiTabBarFlags_None ) )
            {
                if ( ImGui::BeginTabItem( "Parameters" ) )
                {
                    if ( DrawParameters( *drawn, parent ? &parent->Data() : nullptr, isInstance ) )
                        PublishToRuntime( *drawn, isInstance );
                    ImGui::EndTabItem();
                }

                // Offered only while there IS a preview world to configure. A material whose domain has
                // no pane (Terrain, Post Process) would otherwise get a tab full of controls over a scene
                // nothing renders — the dead-setting shape, one level up.
                if ( m_Preview && m_PreviewUnavailable.empty() )
                {
                    if ( ImGui::BeginTabItem( "Preview Scene" ) )
                    {
                        DrawPreviewSceneTab();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }

            ImGui::EndDisabled();
        }
        ImGui::EndGroup();

        // Claim the render for next frame's OnPreUpdate. Re-affirmed every frame on purpose: stop drawing
        // and the GPU work stops with it.
        m_DrewThisFrame = true;
    }
} // namespace Desert::Editor
