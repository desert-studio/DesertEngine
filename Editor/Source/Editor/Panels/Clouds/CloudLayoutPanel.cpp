#include "CloudLayoutPanel.hpp"

#include "CloudDocumentOpen.hpp"

#include <Common/Core/Math/Rounding.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Clouds/CloudMaterialValues.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/CloudType/CloudTypeService.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>

namespace Desert::Editor
{
    // The editor's ImGui lives in the global namespace; unqualified `ImGui::` inside `Desert::` would
    // resolve to `Desert::ImGui`, which is the engine's own runtime UI. Every panel in this folder opens
    // with the same alias for the same reason.
    namespace ImGui = ::ImGui;

    namespace
    {
        /// The panel's own UIHelper, created on first use exactly as the noise volume panel's is: the
        /// helper caches ImGui texture ids by image view, and a panel that never opens should not build
        /// one.
        std::unique_ptr<UI::UIHelper>& LayoutPreviewHelper()
        {
            static std::unique_ptr<UI::UIHelper> helper;
            if ( !helper )
            {
                helper = std::make_unique<UI::UIHelper>();
                helper->Init();
            }
            return helper;
        }

        bool LooksLikeAnImage( const std::filesystem::path& path )
        {
            std::string extension = path.extension().string();
            std::transform( extension.begin(), extension.end(), extension.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );

            return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
                   extension == ".bmp";
        }

        const ImVec4 kErrorColour( 0.95f, 0.45f, 0.40f, 1.0f );
        const ImVec4 kWarnColour( 0.95f, 0.78f, 0.35f, 1.0f );
        const ImVec4 kGoodColour( 0.55f, 0.85f, 0.55f, 1.0f );

