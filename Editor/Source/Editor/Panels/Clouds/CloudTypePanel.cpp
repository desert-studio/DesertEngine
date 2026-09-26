#include "CloudTypePanel.hpp"

#include "CloudDocumentOpen.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudProceduralVolume.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Graphic/Clouds/CloudPayload.hpp>
#include <Engine/Graphic/Clouds/CloudTypeShape.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <Editor/Core/AssetPickerRows.hpp>

namespace Desert::Editor
{
    // The editor's ImGui lives in the global namespace; unqualified `ImGui::` inside `Desert::` would
    // resolve to `Desert::ImGui`, which is the engine's own runtime UI. Every panel in this folder opens
    // with the same alias for the same reason.
    namespace ImGui = ::ImGui;

    namespace
    {
        // How many points the READ-BACK preview below the editor is drawn with. It is deliberately far
        // finer than the sixteen the profile is authored at: the preview reads the field the lumps
        // actually make, and the whole question it answers is what the sixteen turned into.
        constexpr int kPreviewSamples = 256;

        void CopyInto( char* buffer, size_t size, const std::string& text )
        {
            const size_t n = text.size() < size - 1 ? text.size() : size - 1;
            std::memcpy( buffer, text.data(), n );
            buffer[n] = '\0';
        }

        // A `.dcnv` path as the type file stores it: relative to the project's assets root, forward
        // slashes. The asset joins it back on load, and doing the relativisation anywhere else is how a
        // library ends up carrying one developer's home directory.
        std::string RelativeToAssets( const Common::Filepath& full )
        {
            std::error_code ec;
            const auto      rel = std::filesystem::relative( full, Common::Constants::Path::ASSETS_PATH, ec );
            if ( ec || rel.empty() )
                return full.generic_string();
            return rel.generic_string();
        }

        // -- EVERY AUTHORED SCALAR OF A CLOUD TYPE, ONCE ------------------------------------------------
        //
        // TWO READERS AND ONE LIST. The sliders this panel draws and the properties the control channel
        // offers are the same rows, in the same order, with the same clamps - because they are read off
        // this table rather than written out twice. The alternative was a second list inside
        // EditableProperties, and the failure it produces is silent in both directions: a channel that can
        // set a number the panel does not show, or a slider the channel cannot reach, with nothing
        // anywhere saying so.
        //
        // THE NAME IS THE C++ FIELD'S OWN NAME, not the caption. A caption is what a person reads and may
        // be reworded; the name is what a client addresses and must not move under it.
        //
        // The three authored things that are NOT here are absent by construction rather than by omission:
        // the vertical profile is a CURVE of sixteen samples (drawn by DrawProfileEditor, and a channel
        // that carries at most four floats cannot express it), and the display name and the notes are
        // text. EditableProperties reports what it can carry and the panel draws the rest.
        struct ShapeField
        {
            const char* Name;
            const char* Label;
            float Graphic::CloudTypeShape::*Member;
            float                           Min;
            float                           Max;
            const char*                     Format;
            /// A section header drawn before this row, or nullptr.
            const char* SectionBefore;
            /// The profile canvas is drawn before this row. Exactly one row carries it.
            bool ProfileEditorBefore;
            /// Drawn disabled while the type has no anvil - a second lobe's altitude means nothing without
            /// the lobe, and a live slider for it is a control that changes nothing.
            bool        GatedByAnvil;
            const char* Tooltip;
        };

