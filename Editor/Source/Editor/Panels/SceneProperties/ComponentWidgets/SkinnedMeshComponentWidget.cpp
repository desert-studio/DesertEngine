#include "SkinnedMeshComponentWidget.hpp"

#include <ImGui/imgui.h>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/Selection/SkeletonEditMode.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include "Helper/MeshDetailsWidget.hpp"
#include "MaterialsPanelComponent.hpp"

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>

#include <functional>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // What the vertex weights say about a rig. Real, checkable problems only — the engine has no
        // fixed bone cap to warn about (the pose lives in a storage buffer that grows on demand), but a
        // vertex no bone moves, or one pointing past the end of the skeleton, is a genuine bug.
        struct SkinningAudit
        {
            uint64_t Unweighted      = 0; // no influence at all -> the vertex stays in bind pose
            uint64_t OutOfRange      = 0; // references a bone index the skeleton does not have
            uint64_t FullyInfluenced = 0; // uses all 4 slots -> the importer may have dropped weights
        };

        // Scanning every vertex per UI frame would be silly, and the answer only changes when the mesh
        // does: cache it against (mesh, vertex count, bone count). Editor UI is single-threaded.
        const SkinningAudit& AuditSkinning( const SkinnedMesh& mesh, size_t boneCount )
        {
            static const void*   cachedMesh  = nullptr;
            static size_t        cachedVerts = 0;
            static size_t        cachedBones = 0;
            static SkinningAudit cached;

            const auto& vertices = mesh.GetVertices();
            if ( cachedMesh == &mesh && cachedVerts == vertices.size() && cachedBones == boneCount )
                return cached;

            cached = {};
            for ( const auto& v : vertices )
            {
                float  weight   = 0.0f;
                size_t active   = 0;
                bool   outRange = false;
                for ( size_t i = 0; i < SkinnedVertex::MAX_BONE_INFLUENCES; ++i )
                {
                    if ( v.BoneWeights[i] <= 0.0f )
                        continue;
                    weight += v.BoneWeights[i];
                    ++active;
                    if ( v.BoneIDs[i] >= boneCount )
                        outRange = true;
                }
                if ( weight <= 0.0f )
                    ++cached.Unweighted;
                if ( outRange )
                    ++cached.OutOfRange;
                if ( active == SkinnedVertex::MAX_BONE_INFLUENCES )
                    ++cached.FullyInfluenced;
            }

            cachedMesh  = &mesh;
            cachedVerts = vertices.size();
            cachedBones = boneCount;
            return cached;
        }

        // UE's asset-type colour for a skeletal mesh, sampled off the reference: the bar under the slot's
        // preview says WHAT KIND of asset the slot takes, before you have read a single word of the name.
        constexpr ImU32 kSkeletalMeshTint = IM_COL32( 241, 163, 241, 255 );

        // The framed preview box beside an asset slot. This panel renders nothing offscreen (one preview
        // renderer per panel, and it belongs to the panel, not to a component row), so the box carries the
        // asset's GLYPH rather than a fake render — and the type bar underneath when the slot is filled.
        void DrawAssetBox( float size, const char* icon, bool filled, ImU32 tint )
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy( ImVec2( size, size ) );

            const ImVec2 br( at.x + size, at.y + size );
            ImDrawList*  dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled( at, br, IM_COL32( 15, 15, 15, 255 ), 2.0f );
            dl->AddRect( at, br, ImGui::GetColorU32( ImGuiCol_Border ), 2.0f );

            const ImVec2 ts = ImGui::CalcTextSize( icon );
            dl->AddText( ImVec2( at.x + ( size - ts.x ) * 0.5f, at.y + ( size - ts.y ) * 0.5f ),
                         ImGui::GetColorU32( filled ? ImGuiCol_Text : ImGuiCol_TextDisabled ), icon );

            if ( filled )
                dl->AddRectFilled( ImVec2( at.x + 1.0f, br.y - 3.0f ), ImVec2( br.x - 1.0f, br.y ), tint );
        }
    } // namespace

    SkinnedMeshComponentWidget::SkinnedMeshComponentWidget(
         const std::weak_ptr<Assets::AssetManager>& assetManager )
         : IComponentWidget( "Skinned Mesh" ), m_AssetManager( assetManager )
    {
    }

    void SkinnedMeshComponentWidget::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene )
    {
        auto& skinnedMesh  = entity.GetComponent<ECS::SkinnedMeshComponent>();
        auto  assetManager = m_AssetManager.lock();
        if ( !assetManager )
            return;

        Utils::ImGuiUtilities::PushID();

        auto meshAssets = assetManager->FindAllByType<Assets::MeshAsset>();

        // The mesh that is ACTUALLY drawn: an in-editor rig (Convert to Skinned) overrides the asset.
        ::Desert::Mesh* mesh = skinnedMesh.RuntimeMesh.get();
        if ( !mesh && skinnedMesh.MeshHandle )
            mesh = Runtime::ResourceRegistry::GetMeshService()->Get( skinnedMesh.MeshHandle );

        // What the slot says. A PROCEDURAL mesh (the built-in humanoid, or a mesh rigged in the editor)
        // is registered straight with the MeshService and has no MeshAsset at all, so looking it up in the
        // AssetManager returns nothing — the row used to call that "None", which reads as an empty slot on
        // a character that is plainly standing in the viewport. Say what it actually is instead.
        auto        asset = assetManager->FindByHandle<Assets::MeshAsset>( skinnedMesh.MeshHandle );
        std::string currentMeshName;
        if ( asset )
            currentMeshName = Common::Utils::FileSystem::GetFileName( asset->GetMetadata().Filepath );
        else if ( skinnedMesh.RuntimeMesh )
            currentMeshName = "Procedural (rigged in the editor)";
        else if ( mesh )
            currentMeshName = "Procedural (generated)";

        const bool emptySlot = currentMeshName.empty();
        if ( emptySlot )
            currentMeshName = "None";

        // UE's SkeletalMeshComponent leads with exactly this row — the asset the component renders, as a
        // preview box beside a sunk slot field — before any statistic about it.
        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_HUMAN "  Skeletal Mesh" ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();

            // Same 64px as a material slot's preview — one preview size across Details.
            constexpr float kBox = 64.0f;
            const float     rowH = std::max( kBox, ImGui::GetFrameHeight() ) + ImGui::GetStyle().ItemSpacing.y;

            Utils::ImGuiUtilities::BeginPropertyRow( "Skeletal Mesh Asset",
                                                     "The skinned mesh asset this component renders", rowH );

            DrawAssetBox( kBox, ICON_MDI_HUMAN, !emptySlot, kSkeletalMeshTint );
            ImGui::SameLine();
            if ( Utils::ImGuiUtilities::AssetSlot( "SkinnedMeshSlot", currentMeshName.c_str(), emptySlot ) )
                ImGui::OpenPopup( "skinned_mesh_selector" );

            if ( ImGui::BeginPopup( "skinned_mesh_selector" ) )
            {
                static ImGuiTextFilter filter;
                filter.Draw( "##Search", 200 );
                ImGui::Separator();

                for ( const auto& [handle, meshAsset] : meshAssets )
                {
                    auto isSkinned = Runtime::ResourceRegistry::GetMeshService()->IsSkinned( handle );

                    if ( !isSkinned.has_value() || !isSkinned.value() )
                    {
                        continue;
                    }

                    const std::string name =
                         Common::Utils::FileSystem::GetFileName( meshAsset->GetMetadata().Filepath );

                    if ( filter.PassFilter( name.c_str() ) )
                    {
                        bool selected = skinnedMesh.MeshHandle == handle;
                        if ( ImGui::Selectable( name.c_str(), selected ) )
                        {
                            skinnedMesh.MeshHandle = handle;
                        }
                    }
                }

                ImGui::EndPopup();
            }

            Utils::ImGuiUtilities::EndPropertyRow();
        }

        Utils::ImGuiUtilities::PopID();

        {
            MeshDetailsWidget::Context ctx;
            ctx.Asset       = skinnedMesh.RuntimeMesh ? nullptr : asset;
            ctx.RuntimeMesh = mesh;
            ctx.Entity      = &entity;
            ctx.Scene       = scene;
            MeshDetailsWidget::Show( ctx );
        }

        // Material slots. A skinned mesh carries the same MaterialSlots the renderer maps per submesh, but
        // the slot editor used to be hard-wired to StaticMeshComponent — so a character's materials could
        // only be set by editing the scene file. Same widget, same rows, same drag-drop.
        {
            static MaterialComponentWidget materials( assetManager.get() );
            materials.Render( entity, scene );
        }

        if ( !mesh || !mesh->IsSkinned() )
        {
            return;
        }

        auto        skinned  = static_cast<SkinnedMesh*>( mesh );
        const auto& skeleton = skinned->GetSkeleton();

        const auto& bones = skeleton.GetBones();

        // The bone count on the bar, UE-style: a collapsed Skeleton section still says how big the rig is.
        const std::string skeletonDetail = std::to_string( bones.size() ) + " bones";
        if ( Utils::ImGuiUtilities::SectionHeader( ICON_MDI_BONE "  Skeleton", true, skeletonDetail.c_str() ) )
        {
            Utils::ImGuiUtilities::ResetPropertyRows();

            // The facts as ROWS, on the panel's shared grid — the same label column as every other
            // component, instead of a paragraph of TextDisabled lines floating under a header.
            const auto statRow = []( const char* label, const std::string& value, const char* tooltip )
            {
                Utils::ImGuiUtilities::BeginPropertyRow( label, tooltip );
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( value.c_str() );
                Utils::ImGuiUtilities::EndPropertyRow();
            };

            char buf[128];

            statRow( "Bone Count", std::to_string( bones.size() ), nullptr );

            // Rig identity: the signature is what links a SkinnedMeshAsset to its SkeletonAsset, so it is
            // the thing to compare when a mesh refuses to bind to the rig you expect.
            std::snprintf( buf, sizeof( buf ), "%016llx",
                           static_cast<unsigned long long>( skeleton.GetSignature() ) );
            statRow( "Signature", buf,
                     "Hash of the bone hierarchy. A mesh binds to the skeleton with the same signature." );

            // Cost + format limits, stated instead of implied: the pose is uploaded per frame as one
            // mat4 per bone, and the vertex format carries at most 4 influences per vertex.
            std::snprintf( buf, sizeof( buf ), "%zu x %zu B = %.1f KB / frame", bones.size(), sizeof( glm::mat4 ),
                           static_cast<double>( bones.size() * sizeof( glm::mat4 ) ) / 1024.0 );
            statRow( "Pose Upload", buf, "One mat4 per bone, uploaded every frame the mesh is drawn" );

            std::snprintf( buf, sizeof( buf ), "up to %zu per vertex", SkinnedVertex::MAX_BONE_INFLUENCES );
            statRow( "Influences", buf, nullptr );

            if ( entity.HasComponent<ECS::AnimationComponent>() )
            {
                const auto& anim = entity.GetComponent<ECS::AnimationComponent>();
                const char* clip = anim.CurrentClip.empty() ? "<none>" : anim.CurrentClip.c_str();
                if ( anim.Graph )
                    std::snprintf( buf, sizeof( buf ), "%s  (driven by the Anim Graph)", clip );
                else
                    std::snprintf( buf, sizeof( buf ), "%s%s", clip, anim.Playing ? "" : "  (paused)" );
                statRow( "Clip", buf, nullptr );
            }

            ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );

            // Weight problems the GPU can't tell you about. Only real ones — see AuditSkinning. Wrapped,
            // because a warning that runs off the edge of a docked panel is a warning nobody reads.
            const SkinningAudit& audit = AuditSkinning( *skinned, bones.size() );
            ImGui::PushTextWrapPos( 0.0f );
            if ( audit.OutOfRange > 0 )
                ImGui::TextColored( ThemeManager::GetErrorColor(),
                                    ICON_MDI_ALERT " %llu vertices reference a bone this skeleton does not "
                                                   "have - the mesh is bound to the wrong rig",
                                    static_cast<unsigned long long>( audit.OutOfRange ) );
            if ( audit.Unweighted > 0 )
                ImGui::TextColored( ThemeManager::GetWarningColor(),
                                    ICON_MDI_ALERT " %llu vertices have no bone weights - they stay in bind "
                                                   "pose while the rest animates",
                                    static_cast<unsigned long long>( audit.Unweighted ) );
            ImGui::PopTextWrapPos();
            if ( audit.FullyInfluenced > 0 )
            {
                ImGui::TextDisabled( "%llu vertices use all %zu influence slots",
                                     static_cast<unsigned long long>( audit.FullyInfluenced ),
                                     SkinnedVertex::MAX_BONE_INFLUENCES );
                Utils::ImGuiUtilities::Tooltip( "The import keeps the 4 heaviest influences per vertex; "
                                                "weights beyond that were dropped." );
            }

            ImGui::Separator();

            // Build child adjacency and collect root bones.
            std::unordered_map<size_t, std::vector<size_t>> children;
            std::vector<size_t>                             roots;
            for ( size_t i = 0; i < bones.size(); ++i )
            {
                const auto& parentBone = bones[i].ParentBoneID;
                if ( parentBone.has_value() )
                {
                    children[*parentBone].push_back( i );
                }
                else
                    roots.push_back( i );
            }

            std::function<void( size_t )> DrawBone;
            DrawBone = [&]( size_t boneIndex )
            {
                const auto& bone = bones[boneIndex];

                ImGuiTreeNodeFlags flags =
                     ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
                if ( children[boneIndex].empty() )
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;

                const bool boneSelected =
                     ::Desert::Editor::Core::SkeletonEditMode::GetSelectedBone() == static_cast<int>( boneIndex );
                if ( boneSelected )
                    flags |= ImGuiTreeNodeFlags_Selected;

                const std::string label =
                     std::string( ICON_MDI_BONE ) + "  " + bone.Name + "##" + std::to_string( boneIndex );
                const bool open = ImGui::TreeNodeEx( label.c_str(), flags );
                // Clicking a bone selects it (highlighted in the Skeleton Edit viewport overlay).
                if ( ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() )
                    ::Desert::Editor::Core::SkeletonEditMode::SetSelectedBone( static_cast<int>( boneIndex ) );
                if ( open )
                {
                    for ( auto child : children[boneIndex] )
                        DrawBone( child );
                    ImGui::TreePop();
                }
            };

            // UE-style: a single "Armature" root (accent-coloured) that holds the actual root bones.
            ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetIconColor() );
            const bool armatureOpen = ImGui::TreeNodeEx(
                 ICON_MDI_HUMAN "  Armature",
                 ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth );
            ImGui::PopStyleColor();
            if ( armatureOpen )
            {
                for ( size_t r : roots )
                    DrawBone( r );
                ImGui::TreePop();
            }
        }
    }

    DESERT_REGISTER_CUSTOM_COMPONENT(
         ECS::SkinnedMeshComponent, "Skinned Mesh", false,
         ( []( ECS::Entity& e, ::Desert::Core::Scene* s, const ComponentEditContext& ctx )
           { SkinnedMeshComponentWidget( ctx.AssetManager ).Render( e, s ); } ) )
} // namespace Desert::Editor