        // The document's VISIBLE title: the subject's file name. Computed before the base class is
        // constructed — ISubjectDocument bakes the title in its own constructor and holds it for the
        // window's life — so it is a free function rather than a member.
        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets )
            {
                if ( const auto asset = assets->FindByHandle<Assets::CloudLayoutAsset>( subject ) )
                    return asset->GetMetadata().Filepath.filename().string();
            }
            return "Cloud Layout";
        }
    } // namespace

    CloudLayoutPanel::CloudLayoutPanel( const Assets::AssetHandle&             subject,
                                        std::shared_ptr<::Desert::Core::Scene> scene,
                                        Assets::AssetManager*                  assets )
         : ISubjectDocument( SubjectTitle( subject, assets ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::CloudLayout ) ) ),
           m_Scene( std::move( scene ) ), m_Assets( assets )
    {
        LoadSubject();
    }

    void CloudLayoutPanel::LoadSubject()
    {
        if ( !m_Assets )
            return;

        const auto painting =
             m_Assets->FindByHandle<Assets::CloudLayoutAsset>( Assets::AssetHandle( Subject().Owner ) );

        // REGISTERED IS NOT LOADED — see the identical note in CloudNoiseVolumePanel::LoadSubject. A
        // painting the manager knows about but has not read yet answers false to IsReadyForUse, and this
        // window gave up on it: the canvas came up blank and a Bake from it would have written an empty
        // layout over the artist's file. CloudTypePanel has always recovered from this; these two did not.
        if ( painting && !painting->IsReadyForUse() )
            painting->Load();

        if ( !painting || !painting->IsReadyForUse() )
        {
            m_Status        = "This painting is not loaded - the log says why.";
            m_StatusIsError = true;
            return;
        }

        m_SubjectPath = painting->GetMetadata().Filepath;

        m_Layout     = painting->GetLayout();
        m_HasLayout  = true;
        m_SourceName = m_SubjectPath.filename().string();

        // STRAIGHT ONTO THE CANVAS, and there is no button between the two any more. There used to be
        // "Edit this painting", and it existed for one reason: the recovery could FAIL — a layout whose
        // mask differed from its fourth pattern channel needed five planes and a canvas had four, so
        // opening it was a thing that had to be asked for and could be refused. O-4 gave the mask its own
        // plane and there is nothing left to refuse, so a document opened on a `.dclayout` is simply a
        // document you can paint on and export from.
        //
        // A recovery that fails here is a layout that was never valid, and it is reported rather than
        // leaving an empty canvas behind a panel that looks ready.
        auto canvas = Assets::MakeCloudLayoutCanvasFromLayout( m_Layout );
        if ( !canvas )
        {
            m_Status        = "This painting could not be opened: " + canvas.GetError();
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        m_Canvas           = canvas.ExtractValue();

        // WHAT THE FILE HOLDS, kept so GetDiskState has something to be clean against. Taken

        // here rather than derived later: after this line the canvas starts being painted on, and a

        // snapshot taken at the first stroke would already be one stroke late.

        m_OnDiskCanvas = m_Canvas;

        m_Tracked          = true;
        m_CanvasImageDirty = true;
        m_PreviewDirty     = true;
    }

    // -------------------------------------------------------------------------------------------------
    // The layer
    // -------------------------------------------------------------------------------------------------

    bool CloudLayoutPanel::LayerContext::Matches( const LayerContext& other ) const
    {
        if ( FromScene != other.FromScene || RegionSizeKm != other.RegionSizeKm || LatticeKm != other.LatticeKm ||
             PatchTileKm != other.PatchTileKm || Coverage != other.Coverage ||
             PatchStrength != other.PatchStrength || ResolvableChordKm != other.ResolvableChordKm ||
             Seed != other.Seed || SpeciesCount != other.SpeciesCount )
            return false;

        // THE SPECIES ARE COMPARED TOO, and they are the ones that moved most often in practice: dropping a
        // .decloudtype into a free slot renumbers every channel after it, and a map that did not redraw
        // would go on describing the sky the layer had a type ago.
        for ( uint32_t species = 0; species < SpeciesCount; ++species )
        {
            if ( Species[species].AuthoredSlot != other.Species[species].AuthoredSlot ||
                 Species[species].BuiltIn != other.Species[species].BuiltIn ||
                 Species[species].Scale != other.Species[species].Scale ||
                 Species[species].Anisotropy != other.Species[species].Anisotropy )
                return false;
        }

        return Assets::CloudLayoutPlacementEqual( Placement, other.Placement );
    }

    CloudLayoutPanel::LayerContext CloudLayoutPanel::ReadLayer() const
    {
        LayerContext layer;

        // THE NO-LAYER ANSWER IS FILLED IN, not left blank. With no cloud component in the scene the panel
        // still draws a map, against the shipped defaults, and the one species it has must have a NAME —
        // otherwise the channel labels read "-> " and the artist is told nothing by a control that is
        // trying to tell them everything.
        layer.SpeciesCount        = 1u;
        layer.Species[0].BuiltIn  = true;
        layer.Species[0].TypeName = "the built-in cumulus congestus";

        if ( !m_Scene )
            return layer;

        auto view = m_Scene->GetRegistry().view<ECS::VolumetricCloudComponent>();
        if ( view.begin() == view.end() )
            return layer;

        const ECS::VolumetricCloudData& data = view.get<ECS::VolumetricCloudComponent>( *view.begin() ).Data;

        // THE LOOK IS THE MATERIAL'S SINCE O1, resolved here exactly as the renderer resolves it —
        // schema defaults, `.demat` chain over them — so the map this panel draws is the sky the layer
        // renders, whichever `.demat` the component names and even when it names none.
        const Core::Formats::ShaderProgramMeta* schema = nullptr;
        if ( const auto shaderService = Runtime::ResourceRegistry::GetShaderService() )
        {
            if ( const auto marchShader = shaderService->GetByName( Graphic::kCloudMaterialShaderName ) )
                schema = &marchShader->GetProgramMeta();
        }
        Graphic::MaterialOverrides overrides;
        if ( static_cast<uint64_t>( data.Material ) != 0 )
        {
            if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
                materialService->ResolveOverrides( data.Material, overrides );
            // An unresolvable handle is the renderer's warning to give (once, with the number); the map
            // simply shows the schema defaults the sky is actually rendering.
        }
        const Graphic::CloudMaterialValues material = Graphic::BuildCloudMaterialValues( schema, overrides );

        layer.FromScene     = true;
        layer.RegionSizeKm  = std::max( data.RegionSize, 1.0f ) / Graphic::kCloudWorldUnitsPerKm;
        layer.Coverage      = std::clamp( material.Coverage, 0.0f, 1.0f );
        layer.PatchStrength = std::clamp( material.PatchStrength, 0.0f, 1.0f );
        layer.Seed          = static_cast<uint32_t>( std::max( material.Seed, 0 ) );
        layer.ResolvableChordKm =
             Graphic::CloudFinestResolvableChordKm( static_cast<float>( std::clamp( data.MaxSteps, 8, 512 ) ) );

        layer.LatticeKm = ECS::CloudLayerLatticeKm( material.WeatherTileSize );

        // THE LAYER'S OWN WEATHER PATCH TILE, and not a constant. It used to be a hard-coded 21 km here,
        // which is the component's DEFAULT — so the map agreed with the sky in every scene that had never
        // touched the field and disagreed silently in every scene that had. The patch is what decides a
        // cell's coverage whenever the painting is not the source (no layout bound, or Layout Pattern
        // Strength at zero), so getting it wrong draws a plausible sky that is not this layer's.
        layer.PatchTileKm = std::max( material.PatchTileSize, 1.0f ) / Graphic::kCloudWorldUnitsPerKm;

        // WHICH SLOT IS WHICH SPECIES IS ASKED OF THE ENGINE, never worked out here — the renderer resolves
        // the same call, so a channel this panel labels "Cirrus" is the channel the sky gives to cirrus.
        Assets::AssetHandle authored[ECS::kCloudTypeSlots];
        material.TypeSlots( authored );

        const ECS::CloudSpeciesResolution resolved = ECS::ResolveCloudSpecies( authored );
        layer.SpeciesCount                         = resolved.Count;

        auto* types = Runtime::ResourceRegistry::GetCloudTypeService();

        for ( uint32_t species = 0; species < resolved.Count; ++species )
        {
            LayerContext::SpeciesSlot& slot = layer.Species[species];

            slot.BuiltIn      = resolved.BuiltInDefault;
            slot.AuthoredSlot = resolved.AuthoredSlot[species];

            const Assets::AssetHandle handle =
                 resolved.BuiltInDefault ? Assets::AssetHandle::Null() : authored[slot.AuthoredSlot];

            if ( types )
            {
                const Graphic::CloudTypeShape& shape = types->GetShape( handle );
                slot.Scale                           = std::max( shape.PlacementScale, 1e-3f );
                slot.Anisotropy                      = std::max( shape.PlacementAnisotropy, 1e-3f );
            }

            if ( slot.BuiltIn )
                slot.TypeName = "the built-in cumulus congestus";
            else if ( m_Assets )
            {
                if ( auto type = m_Assets->FindByHandle<Assets::CloudTypeAsset>( handle ) )
                    slot.TypeName = type->GetMetadata().Filepath.stem().string();
            }

            // NAMED BY ITS HANDLE WHEN NOTHING CLAIMS IT, rather than left blank: a slot whose type the
            // asset manager does not know is a scene pointing at a file that is not there, and a blank
            // label would read as an empty slot — which is the one thing it is not.
            if ( slot.TypeName.empty() )
                slot.TypeName = "an unregistered type " + std::to_string( static_cast<uint64_t>( handle ) );
        }

        layer.Placement.RepeatsPerRegion = static_cast<uint32_t>( std::clamp( material.LayoutRepeats, 1, 16 ) );
        layer.Placement.QuarterTurns     = static_cast<uint32_t>( std::clamp( material.LayoutRotation, 0, 3 ) );
        layer.Placement.OffsetKm =
             glm::vec2( material.LayoutOffset.x, material.LayoutOffset.y ) / Graphic::kCloudWorldUnitsPerKm;
        layer.Placement.PatternStrength = std::clamp( material.LayoutPatternStrength, 0.0f, 1.0f );
        layer.Placement.MaskStrength    = std::clamp( material.LayoutMaskStrength, 0.0f, 1.0f );

        return layer;
    }

    void CloudLayoutPanel::OnUIRender()
    {
        // NO ImGui::Begin HERE, and it is not an omission. EditorLayer's panel loop already wraps
        // OnUIRender in Begin/End for this panel's name, and it owns the p_open bool the title-bar X
        // writes to. A Begin for the SAME name nested inside that one is not appending -- ImGui only
        // supports appending between Begin/End PAIRS -- and the window comes out with its title bar
        // and nothing else. Every panel in this folder had it and drew nothing; no panel outside it
        // does. See CALIBRATION.md §PTP.
        const LayerContext layer = ReadLayer();

        // THE LAYER IS COMPARED FIELD BY FIELD rather than by a dirty flag somebody has to set: the
        // numbers live on a component the Details panel edits, and there is no notification. Comparing is
        // a few dozen floats a frame and it is what makes moving Layout Repeats in Details redraw this map.
        if ( !layer.Matches( m_LastLayer ) )
            m_PreviewDirty = true;
        m_LastLayer = layer;

        // A SLOT THIS LAYER DOES NOT HAVE CANNOT BE MAPPED. Dropping a type out of Details shortens the
        // species list under the panel's feet, and BuildCloudLayoutPreview refuses a slot past the end —
        // correctly, and as an error the artist would have to read rather than a map they could look at.
        if ( m_PreviewSlot >= static_cast<int>( layer.SpeciesCount ) )
        {
            m_PreviewSlot  = static_cast<int>( layer.SpeciesCount ) - 1;
            m_PreviewDirty = true;
        }

        // THE "ADOPT THE LAYER'S BOUND PAINTING" BLOCK THAT WAS HERE IS GONE, and its absence is required
        // rather than tidy. It existed because the singleton had no subject: with nothing to show, showing
        // the sky you were looking at was the best guess available. A DOCUMENT has a subject, and the guess
        // becomes a defect — a window opened on painting A whose layer happens to wear painting B would
        // have replaced A's pixels with B's on its very first frame, then baked B over A's file. The layer
        // is still READ, for the preview's numbers; it no longer decides what is being edited.

        ImGui::TextWrapped( "A PAINTED SKY. Draw the layout with the brush, or point at a picture and say "
                            "which of its channels feeds which of this layer's cloud species, then bake it "
                            "to a .dclayout the cloud component's Cloud Layout slot accepts. The painting "
                            "is read when the clouds are PLACED, so it costs the frame nothing." );
        ImGui::Separator();

        DrawSourceSection();
        ImGui::Separator();
        DrawChannelSection();
        ImGui::Separator();
        DrawLayerSection();
        ImGui::Separator();

        if ( m_PreviewDirty )
            RefreshPreview( layer );

        // AFTER THE REFRESH, and that placement is the whole reason the section sits here rather than
        // directly under Source. The canvas draws the placement CELL, and the cell it draws has to be the
        // one the verdict below quotes — so it is read out of the map that was just built rather than
        // worked out a second time from the layer.
        DrawBrushSection();
        ImGui::Separator();

        DrawPreviewSection();
        ImGui::Separator();
        DrawVerdictSection();
        ImGui::Separator();
        DrawSaveSection();

        if ( !m_Status.empty() )
        {
            ImGui::Separator();
            if ( m_StatusIsError )
                ImGui::TextColored( kErrorColour, "%s", m_Status.c_str() );
            else
                ImGui::TextWrapped( "%s", m_Status.c_str() );
        }
    }

    // -------------------------------------------------------------------------------------------------
    // Source
    // -------------------------------------------------------------------------------------------------

    void CloudLayoutPanel::LoadSourceImage( const std::filesystem::path& path, Table table )
    {
        int      width = 0, height = 0, sourceChannels = 0;
        stbi_uc* decoded = stbi_load( path.string().c_str(), &width, &height, &sourceChannels, 4 );
        if ( !decoded )
        {
            // NAMED, WITH THE REASON stb gives. A picture that silently failed to load would leave the
            // previous painting on screen and the artist would bake the wrong file.
            m_Status = "'" + path.filename().string() + "' could not be read as an image: " +
                       ( stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        const std::vector<unsigned char> pixels( decoded, decoded + static_cast<size_t>( width ) * height * 4 );
        stbi_image_free( decoded );

        // THE ENGINE'S OWN FUNCTIONS, and this is the line that makes the panel not a second path: what a
        // picture means is stated once, in Engine/Assets/CloudLayout.cpp, and Tools/CloudLayoutBaker calls
        // the same ones. A panel with its own reading of a channel mapping would produce files that differ
        // from the tool's for reasons nobody could see.
        const auto brought =
             table == Table::Pattern
                  ? Assets::SetCloudLayoutCanvasPatternFromImage( m_Canvas, pixels, static_cast<uint32_t>( width ),
                                                                  static_cast<uint32_t>( height ),
                                                                  m_ChannelForSlot )
                  : Assets::SetCloudLayoutCanvasMaskFromImage( m_Canvas, pixels, static_cast<uint32_t>( width ),
                                                               static_cast<uint32_t>( height ),
                                                               static_cast<uint32_t>( m_MaskSourceChannel ) );

        if ( !brought )
        {
            m_Status        = "'" + path.filename().string() + "' was not taken: " + brought.GetError();
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        m_SourceName = path.filename().string();

        // A NEW SURFACE MEANS A NEW DEVICE IMAGE. Left alone, the canvas pane would go on showing the
        // previous picture's channel until something else happened to move, which reads as an import that
        // did nothing.
        m_CanvasImageDirty = true;

        m_Status = std::string( "Read the " ) + ( table == Table::Pattern ? "pattern" : "mask" ) + " from '" +
                   m_SourceName + "': " + std::to_string( width ) + "x" + std::to_string( height ) + ", " +
                   std::to_string( sourceChannels ) + " channels in the file.";
        m_StatusIsError = false;

        RebuildLayout();
    }

    void CloudLayoutPanel::ExportImage( Table table )
    {
        // TAKEN FROM THE CANVAS AND NOT RE-READ FROM THE LAYOUT, so what comes out is exactly what the
        // matching import would take back in rather than a second reading of a table.
        const auto image = table == Table::Pattern ? Assets::EncodeCloudLayoutCanvasPatternToImage( m_Canvas )
                                                   : Assets::EncodeCloudLayoutCanvasMaskToImage( m_Canvas );
        if ( !image )
        {
            m_Status        = "Export failed: " + image.GetError();
            m_StatusIsError = true;
            return;
        }

        std::filesystem::path target = Common::Utils::FileSystem::SaveFileDialog( "PNG image\0*.png\0" );
        if ( target.empty() )
            return;

        if ( target.extension() != ".png" )
            target.replace_extension( ".png" );

        const Assets::CloudLayoutImage& picture = image.GetValue();
        const int written = stbi_write_png( target.string().c_str(), static_cast<int>( picture.Side ),
                                            static_cast<int>( picture.Side ), 4, picture.Pixels.data(),
                                            static_cast<int>( picture.Side ) * 4 );

        const char* what = table == Table::Pattern ? "pattern" : "mask";

        if ( written == 0 )
        {
            m_Status        = "Export failed: '" + target.string() + "' could not be written.";
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        m_Status = std::string( "Exported the " ) + what + ", " + std::to_string( picture.Side ) + "x" +
                   std::to_string( picture.Side ) + ", to " + target.string();
        m_StatusIsError = false;
        LOG_INFO( "[CloudLayout] {} exported: '{}', {}x{}.", what, target.string(), picture.Side, picture.Side );
    }

    void CloudLayoutPanel::RebuildLayout()
    {
        m_HasLayout = false;

        // A CANVAS CARRYING EITHER TABLE IS A LAYOUT. Requiring a pattern here would leave an artist who
        // imported only a mask unable to bake at all — the Bake buttons key on m_HasLayout — which is a
        // dead end reached by doing exactly what the Global Cloud Mask input asks for.
        if ( m_Canvas.Side == 0u || ( m_Canvas.Pattern.empty() && !m_Canvas.HasMask() ) )
            return;

        auto made = Assets::MakeCloudLayoutFromCanvas( m_Canvas );
        if ( !made )
        {
            m_Status        = made.GetError();
            m_StatusIsError = true;
            return;
        }

        // THE PAINTING KEEPS ITS IDENTITY THROUGH AN EDIT: the canvas carries pixels only, so the GUID the
        // file was loaded with is carried over by hand. Dropping it would mint a fresh GUID on the next
        // save and orphan every scene and material that names this layout by its handle.
        const Common::Content::AssetGuid guid = m_Layout.Guid;
        m_Layout                              = made.ExtractValue();
        m_Layout.Guid                         = guid;
        m_HasLayout     = true;
        m_PreviewDirty  = true;
        m_StatusIsError = false;
    }

    void CloudLayoutPanel::DrawSourceSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Source" ) )
            return;

        // THE BLANK CANVAS COMES FIRST because it is the answer to the request this panel was extended
        // for: the designer CREATES the texture rather than brings one. Importing is the other button,
        // unchanged and beside it, and the two produce the identical kind of surface — so an artist can
        // start from a picture and paint on it without either path knowing about the other.
        if ( ImGui::Button( "New canvas", ImVec2( 120.0f, 0.0f ) ) )
            StartCanvas( static_cast<uint32_t>( m_NewCanvasSide ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A blank square to draw on. Nothing painted anywhere and NO add/remove mask "
                               "- a mask of uniform neutral changes nothing, so carrying one would be a "
                               "table written to the file for no effect. Add one below when you want it." );

        ImGui::SameLine();
        ImGui::SetNextItemWidth( 140.0f );
        int sideChoice = m_NewCanvasSide == 128 ? 0 : m_NewCanvasSide == 256 ? 1 : m_NewCanvasSide == 512 ? 2 : 3;
        if ( ImGui::Combo( "##canvasside", &sideChoice,
                           "128\0"
                           "256\0"
                           "512\0"
                           "1024\0" ) )
        {
            const int sides[] = { 128, 256, 512, 1024 };
            m_NewCanvasSide   = sides[std::clamp( sideChoice, 0, 3 )];
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Texels a side. At the shipped 48 km region and one repeat, 512 puts a "
                               "texel at 94 m and 128 puts it at 375 m - and a texel coarser than a "
                               "placement cell cannot tell two clouds apart at all." );

        // TWO IMPORTS AND NOT ONE — Unreal's two layout texture slots, as two buttons. The pattern says
        // WHERE the clouds are, the mask says how much to ADD or REMOVE, and until O-4 they had to arrive
        // in one RGBA picture with the mask living in its alpha. Bringing them separately is what lets a
        // painting use all four species slots AND carry a mask, which one image cannot express.
        ImGui::SameLine();
        if ( ImGui::Button( "Pattern image...", ImVec2( 150.0f, 0.0f ) ) )
        {
            const std::filesystem::path picked =
                 Common::Utils::FileSystem::OpenFileDialog( "Image\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0" );
            if ( !picked.empty() )
                LoadSourceImage( picked, Table::Pattern );
        }

        // BOTH PAYLOADS, and accepting only the generic one was a live defect: the tooltip below invites the
        // artist to "drag a .png here from the Content Browser", and that is precisely the drag this target
        // used to ignore. FileExplorerPanel::EmitAssetDragSource types a payload by FileType, and every
        // image is FileType::Texture, so the browser emits TEXTURE_ASSET for a .png and falls back to
        // AssetFile only for what it has no specific type for. A payload id that does not match fails
        // SILENTLY in ImGui — nothing logs, the drop simply does nothing — which is why this survived.
        // SkyboxComponent's slot already accepts the pair; this is the same fix.
        //
        // The extension filter stays for the AssetFile half, for its original reason: that payload carries
        // everything the browser has no icon for, so without it a dropped .desce would reach an image decoder.
        if ( ImGui::BeginDragDropTarget() )
        {
            for ( const char* accepted :
                  { ::Desert::Editor::DragPayloads::TextureAsset, ::Desert::Editor::DragPayloads::AssetFile } )
            {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( accepted );
                if ( !payload )
                    continue;

                const std::string dropped( static_cast<const char*>( payload->Data ),
                                           payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                if ( !dropped.empty() && LooksLikeAnImage( dropped ) )
                    LoadSourceImage( dropped, Table::Pattern );
                break;
            }
            ImGui::EndDragDropTarget();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A SQUARE picture, 4 to 1024 a side, whose channels become the four species "
                               "slots. Non-square and oversized sources are refused by name rather than "
                               "resampled - resampling would be an opinion about your painting, and one "
                               "taken silently is the worst kind. Or drag a .png here from the Content "
                               "Browser." );

        ImGui::SameLine();
        if ( ImGui::Button( "Mask image...", ImVec2( 150.0f, 0.0f ) ) )
        {
            const std::filesystem::path picked =
                 Common::Utils::FileSystem::OpenFileDialog( "Image\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0" );
            if ( !picked.empty() )
                LoadSourceImage( picked, Table::Mask );
        }
        if ( ImGui::BeginDragDropTarget() )
        {
            for ( const char* accepted :
                  { ::Desert::Editor::DragPayloads::TextureAsset, ::Desert::Editor::DragPayloads::AssetFile } )
            {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( accepted );
                if ( !payload )
                    continue;

                const std::string dropped( static_cast<const char*>( payload->Data ),
                                           payload->DataSize > 0 ? payload->DataSize - 1 : 0 );
                if ( !dropped.empty() && LooksLikeAnImage( dropped ) )
                    LoadSourceImage( dropped, Table::Mask );
                break;
            }
            ImGui::EndDragDropTarget();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The ADD/REMOVE mask, as its own picture: mid-grey changes nothing, "
                               "brighter adds cloud, darker removes it. Its bytes are taken unchanged, so "
                               "what you flood-filled with 128 is exactly what does nothing. It must be "
                               "the same side as the pattern already on the canvas, and says so if not." );

        // THE "OPEN A .dclayout" COMBO THAT WAS HERE IS GONE, and its absence is the feature. It was how
        // this window came to edit a different painting than the one it was opened on — which, now that the
        // title, the ImGui id and open-or-focus are all keyed on the subject handle, would leave a window
        // called one thing baking over another. A different painting is a different window, opened by
        // double-clicking it in the asset browser.

        if ( m_HasLayout )
        {
            ImGui::Text( "%s - %ux%u, pattern %s, mask %s, content %08x", m_SourceName.c_str(),
                         m_Layout.Resolution, m_Layout.Resolution, m_Layout.HasPattern() ? "yes" : "no",
                         m_Layout.HasMask() ? "yes" : "no", m_Layout.ContentHash );
            ImGui::TextDisabled( "channel means %.4f %.4f %.4f %.4f", m_Layout.PatternMean[0],
                                 m_Layout.PatternMean[1], m_Layout.PatternMean[2], m_Layout.PatternMean[3] );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "The painting is applied about its OWN average, so it moves cloud "
                                   "around the sky rather than adding it and Coverage keeps meaning the "
                                   "fraction of sky it delivers. A mean near 0 or near 1 leaves very "
                                   "little room to redistribute anything." );
        }
        else
        {
            ImGui::TextDisabled( "Nothing loaded yet - start a canvas, or point at a picture." );
        }
    }

    // -------------------------------------------------------------------------------------------------
    // The brush
    // -------------------------------------------------------------------------------------------------

    bool CloudLayoutPanel::CanPaint() const
    {
        return m_Canvas.Side >= Assets::kCloudLayoutMinResolution &&
               m_Canvas.Side <= Assets::kCloudLayoutMaxResolution &&
               m_Canvas.Pattern.size() == static_cast<size_t>( m_Canvas.Side ) * m_Canvas.Side * 4u &&
               ( !m_Canvas.HasMask() ||
                 m_Canvas.Mask.size() == static_cast<size_t>( m_Canvas.Side ) * m_Canvas.Side );
    }

    bool CloudLayoutPanel::PaintingMask() const
    {
        return m_PaintChannel == kMaskPaintChannel;
    }

    std::vector<unsigned char>& CloudLayoutPanel::PaintPlane()
    {
        return PaintingMask() ? m_Canvas.Mask : m_Canvas.Pattern;
    }

    uint32_t CloudLayoutPanel::PaintPlaneChannels() const
    {
        return PaintingMask() ? 1u : Assets::kCloudLayoutChannels;
    }

    uint32_t CloudLayoutPanel::PaintPlaneChannel() const
    {
        return PaintingMask() ? 0u : static_cast<uint32_t>( m_PaintChannel );
    }

    void CloudLayoutPanel::StartCanvas( uint32_t side )
    {
        auto made = Assets::MakeCloudLayoutCanvas( side );
        if ( !made )
        {
            m_Status        = made.GetError();
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        m_Canvas           = made.ExtractValue();
        m_SourceName       = "a canvas " + std::to_string( m_Canvas.Side ) + " a side";
        m_Painting         = false;
        m_Stroke           = Assets::CloudLayoutStroke{};
        m_CanvasImageDirty = true;

        // A BLANK CANVAS HAS NO MASK, so the brush cannot be left aimed at a plane that no longer exists.
        // Silently painting into the pattern instead would be a stroke landing somewhere the artist did
        // not point it, and the symptom is a species slot filling up for no reason.
        if ( PaintingMask() )
            m_PaintChannel = 0;

        m_Status        = "A blank canvas. Draw on it below, then bake.";
        m_StatusIsError = false;

        RebuildLayout();
    }

    float CloudLayoutPanel::CellTexels() const
    {
        // ONE DERIVATION, TAKEN OUT OF THE MAP. The verdict section reports a percentage against exactly
        // this number; a grid computed a second way from the layer would drift from it the first time
        // BuildCloudLayoutPreview clipped a span, and the artist would be shown a cell the sky does not
        // have.
        if ( !m_HasPreview || m_TexelKm <= 0.0f )
            return 0.0f;

        return m_Preview.CellKm / m_TexelKm;
    }

    void CloudLayoutPanel::RefreshCanvasImage()
    {
        if ( !CanPaint() )
            return;

        // A PLANE THAT IS NOT THERE HAS NO PICTURE. The mask can be removed while the brush is aimed at
        // it, and reading an empty buffer at a texel index is the one way this function can be made to
        // walk off the end of memory.
        if ( PaintingMask() && !m_Canvas.HasMask() )
            return;

        const uint32_t side   = m_Canvas.Side;
        const size_t   texels = static_cast<size_t>( side ) * side;

        const std::vector<unsigned char>& plane   = PaintingMask() ? m_Canvas.Mask : m_Canvas.Pattern;
        const size_t                      stride  = PaintPlaneChannels();
        const size_t                      channel = PaintPlaneChannel();

        std::vector<unsigned char> grey( texels * 4u, 255u );
        for ( size_t t = 0; t < texels; ++t )
        {
            const unsigned char value = plane[t * stride + channel];
            grey[t * 4u + 0]          = value;
            grey[t * 4u + 1]          = value;
            grey[t * 4u + 2]          = value;
            grey[t * 4u + 3]          = 255u;
        }

        // STREAMED WHEN IT CAN BE. A drag reaches here every frame it moves, and rebuilding the VkImage
        // each time churns a descriptor set for a picture that is one buffer copy. The image is recreated
        // only when its SHAPE changed — a new canvas side — because SetData refuses a size that disagrees.
        //
        // AND IT IS GUARDED BY A DIRTY FLAG BECAUSE THE UPLOAD IS EXPENSIVE, which was MEASURED rather
        // than assumed. Forced to stream every frame, the panel's own profiler scope read 36.5 ms at a 512
        // canvas and 23.8 ms at 128 — sixteen times less data for a third less time, so the cost is the
        // SYNCHRONOUS FLUSH inside SetData (staging allocation, submit, wait, destroy) and not the bytes.
        // Shrinking the canvas would therefore buy almost nothing. Left alone, the same scope reads
        // 0.15-0.22 ms, so the flag is worth a factor of two hundred, and a drag pays the flush only on
        // the frames it actually changed a texel. Debug, validation layers on, machine shared with another
        // agent's renders — the absolute figures are not a budget, the RATIO between them is the finding.
        if ( m_CanvasImage && m_CanvasImageSide == side )
        {
            if ( const auto streamed = m_CanvasImage->SetData( Core::Formats::ImagePixelData( grey ) ); !streamed )
            {
                m_Status        = "The canvas could not be updated on the device: " + streamed.GetError();
                m_StatusIsError = true;
                LOG_ERROR( "[CloudLayout] {}", m_Status );
                return;
            }

            m_CanvasImageChannel = m_PaintChannel;
            m_CanvasImageDirty   = false;
            return;
        }

        const Core::Formats::Image2DSpecification spec{
             .Tag        = "CloudLayoutCanvas",
             .Width      = side,
             .Height     = side,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Data       = std::move( grey ),
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Sample,
        };

        m_CanvasImage = Graphic::Image2D::Create( spec );
        if ( !m_CanvasImage )
        {
            m_Status        = "The canvas image could not be created on the device.";
            m_StatusIsError = true;
            LOG_ERROR( "[CloudLayout] {}", m_Status );
            return;
        }

        m_CanvasImageSide    = side;
        m_CanvasImageChannel = m_PaintChannel;
        m_CanvasImageDirty   = false;
    }

    void CloudLayoutPanel::DrawBrushSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Brush" ) )
            return;

        if ( !CanPaint() )
        {
            // The device image is dropped rather than kept warm: a canvas nobody can draw on is a megabyte
            // of GPU memory showing a picture that is not the one on screen.
            m_CanvasImage.reset();
            m_CanvasImageSide    = 0u;
            m_CanvasImageChannel = -1;
            m_Painting           = false;

            ImGui::TextDisabled( "Start a canvas, or open a SQUARE picture, and the brush appears here." );
            return;
        }

        const float side = static_cast<float>( m_Canvas.Side );

        // ---- what the stroke lands in ------------------------------------------------------------
        //
        // FIVE ENTRIES, because a layout has five tables. Before O-4 there were four and the last of them
        // meant either species slot 3 or the mask depending on a checkbox in another section, which is one
        // control with two meanings and a paragraph of panel explaining which was in force.

        ImGui::SetNextItemWidth( 240.0f );
        if ( ImGui::Combo( "Painting on", &m_PaintChannel,
                           "Channel 0 (red)\0"
                           "Channel 1 (green)\0"
                           "Channel 2 (blue)\0"
                           "Channel 3 (alpha)\0"
                           "The add/remove mask\0" ) )
            m_CanvasImageDirty = true;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Which plane of the canvas the brush writes. Channels 0..3 are the four "
                               "species slots of the PATTERN - each its own field of 'is there cloud of "
                               "this kind here'. The mask is a fifth plane of its own and says how much "
                               "cloud to add or take away." );

        const bool paintingMask = PaintingMask();

        if ( paintingMask )
        {
            if ( m_Canvas.HasMask() )
                ImGui::TextColored( kGoodColour, "The mask: 128 is neutral, brighter ADDS cloud, darker "
                                                 "REMOVES it." );
            else
            {
                ImGui::TextColored( kWarnColour, "This painting carries no mask, so there is nothing here "
                                                 "to draw on." );
                if ( ImGui::Button( "Add an add/remove mask" ) )
                {
                    if ( const auto added = Assets::SetCloudLayoutCanvasMask( m_Canvas, true ); !added )
                    {
                        m_Status        = added.GetError();
                        m_StatusIsError = true;
                    }
                    else
                    {
                        m_CanvasImageDirty = true;
                        RebuildLayout();
                    }
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Gives this painting a mask plane, flooded with the neutral 128 - so "
                                       "adding one changes nothing until you draw on it, rather than "
                                       "filling the sky." );
                return;
            }
        }

        // ---- the three numbers -------------------------------------------------------------------
        //
        // NOTHING IS REBUILT WHEN THEY MOVE, and that is not an omission: they describe the NEXT stroke.
        // The canvas already holds what earlier strokes left, so a brush slider has nothing to recompute —
        // which is the same property that lets a stroke's width be promised at all.

        ImGui::SetNextItemWidth( 240.0f );
        ImGui::SliderFloat( "Radius", &m_Brush.RadiusTexels, 1.0f, std::max( 4.0f, side * 0.25f ), "%.1f texels" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Half the stroke's width at full hardness. This is the number the placement "
                               "cell has to be held against." );

        ImGui::SetNextItemWidth( 240.0f );
        ImGui::SliderFloat( "Hardness", &m_Brush.Hardness, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "1 is a crisp disc, 0 ramps from the centre to nothing at the rim. A soft "
                               "brush lays a NARROWER stroke than a hard one of the same radius, and the "
                               "width printed below already accounts for it." );

        ImGui::SetNextItemWidth( 240.0f );
        ImGui::SliderFloat( "Ink", &m_Brush.Ink, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( paintingMask ? "On the mask: 1 adds cloud, 0 removes it, 0.5 is the neutral "
                                              "an eraser returns to. Right-drag erases."
                                            : "How much of this species the stroke leaves behind. "
                                              "Right-drag erases back to nothing." );

        // ---- the width rule, in the units the artist has to act in -------------------------------

        const float widthTexels = Assets::CloudLayoutBrushWidthTexels( m_Brush );
        const float cellTexels  = CellTexels();

        if ( m_TexelKm > 0.0f )
            ImGui::Text( "Stroke %.1f texels = %.2f km wide.", widthTexels, widthTexels * m_TexelKm );
        else
            ImGui::Text( "Stroke %.1f texels wide.", widthTexels );

        ImGui::SameLine();
        if ( cellTexels <= 0.0f )
            ImGui::TextDisabled( "(no map yet, so no cell to hold it against)" );
        else if ( widthTexels >= cellTexels )
            ImGui::TextColored( kGoodColour, "Clears the %.2f km placement cell.", m_Preview.CellKm );
        else
            ImGui::TextColored( kWarnColour,
                                "NARROWER than the %.2f km placement cell - it will break into clumps.",
                                m_Preview.CellKm );

        // ---- the canvas --------------------------------------------------------------------------

        if ( m_CanvasImageDirty || m_CanvasImageChannel != m_PaintChannel )
            RefreshCanvasImage();

        if ( !m_CanvasImage )
            return;

        ImGui::SetNextItemWidth( 240.0f );
        ImGui::SliderInt( "Canvas size", &m_CanvasPane, 192, 640, "%d px" );

        ImGui::SameLine();
        ImGui::Checkbox( "Show the placement cell", &m_ShowCellGrid );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The sky cannot place a cloud finer than one cell, so a stroke narrower than "
                               "a square of this grid does not survive to the sky - it comes out as evenly "
                               "spaced clumps. The engine's own validator checks one TEXEL against the "
                               "cell, which is a different and weaker question." );

        const float  pane   = static_cast<float>( m_CanvasPane );
        const float  scale  = pane / side;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // THE INVISIBLE BUTTON IS SUBMITTED FIRST AND THE PICTURE IS DRAWN INTO THE DRAW LIST, rather than
        // an ImGui::Image with a button laid over it. Two overlapping items would leave which one owns the
        // hover to submission order; one item that happens to have a picture under it cannot.
        ImGui::InvisibleButton( "##cloudlayoutcanvas", ImVec2( pane, pane ),
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );

        const bool active  = ImGui::IsItemActive();
        const bool started = ImGui::IsItemActivated();
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddImage( reinterpret_cast<ImTextureID>(
                             const_cast<void*>( LayoutPreviewHelper()->GetTextureID( m_CanvasImage ) ) ),
                        origin, ImVec2( origin.x + pane, origin.y + pane ) );

        bool cellTooFine = false;
        if ( m_ShowCellGrid && cellTexels > 0.0f )
        {
            const float step = cellTexels * scale;
            if ( step >= 4.0f )
            {
                const ImU32 line  = IM_COL32( 255, 210, 90, 90 );
                const int   count = static_cast<int>( pane / step );
                for ( int i = 0; i <= count; ++i )
                {
                    const float at = static_cast<float>( i ) * step;
                    draw->AddLine( ImVec2( origin.x + at, origin.y ), ImVec2( origin.x + at, origin.y + pane ),
                                   line );
                    draw->AddLine( ImVec2( origin.x, origin.y + at ), ImVec2( origin.x + pane, origin.y + at ),
                                   line );
                }
            }
            else
            {
                cellTooFine = true;
            }
        }

        draw->AddRect( origin, ImVec2( origin.x + pane, origin.y + pane ), IM_COL32( 130, 130, 130, 255 ) );

        // ---- the drag ----------------------------------------------------------------------------
        //
        // THE ADAPTER IS THIS AND NOTHING MORE: a screen point becomes a texel, and the segment between
        // the last one and this one is handed to the engine's stroke. Every decision about what a stroke
        // IS - its profile, its width, its wrap, that it is laid once at its strongest - lives in
        // Engine/Assets/CloudLayout.cpp, where a test can reach it and a command can reproduce it.
        const ImVec2    mouse = ImGui::GetIO().MousePos;
        const glm::vec2 texel( ( mouse.x - origin.x ) / scale, ( mouse.y - origin.y ) / scale );

        if ( started )
        {
            Assets::CloudLayoutStroke opened;
            if ( const auto began = Assets::BeginCloudLayoutStroke( opened, PaintPlane(), m_Canvas.Side,
                                                                    PaintPlaneChannel(), PaintPlaneChannels() );
                 !began )
            {
                m_Status        = began.GetError();
                m_StatusIsError = true;
                LOG_ERROR( "[CloudLayout] {}", m_Status );
            }
            else
            {
                m_Stroke         = std::move( opened );
                m_Painting       = true;
                m_LastPaintTexel = texel;
            }
        }

        if ( m_Painting && active )
        {
            Assets::CloudLayoutBrush stroke = m_Brush;

            // RIGHT-DRAG IS THE SAME BRUSH WITH THE REST VALUE AS ITS INK. An eraser that is anything else
            // is a second code path doing one thing — and the rest value differs per plane, because the
            // mask is signed about neutral and a pattern channel is not.
            if ( ImGui::IsMouseDown( ImGuiMouseButton_Right ) && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
                stroke.Ink = paintingMask ? static_cast<float>( Assets::kCloudLayoutMaskNeutral ) / 255.0f : 0.0f;

            if ( Assets::ExtendCloudLayoutStroke( m_Stroke, PaintPlane(), m_LastPaintTexel, texel, stroke ) > 0u )
                m_CanvasImageDirty = true;

            m_LastPaintTexel = texel;
        }
        else if ( m_Painting )
        {
            // THE LAYOUT IS REBUILT WHEN THE DRAG ENDS, not on every frame of it. A rebuild encodes a
            // megabyte, hashes it and decodes it again; per frame that turns a brush into a slideshow and
            // tells the artist nothing they could not wait one stroke for.
            m_Painting = false;
            m_Stroke   = Assets::CloudLayoutStroke{};
            RebuildLayout();
        }

        // ---- the cursor, against the grid --------------------------------------------------------

        if ( hovered || m_Painting )
        {
            const ImVec2 at( origin.x + texel.x * scale, origin.y + texel.y * scale );

            // TWO CIRCLES AND THEY MEAN DIFFERENT THINGS. The bright one is the width the stroke MEASURES
            // at, which is the one to hold against a square of the grid; the faint one is how far any ink
            // at all reaches. At full hardness they are the same circle, which is itself the point.
            draw->AddCircle( at, 0.5f * widthTexels * scale, IM_COL32( 255, 255, 255, 220 ), 0, 2.0f );
            if ( m_Brush.Hardness < 0.999f )
                draw->AddCircle( at, m_Brush.RadiusTexels * scale, IM_COL32( 255, 255, 255, 80 ) );
        }

        if ( cellTooFine )
            ImGui::TextDisabled( "One placement cell is %.1f screen pixels here - too fine to draw. Make the "
                                 "canvas bigger, or lower Layout Repeats.",
                                 cellTexels * scale );

        ImGui::TextDisabled( "Left-drag paints, right-drag erases. The canvas is drawn as AUTHORED - the "
                             "map below is it flipped top to bottom." );

        // WHY A BLANK CANVAS MAKES THE SKY FLAT, said where the surprise happens. A cell's coverage has
        // exactly ONE modulator: the painting when one is bound and turned up, the procedural weather
        // patch otherwise. So pressing New Canvas does not leave the sky alone and then add to it — it
        // takes the weather away and hands the sky to a drawing with nothing on it yet. The symptom
        // without this line is an artist reporting that the panel "flattened my clouds".
        if ( m_LastLayer.FromScene && m_LastLayer.PatchStrength > 0.0f &&
             m_LastLayer.Placement.PatternStrength > 0.0f )
            ImGui::TextDisabled( "While a painting is bound with Layout Pattern Strength above zero, it "
                                 "REPLACES this layer's Weather Patch Strength of %.2f - one modulator per "
                                 "cell, never two. An empty canvas is therefore a flat sky at the Coverage "
                                 "slider, not the sky you had.",
                                 m_LastLayer.PatchStrength );
    }

    // -------------------------------------------------------------------------------------------------
    // Channels — the convention, made visible
    // -------------------------------------------------------------------------------------------------

    void CloudLayoutPanel::DrawChannelSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Channels" ) )
            return;

        ImGui::TextWrapped( "Which channel of the picture feeds which of the layer's cloud SPECIES. A "
                            "species' channel is its own field of 'is there cloud of this kind here' - "
                            "they overlap freely and the sky takes whichever wins." );

        // THE RENUMBERING, SAID OUT LOUD. Details has four type slots; the layer has as many SPECIES as it
        // has distinct filled ones, and the painting's channels are indexed by species. So a layer whose
        // only type sits in Cloud Type 3 is driven by the painting's RED channel, and nothing in Details
        // hints at it — the symptom is a channel an artist swears they painted that does nothing.
        const LayerContext& layer = m_LastLayer;

        ImGui::BeginDisabled( m_Canvas.Pattern.empty() );

        bool remap = false;
        for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
        {
            std::string label;
            if ( slot < layer.SpeciesCount )
            {
                const LayerContext::SpeciesSlot& species = layer.Species[slot];
                label                                    = "-> " + species.TypeName +
                        ( species.BuiltIn ? std::string()
                                          : " (Cloud Type " + std::to_string( species.AuthoredSlot + 1u ) + ")" );
            }
            else
            {
                label = "-> nothing in this scene";
            }

            // The visible text is a TYPE NAME and two slots can hold the same words, so the widget's
            // identity is the slot number after `##` rather than the label. Without it two channels
            // mapped to one unfilled slot would be one control wearing two labels.
            label += "##slot" + std::to_string( slot );

            // A THIRD OF THE ROW FOR THE COMBO, because the LABEL is the part that carries the answer here
            // and ImGui draws it after the widget. At the default full width the type name ran off the
            // panel's right edge and read "(Cloud Type" — found by looking at the panel, which is the only
            // way this class of defect is ever found.
            ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x * 0.33f );

            int channel = static_cast<int>( m_ChannelForSlot[slot] );
            if ( ImGui::Combo( label.c_str(), &channel, "Red\0Green\0Blue\0Alpha\0" ) )
            {
                m_ChannelForSlot[slot] = static_cast<uint32_t>( channel );
                remap                  = true;
            }
            if ( ImGui::IsItemHovered() )
            {
                if ( slot < layer.SpeciesCount )
                    ImGui::SetTooltip( "Species %u of this layer. Details calls it Cloud Type %u; the "
                                       "painting calls it channel %u, because empty and repeated type "
                                       "slots are dropped before the sky is placed.",
                                       slot, layer.Species[slot].AuthoredSlot + 1u, slot );
                else
                    ImGui::SetTooltip( "This layer has %u species, so channel %u is carried in the file and "
                                       "placed by nothing. Fill another Cloud Type slot in Details and it "
                                       "comes alive - the painting does not have to be baked again.",
                                       layer.SpeciesCount, slot );
            }
        }

        if ( ImGui::Button( "One drawing on every channel" ) )
        {
            for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
                m_ChannelForSlot[slot] = 0u;
            remap = true;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Red into all four channels - what a greyscale painting usually wants, and "
                               "the tool's `--channels 0,0,0,0`." );

        ImGui::SameLine();
        if ( ImGui::Button( "Straight RGBA" ) )
        {
            for ( uint32_t slot = 0; slot < Assets::kCloudLayoutChannels; ++slot )
                m_ChannelForSlot[slot] = slot;
            remap = true;
        }

        // THE MASK'S OWN SOURCE CHANNEL, and it is a separate control because the mask is a separate
        // picture. It used to be a checkbox saying "take the alpha as the mask", which existed only
        // because one image had to carry both tables.
        ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x * 0.33f );
        ImGui::Combo( "-> the add/remove mask##masksource", &m_MaskSourceChannel, "Red\0Green\0Blue\0Alpha\0" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Which channel of a MASK picture is read by 'Mask image...'. A greyscale PNG "
                               "loads with red, green and blue equal, so Red is right for anything drawn as "
                               "grey; the other three are for a mask somebody packed into a channel." );

        ImGui::EndDisabled();

        if ( m_Canvas.HasMask() )
        {
            if ( ImGui::Button( "Remove the add/remove mask" ) )
            {
                if ( const auto dropped = Assets::SetCloudLayoutCanvasMask( m_Canvas, false ); !dropped )
                {
                    m_Status        = dropped.GetError();
                    m_StatusIsError = true;
                }
                else
                {
                    // The brush cannot stay pointed at a plane that is gone — see StartCanvas for the
                    // same rule and the same reason.
                    if ( PaintingMask() )
                        m_PaintChannel = 0;
                    m_CanvasImageDirty = true;
                    remap              = true;
                }
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Drops the mask plane, so the baked file carries no mask table at all "
                                   "and the Global Cloud Mask input costs exactly zero." );
        }

        if ( remap )
            RebuildLayout();
    }

    // -------------------------------------------------------------------------------------------------
    // The layer, READ and not edited
    // -------------------------------------------------------------------------------------------------

    void CloudLayoutPanel::DrawLayerSection()
    {
        const LayerContext& layer = m_LastLayer;

        // THE TWO THINGS THAT MUST NOT FOLD AWAY ARE DRAWN OUTSIDE THE SECTION. A map built against the
        // shipped defaults instead of the artist's layer, and a placement the bake will refuse, are both
        // reasons not to trust the picture below — and a reason not to trust a picture that is only
        // visible when a header happens to be open is a reason nobody reads.
        if ( !layer.FromScene )
            ImGui::TextColored( kWarnColour, "This scene has NO cloud layer, so the map below is drawn "
                                             "against the shipped defaults." );

        // The SAME validator the bake runs, so the panel refuses for the same reason the bake refuses
        // rather than the two disagreeing about what is legal.
        if ( const auto valid = Assets::ValidateCloudLayoutPlacement( layer.Placement ); !valid )
            ImGui::TextColored( kErrorColour, "%s", valid.GetError().c_str() );

        // CLOSED UNTIL ASKED FOR, and the reason is a screenshot: with it open the verdict under the two
        // panes — the part of this panel an artist opens it for — fell below the window's bottom edge on a
        // 1289-point display. These numbers are reference and the map is the tool, so the reference is the
        // one that folds away. Nothing here is editable, so nothing is hidden that could be changed by
        // accident.
        if ( !Utils::ImGuiUtilities::SectionHeader( "The layer this hangs in", /*defaultOpen=*/false ) )
            return;

        if ( layer.FromScene )
            ImGui::TextWrapped( "Read from this scene's cloud layer. Change any of it in Details and the "
                                "map below follows on the same frame - there is deliberately no copy of "
                                "these numbers here." );

        ImGui::Text( "Region Size %.1f km      placement lattice %.2f km      weather patch tile %.1f km",
                     layer.RegionSizeKm, layer.LatticeKm, layer.PatchTileKm );
        ImGui::Text( "Coverage %.2f      Weather Patch Strength %.2f      Seed %u", layer.Coverage,
                     layer.PatchStrength, layer.Seed );

        // THE CELL PER SPECIES, because the lattice above is the layer's and every type multiplies it by
        // its own Placement Scale. A stroke has to clear the cell of the species it is painted for, and
        // these are the numbers that differ between them.
        for ( uint32_t species = 0; species < layer.SpeciesCount; ++species )
        {
            ImGui::Text( "  channel %u -> %s: cell %.2f km", species, layer.Species[species].TypeName.c_str(),
                         layer.LatticeKm * layer.Species[species].Scale );
            if ( layer.Species[species].Anisotropy != 1.0f )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "stretched %.2fx along the wind", layer.Species[species].Anisotropy );
            }
        }
        ImGui::Text( "Layout Repeats %u      Rotation %u quarter turns      Offset (%.2f, %.2f) km",
                     layer.Placement.RepeatsPerRegion, layer.Placement.QuarterTurns, layer.Placement.OffsetKm.x,
                     layer.Placement.OffsetKm.y );
        ImGui::Text( "Layout Pattern Strength %.2f      Layout Mask Strength %.2f",
                     layer.Placement.PatternStrength, layer.Placement.MaskStrength );
    }

    // -------------------------------------------------------------------------------------------------
    // Preview
    // -------------------------------------------------------------------------------------------------

    Assets::CloudProceduralFieldParams CloudLayoutPanel::BuildParams( const LayerContext& layer ) const
    {
        Assets::CloudProceduralFieldParams params;

        params.RegionSizeKm      = std::max( layer.RegionSizeKm, 1e-3f );
        params.Coverage          = layer.Coverage;
        params.Seed              = layer.Seed;
        params.PatchStrength     = layer.PatchStrength;
        params.ResolvableChordKm = layer.ResolvableChordKm;
        params.LayoutPlacement   = layer.Placement;

        // BOTH INPUTS, FROM THE ONE PAINTING ON THE CANVAS. The material's two slots can name two different
        // files; this panel edits ONE, so the map it draws is what that painting produces when it feeds
        // both — which is the ordinary way a `.dclayout` is bound and the only sky this window can honestly
        // claim to be showing.
        if ( m_HasLayout )
        {
            auto shared          = std::make_shared<const Assets::CloudLayoutData>( m_Layout );
            params.PatternSource = shared;
            params.MaskSource    = std::move( shared );
        }

        // EACH SPECIES ON ITS OWN LATTICE, exactly as VolumetricCloudRenderer builds them: the layer's
        // lattice times the type's Placement Scale, stretched by its Placement Anisotropy. The first draft
        // gave all four one square cell taken from the FINEST type, which draws a coarse species' map at a
        // resolution it does not have and quotes the most permissive legibility bound in the layer for
        // every channel — the artist is then told a 1.2 km stroke is safe on a 4 km cell.
        params.Species.resize( layer.SpeciesCount );
        for ( uint32_t species = 0; species < layer.SpeciesCount; ++species )
        {
            params.Species[species].CellKm     = std::max( layer.LatticeKm * layer.Species[species].Scale, 1e-3f );
            params.Species[species].Anisotropy = std::max( layer.Species[species].Anisotropy, 1e-3f );
        }

        // THE PATCH TILE IS THE LAYER'S OWN, THEN FLOORED AGAINST THE CELL exactly as the renderer floors
        // it, because a modulation finer than three cells decides cells one at a time and reads as a
        // checkerboard. A preview drawn with an unfloored tile would show a sky the layer cannot produce.
        params.PatchTileKm = layer.PatchTileKm;
        for ( const Assets::CloudProceduralSpecies& species : params.Species )
        {
            const glm::vec2 extent = Assets::CloudProceduralCellExtentKm( params, species );
            params.PatchTileKm     = std::max( params.PatchTileKm, 3.0f * std::max( extent.x, extent.y ) );
        }

        return params;
    }

    void CloudLayoutPanel::RefreshPreview( const LayerContext& layer )
    {
        m_PreviewDirty = false;
        m_HasPreview   = false;
        m_PaintingImage.reset();
        m_SkyImage.reset();
        m_Strokes = Assets::CloudLayoutStrokeStats{};

        if ( !m_HasLayout )
            return;

        const Assets::CloudProceduralFieldParams params = BuildParams( layer );
        const uint32_t                           slot =
             static_cast<uint32_t>( std::clamp( m_PreviewSlot, 0, static_cast<int>( layer.SpeciesCount ) - 1 ) );

        const float spanKm = params.RegionSizeKm * static_cast<float>( std::clamp( m_SpanRegions, 1, 4 ) );

        // 256 cells a side is the ceiling: it is about what the pane is drawn at, so a finer map could
        // not be seen, and it bounds the work behind a slider at 65 536 evaluations whatever the cell.
        auto mapped = Assets::BuildCloudLayoutPreview( params, slot, spanKm, 256u );
        if ( !mapped )
        {
            m_Status        = "The sky preview could not be built: " + mapped.GetError();
            m_StatusIsError = true;
            return;
        }

        m_Preview    = mapped.ExtractValue();
        m_HasPreview = true;

        // THE STROKE LIMIT IS THE CELL EXPRESSED IN TEXELS OF THIS PAINTING. One texel spans
        // `period / resolution` kilometres with `period = RegionSize / Repeats`, so a cell is
        // `cell / texelKm` texels wide. That conversion is what turns a fact about pixels into a fact
        // about the sky, and it is the whole reason the measure takes a limit rather than a verdict.
        m_PeriodKm = params.RegionSizeKm / static_cast<float>( params.LayoutPlacement.RepeatsPerRegion );
        m_TexelKm  = m_PeriodKm / static_cast<float>( std::max( m_Layout.Resolution, 1u ) );

        const float limitTexels = m_TexelKm > 0.0f ? m_Preview.CellKm / m_TexelKm : 0.0f;

        m_Strokes = Assets::MeasureCloudLayoutStrokes( m_Layout, slot, limitTexels );

        // ---- the painting pane -------------------------------------------------------------------

        const uint32_t             resolution = m_Layout.Resolution;
        std::vector<unsigned char> painting( static_cast<size_t>( resolution ) * resolution * 4u, 255u );

        const bool wantsMask = m_PaintingView == PaintingView::Mask;
        for ( uint32_t y = 0; y < resolution; ++y )
        {
            for ( uint32_t x = 0; x < resolution; ++x )
            {
                const size_t texel  = static_cast<size_t>( y ) * resolution + x;
                const size_t target = texel * 4u;

                unsigned char value = 0u;
                if ( wantsMask )
                    value = m_Layout.HasMask() ? m_Layout.Mask[texel] : 128u;
                else if ( m_Layout.HasPattern() )
                    value = m_Layout.Pattern[texel * 4u + static_cast<size_t>( m_PaintingView )];

                painting[target + 0] = value;
                painting[target + 1] = value;
                painting[target + 2] = value;
                // The alpha is folded in rather than left to the compositor: an image drawn with a real
                // alpha would show the window behind it, which reads as the painting being empty exactly
                // where it is darkest.
                painting[target + 3] = 255u;
            }
        }

        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = "CloudLayoutPainting",
                 .Width      = resolution,
                 .Height     = resolution,
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1,
                 .Data       = std::move( painting ),
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Sample,
            };
            m_PaintingImage = Graphic::Image2D::Create( spec );
        }

        // ---- the sky pane ------------------------------------------------------------------------

        const uint32_t             side = m_Preview.Side;
        std::vector<unsigned char> sky( static_cast<size_t>( side ) * side * 4u, 255u );

        for ( uint32_t iv = 0; iv < side; ++iv )
        {
            for ( uint32_t iu = 0; iu < side; ++iu )
            {
                const float coverage = m_Preview.Coverage[static_cast<size_t>( iv ) * side + iu];

                // NORTH IS UP. The map's v runs north and an image's first row is its top, so the row is
                // flipped here — otherwise the picture would be the sky seen from BELOW, which is the one
                // orientation an artist cannot check against a top-down frame.
                const size_t target = ( static_cast<size_t>( side - 1u - iv ) * side + iu ) * 4u;

                // Clear sky to cloud, so an empty region reads as sky rather than as black. The ramp is
                // linear in coverage because coverage is a FRACTION OF SKY (decision D-20) and a gamma on
                // it would misreport how much cloud a region has.
                const float t = std::clamp( coverage, 0.0f, 1.0f );

                sky[target + 0] = Common::Math::QuantiseUnitToByte( 0.24f + 0.74f * t );
                sky[target + 1] = Common::Math::QuantiseUnitToByte( 0.43f + 0.55f * t );
                sky[target + 2] = Common::Math::QuantiseUnitToByte( 0.74f + 0.26f * t );
                sky[target + 3] = 255u;
            }
        }

        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = "CloudLayoutSky",
                 .Width      = side,
                 .Height     = side,
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1,
                 .Data       = std::move( sky ),
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Sample,
            };
            m_SkyImage = Graphic::Image2D::Create( spec );
        }

        if ( !m_PaintingImage || !m_SkyImage )
        {
            m_Status        = "A preview image could not be created on the device.";
            m_StatusIsError = true;
        }
    }

    void CloudLayoutPanel::DrawPreviewSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Preview" ) )
            return;

        if ( !m_HasLayout )
        {
            ImGui::TextDisabled( "Load a picture, or open a .dclayout, to see what it does to a sky." );
            return;
        }

        // BOUNDED BY THE SPECIES THIS LAYER HAS, not by the four a file can carry: a slider that offers a
        // slot the layer cannot place would answer with an error message where a map should be.
        const int lastSlot = static_cast<int>( m_LastLayer.SpeciesCount ) - 1;
        ImGui::BeginDisabled( lastSlot == 0 );
        if ( ImGui::SliderInt( "Channel to map", &m_PreviewSlot, 0, lastSlot, "channel %d" ) )
        {
            m_PaintingView = static_cast<PaintingView>( m_PreviewSlot );
            m_PreviewDirty = true;
        }
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Which of this layer's %u species to map - %s. Each is its own channel of "
                               "the painting, its own lattice and its own field of clouds.",
                               m_LastLayer.SpeciesCount,
                               m_LastLayer.Species[std::clamp( m_PreviewSlot, 0, lastSlot )].TypeName.c_str() );

        int view = static_cast<int>( m_PaintingView );
        if ( ImGui::Combo( "Painting shows", &view,
                           "Channel 0\0"
                           "Channel 1\0"
                           "Channel 2\0"
                           "Channel 3\0"
                           "The add/remove mask\0" ) )
        {
            m_PaintingView = static_cast<PaintingView>( view );
            m_PreviewDirty = true;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Which table of the FILE the left pane draws. All four channels are carried "
                               "whatever the layer uses them for, so a channel this scene has no species "
                               "for can still be inspected here." );

        if ( ImGui::SliderInt( "Sky span", &m_SpanRegions, 1, 4, "%d regions" ) )
            m_PreviewDirty = true;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How much world the right-hand map covers, in region periods. At 1 you see "
                               "one period of the painting; above 1 you see it TILE, which is what the sky "
                               "does past the region and what the whole-number repeat count protects." );

        ImGui::SliderInt( "Pane size", &m_PreviewSide, 128, 512, "%d px" );

        const float pane = static_cast<float>( m_PreviewSide );

        if ( m_PaintingImage )
        {
            ImGui::BeginGroup();
            ImGui::TextDisabled( "What you drew - %u texels", m_Layout.Resolution );
            LayoutPreviewHelper()->Image( m_PaintingImage, ImVec2( pane, pane ) );
            ImGui::EndGroup();
        }

        if ( m_SkyImage && m_HasPreview )
        {
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled( "The sky from above, north up - %u cells, %.2f km each", m_Preview.Side,
                                 m_Preview.SamplePitchKm );
            LayoutPreviewHelper()->Image( m_SkyImage, ImVec2( pane, pane ) );
            ImGui::EndGroup();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Looking straight DOWN on the layer, north up. White is cloud, blue is "
                                   "clear. One sample per PLACEMENT CELL where the span allows it - that is "
                                   "the resolution the sky has, and a stroke finer than a cell cannot "
                                   "survive it. Over a wide span the map is coarser still, and the pitch "
                                   "printed above it is always the one drawn." );

            // THE FLIP IS STATED RATHER THAN DISCOVERED. The layout's v runs NORTH and an image's first row
            // is its TOP, so a picture placed in the world stands on its head relative to a north-up map.
            // Both panes are honest to their own reader and the difference between them is real; an artist
            // who does not know it paints a letter and finds it upside down in a rendered sky.
            ImGui::TextWrapped( "The map is your picture FLIPPED TOP TO BOTTOM: the painting's first row "
                                "lies to the SOUTH, because the layout's v axis runs north and an image's "
                                "first row is its top." );
        }
    }

    // -------------------------------------------------------------------------------------------------
    // The two things the protocol knew and the artist did not
    // -------------------------------------------------------------------------------------------------

    void CloudLayoutPanel::DrawVerdictSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "What the sky says" ) )
            return;

        if ( !m_HasPreview )
        {
            ImGui::TextDisabled( "No map yet." );
            return;
        }

        ImGui::Text( "Coverage asks for %.2f and this painting delivers %.2f over the map.", m_LastLayer.Coverage,
                     m_Preview.MeanCoverage );

        // ---- 1. the pattern and the mask are not independent -------------------------------------

        const float clampedFraction = m_Preview.Cells > 0u ? static_cast<float>( m_Preview.CellsClamped ) /
                                                                  static_cast<float>( m_Preview.Cells )
                                                           : 0.0f;
        const float movedFraction   = m_Preview.Cells > 0u ? static_cast<float>( m_Preview.CellsPatternMoves ) /
                                                                static_cast<float>( m_Preview.Cells )
                                                           : 0.0f;

        if ( m_Layout.HasPattern() && m_Preview.CellsPatternMoves == 0u )
        {
            ImGui::TextColored( kWarnColour,
                                "Layout Pattern Strength does NOTHING here: both ends of it give the same "
                                "sky, cell for cell." );
            ImGui::TextWrapped( "It is not a dead knob - the mask has saturated it. %.0f%% of the cells "
                                "are already pinned at empty or full by the clamp, and a redistribution "
                                "about the painting's own mean has nowhere left to go. Turn Layout Mask "
                                "Strength down and the pattern comes back.",
                                100.0f * clampedFraction );
        }
        else if ( m_Layout.HasPattern() )
        {
            ImGui::TextColored( kGoodColour,
                                "Layout Pattern Strength moves %.0f%% of the cells across its range (%u of "
                                "%u).",
                                100.0f * movedFraction, m_Preview.CellsPatternMoves, m_Preview.Cells );
            ImGui::TextWrapped( "%.0f%% of the cells are pinned at empty or full by the clamp. The pattern "
                                "and the mask are NOT independent: the mask can drive a region past both "
                                "ends of the clamp, and everything the pattern would have said inside it "
                                "is then eaten.",
                                100.0f * clampedFraction );
        }
        else
        {
            ImGui::TextDisabled( "This layout carries no pattern, so Layout Pattern Strength has nothing "
                                 "to apply - only the mask acts." );
        }

        ImGui::Spacing();

        // ---- 2. legibility is bounded by the STROKE, and the validator checks the TEXEL ----------

        ImGui::Text(
             "One texel is %.3f km; the cell of %s is %.2f km; one period of your painting is "
             "%.1f km.",
             m_TexelKm,
             m_LastLayer.Species[std::clamp( m_PreviewSlot, 0, static_cast<int>( m_LastLayer.SpeciesCount ) - 1 )]
                  .TypeName.c_str(),
             m_Preview.CellKm, m_PeriodKm );

        if ( m_Strokes.PaintedTexels == 0u )
        {
            ImGui::TextDisabled( "Channel %d is flat - nothing was drawn on it, so there is no stroke to "
                                 "measure.",
                                 m_PreviewSlot );
        }
        else
        {
            const float thinKm   = m_Strokes.ThinnestTenthTexels * m_TexelKm;
            const float medianKm = m_Strokes.MedianTexels * m_TexelKm;

            ImGui::Text( "Your strokes: the thinnest tenth is %.2f km, the median is %.2f km.", thinKm, medianKm );

            if ( m_Strokes.FractionBelowLimit > 0.02f )
            {
                ImGui::TextColored( kWarnColour,
                                    "%.0f%% of what you drew on this channel is NARROWER THAN ONE CLOUD "
                                    "CELL.",
                                    100.0f * m_Strokes.FractionBelowLimit );
                ImGui::TextWrapped( "Those strokes will break into evenly spaced clumps rather than read "
                                    "as a shape - the sky cannot place a cloud finer than a cell. Lower "
                                    "Layout Repeats, paint thicker, or give the layer a finer Weather "
                                    "Tile Size." );
            }
            else
            {
                ImGui::TextColored( kGoodColour, "Every stroke clears the cell, so the shape will read." );
            }
        }

        // SAID EVERY TIME, not only when it bites. The engine's own validator compares one TEXEL against
        // the cell, which is the right bound for "can the painting tell two cells apart" and says nothing
        // about whether a letter still looks like a letter. That gap is the reason this section exists,
        // and an artist who never sees the warning still has to know which question was answered.
        ImGui::TextDisabled( "The engine checks one TEXEL against the cell - that a painting can tell two "
                             "cells apart. Legibility is your thinnest STROKE against the cell, which no "
                             "validator can know, so it is measured here and reported rather than "
                             "refused." );
    }

    // -------------------------------------------------------------------------------------------------
    // Bake
    // -------------------------------------------------------------------------------------------------

    void CloudLayoutPanel::DrawSaveSection()
    {
        if ( !Utils::ImGuiUtilities::SectionHeader( "Bake" ) )
            return;

        // BAKE WRITES THE SUBJECT. BAKE AS CREATES A SECOND ASSET AND OPENS ITS OWN WINDOW — it does NOT
        // repoint this one. The subject is this window's identity: its title, its ImGui id and the key
        // open-or-focus matches on are all built from it, so a document that followed a Bake As would be a
        // window named after a file it no longer edits. Authoring a new painting is still exactly what it
        // was — draw or import, Bake As — and now the new one arrives as its own document.
        ImGui::BeginDisabled( !m_HasLayout || m_SubjectPath.empty() );
        const bool bake = ImGui::Button( "Bake", ImVec2( 180.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( !m_HasLayout );
        const bool bakeAs = ImGui::Button( "Bake As...", ImVec2( 180.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled( "Paintings live in %s", Common::Constants::Path::CLOUD_LAYOUT_PATH.string().c_str() );

        ImGui::TextDisabled( "Then drag it onto a cloud material's Global Pattern or Global Cloud Mask "
                             "slot - they are separate inputs, and one file can feed both." );

        // THE WAY BACK OUT, ONE BUTTON PER TABLE. A `.dclayout` used to be a one-way door and then a
        // one-picture door; since O-4 the pattern and the mask leave separately, exactly as they arrive,
        // so an artist can take the mask into their painting tool without also being handed four channels
        // of placement they did not ask about.
        ImGui::BeginDisabled( m_Canvas.Pattern.empty() );
        if ( ImGui::Button( "Export pattern...", ImVec2( 170.0f, 0.0f ) ) )
            ExportImage( Table::Pattern );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Writes the four species-slot planes back out as the same kind of RGBA "
                               "picture 'Pattern image...' reads. Lossless, so a pattern that goes out and "
                               "comes straight back in is the same table byte for byte." );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( !m_Canvas.HasMask() );
        if ( ImGui::Button( "Export mask...", ImVec2( 170.0f, 0.0f ) ) )
            ExportImage( Table::Mask );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Writes the add/remove mask out as a grey picture - its byte in red, green "
                               "and blue and an opaque alpha, so no tool reads the half of it that REMOVES "
                               "cloud as transparency and composites it away." );
        ImGui::EndDisabled();

        std::filesystem::path target;
        if ( bake )
        {
            target = m_SubjectPath;
        }
        else if ( bakeAs )
        {
            target = Common::Utils::FileSystem::SaveFileDialog( "Cloud Layout\0*.dclayout\0" );
            if ( !target.empty() && target.extension() != Assets::kCloudLayoutExtension )
                target.replace_extension( Assets::kCloudLayoutExtension );
        }

        if ( target.empty() )
            return;

        // Compared before the write, because after it the file exists and the two paths would be
        // indistinguishable by anything on disk.
        (void)WriteTo( target, /*isCopy=*/target != m_SubjectPath );
    }

    bool CloudLayoutPanel::WriteTo( const std::filesystem::path& target, const bool isCopy )
    {
        // LIFTED OUT OF THE BUTTON so SaveDocument runs it too. The whole sequence and not just the write:
        // the file, the re-registration that makes the material's slot show the new pixels without a
        // restart, and the copy's own document.
        // A COPY IS A NEW ASSET and so gets a GUID of its own (the encoder mints one for a null GUID);
        // writing the original's would make two files claim one handle.
        Assets::CloudLayoutData toWrite = m_Layout;
        if ( isCopy )
            toWrite.Guid = {};
        const auto written = Assets::CloudLayoutAsset::Save( target, toWrite );
        if ( !written )
        {
            m_Status        = "Bake failed: " + written.GetError();
            m_StatusIsError = true;
            return false;
        }

        if ( !isCopy )
        {
            m_SourceName = target.filename().string();
            // These pixels are what the file holds now.
            m_OnDiskCanvas = m_Canvas;
            m_Tracked      = true;
        }
        m_Status        = "Baked to " + target.string();
        m_StatusIsError = false;

        // Re-registered straight away so the cloud material's slot shows the new pixels without a restart.
        // A tool whose output only appears after the editor is reopened is a tool nobody iterates in.
        if ( !m_Assets )
            return true;

        auto painting = m_Assets->FindByPath<Assets::CloudLayoutAsset>( target );
        if ( painting )
            painting->Load(); // overwritten in place: re-read so the cached bytes are new
        else
            painting = m_Assets->CreateAsset<Assets::CloudLayoutAsset>( Assets::AssetPriority::Medium, target );

        if ( !painting )
            return true;

        if ( const auto registered = Runtime::ResourceRegistry::GetCloudLayoutService()->Register( painting );
             !registered )
        {
            // THE FILE IS ON DISK, so the document is clean — a registration failure is about the running
            // sky and not about what was written.
            m_Status        = "Baked, but the painting could not be registered: " + registered.GetError();
            m_StatusIsError = true;
            return true;
        }

        if ( isCopy )
        {
            // The copy is a new asset, so it gets its own document. Queued rather than constructed here:
            // EditorLayer is the only place that may create a panel, and this is the same wire the asset
            // browser's double-click uses.
            RequestCloudDocument( m_Assets, target.string() );
            m_Status        = "Baked a copy to " + target.string() + " - it has opened in its own window.";
            m_StatusIsError = false;
        }
        return true;
    }

    ISubjectDocument::DiskState CloudLayoutPanel::GetDiskState() const
    {
        if ( !m_Tracked )
            return DiskState::Untracked;
        return m_Canvas == m_OnDiskCanvas ? DiskState::Clean : DiskState::Dirty;
    }

    bool CloudLayoutPanel::SaveDocument()
    {
        // A document with no file of its own reports false rather than inventing a path, and a canvas that
        // carries neither table has nothing to bake — writing an empty layout over the subject would be a
        // save that destroys the thing it claims to have saved.
        if ( m_SubjectPath.empty() || !m_HasLayout )
            return false;

        return WriteTo( m_SubjectPath, /*isCopy=*/false );
    }

    // -- WHAT A CHANNEL THAT CARRIES NUMBERS CAN DO WITH A PAINTING ---------------------------------
    //
    // A MEASURED REFUSAL, REPORTED ROW BY ROW RATHER THAN AS AN EMPTY CENSUS. The authored state of a
    // `.dclayout` is two PICTURES — four planes of placement and one of mask, 1.3 MiB at the shipped 512
    // side. Nothing about that fits in a channel whose unit is "up to four floats", and the honest answer
    // is not an empty list: an empty census reads as "this document exposes nothing", which is what a
    // document with no properties AT ALL answers, and those are different facts (contract §1.4).
    //
    // So every authored thing is named, its size is stated, and the reason it cannot be set is the reason
    // rather than a shrug. What a client CAN do with a painting is import and export it as a picture,
    // which is a path and not a value — the panel's own buttons.
    std::vector<EditableProperty> CloudLayoutPanel::EditableProperties() const
    {
        std::vector<EditableProperty> properties;

        EditableProperty side;
        side.Name       = "Side";
        side.Label      = "Side (texels)";
        side.Group      = "Painting";
        side.Type       = "int";
        side.Components = 1;
        side.Value[0]   = static_cast<float>( m_Canvas.Side );
        side.Settable   = false;
        side.NotSettableReason =
             "'Side' is the size of the painting that is loaded. Changing it is starting a new canvas or "
             "importing a different picture, not writing a number - a resize would have to invent or throw "
             "away texels an artist painted.";
        properties.push_back( std::move( side ) );

        const auto picture = [&]( const char* name, const char* label, const std::size_t bytes, const char* what )
        {
            EditableProperty property;
            property.Name       = name;
            property.Label      = label;
            property.Group      = "Painting";
            property.Type       = "image";
            property.Components = 1;
            property.Value[0]   = static_cast<float>( bytes );
            property.Settable   = false;
            property.NotSettableReason =
                 std::string( "'" ) + name + "' is " + what + " - " + std::to_string( bytes ) +
                 " bytes of picture. This channel carries at most four numbers, so it cannot express one; "
                 "the panel imports and exports it as a file.";
            return property;
        };
        properties.push_back( picture( "Pattern", "Pattern (4 species planes)", m_Canvas.Pattern.size(),
                                       "the map of where each cloud species may appear" ) );
        properties.push_back( picture( "Mask", "Mask (add / remove)", m_Canvas.Mask.size(),
                                       "the map of where cloud is added and taken away" ) );

        return properties;
    }

    Common::BoolResultStr CloudLayoutPanel::SetEditableProperty( const std::string&        name,
                                                                 const std::vector<float>& value )
    {
        (void)value;

        // NAMED REFUSALS FOR THE ROWS THE CENSUS OFFERS, so a client that read the census and tried anyway
        // gets the same sentence twice rather than a generic "no such property" that would read as the
        // census having been wrong.
        if ( name == "Side" || name == "Pattern" || name == "Mask" )
        {
            for ( const EditableProperty& property : EditableProperties() )
                if ( property.Name == name )
                    return Common::MakeError<bool>( property.NotSettableReason );
        }

        return Common::MakeFormattedError<bool>(
             "this cloud layout has no property called '{}'. Ask 'properties' for the ones it offers - and "
             "note that all of them are pictures, which this channel cannot carry.",
             name );
    }

    bool CloudLayoutPanel::IsSubjectAlive() const
    {
        // ASKED OF THE METADATA rather than of a typed lookup: the question is whether the asset is still
        // THERE, and a typed lookup answers a different one (whether it is still that type) — a subject
        // that failed to reload as its own class would read as deleted and the window would close on a
        // load error instead of reporting it.
        return m_Assets && m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }
} // namespace Desert::Editor