        constexpr ShapeField kShapeFields[] = {
             { "BaseAltitudeKm", "Base Altitude (km)", &Graphic::CloudTypeShape::BaseAltitudeKm, 0.0f, 14.0f,
               "%.2f", nullptr, false, false,
               "Where this kind of cloud's base sits - its lifting condensation level. ABSOLUTE, not a "
               "fraction of a layer: the shell the march intersects is computed from this and the top, so "
               "moving it moves the clouds." },
             { "TopAltitudeKm", "Top Altitude (km)", &Graphic::CloudTypeShape::TopAltitudeKm, 0.0f, 16.0f, "%.2f",
               nullptr, false, false, "The top this kind reaches at the CORE of a placement patch." },
             { "EdgeTopFraction", "Edge Top Fraction", &Graphic::CloudTypeShape::EdgeTopFraction, 0.0f, 1.0f,
               "%.2f", nullptr, false, false,
               "How much of its full height it reaches where a patch has only just begun. Near 1 is a sheet "
               "- the same everywhere. Near 0.1 is a cumulus field: a flat pad at the rim of a patch and a "
               "tower in its middle." },
             { "BaseRampFraction", "Base Ramp", &Graphic::CloudTypeShape::BaseRampFraction, 0.001f, 1.0f, "%.3f",
               nullptr, false, false,
               "How much of the cloud's own height the base takes to reach full density. Small is the flat "
               "bottom of a cumulus; as long as the taper it becomes the symmetric section of a lenticular." },
             { "AnvilStrength", "Anvil Strength", &Graphic::CloudTypeShape::AnvilStrength, 0.0f, 1.0f, "%.2f",
               nullptr, true, false,
               "A SECOND lobe of cloud, above the tower and spreading wider than it, with a GAP between the "
               "two. Zero means this kind has none. The profile above cannot express it and is not meant to: "
               "one curve is one connected body, and what makes a cumulonimbus recognisable is the gap." },
             { "AnvilAltitudeKm", "Anvil Altitude (km)", &Graphic::CloudTypeShape::AnvilAltitudeKm, 0.0f, 16.0f,
               "%.2f", nullptr, false, true, nullptr },
             { "AnvilThicknessKm", "Anvil Thickness (km)", &Graphic::CloudTypeShape::AnvilThicknessKm, 0.0f, 5.0f,
               "%.2f", nullptr, false, true, nullptr },
             { "DetailCharacter", "Detail Character", &Graphic::CloudTypeShape::DetailCharacter, 0.0f, 1.0f,
               "%.2f", "Matter and edge", false, false,
               "0 is wispy erosion, 1 is billowy. This is the type's EDGE, not its silhouette: a cirrus is 0 "
               "and a stratocumulus is nearly 1." },
             { "DetailFactor", "Detail Factor", &Graphic::CloudTypeShape::DetailFactor, 0.0f, 8.0f, "%.2f",
               nullptr, false, false, nullptr },
             { "DensityFactor", "Density Factor", &Graphic::CloudTypeShape::DensityFactor, 0.0f, 8.0f, "%.2f",
               nullptr, false, false, nullptr },
             { "ExtinctionFactor", "Extinction Factor", &Graphic::CloudTypeShape::ExtinctionFactor, 0.0f, 8.0f,
               "%.2f", nullptr, false, false,
               "The three factors MULTIPLY the layer's own Detail Strength, Density Scale and Extinction "
               "Scale rather than replacing them, so 1 means 'this kind as it is' and the layer's sliders "
               "keep meaning what they meant." },
             { "PlacementScale", "Placement Scale", &Graphic::CloudTypeShape::PlacementScale, 0.05f, 8.0f, "%.2f",
               "Where it sits in the sky", false, false,
               "How big this kind's patches are, as a MULTIPLE of the layer's Weather Tile Size. Below 1 "
               "gives many small cells - a stratocumulus deck of one-kilometre lumps; above 1 gives few "
               "large ones - a storm cell, or a sheet that never ends. Each kind of cloud in the layer has "
               "its own, which is why a low deck and a tall tower can be different sizes in the same sky." },
             { "PlacementAnisotropy", "Placement Anisotropy", &Graphic::CloudTypeShape::PlacementAnisotropy, 1.0f,
               8.0f, "%.2f", nullptr, false, false,
               "How much longer this kind's patches are ALONG THE WIND than across it. 1 is round; above 1 "
               "they are drawn out downwind into bands, which is what makes fibrous cirrus read as cirrus." },
        };

