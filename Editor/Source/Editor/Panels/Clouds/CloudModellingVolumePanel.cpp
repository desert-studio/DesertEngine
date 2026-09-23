#include "CloudModellingVolumePanel.hpp"

#include "CloudDocumentOpen.hpp"

#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudModellingCatalogue.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <ImGui/imgui.h>

#include <chrono>
#include <fstream>

namespace Desert::Editor
{
    // The editor's ImGui lives in the global namespace; unqualified `ImGui::` inside `Desert::` would
    // resolve to `Desert::ImGui`, which is the engine's own runtime UI. Every panel in this folder opens
    // with the same alias for the same reason.
    namespace ImGui = ::ImGui;

    namespace
    {
        // The panel's own UIHelper, created on first use exactly as the noise volume panel's is: the helper
        // caches ImGui texture ids by image view, and a panel that never opens should not build one.
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

        // A new lump lands where the body already is, not at the origin, so that adding one to a sculpted
        // cloud produces something joined to it rather than a bead floating in the corner of the box.
        Assets::CloudModellingBlob NewLumpNear( const Assets::CloudModellingBlob& neighbour )
        {
            Assets::CloudModellingBlob blob;
            blob.CentreKm  = neighbour.CentreKm + glm::vec3( 0.0f, neighbour.RadiiKm.y * 0.8f, 0.0f );
            blob.RadiiKm   = glm::vec3( neighbour.RadiiKm.y * 0.7f );
            blob.Primitive = Assets::CloudModellingPrimitive::Sphere;
            return blob;
        }

        // The document's VISIBLE title: the subject's file name. Computed before the base class is
        // constructed — ISubjectDocument bakes the title in its own constructor and holds it for the
        // window's life — so it is a free function rather than a member.
        std::string SubjectTitle( const Assets::AssetHandle& subject, Assets::AssetManager* assets )
        {
            if ( assets )
            {
                if ( const auto asset = assets->FindByHandle<Assets::CloudModellingVolumeAsset>( subject ) )
                    return asset->GetMetadata().Filepath.filename().string();
            }
            return "Cloud Modelling Volume";
        }
    } // namespace

    CloudModellingVolumePanel::CloudModellingVolumePanel( const Assets::AssetHandle& subject,
                                                          Assets::AssetManager*      assets )
         : ISubjectDocument(
                SubjectTitle( subject, assets ),
                AssetSubject( subject, static_cast<uint32_t>( Assets::AssetTypeID::CloudModellingVolume ) ) ),
           m_Assets( assets )
    {
        // STARTING FROM THE SHIPPED EXAMPLE RATHER THAN FROM AN EMPTY BOX. An empty recipe is refused by
        // Validate (a volume with no lumps is a box, not a cloud), so a panel that opened empty would greet
        // its first user with an error message. The precedent is CloudTypeDefaultShape: a tool that has not
        // been given anything still has to have something to show.
        //
        // Still the fallback, not the norm: LoadSubject overwrites it with the subject's own recipe, and
        // leaves it in place only for a `.dcmv` whose header would not decode.
        m_Recipe     = Assets::CloudModellingDefaultRecipe();
        m_SourceName = "(the shipped example)";

        LoadSubject( assets );
    }

