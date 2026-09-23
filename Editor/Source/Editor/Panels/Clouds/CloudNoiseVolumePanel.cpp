#include "CloudNoiseVolumePanel.hpp"

#include "CloudDocumentOpen.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudNoiseVolumeGenerator.hpp>
#include <Engine/Assets/CloudNoiseVolumeSheet.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace Desert::Editor
{
    // The editor's ImGui lives in the global namespace; unqualified `ImGui::` inside `Desert::` would
    // resolve to `Desert::ImGui`, which is the engine's own runtime UI. Every panel in this folder opens
    // with the same alias for the same reason.
    namespace ImGui = ::ImGui;

    namespace
    {
        // The panel's own UIHelper, created on first use exactly as NodeGraphPanel's is: the helper caches
        // ImGui texture ids by image view, and a panel that never opens should not build one.
        std::unique_ptr<UI::UIHelper>& SlicePreviewHelper()
        {
            static std::unique_ptr<UI::UIHelper> helper;
            if ( !helper )
            {
                helper = std::make_unique<UI::UIHelper>();
                helper->Init();
            }
            return helper;
        }

        const char* AxisName( int axis )
        {
            return axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
        }

        // The document's VISIBLE title: the subject's file name. Computed before the base class is
        // constructed — ISubjectDocument bakes the title in its own constructor and holds it for the
        // window's life — so it is a free function rather than a member.
        //
        // Falls back to the type's name rather than to something empty: a document whose asset has gone
        // missing still has to be a window the user can find and close.
        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets )
            {
                if ( const auto asset = assets->FindByHandle<Assets::CloudNoiseVolumeAsset>( subject ) )
                    return asset->GetMetadata().Filepath.filename().string();
            }
            return "Cloud Noise Volume";
        }

        // -- EVERY NUMBER OF THE RECIPE, ONCE ----------------------------------------------------------
        //
        // TWO READERS AND ONE LIST, for the reason CloudTypePanel's kShapeFields carries at length: the
        // controls this panel draws and the properties the control channel offers are the same rows with
        // the same clamps, because neither is written out twice.
        //
        // RESOLUTION IS NOT HERE. It is one of exactly two legal values and the panel draws it as a combo;
        // SetEditableProperty handles it by name and refuses anything that is not 64 or 128, which is a
        // different refusal from "outside a range" and reads as one.
        struct RecipeField
        {
            const char* Name;
            const char* Label;
            float Assets::CloudNoiseVolumeParams::*Member;
            float                                  Min;
            float                                  Max;
        };

        constexpr RecipeField kRecipeFields[] = {
             { "CurlStrength", "Curl Strength", &Assets::CloudNoiseVolumeParams::CurlStrength, 0.0f, 0.5f },
             { "WispyPeriodLowFrequency", "Wispy Period LF",
               &Assets::CloudNoiseVolumeParams::WispyPeriodLowFrequency, 1.0f, 32.0f },
             { "WispyPeriodHighFrequency", "Wispy Period HF",
               &Assets::CloudNoiseVolumeParams::WispyPeriodHighFrequency, 1.0f, 32.0f },
             { "BillowPeriodLowFrequency", "Billow Period LF",
               &Assets::CloudNoiseVolumeParams::BillowPeriodLowFrequency, 1.0f, 32.0f },
             { "BillowPeriodHighFrequency", "Billow Period HF",
               &Assets::CloudNoiseVolumeParams::BillowPeriodHighFrequency, 1.0f, 32.0f },
        };

        /// The one sentence a client has to be told and the type cannot carry: these are the BAKE's
        /// inputs, so setting one changes nothing until the volume is baked again.
        constexpr const char* kRecipeGroup = "Recipe (applied at the next Bake)";
    } // namespace

    CloudNoiseVolumePanel::CloudNoiseVolumePanel( const Assets::AssetHandle& subject,
                                                  Assets::AssetManager*      assets )
         : ISubjectDocument(
                SubjectTitle( subject, assets ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::CloudNoiseVolume ) ) ),
           m_Assets( assets )
    {
        LoadSubject( assets );
    }

    void CloudNoiseVolumePanel::LoadSubject( Assets::AssetManager* assets )
    {
        if ( !assets )
            return;

        const auto asset =
             assets->FindByHandle<Assets::CloudNoiseVolumeAsset>( Assets::AssetHandle( Subject().Owner ) );

        // REGISTERED IS NOT LOADED, and this window used to read the two as one. An asset the manager knows
        // about but has not read yet answers false to IsReadyForUse, and the panel gave up on it — so a
        // `.dcnv` opened from the command palette (which asks for the SUBJECT, not for a path) came up as
        // an empty editor with a status line nobody was looking at, and Save from it would have written a
        // volume that was never the file's. Found by GetDiskState answering "untracked" for a document
        // whose file is plainly there.
        //
        // Loading it here is what CloudTypePanel::OpenType already does for the same situation; the
        // difference between the two panels was an accident of how each was written, not a decision.
        if ( asset && !asset->IsReadyForUse() )
            asset->Load();

        if ( !asset || !asset->IsReadyForUse() )
        {
            // NAMED rather than left as an empty panel. The opener (CloudDocumentOpen.hpp) refuses to queue
            // a document for an asset that would not load, so reaching this means the asset was evicted
            // between the request and the frame that serviced it — rare, and invisible without this line.
            m_Status        = "This volume is not loaded - the log says why. Nothing can be saved from here.";
            m_StatusIsError = true;
            return;
        }

        AdoptVolume( Assets::CloudNoiseVolumeData( asset->GetVolume() ) );
        m_Params      = m_Volume.Params;
        // Read off disk, so the document starts CLEAN against the file it is a window over.
        m_SavedRevision = m_VolumeRevision;
        m_Tracked       = true;
        m_HasVolume   = true;
        m_SubjectPath = asset->GetMetadata().Filepath;
        m_SourceName  = m_SubjectPath.filename().string();
        m_SliceIndex  = static_cast<int>( m_Volume.Params.Resolution ) / 2;
        m_SliceDirty  = true;
    }

    CloudNoiseVolumePanel::~CloudNoiseVolumePanel()
    {
        // A bake writes into a std::future this object owns, so the panel must outlive it. Waiting here
        // rather than detaching is the difference between a slow editor shutdown and a use-after-free.
        if ( m_Baking.valid() )
            m_Baking.wait();
    }

    void CloudNoiseVolumePanel::OnUIRender()
    {
        // NO ImGui::Begin HERE, and it is not an omission. EditorLayer's panel loop already wraps
        // OnUIRender in Begin/End for this panel's name, and it owns the p_open bool the title-bar X
        // writes to. A Begin for the SAME name nested inside that one is not appending -- ImGui only
        // supports appending between Begin/End PAIRS -- and the window comes out with its title bar
        // and nothing else. Every panel in this folder had it and drew nothing; no panel outside it
        // does. See CALIBRATION.md §PTP.
        ImGui::TextWrapped(
             "The 3D noise the cloud shape is eroded with. Four channels, following Nubis Cubed "
             "(SIGGRAPH 2023, slide 96): R and G are low- and high-frequency Curly-Alligator (wispy), "
             "B and A are low- and high-frequency Alligator (billowy)." );
        ImGui::Separator();

        DrawGenerateSection();
        ImGui::Separator();
        DrawPreviewSection();
        ImGui::Separator();
        DrawSheetSection();
        ImGui::Separator();
        DrawSaveSection();

        if ( !m_Status.empty() )
        {
            ImGui::Separator();
            if ( m_StatusIsError )
                ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ), "%s", m_Status.c_str() );
            else
                ImGui::TextWrapped( "%s", m_Status.c_str() );
        }
    }

    void CloudNoiseVolumePanel::DrawGenerateSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Generate" );

        // Collect a finished bake before drawing anything that depends on m_Volume, so the frame the bake
        // ends is already the frame that shows it.
        if ( m_BakeRunning && m_Baking.valid() &&
             m_Baking.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
        {
            auto result   = m_Baking.get();
            m_BakeRunning = false;
            if ( result )
            {
                AdoptVolume( result.ExtractValue() );
                m_HasVolume     = true;
                m_SourceName    = "(baked, unsaved)";
                m_SliceIndex    = static_cast<int>( m_Volume.Params.Resolution ) / 2;
                m_SliceDirty    = true;
                m_Status        = "Baked. Save it to make it an asset the cloud component can point at.";
                m_StatusIsError = false;
            }
            else
            {
                m_Status        = "Bake failed: " + result.GetError();
                m_StatusIsError = true;
            }
        }

        const bool busy = m_BakeRunning;
        ImGui::BeginDisabled( busy );

        int resolutionIndex = m_Params.Resolution == 64u ? 0 : 1;
        if ( ImGui::Combo( "Resolution", &resolutionIndex,
                           "64 (preview)\0"
                           "128 (ship)\0" ) )
            m_Params.Resolution = resolutionIndex == 0 ? 64u : 128u;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "128 is the shipping size and the deck's (8 MiB in RGBA8). 64 bakes eight "
                               "times faster and is for finding a look, not for shipping." );

        int seed = static_cast<int>( m_Params.Seed );
        if ( ImGui::DragInt( "Seed", &seed, 1.0f, 0, 1000000 ) )
            m_Params.Seed = static_cast<uint32_t>( seed < 0 ? 0 : seed );
        ImGui::SameLine();
        if ( ImGui::Button( "Re-roll" ) )
            m_Params.Seed = m_Params.Seed * 1664525u + 1013904223u;

        ImGui::DragFloat( "Curl Strength", &m_Params.CurlStrength, 0.005f, 0.0f, 0.5f, "%.3f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How far the divergence-free flow shears the wispy channels, in lattice "
                               "cells. 0 leaves plain inverted Alligator - a web with no hooks in it." );

        ImGui::DragFloat( "Wispy Period LF", &m_Params.WispyPeriodLowFrequency, 1.0f, 1.0f, 32.0f, "%.0f" );
        ImGui::DragFloat( "Wispy Period HF", &m_Params.WispyPeriodHighFrequency, 1.0f, 1.0f, 32.0f, "%.0f" );
        ImGui::DragFloat( "Billow Period LF", &m_Params.BillowPeriodLowFrequency, 1.0f, 1.0f, 32.0f, "%.0f" );
        ImGui::DragFloat( "Billow Period HF", &m_Params.BillowPeriodHighFrequency, 1.0f, 1.0f, 32.0f, "%.0f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Lattice cells across the volume. Whole numbers only - a fractional period "
                               "does not tile, and the seam runs the length of the sky." );

        ImGui::EndDisabled();

        // The SAME validation the loader runs, so the button and the file agree about what is legal.
        const auto valid = Assets::ValidateCloudNoiseVolumeParams( m_Params );
        if ( !valid )
            ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ), "%s", valid.GetError().c_str() );

        // THE TRAP THIS WARNING EXISTS FOR. The fields above are an editable recipe, and an imported volume
        // has none — so Bake here does not "re-bake what you are looking at", it discards the artist's
        // imported voxels and generates unrelated ones from whatever is in the boxes. Said before the click
        // rather than regretted after it; the button is deliberately still enabled, because generating a
        // fresh volume in this window is a legitimate thing to want.
        if ( m_HasVolume && m_Volume.Origin == Assets::CloudNoiseVolumeOrigin::Imported )
            ImGui::TextColored( ImVec4( 0.95f, 0.75f, 0.35f, 1.0f ),
                                "This volume was imported. Baking REPLACES it with the recipe above." );

        ImGui::BeginDisabled( busy || !valid );
        if ( ImGui::Button( "Bake", ImVec2( 120.0f, 0.0f ) ) )
        {
            m_BakeProgress.store( 0.0f );
            m_BakeRunning   = true;
            m_Status        = "Baking...";
            m_StatusIsError = false;

            const Assets::CloudNoiseVolumeParams params = m_Params;
            m_Baking                                    = std::async( std::launch::async, [this, params]
                                                                      { return Assets::GenerateCloudNoiseVolume( params, &m_BakeProgress ); } );
        }
        ImGui::EndDisabled();

        if ( busy )
        {
            ImGui::SameLine();
            ImGui::ProgressBar( m_BakeProgress.load(), ImVec2( -1.0f, 0.0f ) );
        }

        // THE "OPEN A VOLUME" COMBO THAT WAS HERE IS GONE, and its absence is the feature. It was how this
        // window came to edit a different asset than the one it was opened on — which, now that the title,
        // the ImGui id and open-or-focus are all keyed on the subject handle, would leave a window called
        // one thing editing another. A different volume is a different window, opened by double-clicking it
        // in the asset browser. See Editor/Panels/IPanel.hpp on why the subject is immutable.
    }

    std::vector<unsigned char> CloudNoiseVolumePanel::BuildSlicePixels() const
    {
        const uint32_t             n = m_Volume.Params.Resolution;
        std::vector<unsigned char> pixels( static_cast<size_t>( n ) * n * 4u, 0u );

        const int slice = m_SliceIndex < 0 ? 0
                                           : ( m_SliceIndex >= static_cast<int>( n ) ? static_cast<int>( n ) - 1
                                                                                     : m_SliceIndex );

        for ( uint32_t v = 0; v < n; ++v )
        {
            for ( uint32_t u = 0; u < n; ++u )
            {
                uint32_t x = 0;
                uint32_t y = 0;
                uint32_t z = 0;
                switch ( m_Axis )
                {
                    case SliceAxis::X:
                        x = static_cast<uint32_t>( slice );
                        y = u;
                        z = v;
                        break;
                    case SliceAxis::Y:
                        x = u;
                        y = static_cast<uint32_t>( slice );
                        z = v;
                        break;
                    case SliceAxis::Z:
                        x = u;
                        y = v;
                        z = static_cast<uint32_t>( slice );
                        break;
                }

                const size_t source = ( ( static_cast<size_t>( z ) * n + y ) * n + x ) * 4u;
                const size_t target = ( static_cast<size_t>( v ) * n + u ) * 4u;

                if ( m_ChannelView == ChannelView::AllFour )
                {
                    // All four at once, and the alpha channel is folded into the picture rather than left
                    // to the compositor: an image drawn with a real alpha would show the ImGui window
                    // behind it, which reads as the volume being empty exactly where it is densest.
                    pixels[target + 0] = m_Volume.Voxels[source + 0];
                    pixels[target + 1] = m_Volume.Voxels[source + 1];
                    pixels[target + 2] =
                         static_cast<unsigned char>( ( static_cast<int>( m_Volume.Voxels[source + 2] ) +
                                                       static_cast<int>( m_Volume.Voxels[source + 3] ) ) /
                                                     2 );
                    pixels[target + 3] = 255u;
                }
                else
                {
                    const int           channel = static_cast<int>( m_ChannelView ) - 1;
                    const unsigned char value   = m_Volume.Voxels[source + static_cast<size_t>( channel )];
                    pixels[target + 0]          = value;
                    pixels[target + 1]          = value;
                    pixels[target + 2]          = value;
                    pixels[target + 3]          = 255u;
                }
            }
        }

        return pixels;
    }

    void CloudNoiseVolumePanel::RefreshSlice()
    {
        m_SliceDirty = false;
        m_SliceImage.reset();

        if ( !m_HasVolume || m_Volume.Voxels.empty() )
            return;

        const uint32_t n = m_Volume.Params.Resolution;

        const Core::Formats::Image2DSpecification spec{
             .Tag        = "CloudNoiseSlice",
             .Width      = n,
             .Height     = n,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Data       = BuildSlicePixels(),
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Sample,
        };

        m_SliceImage = Graphic::Image2D::Create( spec );
        if ( !m_SliceImage )
        {
            m_Status        = "The slice preview image could not be created on the device.";
            m_StatusIsError = true;
        }
    }

    void CloudNoiseVolumePanel::DrawPreviewSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Preview" );

        if ( !m_HasVolume )
        {
            ImGui::TextDisabled( "Bake a volume, or open one, to see its slices." );
            return;
        }

        const uint32_t n = m_Volume.Params.Resolution;

        // AN IMPORTED VOLUME HAS NO SEED, so it must not be shown one. Printing "seed 0" beside imported
        // voxels is the same lie the container's origin field exists to prevent, just told in the UI
        // instead of in the file.
        if ( m_Volume.Origin == Assets::CloudNoiseVolumeOrigin::Imported )
            ImGui::Text( "%s - %u^3 RGBA8, %.2f MiB, imported (no recipe)", m_SourceName.c_str(), n,
                         static_cast<double>( m_Volume.Voxels.size() ) / ( 1024.0 * 1024.0 ) );
        else
            ImGui::Text( "%s - %u^3 RGBA8, %.2f MiB, seed %u", m_SourceName.c_str(), n,
                         static_cast<double>( m_Volume.Voxels.size() ) / ( 1024.0 * 1024.0 ),
                         m_Volume.Params.Seed );

        int axis = static_cast<int>( m_Axis );
        if ( ImGui::Combo( "Axis", &axis, "X\0Y\0Z\0" ) )
        {
            m_Axis       = static_cast<SliceAxis>( axis );
            m_SliceDirty = true;
        }

        if ( ImGui::SliderInt( "Slice", &m_SliceIndex, 0, static_cast<int>( n ) - 1, "%d" ) )
            m_SliceDirty = true;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A 3D field has no picture, only slices. Scrub this to see how the structure "
                               "continues through the volume - a field that changes abruptly between "
                               "neighbouring slices will read as noise rather than as cloud." );

        int view = static_cast<int>( m_ChannelView );
        if ( ImGui::Combo( "Channels", &view,
                           "All four\0"
                           "R  Curly-Alligator LF (wispy, coarse)\0"
                           "G  Curly-Alligator HF (wispy, fine)\0"
                           "B  Alligator LF (billowy, coarse)\0"
                           "A  Alligator HF (billowy, fine)\0" ) )
        {
            m_ChannelView = static_cast<ChannelView>( view );
            m_SliceDirty  = true;
        }

        ImGui::SliderInt( "Zoom", &m_PreviewZoom, 1, 6, "%dx" );

        if ( m_SliceDirty )
            RefreshSlice();

        if ( m_SliceImage )
        {
            const float side = static_cast<float>( n * static_cast<uint32_t>( m_PreviewZoom ) );
            SlicePreviewHelper()->Image( m_SliceImage, ImVec2( side, side ) );
        }

        ImGui::TextDisabled( "%s slice %d of %u", AxisName( axis ), m_SliceIndex, n );
    }

    void CloudNoiseVolumePanel::ImportSheet( const std::filesystem::path& path )
    {
        // stb directly, exactly as CloudLayoutPanel::LoadSourceImage does it. There is no cook, no
        // TextureAsset and no AssetManager on this path, and that is not an omission: a cooked `.tex` in this
        // engine is an identity record carrying no pixels, so registering the sheet as an asset would buy
        // nothing and leave a texture in the browser that is not a texture anybody should use.
        int      width = 0, height = 0, sourceChannels = 0;
        stbi_uc* decoded = stbi_load( path.string().c_str(), &width, &height, &sourceChannels, 4 );
        if ( !decoded )
        {
            m_Status = "'" + path.filename().string() + "' could not be read as an image: " +
                       ( stbi_failure_reason() ? stbi_failure_reason() : "unknown" );
            m_StatusIsError = true;
            LOG_ERROR( "[Clouds] {}", m_Status );
            return;
        }

        const std::vector<unsigned char> pixels( decoded, decoded + static_cast<size_t>( width ) * height * 4 );
        stbi_image_free( decoded );

        auto imported = Assets::DecodeCloudNoiseVolumeFromSheet( pixels, static_cast<uint32_t>( width ),
                                                                 static_cast<uint32_t>( height ) );
        if ( !imported )
        {
            // The refusal is shown WHOLE, numbers and all, rather than reduced to "import failed". It already
            // names the size that arrived and the sizes that would have worked, which is the only form of
            // this message an artist can act on without opening a log.
            m_Status        = imported.GetError();
            m_StatusIsError = true;
            LOG_ERROR( "[Clouds] Noise sheet '{}' refused: {}", path.string(), imported.GetError() );
            return;
        }

        AdoptVolume( imported.ExtractValue() );
        m_HasVolume = true;

        // The PARAMETERS PANEL IS NOT UPDATED FROM AN IMPORT, and that is deliberate. m_Params drives the
        // Bake button; leaving the artist's last recipe in it means Bake still means "generate what I set
        // up", while the imported volume's own (empty) recipe stays where it belongs — in m_Volume.
        m_SourceName = path.filename().string() + " (imported, unsaved)";
        m_SliceIndex = static_cast<int>( m_Volume.Params.Resolution ) / 2;
        m_SliceDirty = true;
        m_Status     = "Imported " + std::to_string( width ) + "x" + std::to_string( height ) + " as a " +
                   std::to_string( m_Volume.Params.Resolution ) +
                   "^3 volume. It has NO recipe - a picture cannot carry one - so Bake would replace it "
                   "with something else entirely. Save it to keep it.";
        m_StatusIsError = false;

        LOG_INFO( "[Clouds] Noise sheet '{}' imported: {}x{} -> {}^3 RGBA8.", path.string(), width, height,
                  m_Volume.Params.Resolution );
    }

    void CloudNoiseVolumePanel::DrawSheetSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Slice sheet (import / export)" );

        ImGui::TextWrapped( "A flat picture with the volume's Z slices tiled across it - the format Unreal "
                            "builds a Volume Texture from and Houdini's Volume Texture Export writes. "
                            "Export one, edit it in your own tool, bring it back." );

        const bool busy = m_BakeRunning;

        ImGui::BeginDisabled( busy );
        if ( ImGui::Button( "Import sheet...", ImVec2( 140.0f, 0.0f ) ) )
        {
            const std::filesystem::path picked =
                 Common::Utils::FileSystem::OpenFileDialog( "Image\0*.png;*.tga;*.bmp\0" );
            if ( !picked.empty() )
                ImportSheet( picked );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s",
                               ( "The sheet must be exactly one of: " + Assets::CloudNoiseSheetSizesDescription() +
                                 ". A size that is not a whole cube is refused with the numbers "
                                 "rather than resampled - a stretched volume is a cloud shape you "
                                 "could not trace back to what you exported." )
                                    .c_str() );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( busy || !m_HasVolume );
        if ( ImGui::Button( "Export sheet...", ImVec2( 140.0f, 0.0f ) ) )
        {
            std::filesystem::path target = Common::Utils::FileSystem::SaveFileDialog( "PNG image\0*.png\0" );
            if ( !target.empty() )
            {
                if ( target.extension() != Assets::kCloudNoiseSheetExtension )
                    target.replace_extension( Assets::kCloudNoiseSheetExtension );

                auto sheet = Assets::EncodeCloudNoiseVolumeToSheet( m_Volume );
                if ( !sheet )
                {
                    m_Status        = "Export failed: " + sheet.GetError();
                    m_StatusIsError = true;
                }
                else
                {
                    const auto& image = sheet.GetValue();

                    // CHECKED, not fired and forgotten. stbi_write_png returns 0 on a path it cannot open,
                    // and an export that silently wrote nothing would be discovered by the artist only when
                    // the file they went looking for was not there.
                    const int written =
                         stbi_write_png( target.string().c_str(), static_cast<int>( image.Layout.Width ),
                                         static_cast<int>( image.Layout.Height ), 4, image.Pixels.data(),
                                         static_cast<int>( image.Layout.Width ) * 4 );

                    if ( written == 0 )
                    {
                        m_Status        = "Export failed: '" + target.string() + "' could not be written.";
                        m_StatusIsError = true;
                        LOG_ERROR( "[Clouds] {}", m_Status );
                    }
                    else
                    {
                        m_Status = "Exported " + std::to_string( image.Layout.Width ) + "x" +
                                   std::to_string( image.Layout.Height ) + " (" +
                                   std::to_string( image.Layout.TilesX ) + "x" +
                                   std::to_string( image.Layout.TilesY ) + " tiles of " +
                                   std::to_string( image.Layout.Resolution ) + ") to " + target.string() +
                                   ". The voxels survive a return trip exactly; the recipe does not.";
                        m_StatusIsError = false;
                        LOG_INFO( "[Clouds] Noise sheet written: '{}', {}x{}.", target.string(),
                                  image.Layout.Width, image.Layout.Height );
                    }
                }
            }
        }
        ImGui::EndDisabled();

        if ( m_HasVolume )
        {
            const auto layout = Assets::CloudNoiseSheetLayoutFor( m_Volume.Params.Resolution );
            if ( layout )
            {
                ImGui::TextDisabled( "This volume exports as %ux%u (%ux%u tiles of %u).", layout.GetValue().Width,
                                     layout.GetValue().Height, layout.GetValue().TilesX, layout.GetValue().TilesY,
                                     layout.GetValue().Resolution );

                // SAID WHERE IT COSTS AN AFTERNOON OTHERWISE. Unreal guesses a sheet's tile size from the
                // image area assuming a SQUARE sheet, and a 128^3 volume cannot produce one — 2048x1024
                // makes it derive 186x93 tiles and a depth of 121, which is silently garbage.
                if ( layout.GetValue().Width != layout.GetValue().Height )
                    ImGui::TextDisabled( "Taking it into Unreal: set Tile Size X and Y to %u by hand - its "
                                         "auto-detect only works on square sheets.",
                                         layout.GetValue().Resolution );
            }
        }
    }

    void CloudNoiseVolumePanel::DrawSaveSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Save" );

        // SAVE WRITES THE SUBJECT. SAVE AS CREATES A SECOND ASSET AND OPENS ITS OWN WINDOW — it does NOT
        // repoint this one. The subject is this window's identity: its title, its ImGui id and the key
        // open-or-focus matches on are all built from it, so a document that followed a Save As would be a
        // window named after a file it no longer edits. Baking a variant and keeping it is still exactly
        // what it was — re-roll, Save As — and now the variant arrives as its own document.
        const bool busy = m_BakeRunning;

        ImGui::BeginDisabled( !m_HasVolume || busy || m_SubjectPath.empty() );
        const bool save = ImGui::Button( "Save", ImVec2( 120.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( !m_HasVolume || busy );
        const bool saveAs = ImGui::Button( "Save As...", ImVec2( 120.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled( "Volumes live in %s", Common::Constants::Path::CLOUD_NOISE_PATH.string().c_str() );

        std::filesystem::path target;
        if ( save )
        {
            target = m_SubjectPath;
        }
        else if ( saveAs )
        {
            target = Common::Utils::FileSystem::SaveFileDialog( "Cloud Noise Volume\0*.dcnv\0" );
            if ( !target.empty() && target.extension() != Assets::kCloudNoiseVolumeExtension )
                target.replace_extension( Assets::kCloudNoiseVolumeExtension );
        }

        if ( target.empty() )
            return;

        // Compared before the write, because after it the file exists and the two paths would be
        // indistinguishable by anything on disk.
        (void)WriteTo( target, /*isCopy=*/target != m_SubjectPath );
    }

    void CloudNoiseVolumePanel::AdoptVolume( Assets::CloudNoiseVolumeData&& volume )
    {
        // THE ONE PLACE THE VOXELS CHANGE. Everything that follows a new volume lives here so that no
        // caller can do half of it: the buffer, the count that makes GetDiskState answerable, the slice
        // index that must be inside the new resolution, and the stale preview.
        m_Volume = std::move( volume );
        ++m_VolumeRevision;
        m_SliceIndex = static_cast<int>( m_Volume.Params.Resolution ) / 2;
        m_SliceDirty = true;
    }

    bool CloudNoiseVolumePanel::WriteTo( const std::filesystem::path& target, const bool isCopy )
    {
        // LIFTED OUT OF THE BUTTON so SaveDocument runs it too. The whole sequence and not just the write:
        // the file, the re-registration that makes the component's slot show the new bytes without a
        // restart, and the copy's own document.
        const auto written = Assets::CloudNoiseVolumeAsset::Save( target, m_Volume );
        if ( !written )
        {
            m_Status        = "Save failed: " + written.GetError();
            m_StatusIsError = true;
            return false;
        }

        if ( !isCopy )
        {
            m_SourceName = target.filename().string();
            // These voxels are what the file holds now.
            m_SavedRevision = m_VolumeRevision;
            m_Tracked       = true;
        }
        m_Status        = "Saved to " + target.string();
        m_StatusIsError = false;

        // Re-registered straight away so the component's slot shows the new bytes without a restart. A tool
        // whose output only appears after the editor is reopened is a tool nobody iterates in.
        if ( !m_Assets )
            return true;

        auto asset = m_Assets->FindByPath<Assets::CloudNoiseVolumeAsset>( target );
        if ( asset )
            asset->Load(); // overwritten in place: re-read so the cached bytes are the new ones
        else
            asset = m_Assets->CreateAsset<Assets::CloudNoiseVolumeAsset>( Assets::AssetPriority::Medium, target );

        if ( !asset )
            return true;

        if ( const auto uploaded = Runtime::ResourceRegistry::GetCloudNoiseService()->Register( asset );
             !uploaded )
        {
            // THE FILE IS ON DISK, so the document is clean — an upload failure is about the running sky
            // and not about what was written, and reporting the save as failed would make "Save All" try
            // again for ever.
            m_Status        = "Saved, but the volume could not be uploaded: " + uploaded.GetError();
            m_StatusIsError = true;
            return true;
        }

        if ( isCopy )
        {
            // The copy is a new asset, so it gets its own document. Queued rather than constructed here:
            // EditorLayer is the only place that may create a panel, and this is the same wire the asset
            // browser's double-click uses.
            RequestCloudDocument( m_Assets, target.string() );
            m_Status        = "Saved a copy to " + target.string() + " - it has opened in its own window.";
            m_StatusIsError = false;
        }
        return true;
    }

    ISubjectDocument::DiskState CloudNoiseVolumePanel::GetDiskState() const
    {
        if ( !m_Tracked )
            return DiskState::Untracked;
        return m_VolumeRevision == m_SavedRevision ? DiskState::Clean : DiskState::Dirty;
    }

    bool CloudNoiseVolumePanel::SaveDocument()
    {
        // A document with no file of its own reports false rather than inventing a path; Save As is a
        // gesture with a dialog behind it and is not what "Save the focused document" means. A window
        // whose asset would not load has no volume to write either, and saying so is better than writing
        // an empty one over the file.
        if ( m_SubjectPath.empty() || !m_HasVolume )
            return false;

        return WriteTo( m_SubjectPath, /*isCopy=*/false );
    }

    std::vector<EditableProperty> CloudNoiseVolumePanel::EditableProperties() const
    {
        std::vector<EditableProperty> properties;
        properties.reserve( std::size( kRecipeFields ) + 2u );

        EditableProperty resolution;
        resolution.Name       = "Resolution";
        resolution.Label      = "Resolution";
        resolution.Group      = kRecipeGroup;
        resolution.Type       = "int";
        resolution.Components = 1;
        resolution.Min        = 64.0f;
        resolution.Max        = 128.0f;
        resolution.Value[0]   = static_cast<float>( m_Params.Resolution );
        properties.push_back( std::move( resolution ) );

        EditableProperty seed;
        seed.Name       = "Seed";
        seed.Label      = "Seed";
        seed.Group      = kRecipeGroup;
        seed.Type       = "int";
        seed.Components = 1;
        seed.Min        = 0.0f;
        // The panel's own field is an int; the ceiling is what a float can still carry exactly, so a value
        // a client sends and reads back cannot differ from the one it meant.
        seed.Max      = 16777216.0f;
        seed.Value[0] = static_cast<float>( m_Params.Seed );
        properties.push_back( std::move( seed ) );

        for ( const RecipeField& field : kRecipeFields )
        {
            EditableProperty property;
            property.Name       = field.Name;
            property.Label      = field.Label;
            property.Group      = kRecipeGroup;
            property.Type       = "float";
            property.Components = 1;
            property.Min        = field.Min;
            property.Max        = field.Max;
            property.Value[0]   = m_Params.*field.Member;
            properties.push_back( std::move( property ) );
        }

        return properties;
    }

    Common::BoolResultStr CloudNoiseVolumePanel::SetEditableProperty( const std::string&        name,
                                                                      const std::vector<float>& value )
    {
        if ( value.size() != 1u )
        {
            return Common::MakeFormattedError<bool>(
                 "every property of a cloud noise volume is a single number and {} were sent for '{}'.",
                 value.size(), name );
        }

        if ( name == "Resolution" )
        {
            // TWO LEGAL VALUES, not a range, and the refusal says both of them. 64 and 128 are the sizes
            // the format and the sheet layout are defined for; a clamp into "the nearest legal one" would
            // hand a caller a volume of a size it did not ask for.
            const int wanted = static_cast<int>( value[0] );
            if ( wanted != 64 && wanted != 128 )
            {
                return Common::MakeFormattedError<bool>(
                     "'Resolution' is 64 or 128 and nothing between; {} is neither.", wanted );
            }
            m_Params.Resolution = static_cast<uint32_t>( wanted );
            return BOOLSUCCESS;
        }

        if ( name == "Seed" )
        {
            if ( value[0] < 0.0f )
                return Common::MakeFormattedError<bool>( "'Seed' cannot be negative; {} was sent.", value[0] );
            m_Params.Seed = static_cast<uint32_t>( value[0] );
            return BOOLSUCCESS;
        }

        for ( const RecipeField& field : kRecipeFields )
        {
            if ( name != field.Name )
                continue;

            // REFUSED RATHER THAN CLAMPED: a value silently moved is a value the caller reads back as its
            // own and then cannot explain.
            if ( value[0] < field.Min || value[0] > field.Max )
            {
                return Common::MakeFormattedError<bool>( "'{}' takes {} to {}; {} is outside it.", name, field.Min,
                                                         field.Max, value[0] );
            }
            m_Params.*field.Member = value[0];
            return BOOLSUCCESS;
        }

        return Common::MakeFormattedError<bool>(
             "this cloud noise volume has no property called '{}'. Ask 'properties' for the ones it offers.",
             name );
    }

    bool CloudNoiseVolumePanel::IsSubjectAlive() const
    {
        // ASKED OF THE METADATA rather than of a typed lookup: the question is whether the asset is still
        // THERE, and a typed lookup answers a different one (whether it is still that type) — a subject
        // that failed to reload as its own class would read as deleted and the window would close on a
        // load error instead of reporting it.
        return m_Assets && m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }
} // namespace Desert::Editor