        // The document's VISIBLE title: the subject's file name. Computed before the base class is
        // constructed — ISubjectDocument bakes the title in its own constructor and holds it for the
        // window's life — so it is a free function rather than a member.
        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets )
            {
                if ( const auto asset = assets->FindByHandle<Assets::CloudTypeAsset>( subject ) )
                    return asset->GetMetadata().Filepath.filename().string();
            }
            return "Cloud Type";
        }
    } // namespace

    CloudTypePanel::CloudTypePanel( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
         : ISubjectDocument( SubjectTitle( subject, assets ),
                             AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::CloudType ) ) ),
           m_Assets( assets )
    {
        // The subject is read through the SAME OpenType the library combo used to call, so a document opened
        // from the browser and a type opened from the old list cannot end up with differently-populated
        // buffers. The built-in default stays as the fallback for a subject that will not resolve.
        if ( assets )
        {
            if ( const auto asset = assets->FindByHandle<Assets::CloudTypeAsset>( subject ) )
                OpenType( asset->GetMetadata().Filepath );
        }

        if ( m_SourcePath.empty() )
        {
            CopyInto( m_NameBuffer, sizeof( m_NameBuffer ), m_Data.DisplayName.value_or( "" ) );
            CopyInto( m_NotesBuffer, sizeof( m_NotesBuffer ), m_Data.Notes.value_or( "" ) );
        }
    }

    void CloudTypePanel::OnUIRender()
    {
        // NO ImGui::Begin HERE, and it is not an omission. EditorLayer's panel loop already wraps
        // OnUIRender in Begin/End for this panel's name, and it owns the p_open bool the title-bar X
        // writes to. A Begin for the SAME name nested inside that one is not appending -- ImGui only
        // supports appending between Begin/End PAIRS -- and the window comes out with its title bar
        // and nothing else. Every panel in this folder had it and drew nothing; no panel outside it
        // does. See CALIBRATION.md §PTP.
        ImGui::TextWrapped(
             "A cloud TYPE is thirteen numbers, a vertical profile curve, and the noise its edge is cut "
             "from. Together they place the pile of lumps this kind of cloud is made of, so a type stays "
             "small, readable and editable - there is no baked texture here to go stale against the "
             "maths." );
        ImGui::Separator();

        DrawLibrarySection();
        ImGui::Separator();
        DrawShapeSection();
        ImGui::Separator();
        DrawNoiseSection();
        ImGui::Separator();
        DrawPreviewSection();
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

    void CloudTypePanel::OpenType( const Common::Filepath& path )
    {
        if ( !m_Assets )
            return;

        auto asset = m_Assets->FindByPath<Assets::CloudTypeAsset>( path );
        if ( !asset )
            asset = m_Assets->CreateAsset<Assets::CloudTypeAsset>( Assets::AssetPriority::Medium, path );

        // REGISTERED IS NOT LOADED. An asset the manager knows about but has not read yet answers false to
        // IsReadyForUse, and this window used to give up on it — so a type opened in the first seconds of a
        // session, before the preloader reached it, came up showing the BUILT-IN DEFAULT under the file's
        // own name, and a Save from there would have written the default over the artist's type.
        //
        // Found by GetDiskState answering "untracked" for a document whose file is plainly there, and it
        // was the THIRD of three panels with the same hole (CloudNoiseVolumePanel and CloudLayoutPanel
        // carry the same fix and the same note). One defect written three times, which is what happens
        // when three windows each resolve their own subject.
        if ( asset && !asset->IsReadyForUse() )
            asset->Load();

        if ( !asset || !asset->IsReadyForUse() )
        {
            m_Status        = "'" + path.string() + "' could not be opened - the log says why.";
            m_StatusIsError = true;
            return;
        }

        m_Data       = asset->GetData();
        // WHAT THE FILE HOLDS, kept so GetDiskState has something to be clean against. Taken here rather
        // than derived later: after this line the buffer starts being edited, and a snapshot taken at the
        // first edit would already be one edit late.
        m_OnDisk     = m_Data;
        m_Tracked    = true;
        m_SourcePath = path;
        m_SourceName = path.filename().string();
        CopyInto( m_NameBuffer, sizeof( m_NameBuffer ), m_Data.DisplayName.value_or( "" ) );
        CopyInto( m_NotesBuffer, sizeof( m_NotesBuffer ), m_Data.Notes.value_or( "" ) );

        m_Status        = "Opened " + m_SourceName + ".";
        m_StatusIsError = false;
    }

    void CloudTypePanel::DrawLibrarySection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Library" );

        ImGui::Text( "Editing: %s", m_SourceName.c_str() );

        // THE "OPEN A TYPE..." COMBO, "NEW FROM BUILT-IN" AND "OPEN FILE..." THAT WERE HERE ARE GONE, and
        // their absence is the feature. They were how this window came to edit a different asset than the
        // one it was opened on — which, now that the title, the ImGui id and open-or-focus are all keyed on
        // the subject handle, would leave a window called one thing saving over another. A different type is
        // a different window, opened by double-clicking it in the asset browser; a NEW type is Save As
        // below, which writes a copy and opens ITS window rather than repointing this one.

        if ( ImGui::InputText( "Display Name", m_NameBuffer, sizeof( m_NameBuffer ) ) )
            m_Data.DisplayName = std::string( m_NameBuffer );
        if ( ImGui::InputTextMultiline( "Notes", m_NotesBuffer, sizeof( m_NotesBuffer ), ImVec2( -1.0f, 54.0f ) ) )
            m_Data.Notes = std::string( m_NotesBuffer );
    }

    void CloudTypePanel::DrawShapeSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Shape" );

        Graphic::CloudTypeShape& shape = m_Data.Shape;

        // DRAWN FROM kShapeFields, not from thirteen hand-written slider calls. The captions, the clamps
        // and the tooltips that used to live here are IN the table now, which is what makes the control
        // channel's census of this document the same list a person sees rather than a second one beside it.
        for ( const ShapeField& field : kShapeFields )
        {
            if ( field.SectionBefore )
                Utils::ImGuiUtilities::SectionHeader( field.SectionBefore );

            // The one control on this panel that is a CANVAS rather than a number, drawn where the table
            // says it belongs. Its own method for the reason it always had one: it owns a hit region, a
            // drag that runs across several samples and a set of presets.
            if ( field.ProfileEditorBefore )
                DrawProfileEditor();

            const bool gated = field.GatedByAnvil && shape.AnvilStrength <= 0.0f;
            ImGui::BeginDisabled( gated );
            ImGui::SliderFloat( field.Label, &( shape.*field.Member ), field.Min, field.Max, field.Format );
            ImGui::EndDisabled();

            if ( field.Tooltip && ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", field.Tooltip );
        }
    }

    void CloudTypePanel::DrawProfileEditor()
    {
        Utils::ImGuiUtilities::SectionHeader( "Vertical profile" );

        Graphic::CloudVerticalProfile& profile = m_Data.Shape.Profile;

        ImGui::TextWrapped( "Drag inside the box to shape the cloud. Height runs up the box - the bottom "
                            "edge is this type's base altitude and the top edge its top - and the width of "
                            "the silhouette is how wide the cloud is there." );

        // THE SILHOUETTE IS DRAWN MIRRORED AND EDITED ON EITHER SIDE, because that is the thing an artist
        // is thinking about: the outline of a cloud seen from the side. A single-sided plot of half-width
        // against height is the same sixteen numbers and reads as a graph, which is what the panel used to
        // show as a RESULT. This is the input, so it looks like the cloud.
        ImGui::InvisibleButton( "##profileCanvas", ImVec2( ImGui::GetContentRegionAvail().x, 220.0f ) );

        const bool   active = ImGui::IsItemActive();
        const ImVec2 min    = ImGui::GetItemRectMin();
        const ImVec2 max    = ImGui::GetItemRectMax();
        const float  width  = max.x - min.x;
        const float  height = max.y - min.y;
        const float  centre = min.x + width * 0.5f;

        // THE FULL-SCALE HALF-WIDTH THE BOX IS DRAWN AGAINST. It is the validator's ceiling of 4 divided
        // by four rather than the widest sample present: a box that rescaled itself to its own content
        // would make every profile look the same and hide exactly the comparison the artist is making.
        constexpr float kFullScaleHalfWidth = 1.0f;

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled( min, max, IM_COL32( 24, 26, 30, 255 ) );

        // The base and top edges, so "which way is up" is not something to remember.
        draw->AddLine( ImVec2( min.x, max.y - 1.0f ), ImVec2( max.x, max.y - 1.0f ),
                       IM_COL32( 90, 96, 110, 255 ) );
        draw->AddLine( ImVec2( min.x, min.y + 1.0f ), ImVec2( max.x, min.y + 1.0f ),
                       IM_COL32( 90, 96, 110, 255 ) );
        draw->AddLine( ImVec2( centre, min.y ), ImVec2( centre, max.y ), IM_COL32( 60, 64, 74, 255 ) );

        constexpr uint32_t kLast = Graphic::kCloudProfileSamples - 1;

        // WHERE SAMPLE `i` SITS IN THE BOX. Sample 0 is the base, so it is at the BOTTOM — the y axis is
        // inverted against ImGui's, which is the one place this widget has to remember that.
        const auto sampleY = [&]( uint32_t i )
        { return max.y - ( static_cast<float>( i ) / static_cast<float>( kLast ) ) * height; };

        const auto sampleHalfPixels = [&]( uint32_t i )
        { return ( profile.HalfWidth[i] / kFullScaleHalfWidth ) * ( width * 0.5f ); };

        // THE BODY AS ONE FILLED SHAPE rather than sixteen bars: the artist is authoring a continuous
        // silhouette and the layout interpolates linearly between samples, so a filled quad strip between
        // consecutive samples is literally what the generator will read.
        for ( uint32_t i = 0; i < kLast; ++i )
        {
            const float lowY  = sampleY( i );
            const float highY = sampleY( i + 1 );
            const float lowW  = sampleHalfPixels( i );
            const float highW = sampleHalfPixels( i + 1 );

            const ImVec2 quad[4] = { ImVec2( centre - lowW, lowY ), ImVec2( centre + lowW, lowY ),
                                     ImVec2( centre + highW, highY ), ImVec2( centre - highW, highY ) };
            draw->AddConvexPolyFilled( quad, 4, IM_COL32( 168, 186, 214, 210 ) );
        }

        for ( uint32_t i = 0; i <= kLast; ++i )
        {
            const float y = sampleY( i );
            const float w = sampleHalfPixels( i );
            draw->AddCircleFilled( ImVec2( centre + w, y ), 2.5f, IM_COL32( 250, 250, 250, 235 ) );
            draw->AddCircleFilled( ImVec2( centre - w, y ), 2.5f, IM_COL32( 250, 250, 250, 120 ) );
        }

        // THE DRAG WRITES EVERY SAMPLE IT CROSSES, not just the nearest one to where the mouse ended up.
        // A mouse moving faster than one sample per frame would otherwise leave gaps in the curve — the
        // same defect a painting brush has when it stamps instead of sweeping, which is why Р1's brush
        // draws a stroke as a capsule rather than a dot. Here the stroke is one-dimensional, so the
        // capsule degenerates to the closed interval between the last position and this one.
        if ( active )
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImVec2 prev( mouse.x - ImGui::GetIO().MouseDelta.x, mouse.y - ImGui::GetIO().MouseDelta.y );

            const auto sampleAt = [&]( float y )
            {
                const float fraction = std::clamp( ( max.y - y ) / std::max( height, 1.0f ), 0.0f, 1.0f );
                return static_cast<uint32_t>( std::lround( fraction * static_cast<float>( kLast ) ) );
            };

            const uint32_t from = sampleAt( std::max( prev.y, mouse.y ) );
            const uint32_t to   = sampleAt( std::min( prev.y, mouse.y ) );

            const float halfWidth =
                 std::clamp( std::abs( mouse.x - centre ) / std::max( width * 0.5f, 1.0f ), 0.0f, 1.0f ) *
                 kFullScaleHalfWidth;

            for ( uint32_t i = from; i <= to && i <= kLast; ++i )
                profile.HalfWidth[i] = halfWidth;
        }

        // The numbers, because a curve that can only be dragged cannot be typed and an artist reproducing
        // a reference needs to be able to type. Sixteen floats in four rows of four.
        if ( ImGui::TreeNode( "Samples (base first)" ) )
        {
            for ( uint32_t row = 0; row < Graphic::kCloudProfileSamples; row += 4 )
            {
                ImGui::PushID( static_cast<int>( row ) );
                ImGui::SetNextItemWidth( -1.0f );
                ImGui::DragFloat4( "##profileRow", &profile.HalfWidth[row], 0.005f, 0.0f, 4.0f, "%.3f" );
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        // THE PRESETS ARE THE ENGINE'S OWN FUNCTIONS, not a second set of numbers written out here. The
        // tower and the deck are what the delivery frames were shot with and what the tests assert
        // against, so a preset that drifted from them would make the panel and the evidence disagree.
        if ( ImGui::Button( "Flat deck" ) )
            profile = Graphic::CloudProfileFlatDeck();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The same width from base to top - a sheet seen edge on." );

        ImGui::SameLine();
        if ( ImGui::Button( "Tower" ) )
            profile = Graphic::CloudProfileTower();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Pinched at the base, swelling through the upper half. NOT reachable under "
                               "the old Top Taper knob at any setting: that law was a product of two "
                               "falling lines, so it narrowed all the way up whatever it was set to." );

        ImGui::SameLine();
        if ( ImGui::Button( "Classic taper" ) )
            profile = Graphic::CloudProfileDefault();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The shape every type had before the curve existed - the built-in "
                               "congestus, which stood at a Top Taper of 0.5." );
    }

    void CloudTypePanel::DrawNoiseSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Noise volume" );

        const std::string current = m_Data.NoiseVolume ? m_Data.NoiseVolume->Path : std::string{};
        const std::string preview = current.empty() ? "Default (built-in volume)" : current;

        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##typeNoise", preview.c_str() ) )
        {
            if ( ImGui::Selectable( "Default (built-in volume)", current.empty() ) )
                m_Data.NoiseVolume.reset();

            if ( m_Assets )
            {
                for ( const auto& row :
                      Assets::ContentRegistry::Rows( Common::Content::ContentKind::CloudNoiseVolume ) )
                {
                    // The GUID the volume's envelope states is what the file resolves by; a volume with
                    // none (a bare container the load refuses) cannot be named and is not offered.
                    const Common::Content::AssetGuid guid =
                         Assets::CloudNoiseVolumeAsset::ReadCloudNoiseVolumeGuid( row.Path );
                    if ( guid.IsNull() )
                        continue;
                    const std::string relative = RelativeToAssets( row.Path );
                    if ( ImGui::Selectable( relative.c_str(), relative == current ) )
                        m_Data.NoiseVolume =
                             Assets::AssetGuidRef{ Common::Content::AssetGuidToText( guid ), relative };
                }
            }
            ImGui::EndCombo();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The 3D noise this kind of cloud's edge is cut from. It belongs to the TYPE "
                               "rather than to the layer because the character of an edge is a property of "
                               "the kind of cloud: the shipped Cirrus names the finer of the two volumes, "
                               "and a cirrus cut from a cumulonimbus' noise is not a cirrus." );
    }

    void CloudTypePanel::DrawPreviewSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Profile" );

        const Graphic::CloudTypeShape& s = m_Data.Shape;

        const float bottomKm = Graphic::CloudTypeBaseKm( s );
        const float topKm    = Graphic::CloudTypeTopKm( s );
        const float spanKm   = topKm - bottomKm;

        if ( !( spanKm > 0.0f ) )
        {
            ImGui::TextDisabled( "The top is not above the base, so there is no profile to draw." );
            return;
        }

        // THE PREVIEW IS THE FIELD ITSELF, NOT A CURVE THAT DESCRIBES IT. It used to plot
        // Graphic::CloudProfileCurve, which was the generator of a table the march sampled; there is no
        // table now — the profile is the normalised distance field of a joined pile of lumps
        // (Engine/Assets/CloudProceduralVolume.hpp). So the panel places the lumps this type would place,
        // with the SAME function the bake calls, and reads the field down two vertical lines. "The preview
        // shows what will be baked" is then true by construction rather than by vigilance, which is the
        // property the sculpting panel's slice preview was built for too.
        Assets::CloudProceduralFieldParams params;
        params.LayerBottomKm    = bottomKm;
        params.LayerThicknessKm = spanKm;
        params.Seed             = 1u;
        params.CoverageContrast = 1.0f;
        params.WindAxis         = glm::vec2( 1.0f, 0.0f );

        // A CELL, and the region is a handful of them. The lattice is the component's shipped default —
        // 12 km over four cells — scaled by this type's own Placement Scale, so the preview is the type at
        // the settings a scene gets before anybody touches a slider.
        const float latticeKm = 3.0f * std::max( s.PlacementScale, 1e-3f );

        params.RegionSizeKm      = std::max( latticeKm * 6.0f, 16.1f );
        params.BlendRadiusKm     = std::max( 0.02f * latticeKm, 1e-3f );
        params.ProfileDepthKm    = std::max( 0.12f * latticeKm, 1e-3f );
        params.ResolvableChordKm = Graphic::CloudFinestResolvableChordKm( 256.0f );

        // EVERY CELL ALIVE, because the preview is about the SHAPE of one cloud and not about how many
        // there are. Coverage is a layer setting and it has its own slider in the Details panel.
        params.Coverage = 1.0f;

        Assets::CloudProceduralSpecies species;
        species.Shape      = s;
        species.CellKm     = latticeKm;
        species.Anisotropy = std::max( s.PlacementAnisotropy, 1e-3f );
        params.Species.push_back( species );

        const glm::vec2 origin( 0.0f, 0.0f );

        const std::vector<Assets::CloudModellingBlob> blobs =
             Assets::GenerateCloudProceduralBlobs( params, 0u, origin );

        if ( blobs.empty() )
        {
            ImGui::TextDisabled( "This type places no lumps at all, so there is nothing to draw." );
            return;
        }

        // THE FULLEST CLUSTER IN THE PREVIEW REGION, found by its own lumps rather than chosen: a cell's
        // fullness is a hash, so there is no "the core one" to ask for. The tallest stack is the one an
        // artist means when they ask what this type looks like.
        glm::vec3 tallest = blobs.front().CentreKm;
        for ( const Assets::CloudModellingBlob& blob : blobs )
        {
            if ( blob.CentreKm.y > tallest.y )
                tallest = blob.CentreKm;
        }

        // How far to step sideways for the second curve: past the lumps of this cluster, into the skirt.
        float widest = 0.0f;
        for ( const Assets::CloudModellingBlob& blob : blobs )
        {
            if ( std::abs( blob.CentreKm.y - tallest.y ) < spanKm )
                widest = std::max( widest, blob.RadiiKm.x );
        }

        m_ProfileEdge.resize( kPreviewSamples );
        m_ProfileCore.resize( kPreviewSamples );

        for ( int i = 0; i < kPreviewSamples; ++i )
        {
            const float fraction   = ( static_cast<float>( i ) + 0.5f ) / static_cast<float>( kPreviewSamples );
            const float altitudeKm = bottomKm + fraction * spanKm;

            m_ProfileCore[i] = Assets::EvaluateCloudProceduralProfile(
                 params, blobs, glm::vec3( tallest.x, altitudeKm, tallest.z ) );
            m_ProfileEdge[i] = Assets::EvaluateCloudProceduralProfile(
                 params, blobs, glm::vec3( tallest.x + widest, altitudeKm, tallest.z ) );
        }

        ImGui::PlotLines( "##profileCore", m_ProfileCore.data(), kPreviewSamples, 0, "through the core", 0.0f,
                          1.0f, ImVec2( -1.0f, 90.0f ) );
        ImGui::PlotLines( "##profileEdge", m_ProfileEdge.data(), kPreviewSamples, 0, "through the flank", 0.0f,
                          1.0f, ImVec2( -1.0f, 90.0f ) );

        ImGui::TextDisabled( "Left is the base at %.2f km, right is the top of the shell at %.2f km "
                             "(%.2f km thick), on a %.2f km lattice.",
                             bottomKm, topKm, spanKm, latticeKm );
        ImGui::TextDisabled( "TWO LINES THROUGH ONE CLOUD: the profile is a distance field now, so the "
                             "core reaches 1 and the flank is whatever the lumps out there leave." );
    }

    void CloudTypePanel::DrawSaveSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Save" );

        // Validated BEFORE the button rather than after it, so an illegal set is refused where the artist
        // is looking and with the number that is wrong in the message. The same function the loader uses,
        // so the panel and the file format cannot disagree about what is legal.
        const auto valid = Assets::ValidateCloudTypeShape( m_Data.Shape );
        if ( !valid )
            ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ), "Not saveable: %s",
                                valid.GetError().c_str() );

        // SAVE WRITES THE SUBJECT. SAVE AS CREATES A SECOND ASSET AND OPENS ITS OWN WINDOW — it does NOT
        // repoint this one, and that is the one place this differs from UE. The subject is this window's
        // identity: its title, its ImGui id and the key open-or-focus matches on are all built from it, so a
        // document that followed a Save As would be a window named after a file it no longer edits, and the
        // next double-click on the original would focus it and show the wrong asset. Making a new type is
        // still exactly what it was — edit, Save As — and now the copy arrives as its own document.
        ImGui::BeginDisabled( !valid || m_SourcePath.empty() );
        const bool save = ImGui::Button( "Save", ImVec2( 100.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( !valid );
        const bool saveAs = ImGui::Button( "Save As...", ImVec2( 120.0f, 0.0f ) );
        ImGui::EndDisabled();

        std::filesystem::path target;
        if ( save )
        {
            target = m_SourcePath;
        }
        else if ( saveAs )
        {
            target = Common::Utils::FileSystem::SaveFileDialog( "Cloud Type\0*.decloudtype\0" );
            if ( !target.empty() && target.extension() != Assets::kCloudTypeExtension )
                target.replace_extension( Assets::kCloudTypeExtension );
        }

        if ( target.empty() )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "Types live in %s", Common::Constants::Path::CLOUD_TYPE_PATH.string().c_str() );
            return;
        }

        // A Save As to a DIFFERENT file makes a new asset, and the new asset gets its own window rather
        // than stealing this one. Compared before the write, because after it the file exists and the two
        // paths would be indistinguishable by anything on disk.
        (void)WriteTo( target, /*isCopy=*/target != m_SourcePath );
    }

    bool CloudTypePanel::WriteTo( const Common::Filepath& target, const bool isCopy )
    {
        // LIFTED OUT OF THE BUTTON so that SaveDocument runs it too. It is the whole sequence and not just
        // the write: the file, the re-registration that makes a layer already pointing at this type show
        // the new numbers this frame, and the re-read that puts the file's own values back in the buffer.
        // Two copies of that sequence is how one of them comes to lack the registration, and the symptom
        // would be an edit that only appears after a restart.
        const auto written = Assets::CloudTypeAsset::Save( target, m_Data );
        if ( !written )
        {
            m_Status        = "Save failed: " + written.GetError();
            m_StatusIsError = true;
            return false;
        }

        // NOT m_SourcePath = target. On a Save As that is a DIFFERENT file, repointing is exactly the
        // defect the immutable subject exists to prevent — see the note above the buttons.
        if ( !isCopy )
            m_SourceName = target.filename().string();

        // Re-registered straight away so a layer already pointing at this type shows the new numbers in
        // the viewport this frame. A tool whose output only appears after a restart is a tool nobody
        // iterates in — and the renderer needs the service's GENERATION to move, which Register is what
        // does.
        if ( m_Assets )
        {
            auto asset = m_Assets->FindByPath<Assets::CloudTypeAsset>( target );
            if ( asset )
                asset->Load(); // overwritten in place: re-read so the cached numbers are the new ones
            else
                asset = m_Assets->CreateAsset<Assets::CloudTypeAsset>( Assets::AssetPriority::Medium, target );

            if ( asset )
            {
                asset->ResolveDependencies( *m_Assets );
                if ( const auto registered = Runtime::ResourceRegistry::GetCloudTypeService()->Register( asset );
                     !registered )
                {
                    m_Status        = "Saved, but the type could not be registered: " + registered.GetError();
                    m_StatusIsError = true;
                    // THE FILE IS ON DISK, so the document IS clean — a registration failure is about the
                    // running sky, not about what was written, and reporting the save as failed would make
                    // "Save All" try again for ever.
                    if ( !isCopy )
                    {
                        m_OnDisk  = m_Data;
                        m_Tracked = true;
                    }
                    return true;
                }

                // Only for the file this document actually edits. Re-reading a COPY's data into this
                // window would be the repointing above by another route.
                if ( !isCopy )
                    m_Data = asset->GetData(); // re-read from the file, so the buffer is what is on disk
            }
        }

        if ( !isCopy )
        {
            // The document is now CLEAN, and it is clean against what the file holds rather than against
            // what was submitted: the re-read above may have normalised a value, and a snapshot of the
            // pre-write buffer would leave the tab dirty for ever over a difference nobody made.
            m_OnDisk  = m_Data;
            m_Tracked = true;
        }

        if ( isCopy )
        {
            // The copy is a new asset, so it gets its own document. Queued rather than constructed here:
            // EditorLayer is the only place that may create a panel, and this is the same wire the asset
            // browser's double-click uses.
            RequestCloudDocument( m_Assets, target.string() );
            m_Status        = "Saved a copy to " + target.string() + " - it has opened in its own window.";
            m_StatusIsError = false;
            return true;
        }

        m_Status        = "Saved to " + target.string();
        m_StatusIsError = false;
        return true;
    }

    bool CloudTypePanel::IsSubjectAlive() const
    {
        // ASKED OF THE METADATA rather than of a typed lookup: the question is whether the asset is still
        // THERE, and a typed lookup answers a different one (whether it is still that type) — a subject
        // that failed to reload as its own class would read as deleted and the window would close on a
        // load error instead of reporting it.
        return m_Assets && m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    // -- WHAT AN EDIT HAS REACHED, AND HOW IT LEAVES -----------------------------------------------

    ISubjectDocument::DiskState CloudTypePanel::GetDiskState() const
    {
        // UNTRACKED IS NOT CLEAN. A window that fell back to the built-in default has no file to be clean
        // against, and drawing "no dot" for it would assert the file is up to date on no evidence.
        if ( !m_Tracked )
            return DiskState::Untracked;

        // Compared against a COPY of what the file held, with a DEFAULTED operator== - so a field added to
        // CloudTypeData tomorrow is compared tomorrow. A dirty FLAG maintained by each edit path is the
        // thing that falls behind, and the cost of it falling behind is a person's unsaved work thrown
        // away by a close that asked nothing.
        return m_Data == m_OnDisk ? DiskState::Clean : DiskState::Dirty;
    }

    bool CloudTypePanel::SaveDocument()
    {
        // A document with no file of its own reports false rather than inventing a path. Save As is a
        // gesture with a dialog behind it and is not what "Save the focused document" means.
        if ( m_SourcePath.empty() )
            return false;

        const auto valid = Assets::ValidateCloudTypeShape( m_Data.Shape );
        if ( !valid )
        {
            // REFUSED OUT LOUD, with the number that is wrong. A save that quietly did nothing would tell
            // "Save All" that this document is on disk when it is not.
            LOG_ERROR( "[Clouds] '{}' cannot be saved: {}", m_SourcePath.string(), valid.GetError() );
            m_Status        = "Not saveable: " + valid.GetError();
            m_StatusIsError = true;
            return false;
        }

        // THE SAME CALL THE BUTTON MAKES. Not a second route to the same bytes: the write, the
        // re-registration that makes the sky show the new numbers, and the re-read that puts the file's
        // own values back in the buffer are one sequence, and a copy of it here would drift the day a step
        // is added to either.
        return WriteTo( m_SourcePath, /*isCopy=*/false );
    }

    std::vector<EditableProperty> CloudTypePanel::EditableProperties() const
    {
        std::vector<EditableProperty> properties;
        properties.reserve( std::size( kShapeFields ) );

        // DERIVED FROM THE SAME TABLE THE SLIDERS READ. The census a client checks the window against and
        // the rows a person sees are one list, in one order, with one set of clamps.
        //
        // THE GROUP IS THE SECTION A ROW IS UNDER, not the one it happens to CARRY. SectionBefore marks
        // where a header is drawn, so only the first row of a section holds the name — and reporting that
        // literally would have put "Detail Factor" back under Shape while the window draws it under
        // "Matter and edge". A client checks the census against the window row by row (EditableProperty::
        // Group says so at its own declaration), so the two must be the same reading of the same table.
        const char* section = "Shape";
        for ( const ShapeField& field : kShapeFields )
        {
            if ( field.SectionBefore )
                section = field.SectionBefore;

            EditableProperty property;
            property.Name       = field.Name;
            property.Label      = field.Label;
            property.Group      = section;
            property.Type       = "float";
            property.Components = 1;
            property.Min        = field.Min;
            property.Max        = field.Max;
            property.Value[0]   = m_Data.Shape.*field.Member;
            properties.push_back( std::move( property ) );
        }

        // THE CURVE IS REPORTED AND REFUSED, not omitted. A property missing from a census reads as a
        // property the format does not have, and those are two different problems (EditableProperty::
        // Settable). Sixteen samples do not fit in a channel that carries four floats, and the honest
        // answer is to say so where a client will read it.
        EditableProperty profile;
        profile.Name       = "Profile";
        profile.Label      = "Vertical profile";
        profile.Group      = "Shape";
        profile.Type       = "curve";
        profile.Components = 1;
        profile.Settable   = false;
        profile.NotSettableReason =
             "'Profile' is a curve of " + std::to_string( Graphic::kCloudProfileSamples ) +
             " samples - the cloud's own silhouette, dragged in the panel. This channel carries at most "
             "four numbers, so it cannot express one.";
        properties.push_back( std::move( profile ) );

        return properties;
    }

    Common::BoolResultStr CloudTypePanel::SetEditableProperty( const std::string&        name,
                                                               const std::vector<float>& value )
    {
        for ( const ShapeField& field : kShapeFields )
        {
            if ( name != field.Name )
                continue;

            // THE COUNT IS PART OF THE PROPERTY'S IDENTITY. Three numbers for a float is a caller who
            // meant a different property, and quietly taking the first would hand them a write they did
            // not ask for.
            if ( value.size() != 1u )
            {
                return Common::MakeFormattedError<bool>(
                     "'{}' is a single number and {} were sent. Ask 'properties' for what this document "
                     "offers and how many components each row takes.",
                     name, value.size() );
            }

            // REFUSED RATHER THAN CLAMPED, on the panel's OWN range: a value silently moved is a value the
            // caller reads back as its own and then cannot explain.
            if ( value[0] < field.Min || value[0] > field.Max )
            {
                return Common::MakeFormattedError<bool>( "'{}' takes {} to {}; {} is outside it.", name, field.Min,
                                                         field.Max, value[0] );
            }

            m_Data.Shape.*field.Member = value[0];
            return BOOLSUCCESS;
        }

        if ( name == "Profile" )
        {
            return Common::MakeFormattedError<bool>(
                 "'Profile' is a curve of {} samples and this channel carries at most four numbers. It is "
                 "dragged in the panel's own profile editor.",
                 Graphic::kCloudProfileSamples );
        }

        return Common::MakeFormattedError<bool>(
             "this cloud type has no property called '{}'. Ask 'properties' for the ones it offers.", name );
    }
} // namespace Desert::Editor