    void CloudModellingVolumePanel::LoadSubject( Assets::AssetManager* assets )
    {
        if ( !assets )
            return;

        const auto asset =
             assets->FindByHandle<Assets::CloudModellingVolumeAsset>( Assets::AssetHandle( Subject().Owner ) );
        if ( !asset )
        {
            m_Status        = "This body is not registered - the log says why. Save would create it anew.";
            m_StatusIsError = true;
            return;
        }

        m_SubjectPath = asset->GetMetadata().Filepath;

        // DECODED FROM THE FILE RATHER THAN FETCHED FROM THE ASSET, and deliberately — the same reason the
        // "Open..." dialog this replaces did it: the RECIPE lives in the file's own header, and the asset
        // exposes the baked voxels, not the lumps they came from. A0 put the recipe there precisely so a
        // body could be revised rather than re-sculpted.
        std::ifstream file( m_SubjectPath, std::ios::binary );
        if ( !file )
        {
            m_Status        = "'" + m_SubjectPath.string() + "' could not be read.";
            m_StatusIsError = true;
            return;
        }

        const std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( file ) ),
                                                std::istreambuf_iterator<char>() );

        const auto decoded = Assets::DecodeCloudModellingVolume( bytes );
        if ( !decoded )
        {
            m_Status = "'" + m_SubjectPath.filename().string() + "' is not usable: " + decoded.GetError() +
                       " - showing the shipped example instead.";
            m_StatusIsError = true;
            return;
        }

        m_Recipe     = decoded.GetValue().Recipe;
        // WHAT THE FILE HOLDS, kept so GetDiskState has something to be clean against. Taken here rather
        // than derived later: after this line the recipe starts being sculpted.
        m_OnDiskRecipe = m_Recipe;
        m_Tracked      = true;
        m_Selected   = m_Recipe.Blobs.empty() ? -1 : 0;
        m_SourceName = m_SubjectPath.filename().string();
        m_SliceIndex = static_cast<int>( Assets::CloudModellingAxisExtent( m_Axis ) / 2u );
        InvalidateSlice();
    }

    CloudModellingVolumePanel::~CloudModellingVolumePanel()
    {
        // CANCEL, THEN WAIT. A bake writes into a std::future this object owns, so the panel must outlive
        // it — but a full volume is still hundreds of milliseconds in a debug build (Г10 gave the z-slabs
        // to the pool: 1383 ms -> 206 ms for the shipped eight-lump body), and waiting even that is an
        // editor that stutters when a window closes. Asking the bake to stop first turns the wait into
        // one slab.
        m_BakeCancelled.store( true );
        if ( m_Baking.valid() )
            m_Baking.wait();
    }

    void CloudModellingVolumePanel::OnUIRender()
    {
        // NO ImGui::Begin HERE, and it is not an omission. EditorLayer's panel loop already wraps
        // OnUIRender in Begin/End for this panel's name, and it owns the p_open bool the title-bar X
        // writes to. A Begin for the SAME name nested inside that one is not appending -- ImGui only
        // supports appending between Begin/End PAIRS -- and the window comes out with its title bar
        // and nothing else. Every panel in this folder had it and drew nothing; no panel outside it
        // does. See CALIBRATION.md §PTP.
        ImGui::TextWrapped(
             "The body of a hero cloud: smooth lumps fused by an exponential smooth minimum, baked into "
             "a 128 x 64 x 128 volume the cloud march reads directly. This is the half of the sky the "
             "procedural field cannot make - its lobes are separated by a zero on every bisector, so "
             "they never merge; these do." );
        ImGui::Separator();

        // Collected before anything is drawn, so the frame a bake ends is already the frame that says
        // so. A future nobody polls is a thread whose result is thrown away at shutdown.
        PollBake();

        DrawRecipeSection();
        ImGui::Separator();
        DrawLumpListSection();
        ImGui::Separator();
        DrawSelectedLumpSection();
        ImGui::Separator();
        DrawPreviewSection();
        ImGui::Separator();
        DrawBakeSection();

        if ( !m_Status.empty() )
        {
            ImGui::Separator();
            if ( m_StatusIsError )
                ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ), "%s", m_Status.c_str() );
            else
                ImGui::TextWrapped( "%s", m_Status.c_str() );
        }
    }

    void CloudModellingVolumePanel::PollBake()
    {
        if ( !m_BakeRunning || !m_Baking.valid() )
            return;

        if ( m_Baking.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return;

        auto result   = m_Baking.get();
        m_BakeRunning = false;

        if ( result )
        {
            StoreBakedVolume( result.ExtractValue() );
        }
        else
        {
            m_Status        = "Bake failed: " + result.GetError();
            m_StatusIsError = true;
        }
    }

    void CloudModellingVolumePanel::StoreBakedVolume( std::vector<unsigned char>&& voxels )
    {
        Assets::CloudModellingVolumeData data;
        data.Recipe = m_BakingRecipe;
        data.Voxels = std::move( voxels );

        const auto written = Assets::CloudModellingVolumeAsset::Save( m_BakeTarget, data );
        if ( !written )
        {
            m_Status        = "Save failed: " + written.GetError();
            m_StatusIsError = true;
            return;
        }

        // A bake aimed somewhere OTHER than the subject is a new asset — it gets its own document, and this
        // window keeps editing the file it was opened on. See DrawBakeSection for why repointing is refused.
        const bool isCopy = m_BakeTarget != m_SubjectPath;

        if ( !isCopy )
        {
            m_SourceName = m_BakeTarget.filename().string();
            // The recipe the bake was started from is what the file's header now carries. m_BakingRecipe
            // and NOT m_Recipe: the artist may have moved a lump while the bake ran, and claiming the
            // document is clean against an edit that is not in the file is the lie this snapshot exists to
            // prevent.
            m_OnDiskRecipe = m_BakingRecipe;
            m_Tracked      = true;
        }
        m_Status        = "Saved to " + m_BakeTarget.string();
        m_StatusIsError = false;

        // Registered straight away so a hero cloud's slot lists it without a restart. A tool whose output
        // only appears after the editor is reopened is a tool nobody iterates in.
        if ( !m_Assets )
            return;

        auto asset = m_Assets->FindByPath<Assets::CloudModellingVolumeAsset>( m_BakeTarget );
        if ( asset )
            asset->Load(); // overwritten in place: re-read so the cached bytes are the new ones
        else
            asset = m_Assets->CreateAsset<Assets::CloudModellingVolumeAsset>( Assets::AssetPriority::Medium,
                                                                              m_BakeTarget );

        if ( !asset )
            return;

        if ( const auto uploaded = Runtime::ResourceRegistry::GetCloudModellingService()->Register( asset );
             !uploaded )
        {
            m_Status        = "Saved, but the volume could not be uploaded: " + uploaded.GetError();
            m_StatusIsError = true;
            return;
        }

        if ( isCopy )
        {
            // Queued rather than constructed here: EditorLayer is the only place that may create a panel,
            // and this is the same wire the asset browser's double-click uses.
            RequestCloudDocument( m_Assets, m_BakeTarget.string() );
            m_Status        = "Saved a copy to " + m_BakeTarget.string() + " - it has opened in its own window.";
            m_StatusIsError = false;
        }
    }

    void CloudModellingVolumePanel::DrawRecipeSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Body" );

        ImGui::BeginDisabled( m_BakeRunning );

        // THE CATALOGUE, AS A STARTING POINT AND NOT AS A LIBRARY. The ten genera of
        // Engine/Assets/CloudModellingCatalogue.hpp land in the lump list where they can be edited, which
        // is the difference between shipping ten finished clouds and shipping ten shapes an artist begins
        // from. Choosing one REPLACES the recipe, so it is behind a combo that commits on selection rather
        // than a button that could be leant on.
        if ( ImGui::BeginCombo( "Catalogue", "Load a genus..." ) )
        {
            for ( uint32_t i = 0; i < Assets::kCloudModellingSpeciesCount; ++i )
            {
                const auto species = static_cast<Assets::CloudModellingSpecies>( i );
                if ( ImGui::Selectable( Assets::CloudModellingSpeciesName( species ) ) )
                {
                    m_Recipe     = Assets::CloudModellingCatalogueRecipe( species );
                    m_Selected   = m_Recipe.Blobs.empty() ? -1 : 0;
                    m_SourceName = Assets::CloudModellingSpeciesName( species );
                    m_Status     = std::string( "Loaded the catalogue's " ) +
                               Assets::CloudModellingSpeciesName( species ) + " - " +
                               std::to_string( m_Recipe.Blobs.size() ) + " lumps. Bake & Save to keep it.";
                    InvalidateSlice();
                }
            }
            ImGui::EndCombo();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The ten genera phase A3 measured the sculpting tool against: humilis, "
                               "mediocris, congestus, cumulonimbus with an anvil, stratocumulus, stratus, "
                               "altocumulus, cirrus, lenticular and a freeform arch. Loading one REPLACES "
                               "everything in this panel." );

        // 16 km and not 8: the catalogue's cirrus is 12 km across, because a fibrous streak is a long thin
        // thing and 128 voxels of it at 8 km would be a rod rather than a sky. A slider whose range cannot
        // reach a shape the engine ships is a slider that is wrong.
        if ( ImGui::DragFloat3( "Size (km)", &m_Recipe.SizeKm.x, 0.02f, 0.1f, 16.0f, "%.2f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The world box the body is sculpted in, before the entity's own scale. The "
                               "volume is always 128 x 64 x 128 voxels, so this is what sets how big a "
                               "voxel is - the short axis is the VERTICAL one, because a cloud is wider "
                               "than it is tall." );

        // THE KNOB THE WHOLE PHASE IS ABOUT, and it is first among the four for that reason.
        if ( ImGui::DragFloat( "Blend Radius (km)", &m_Recipe.BlendRadiusKm, 0.002f, 0.001f, 1.0f, "%.3f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip(
                 "How far apart two lumps can be and still FUSE into one surface. This is the knob the "
                 "authored producer exists for: turn it down and neighbouring lobes stay separate beads, "
                 "which is all the procedural field can ever be; turn it up and they become one convective "
                 "mass, which it cannot be by construction.\n\n"
                 "It also inflates the body by BlendRadius * ln(sum of weights), which is why growing it "
                 "can push the cloud into the wall of its own box." );

        if ( ImGui::DragFloat( "Profile Depth (km)", &m_Recipe.ProfileDepthKm, 0.005f, 0.001f, 2.0f, "%.3f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How deep inside the body the Dimensional Profile reaches 1. The erosion is "
                               "weighted by (1 - profile), so a small value makes a body that is solid "
                               "almost everywhere and a large one makes a body that is edge all the way "
                               "through." );

        if ( ImGui::DragFloat( "Envelope Margin (km)", &m_Recipe.EnvelopeMarginKm, 0.005f, 0.001f, 2.0f, "%.3f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How far outside the body the CUTOUT still holds the procedural field away. "
                               "Too small and procedural cloud grows through the hero cloud's own edge; too "
                               "large and it punches a visible hole in the deck around it." );

        ImGui::EndDisabled();

        // The arithmetic an artist would otherwise do on paper, and the number that decides whether the
        // silhouette can hold the shape they are sculpting.
        ImGui::TextDisabled(
             "Voxel: %.1f x %.1f x %.1f m   |   4.00 MiB per volume",
             m_Recipe.SizeKm.x * 1000.0f / static_cast<float>( Assets::kCloudModellingVolumeWidth ),
             m_Recipe.SizeKm.y * 1000.0f / static_cast<float>( Assets::kCloudModellingVolumeHeight ),
             m_Recipe.SizeKm.z * 1000.0f / static_cast<float>( Assets::kCloudModellingVolumeDepth ) );
    }

    void CloudModellingVolumePanel::DrawLumpListSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Lumps" );

        ImGui::BeginDisabled( m_BakeRunning );

        if ( ImGui::BeginListBox( "##lumps", ImVec2( -1.0f, 120.0f ) ) )
        {
            for ( size_t i = 0; i < m_Recipe.Blobs.size(); ++i )
            {
                const Assets::CloudModellingBlob& blob = m_Recipe.Blobs[i];

                char label[128];
                std::snprintf( label, sizeof( label ), "%zu  %-9s  (%.2f, %.2f, %.2f) km##lump%zu", i,
                               Assets::CloudModellingPrimitiveName( blob.Primitive ), blob.CentreKm.x,
                               blob.CentreKm.y, blob.CentreKm.z, i );

                if ( ImGui::Selectable( label, m_Selected == static_cast<int>( i ) ) )
                    m_Selected = static_cast<int>( i );
            }
            ImGui::EndListBox();
        }

        const bool hasSelection = m_Selected >= 0 && m_Selected < static_cast<int>( m_Recipe.Blobs.size() );

        if ( ImGui::Button( "Add" ) )
        {
            const Assets::CloudModellingBlob seed =
                 hasSelection ? m_Recipe.Blobs[static_cast<size_t>( m_Selected )] : Assets::CloudModellingBlob{};

            m_Recipe.Blobs.push_back( m_Recipe.Blobs.empty() ? Assets::CloudModellingBlob{}
                                                             : NewLumpNear( seed ) );
            m_Selected = static_cast<int>( m_Recipe.Blobs.size() ) - 1;
            InvalidateSlice();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A new sphere, placed on top of the selected lump so that the join has "
                               "something to fuse it to." );

        ImGui::SameLine();
        ImGui::BeginDisabled( !hasSelection );
        if ( ImGui::Button( "Duplicate" ) )
        {
            m_Recipe.Blobs.push_back( m_Recipe.Blobs[static_cast<size_t>( m_Selected )] );
            m_Selected = static_cast<int>( m_Recipe.Blobs.size() ) - 1;
            InvalidateSlice();
        }

        ImGui::SameLine();

        // The last lump cannot be removed: a recipe with none is refused by Validate, and a panel that can
        // put itself into an unbakeable state is one an artist has to know how to rescue.
        ImGui::BeginDisabled( m_Recipe.Blobs.size() <= 1u );
        if ( ImGui::Button( "Delete" ) )
        {
            m_Recipe.Blobs.erase( m_Recipe.Blobs.begin() + m_Selected );
            m_Selected = m_Selected >= static_cast<int>( m_Recipe.Blobs.size() )
                              ? static_cast<int>( m_Recipe.Blobs.size() ) - 1
                              : m_Selected;
            InvalidateSlice();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextDisabled( "%zu of 64", m_Recipe.Blobs.size() );

        ImGui::EndDisabled();
    }

    void CloudModellingVolumePanel::ConformRadiiToPrimitive( Assets::CloudModellingBlob& blob )
    {
        // The panel keeps every lump legal AS IT IS EDITED. ValidateCloudModellingRecipe refuses a sphere
        // whose radii disagree and a capsule whose cross-section is not round; those refusals exist for
        // hand-written and machine-generated files, and an artist should never be able to reach one by
        // dragging a slider.
        switch ( blob.Primitive )
        {
            case Assets::CloudModellingPrimitive::Sphere:
                blob.RadiiKm.y = blob.RadiiKm.x;
                blob.RadiiKm.z = blob.RadiiKm.x;
                break;

            case Assets::CloudModellingPrimitive::Capsule:
                blob.RadiiKm.z = blob.RadiiKm.x;
                blob.RadiiKm.y = std::max( blob.RadiiKm.y, blob.RadiiKm.x );
                break;

            case Assets::CloudModellingPrimitive::Ellipsoid:
                break;
        }
    }

    void CloudModellingVolumePanel::DrawSelectedLumpSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Selected lump" );

        if ( m_Selected < 0 || m_Selected >= static_cast<int>( m_Recipe.Blobs.size() ) )
        {
            ImGui::TextDisabled( "Select a lump above to edit it." );
            return;
        }

        Assets::CloudModellingBlob& blob = m_Recipe.Blobs[static_cast<size_t>( m_Selected )];

        ImGui::BeginDisabled( m_BakeRunning );

        int primitive = static_cast<int>( blob.Primitive );
        if ( ImGui::Combo( "Primitive", &primitive,
                           "Ellipsoid  (three free semi-axes)\0"
                           "Sphere     (one radius)\0"
                           "Capsule    (a swept sphere along local Y)\0" ) )
        {
            blob.Primitive = static_cast<Assets::CloudModellingPrimitive>( primitive );
            ConformRadiiToPrimitive( blob );
            InvalidateSlice();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A capsule holds its cross-section along its length where an ellipsoid "
                               "tapers to a point - which is what a spreading cumulus base and an "
                               "elongated growth actually are. Rotate it to lay it down." );

        if ( ImGui::DragFloat3( "Centre (km)", &blob.CentreKm.x, 0.005f, -4.0f, 4.0f, "%.3f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Relative to the CENTRE of the box, so a recipe describes a cloud rather "
                               "than a place. The entity's transform is what puts it in the sky." );

        // The size widgets differ per primitive so that the constraint is unreachable rather than merely
        // enforced. One number for a sphere, two for a capsule, three for an ellipsoid.
        switch ( blob.Primitive )
        {
            case Assets::CloudModellingPrimitive::Sphere:
                if ( ImGui::DragFloat( "Radius (km)", &blob.RadiiKm.x, 0.005f, 0.005f, 4.0f, "%.3f" ) )
                {
                    ConformRadiiToPrimitive( blob );
                    InvalidateSlice();
                }
                break;

            case Assets::CloudModellingPrimitive::Capsule:
                if ( ImGui::DragFloat( "Radius (km)", &blob.RadiiKm.x, 0.005f, 0.005f, 4.0f, "%.3f" ) )
                {
                    ConformRadiiToPrimitive( blob );
                    InvalidateSlice();
                }
                if ( ImGui::DragFloat( "Half-height (km)", &blob.RadiiKm.y, 0.005f, 0.005f, 4.0f, "%.3f" ) )
                {
                    ConformRadiiToPrimitive( blob );
                    InvalidateSlice();
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "The TOTAL half-height, caps included. The straight section is this "
                                       "less the radius, so a capsule cannot be shorter than it is wide." );
                break;

            case Assets::CloudModellingPrimitive::Ellipsoid:
                if ( ImGui::DragFloat3( "Semi-axes (km)", &blob.RadiiKm.x, 0.005f, 0.005f, 4.0f, "%.3f" ) )
                    InvalidateSlice();
                break;
        }

        if ( ImGui::DragFloat3( "Rotation (deg)", &blob.RotationDeg.x, 1.0f, -180.0f, 180.0f, "%.1f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "About the lump's own centre, in the same euler convention as an entity's "
                               "transform. A rotation is rigid, so it moves the surface without distorting "
                               "the distance field the Dimensional Profile is read from." );

        if ( ImGui::DragFloat( "Weight", &blob.Weight, 0.01f, 0.125f, 8.0f, "%.3f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How hard this lump pulls in the union. It is exactly a dilation of "
                               "BlendRadius * ln(weight) - at the current blend radius, a weight of 2 grows "
                               "this lump by about %.0f m and a weight of 0.5 shrinks it by the same. Use "
                               "it to move the CREASE between neighbours without moving either centre.",
                               m_Recipe.BlendRadiusKm * 0.6931f * 1000.0f );

        if ( ImGui::SliderFloat( "Detail Type", &blob.DetailType, 0.0f, 1.0f, "%.2f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "0 wispy, 1 billowy - which erosion the up-rez noise cuts into this lump's "
                               "part of the body. The join spreads it across the creases for free, using "
                               "the same weights that fused the shapes." );

        if ( ImGui::SliderFloat( "Density Scale", &blob.DensityScale, 0.0f, 1.0f, "%.2f" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "How much matter this lump carries, relative to the instance's own Density "
                               "Factor. A wispy tail is thinner than the core it grew from." );

        ImGui::EndDisabled();
    }

    void CloudModellingVolumePanel::RefreshSlice()
    {
        m_SliceDirty = false;

        const uint32_t extent = Assets::CloudModellingAxisExtent( m_Axis );
        const uint32_t index =
             static_cast<uint32_t>( std::clamp( m_SliceIndex, 0, static_cast<int>( extent ) - 1 ) );

        // ONE PLANE, NOT THE VOLUME. This is what makes the preview live: 128 x 64 voxels against 128 x 64
        // x 128, i.e. 1/128 of the bake, from the same evaluator the bake uses.
        const auto plane = Assets::GenerateCloudModellingSlice( m_Recipe, m_Axis, index );
        if ( !plane )
        {
            // Not an error banner: an illegal recipe already has its reason printed beside the Bake button,
            // and a second copy of it under the preview would be the same fault reported twice.
            m_SliceImage.reset();
            m_SliceImageWidth  = 0;
            m_SliceImageHeight = 0;
            return;
        }

        const Assets::CloudModellingSlice& slice = plane.GetValue();

        std::vector<unsigned char> pixels( static_cast<size_t>( slice.Width ) * slice.Height * 4u, 0u );
        for ( size_t i = 0; i < pixels.size(); i += 4u )
        {
            const unsigned char profile      = slice.Pixels[i + 0];
            const unsigned char detailType   = slice.Pixels[i + 1];
            const unsigned char densityScale = slice.Pixels[i + 2];
            const unsigned char envelope     = slice.Pixels[i + 3];

            if ( m_ChannelView == ChannelView::AllFour )
            {
                if ( profile > 0u )
                {
                    pixels[i + 0] = profile;
                    pixels[i + 1] = detailType;
                    pixels[i + 2] = densityScale;
                }
                else
                {
                    // The cutout shell, dimmed, so the halo that keeps the procedural field away is visible
                    // as something distinct from the body rather than as more body.
                    const unsigned char shell = static_cast<unsigned char>( envelope / 4u );
                    pixels[i + 0]             = shell;
                    pixels[i + 1]             = shell;
                    pixels[i + 2]             = shell;
                }
            }
            else
            {
                const size_t        channel = static_cast<size_t>( m_ChannelView ) - 1u;
                const unsigned char value   = slice.Pixels[i + channel];
                pixels[i + 0]               = value;
                pixels[i + 1]               = value;
                pixels[i + 2]               = value;
            }

            // The alpha is folded into the picture rather than left to the compositor: an image drawn with
            // a real alpha would show the ImGui window through it, which reads as the volume being empty
            // exactly where it is densest.
            pixels[i + 3] = 255u;
        }

        // RE-UPLOADED RATHER THAN RE-CREATED WHENEVER THE SHAPE IS UNCHANGED, which is every frame of a
        // slider drag. Creating a device image per frame is an allocation and a descriptor per frame for a
        // picture whose dimensions did not move; SetData exists for exactly this and keeps the cached
        // ImGui texture id valid.
        if ( m_SliceImage && m_SliceImageWidth == slice.Width && m_SliceImageHeight == slice.Height )
        {
            if ( const auto uploaded = m_SliceImage->SetData( pixels ); uploaded )
                return;

            // Fall through to a fresh image: a failed re-upload leaves the old picture on screen, and a
            // stale preview is the one thing this panel must never show.
            m_SliceImage.reset();
        }

        const Core::Formats::Image2DSpecification spec{
             .Tag        = "CloudModellingSlice",
             .Width      = slice.Width,
             .Height     = slice.Height,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Mips       = 1,
             .Data       = std::move( pixels ),
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Sample,
        };

        m_SliceImage       = Graphic::Image2D::Create( spec );
        m_SliceImageWidth  = slice.Width;
        m_SliceImageHeight = slice.Height;

        if ( !m_SliceImage )
        {
            m_SliceImageWidth  = 0;
            m_SliceImageHeight = 0;
            m_Status           = "The slice preview image could not be created on the device.";
            m_StatusIsError    = true;
        }
    }

    void CloudModellingVolumePanel::DrawPreviewSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Preview" );

        int axis = static_cast<int>( m_Axis );
        if ( ImGui::Combo( "Axis", &axis,
                           "X  (looking east along the body)\0"
                           "Y  (the horizontal cut, looking down)\0"
                           "Z  (looking north along the body)\0" ) )
        {
            m_Axis = static_cast<Assets::CloudModellingAxis>( axis );

            // Re-centred rather than clamped: the axes have different depths (64 up, 128 across), so a
            // slice index carried over from another axis lands somewhere arbitrary.
            m_SliceIndex = static_cast<int>( Assets::CloudModellingAxisExtent( m_Axis ) / 2u );
            InvalidateSlice();
        }

        const int extent = static_cast<int>( Assets::CloudModellingAxisExtent( m_Axis ) );
        if ( ImGui::SliderInt( "Slice", &m_SliceIndex, 0, extent - 1, "%d" ) )
            InvalidateSlice();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "A 3D body has no picture, only slices. Scrub this to check that the lumps "
                               "fuse the whole way through - two lobes can meet in the middle slice and "
                               "still be separate above and below it." );

        int view = static_cast<int>( m_ChannelView );
        if ( ImGui::Combo( "Channels", &view,
                           "All four\0"
                           "R  Dimensional Profile (depth inside the body)\0"
                           "G  Detail Type (0 wispy, 1 billowy)\0"
                           "B  Density Scale (per-voxel multiplier)\0"
                           "A  Cutout Envelope (the body, dilated)\0" ) )
        {
            m_ChannelView = static_cast<ChannelView>( view );
            InvalidateSlice();
        }

        ImGui::SliderInt( "Zoom", &m_PreviewZoom, 1, 6, "%dx" );

        if ( m_SliceDirty )
            RefreshSlice();

        if ( m_SliceImage )
        {
            const float zoom = static_cast<float>( m_PreviewZoom );
            SlicePreviewHelper()->Image( m_SliceImage, ImVec2( static_cast<float>( m_SliceImageWidth ) * zoom,
                                                               static_cast<float>( m_SliceImageHeight ) * zoom ) );
        }
        else
        {
            ImGui::TextDisabled( "No preview: the recipe below is not bakeable yet." );
        }

        ImGui::TextDisabled( "%s - slice %d of %d", Assets::CloudModellingAxisName( m_Axis ), m_SliceIndex,
                             extent );
    }

    void CloudModellingVolumePanel::DrawBakeSection()
    {
        Utils::ImGuiUtilities::SectionHeader( "Bake" );

        ImGui::Text( "Editing: %s", m_SourceName.c_str() );

        // THE SAME VALIDATION THE LOADER RUNS, so the button and the file agree about what is legal — and
        // the artist is told which number is wrong rather than being handed a disabled button.
        const auto valid = Assets::ValidateCloudModellingRecipe( m_Recipe );
        if ( !valid )
            ImGui::TextColored( ImVec4( 0.95f, 0.45f, 0.40f, 1.0f ), "%s", valid.GetError().c_str() );

        // BAKE & SAVE WRITES THE SUBJECT. BAKE & SAVE AS CREATES A SECOND ASSET AND OPENS ITS OWN WINDOW —
        // it does NOT repoint this one. The subject is this window's identity: its title, its ImGui id and
        // the key open-or-focus matches on are all built from it, so a document that followed a Save As
        // would be a window named after a file it no longer edits. Sculpting a variant and keeping it is
        // still exactly what it was, and now the variant arrives as its own document.
        ImGui::BeginDisabled( m_BakeRunning || !valid || m_SubjectPath.empty() );
        const bool bakeOverSubject = ImGui::Button( "Bake & Save", ImVec2( 160.0f, 0.0f ) );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( m_BakeRunning || !valid );
        const bool bakeAs = ImGui::Button( "Bake & Save As...", ImVec2( 180.0f, 0.0f ) );
        ImGui::EndDisabled();

        std::filesystem::path chosen;
        if ( bakeOverSubject )
        {
            chosen = m_SubjectPath;
        }
        else if ( bakeAs )
        {
            chosen = Common::Utils::FileSystem::SaveFileDialog( "Cloud Modelling Volume\0*.dcmv\0" );
            if ( !chosen.empty() && chosen.extension() != Assets::kCloudModellingVolumeExtension )
                chosen.replace_extension( Assets::kCloudModellingVolumeExtension );
        }

        if ( !chosen.empty() )
        {
            // THE PATH IS CHOSEN BEFORE THE BAKE STARTS, not after it finishes. A file dialog that appears
            // at the end of a thirty-second wait is one the artist has walked away from.
            m_BakeTarget = chosen;

            // SNAPSHOT. Everything the worker reads is copied here, on the UI thread, so that editing
            // while a bake runs cannot make the voxels disagree with the recipe written beside them in
            // the same file.
            m_BakingRecipe = m_Recipe;

            m_BakeProgress.store( 0.0f );
            m_BakeCancelled.store( false );
            m_BakeRunning   = true;
            m_Status        = "Baking " + m_BakeTarget.filename().string() + "...";
            m_StatusIsError = false;

            // The two atomics are the ONLY channel between the worker and the UI thread: one number
            // out, one flag in. Nothing else the worker touches is shared, which is what makes the
            // bake safe to run beside a panel the artist is still typing into.
            const Assets::CloudModellingVolumeRecipe   recipe     = m_BakingRecipe;
            const Assets::CloudModellingBakeProgressFn onProgress = [this]( float fraction )
            {
                m_BakeProgress.store( fraction );
                return !m_BakeCancelled.load();
            };

            // ON THE ENGINE'S POOL, NOT ON A THREAD OF ITS OWN (Г9). `std::async` here was invisible to the
            // profiler, unbounded in thread count — every open sculpting document could start one, and
            // nothing counted them against the machine — and it is the same defect the cloud renderer's own
            // bake had. `DESERT_PROFILE_SCOPE` puts this bake in Optick and in the editor's Profiler panel,
            // which is where its cost belongs rather than in a log line somebody has to correlate by hand.
            //
            // THE DESTRUCTOR STILL WAITS, and must: `onProgress` captures `this`. What the pool changes is
            // where the thread comes from, not who owns the panel.
            m_Baking = Common::JobSystem::Get().Async(
                 [recipe, onProgress]
                 {
                     DESERT_PROFILE_SCOPE( "Clouds: Sculpted volume bake" );
                     return Assets::GenerateCloudModellingVolume( recipe, onProgress );
                 } );
        }

        if ( m_BakeRunning )
        {
            ImGui::SameLine();
            ImGui::ProgressBar( m_BakeProgress.load(), ImVec2( -80.0f, 0.0f ) );
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel" ) )
            {
                m_BakeCancelled.store( true );
                m_Status        = "Cancelling the bake...";
                m_StatusIsError = false;
            }
        }

        // THE "OPEN..." DIALOG THAT WAS HERE IS GONE, and its absence is the feature. Opening the recipe
        // out of a `.dcmv` header is still what makes the format revisable — it now happens once, in
        // LoadSubject, against the file this window was opened ON. A different body is a different window,
        // opened by double-clicking it in the asset browser; a dialog that could repoint this one would
        // leave a window titled after a file it no longer edits.

        ImGui::SameLine();
        ImGui::TextDisabled( "Bodies live in %s", Common::Constants::Path::CLOUD_VOLUME_PATH.string().c_str() );
    }

    // -- WHAT AN EDIT HAS REACHED, AND HOW IT LEAVES -----------------------------------------------

    ISubjectDocument::DiskState CloudModellingVolumePanel::GetDiskState() const
    {
        if ( !m_Tracked )
            return DiskState::Untracked;
        return m_Recipe == m_OnDiskRecipe ? DiskState::Clean : DiskState::Dirty;
    }

    bool CloudModellingVolumePanel::SaveDocument()
    {
        if ( m_SubjectPath.empty() )
            return false;

        // A BAKE ALREADY RUNNING IS NOT A REASON TO START A SECOND ONE, and it is not a save either. Said
        // out loud: a caller that got `false` here and one that got it from a validation failure need to
        // be able to tell the two apart, and the log line is where that difference lives.
        if ( m_BakeRunning )
        {
            LOG_WARN( "[Clouds] '{}' is baking already; the save was not started. Ask again once it has "
                      "finished.",
                      m_SubjectPath.string() );
            return false;
        }

        // Validated with the SAME function the panel's own button uses, so a recipe the button refuses is
        // one this refuses, with the same words.
        if ( const auto valid = Assets::ValidateCloudModellingRecipe( m_Recipe ); !valid )
        {
            LOG_ERROR( "[Clouds] '{}' cannot be saved: {}", m_SubjectPath.string(), valid.GetError() );
            m_Status        = "Not saveable: " + valid.GetError();
            m_StatusIsError = true;
            return false;
        }

        // SYNCHRONOUS, and the header says why. The generator is the same one the pooled bake calls, so a
        // volume written from here is byte for byte the volume the button writes.
        auto voxels = Assets::GenerateCloudModellingVolume( m_Recipe );
        if ( !voxels )
        {
            LOG_ERROR( "[Clouds] '{}' could not be baked: {}", m_SubjectPath.string(), voxels.GetError() );
            m_Status        = "Bake failed: " + voxels.GetError();
            m_StatusIsError = true;
            return false;
        }

        // StoreBakedVolume writes to m_BakeTarget and reads m_BakingRecipe, exactly as it does for the
        // pooled bake — set here so that the one function that knows how to write and register a volume is
        // the one that does it.
        m_BakeTarget   = m_SubjectPath;
        m_BakingRecipe = m_Recipe;
        StoreBakedVolume( voxels.ExtractValue() );

        return !m_StatusIsError;
    }

    // -- THE NUMBERS A CLIENT CAN DRAG ---------------------------------------------------------------
    //
    // DERIVED FROM THE RECIPE, never listed: the body-level numbers, then one group per lump, addressed
    // `Lump0.CentreKm` and so on. A body with three lumps offers three groups and one with nine offers
    // nine, so the census cannot fall behind a sculpt.
    //
    // The index is in the NAME rather than taken from the panel's selection, because a property whose
    // meaning depends on which row is highlighted is a property a client cannot address twice and get the
    // same thing.

    namespace
    {
        struct BodyField
        {
            const char* Name;
            const char* Label;
            float Assets::CloudModellingVolumeRecipe::*Member;
            float                                      Min;
            float                                      Max;
        };

        constexpr BodyField kBodyFields[] = {
             { "BlendRadiusKm", "Blend Radius (km)", &Assets::CloudModellingVolumeRecipe::BlendRadiusKm, 0.001f,
               1.0f },
             { "ProfileDepthKm", "Profile Depth (km)", &Assets::CloudModellingVolumeRecipe::ProfileDepthKm, 0.001f,
               4.0f },
             { "EnvelopeMarginKm", "Envelope Margin (km)", &Assets::CloudModellingVolumeRecipe::EnvelopeMarginKm,
               0.0f, 4.0f },
        };

        struct LumpField
        {
            const char* Suffix;
            const char* Label;
            float Assets::CloudModellingBlob::*Member;
            float                              Min;
            float                              Max;
        };

        constexpr LumpField kLumpScalars[] = {
             { ".Weight", "Weight", &Assets::CloudModellingBlob::Weight, 0.125f, 8.0f },
             { ".DetailType", "Detail Type", &Assets::CloudModellingBlob::DetailType, 0.0f, 1.0f },
             { ".DensityScale", "Density Scale", &Assets::CloudModellingBlob::DensityScale, 0.0f, 1.0f },
        };

        struct LumpVector
        {
            const char* Suffix;
            const char* Label;
            glm::vec3 Assets::CloudModellingBlob::*Member;
            float                                  Min;
            float                                  Max;
        };

        constexpr LumpVector kLumpVectors[] = {
             { ".CentreKm", "Centre (km)", &Assets::CloudModellingBlob::CentreKm, -8.0f, 8.0f },
             { ".RadiiKm", "Radii (km)", &Assets::CloudModellingBlob::RadiiKm, 0.001f, 8.0f },
             { ".RotationDeg", "Rotation (deg)", &Assets::CloudModellingBlob::RotationDeg, -360.0f, 360.0f },
        };
    } // namespace

    std::vector<EditableProperty> CloudModellingVolumePanel::EditableProperties() const
    {
        std::vector<EditableProperty> properties;

        const auto scalar =
             [&]( const char* name, const char* label, const char* group, float value, float min, float max )
        {
            EditableProperty property;
            property.Name       = name;
            property.Label      = label;
            property.Group      = group;
            property.Type       = "float";
            property.Components = 1;
            property.Min        = min;
            property.Max        = max;
            property.Value[0]   = value;
            properties.push_back( std::move( property ) );
        };

        const auto vector3 = [&]( const std::string& name, const char* label, const std::string& group,
                                  const glm::vec3& value, float min, float max )
        {
            EditableProperty property;
            property.Name       = name;
            property.Label      = label;
            property.Group      = group;
            property.Type       = "float3";
            property.Components = 3;
            property.Min        = min;
            property.Max        = max;
            property.Value[0]   = value.x;
            property.Value[1]   = value.y;
            property.Value[2]   = value.z;
            properties.push_back( std::move( property ) );
        };

        vector3( "SizeKm", "Size (km)", "Body", m_Recipe.SizeKm, 0.05f, 32.0f );
        for ( const BodyField& field : kBodyFields )
            scalar( field.Name, field.Label, "Body", m_Recipe.*field.Member, field.Min, field.Max );

        for ( std::size_t i = 0; i < m_Recipe.Blobs.size(); ++i )
        {
            const std::string prefix = "Lump" + std::to_string( i );
            const std::string group  = "Lump " + std::to_string( i );
            const auto&       blob   = m_Recipe.Blobs[i];

            for ( const LumpVector& field : kLumpVectors )
                vector3( prefix + field.Suffix, field.Label, group, blob.*field.Member, field.Min, field.Max );
            for ( const LumpField& field : kLumpScalars )
            {
                EditableProperty property;
                property.Name       = prefix + field.Suffix;
                property.Label      = field.Label;
                property.Group      = group;
                property.Type       = "float";
                property.Components = 1;
                property.Min        = field.Min;
                property.Max        = field.Max;
                property.Value[0]   = blob.*field.Member;
                properties.push_back( std::move( property ) );
            }

            // THE PRIMITIVE IS REPORTED AND REFUSED. It is a KIND and not a number — a sphere, a capsule
            // and an ellipsoid constrain their radii differently (ConformRadiiToPrimitive), so writing one
            // as an index would silently rewrite two other fields.
            EditableProperty primitive;
            primitive.Name       = prefix + ".Primitive";
            primitive.Label      = "Primitive";
            primitive.Group      = group;
            primitive.Type       = "enum";
            primitive.Components = 1;
            primitive.Value[0]   = static_cast<float>( blob.Primitive );
            primitive.Settable   = false;
            primitive.NotSettableReason =
                 "'" + primitive.Name +
                 "' is which SOLID the lump is, and each of the three constrains its radii differently - a "
                 "sphere's three agree, a capsule's cross-section is round. Setting it as a number would "
                 "quietly rewrite the radii too; it is a dropdown in the panel.";
            properties.push_back( std::move( primitive ) );
        }

        return properties;
    }

    Common::BoolResultStr CloudModellingVolumePanel::SetEditableProperty( const std::string&        name,
                                                                          const std::vector<float>& value )
    {
        // The census is the authority on what exists and how many components each row takes; checking
        // against it rather than against a second list here is what keeps the two from parting company.
        const std::vector<EditableProperty> census = EditableProperties();
        const EditableProperty*             row    = nullptr;
        for ( const EditableProperty& property : census )
            if ( property.Name == name )
                row = &property;

        if ( !row )
        {
            return Common::MakeFormattedError<bool>(
                 "this sculpted body has no property called '{}'. Ask 'properties' for the ones it offers - "
                 "a lump's rows are addressed 'Lump0.CentreKm' and so on.",
                 name );
        }
        if ( !row->Settable )
            return Common::MakeError<bool>( row->NotSettableReason );

        if ( static_cast<int>( value.size() ) != row->Components )
        {
            return Common::MakeFormattedError<bool>( "'{}' takes {} number(s) and {} were sent.", name,
                                                     row->Components, value.size() );
        }
        for ( const float component : value )
        {
            if ( ( row->Min && component < *row->Min ) || ( row->Max && component > *row->Max ) )
            {
                return Common::MakeFormattedError<bool>( "'{}' takes {} to {}; {} is outside it.", name,
                                                         row->Min.value_or( 0.0f ), row->Max.value_or( 0.0f ),
                                                         component );
            }
        }

        // ── THE WRITE GOES THROUGH THE SAME REPAIR THE WIDGET DOES ────────────────────────────────────
        //
        // A lump's radii are constrained by its primitive, and the panel keeps every lump legal AS IT IS
        // EDITED so that the validator's refusals are a guard against hand-edited files rather than
        // something an artist meets by moving a slider. A channel that skipped that step could write a
        // recipe the panel itself would never produce.
        if ( name == "SizeKm" )
        {
            m_Recipe.SizeKm = glm::vec3( value[0], value[1], value[2] );
            InvalidateSlice();
            return BOOLSUCCESS;
        }
        for ( const BodyField& field : kBodyFields )
        {
            if ( name != field.Name )
                continue;
            m_Recipe.*field.Member = value[0];
            InvalidateSlice();
            return BOOLSUCCESS;
        }

        const std::size_t dot = name.find( '.' );
        if ( dot == std::string::npos || name.rfind( "Lump", 0 ) != 0 )
            return Common::MakeFormattedError<bool>( "'{}' is not a name this document writes.", name );

        const std::size_t index = static_cast<std::size_t>( std::stoul( name.substr( 4, dot - 4 ) ) );
        if ( index >= m_Recipe.Blobs.size() )
            return Common::MakeFormattedError<bool>( "this body has no lump {}.", index );

        Assets::CloudModellingBlob& blob   = m_Recipe.Blobs[index];
        const std::string           suffix = name.substr( dot );

        for ( const LumpVector& field : kLumpVectors )
        {
            if ( suffix != field.Suffix )
                continue;
            blob.*field.Member = glm::vec3( value[0], value[1], value[2] );
            ConformRadiiToPrimitive( blob );
            InvalidateSlice();
            return BOOLSUCCESS;
        }
        for ( const LumpField& field : kLumpScalars )
        {
            if ( suffix != field.Suffix )
                continue;
            blob.*field.Member = value[0];
            InvalidateSlice();
            return BOOLSUCCESS;
        }

        return Common::MakeFormattedError<bool>( "'{}' is not a name this document writes.", name );
    }

    bool CloudModellingVolumePanel::IsSubjectAlive() const
    {
        // ASKED OF THE METADATA rather than of a typed lookup: the question is whether the asset is still
        // THERE, and a typed lookup answers a different one (whether it is still that type) — a subject
        // that failed to reload as its own class would read as deleted and the window would close on a
        // load error instead of reporting it.
        return m_Assets && m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }
} // namespace Desert::Editor
