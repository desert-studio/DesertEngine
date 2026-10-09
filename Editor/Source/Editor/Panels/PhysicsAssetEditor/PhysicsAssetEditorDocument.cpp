#include "PhysicsAssetEditorDocument.hpp"

#include <Editor/Core/AssetOpen.hpp>
#include <Editor/Core/PreviewViewpoints.hpp>
#include <Editor/Core/SubjectTitle.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonTree.hpp>
#include <Editor/Widgets/PreviewEnvironmentUI.hpp>
#include <Editor/Widgets/PreviewViewport.hpp>
#include <Editor/Widgets/UIHelper/ImGuiUI.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/PhysicsAsset.hpp>
#include <Engine/Physics/PhysicsAssetGeneration.hpp>
#include <Engine/Physics/RagdollPose.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        // The simulation's step is the editor frame, clamped so a hitch does not throw the ragdoll through the
        // floor (UE PhAT clamps its preview tick the same way).
        constexpr float kMaxSimulationStepSeconds = 1.0f / 30.0f;
        constexpr int   kCircleSegments           = 24;

        constexpr std::array<const char*, 3> kShapeNames{ "Sphere", "Box", "Capsule" }; // PhysicsBodyShape order

        const Physics::PhysicsAssetBody* FindBody( const Physics::PhysicsAssetData& data, const std::string& bone )
        {
            const auto it =
                 std::ranges::find_if( data.Bodies, [&]( const auto& body ) { return body.Bone == bone; } );
            return it == data.Bodies.end() ? nullptr : &*it;
        }

        const Physics::PhysicsAssetConstraint* FindJoint( const Physics::PhysicsAssetData& data,
                                                          const std::string&               child )
        {
            const auto it = std::ranges::find_if( data.Constraints,
                                                  [&]( const auto& joint ) { return joint.ChildBone == child; } );
            return it == data.Constraints.end() ? nullptr : &*it;
        }

        // The projection the Animation Editor's bone overlay uses: NDC y up, behind the camera = no point.
        struct Projector
        {
            glm::mat4 ViewProjection;
            glm::vec2 Origin;
            glm::vec2 Size;

            [[nodiscard]] std::optional<ImVec2> operator()( const glm::vec3& world ) const
            {
                const glm::vec4 clip = ViewProjection * glm::vec4( world, 1.0f );
                if ( clip.w <= 0.0001f )
                    return std::nullopt;
                const glm::vec3 ndc = glm::vec3( clip ) / clip.w;
                return ImVec2( Origin.x + ( ndc.x * 0.5f + 0.5f ) * Size.x,
                               Origin.y + ( 1.0f - ( ndc.y * 0.5f + 0.5f ) ) * Size.y );
            }
        };

        void Line( ImDrawList& draw, const Projector& project, const glm::mat4& frame, const glm::vec3& a,
                   const glm::vec3& b, ImU32 colour )
        {
            const auto pa = project( glm::vec3( frame * glm::vec4( a, 1.0f ) ) );
            const auto pb = project( glm::vec3( frame * glm::vec4( b, 1.0f ) ) );
            if ( pa && pb )
                draw.AddLine( *pa, *pb, colour, 1.5f );
        }

        // An arc of radius r about @p centre in the plane of the unit axes @p u and @p v, from angle a0 to a1.
        void Arc( ImDrawList& draw, const Projector& project, const glm::mat4& frame, const glm::vec3& centre,
                  const glm::vec3& u, const glm::vec3& v, float r, float a0, float a1, ImU32 colour )
        {
            const int segments =
                 std::max( 4, static_cast<int>( kCircleSegments * ( a1 - a0 ) / glm::two_pi<float>() ) );
            for ( int i = 0; i < segments; ++i )
            {
                const float t0 = a0 + ( a1 - a0 ) * static_cast<float>( i ) / static_cast<float>( segments );
                const float t1 = a0 + ( a1 - a0 ) * static_cast<float>( i + 1 ) / static_cast<float>( segments );
                Line( draw, project, frame, centre + r * ( std::cos( t0 ) * u + std::sin( t0 ) * v ),
                      centre + r * ( std::cos( t1 ) * u + std::sin( t1 ) * v ), colour );
            }
        }

        // One part's shape, in the description's units (centimetres, kJoltUnitsPerCentimetre = 1): the capsule
        // along the shape frame's +Y (Jolt's CapsuleShape), the box by its half extents.
        void DrawShape( ImDrawList& draw, const Projector& project, const glm::mat4& frame,
                        const Physics::RagdollPartDesc& part, ImU32 colour )
        {
            const glm::vec3 x( 1, 0, 0 ), y( 0, 1, 0 ), z( 0, 0, 1 ), o( 0.0f );
            const float     pi = glm::pi<float>();
            switch ( part.Shape )
            {
                case Physics::PhysicsBodyShape::Sphere:
                    Arc( draw, project, frame, o, x, y, part.Radius, 0.0f, 2.0f * pi, colour );
                    Arc( draw, project, frame, o, y, z, part.Radius, 0.0f, 2.0f * pi, colour );
                    Arc( draw, project, frame, o, z, x, part.Radius, 0.0f, 2.0f * pi, colour );
                    return;
                case Physics::PhysicsBodyShape::Capsule:
                {
                    const float     r = part.Radius;
                    const glm::vec3 top( 0.0f, part.HalfHeight, 0.0f );
                    Arc( draw, project, frame, top, x, z, r, 0.0f, 2.0f * pi, colour );
                    Arc( draw, project, frame, -top, x, z, r, 0.0f, 2.0f * pi, colour );
                    for ( const glm::vec3& side : { x, -x, z, -z } )
                        Line( draw, project, frame, top + r * side, -top + r * side, colour );
                    Arc( draw, project, frame, top, x, y, r, 0.0f, pi, colour );
                    Arc( draw, project, frame, top, z, y, r, 0.0f, pi, colour );
                    Arc( draw, project, frame, -top, x, y, r, pi, 2.0f * pi, colour );
                    Arc( draw, project, frame, -top, z, y, r, pi, 2.0f * pi, colour );
                    return;
                }
                case Physics::PhysicsBodyShape::Box:
                {
                    const glm::vec3 h = part.HalfExtents;
                    for ( int axis = 0; axis < 3; ++axis )
                        for ( const float s1 : { -1.0f, 1.0f } )
                            for ( const float s2 : { -1.0f, 1.0f } )
                            {
                                glm::vec3 a( 0.0f );
                                const int u = ( axis + 1 ) % 3, v = ( axis + 2 ) % 3;
                                a[u]        = s1 * h[u];
                                a[v]        = s2 * h[v];
                                glm::vec3 b = a;
                                a[axis]     = -h[axis];
                                b[axis]     = h[axis];
                                Line( draw, project, frame, a, b, colour );
                            }
                    return;
                }
            }
        }

        // UE's preview mesh pick for a physics asset: the first `.skmesh` by path on the asset's skeleton, asked
        // of the content registry by its Skeleton tag (nothing loaded to list them).
        std::optional<std::filesystem::path> PreviewMeshFor( const Common::Content::AssetGuid& skeleton )
        {
            std::vector<std::filesystem::path> candidates;
            for ( const auto& row : Assets::ContentRegistry::Rows( Common::Content::ContentKind::SkinnedMesh ) )
                if ( row.Skeleton == skeleton )
                    candidates.push_back( row.Path );
            if ( candidates.empty() )
                return std::nullopt;
            std::ranges::sort( candidates );
            return candidates.front();
        }

        std::shared_ptr<Assets::SkinnedMeshAsset>
        LoadSkinnedMesh( Assets::AssetManager& assets, const std::filesystem::path& path, std::string& error )
        {
            auto mesh = assets.FindByPath<Assets::SkinnedMeshAsset>( path );
            if ( !mesh )
                mesh = assets.CreateAsset<Assets::SkinnedMeshAsset>( path, false );
            if ( !mesh )
            {
                error = std::format( "skeletal mesh '{}' could not be registered", path.generic_string() );
                return nullptr;
            }
            if ( const auto loaded = mesh->EnsureLoaded( assets ); !loaded )
            {
                error = std::format( "skeletal mesh '{}' would not load: {}", path.generic_string(),
                                     loaded.GetError() );
                return nullptr;
            }
            return mesh;
        }
    } // namespace

    PhysicsAssetEditorDocument::PhysicsAssetEditorDocument( const Assets::AssetHandle& asset,
                                                            Assets::AssetManager*      assets )
         : PhysicsAssetEditorBase( AssetSubjectTitle( asset, assets, "Physics Asset" ), asset ), m_Assets( assets )
    {
        LoadWorkingCopy();
    }

    PhysicsAssetEditorDocument::~PhysicsAssetEditorDocument() = default;

    bool PhysicsAssetEditorDocument::IsSubjectAlive() const
    {
        return m_Assets != nullptr &&
               m_Assets->FindMetadataByHandle( Assets::AssetHandle( Subject().Owner ) ) != nullptr;
    }

    void PhysicsAssetEditorDocument::LoadWorkingCopy()
    {
        const Assets::AssetHandle handle( Subject().Owner );
        const auto*               meta = m_Assets != nullptr ? m_Assets->FindMetadataByHandle( handle ) : nullptr;
        if ( meta == nullptr )
        {
            m_Unavailable = "This physics asset is not registered with the asset manager — nothing to edit.";
            return;
        }
        m_Path                  = meta->Filepath;
        const std::string name  = m_Path.filename().string();
        const auto        asset = m_Assets->FindByHandle<Assets::PhysicsAsset>( handle );
        if ( !asset )
        {
            m_Unavailable = std::format( "'{}' is not a PhysicsAsset in the asset manager.", name );
            return;
        }
        if ( const auto loaded = asset->EnsureLoaded( *m_Assets ); !loaded )
        {
            m_Unavailable = std::format( "'{}' would not load: {}", name, loaded.GetError() );
            return;
        }
        m_Working = std::make_shared<Physics::PhysicsAssetData>( asset->GetData() );
        m_OnDisk  = asset->GetData();
        if ( !m_Working->Bodies.empty() )
            m_SelectedBone = m_Working->Bodies.front().Bone;
    }

    void PhysicsAssetEditorDocument::EnsurePreview()
    {
        if ( m_Preview || !m_Unavailable.empty() || !m_Working )
            return;

        const std::string name     = m_Path.filename().string();
        const auto        meshPath = PreviewMeshFor( m_Working->Skeleton );
        if ( !meshPath )
        {
            m_Unavailable = std::format(
                 "'{}' is on skeleton {}, and no registered skeletal mesh (.skmesh) uses that skeleton — nothing "
                 "to preview it on.",
                 name, SkeletonSlots::NameOf( Common::Content::ContentKind::Skeleton, m_Working->Skeleton ) );
            return;
        }
        std::string error;
        const auto  mesh = LoadSkinnedMesh( *m_Assets, *meshPath, error );
        if ( !mesh )
        {
            m_Unavailable = std::format( "'{}': {}", name, error );
            return;
        }
        m_MeshName = meshPath->filename().string();

        m_Preview  = std::make_unique<PreviewViewport>();
        m_UIHelper = std::make_unique<UI::UIHelper>();
        m_UIHelper->Init();
        m_PreviewLive = true;

        const auto& slots = mesh->GetMaterialHandles();
        m_Preview->SetSkinnedMesh( mesh->GetMetadata().Handle,
                                   std::vector<Assets::AssetHandle>( slots.begin(), slots.end() ), {} );
        // The pose the window shows is the authoring pose: the rest pose, or the simulation written into it.
        m_Preview->SetPoseOverride( true );

        if ( m_PendingOrbitDegrees )
        {
            m_Preview->SetOrbit( glm::radians( m_PendingOrbitDegrees->x ),
                                 glm::radians( m_PendingOrbitDegrees->y ) );
            m_PendingOrbitDegrees.reset();
        }
    }

    void PhysicsAssetEditorDocument::ReleaseView()
    {
        m_Simulation.Stop();
        m_RestPose.reset();
        m_Preview.reset();
        m_UIHelper.reset();
        m_PreviewLive = false;
    }

    void PhysicsAssetEditorDocument::SetPreviewViewpoint( const PreviewViewpoint& viewpoint )
    {
        if ( !m_Preview )
        {
            m_PendingOrbitDegrees = glm::vec2( viewpoint.YawDegrees, viewpoint.PitchDegrees );
            return;
        }
        m_Preview->SetOrbit( glm::radians( viewpoint.YawDegrees ), glm::radians( viewpoint.PitchDegrees ) );
    }

    bool PhysicsAssetEditorDocument::SaveDocument()
    {
        if ( !m_Working || m_Path.empty() )
            return false;
        const auto saved = SavePhysicsAssetDocument( m_Path, *m_Working );
        Report( saved, "Save" );
        if ( !saved )
            return false;
        m_OnDisk = *m_Working;
        // The runtime reads the asset object, not the file: it re-reads what was just written.
        if ( const auto asset =
                  m_Assets->FindByHandle<Assets::PhysicsAsset>( Assets::AssetHandle( Subject().Owner ) ) )
            if ( const auto reloaded = asset->LoadFromFile(); !reloaded )
                LOG_ERROR( "Physics Asset Editor: '{}' was saved but would not reload: {}",
                           m_Path.generic_string(), reloaded.GetError() );
        return true;
    }

    std::vector<ISubjectDocument::DocumentAction> PhysicsAssetEditorDocument::Actions()
    {
        std::vector<DocumentAction> actions;
        actions.push_back( { "Simulate", [this]() { StartSimulation(); } } );
        actions.push_back( { "Stop", [this]() { StopSimulation(); } } );
        actions.push_back( { "Save", [this]() { (void)SaveDocument(); } } );
        PreviewEnvironment::AppendActions( actions );
        return actions;
    }

    void PhysicsAssetEditorDocument::Report( const Common::BoolResultStr& outcome, const char* what )
    {
        if ( outcome )
        {
            m_LastRefusal.clear();
            return;
        }
        m_LastRefusal = std::format( "{}: {}", what, outcome.GetError() );
        LOG_ERROR( "Physics Asset Editor '{}': {}", m_Path.filename().string(), m_LastRefusal );
    }

    void PhysicsAssetEditorDocument::StartSimulation()
    {
        auto* animator = m_Preview ? m_Preview->GetAnimatorForAuthoring() : nullptr;
        if ( animator == nullptr || !m_RestPose || !m_Working )
        {
            Report( Common::MakeError<bool>( "the preview has not built its skeletal mesh yet" ), "Simulate" );
            return;
        }
        Report( m_Simulation.Start( *m_Working, animator->GetSkeleton(), *m_RestPose ), "Simulate" );
    }

    void PhysicsAssetEditorDocument::StopSimulation()
    {
        m_Simulation.Stop();
        auto* animator = m_Preview ? m_Preview->GetAnimatorForAuthoring() : nullptr;
        if ( animator == nullptr || !m_RestPose )
            return;
        Report( animator->SetAuthoringPose( *m_RestPose ), "Stop" );
        animator->ApplyLocalPose();
    }

    void PhysicsAssetEditorDocument::StepSimulation()
    {
        auto* animator = m_Preview ? m_Preview->GetAnimatorForAuthoring() : nullptr;
        if ( !m_Simulation.IsRunning() || animator == nullptr || !m_RestPose )
            return;
        m_Simulation.Step( std::min( ImGui::GetIO().DeltaTime, kMaxSimulationStepSeconds ) );
        auto overrides = m_Simulation.Overrides( animator->GetSkeleton(), *m_RestPose );
        if ( !overrides )
        {
            Report( Common::MakeError<bool>( overrides.GetError() ), "Simulate" );
            m_Simulation.Stop();
            return;
        }
        // The simulated bodies over the rest pose, kept as the authoring pose the preview's Update renders.
        if ( const auto applied = animator->ApplyPhysicsPose( overrides.GetValue() ); !applied )
        {
            Report( applied, "Simulate" );
            m_Simulation.Stop();
            return;
        }
        Report( animator->SetAuthoringPose( animator->GetLocalPose() ), "Simulate" );
    }

    void PhysicsAssetEditorDocument::RebuildDescriptionIfChanged( const Animation::Skeleton& skeleton )
    {
        if ( !m_Working || ( m_DescribedData && *m_DescribedData == *m_Working ) )
            return;
        m_DescribedData = *m_Working;
        auto desc       = Physics::BuildRagdollDesc( *m_Working, skeleton );
        if ( desc )
        {
            m_Description = desc.ExtractValue();
            m_DescriptionError.clear();
        }
        else
        {
            m_Description      = {};
            m_DescriptionError = desc.GetError();
        }
    }

    void PhysicsAssetEditorDocument::OnPreUpdate()
    {
        // THE SLOT IS NOT CLAIMED UNTIL THE WINDOW HAS BEEN DRAWN (StaticMeshViewerDocument::OnPreUpdate).
        if ( !m_DrewThisFrame )
            return;
        m_DrewThisFrame = false;

        EnsurePreview();
        if ( !m_Preview || m_RenderSize.x == 0u || m_RenderSize.y == 0u )
            return;

        if ( auto* animator = m_Preview->GetAnimatorForAuthoring(); animator != nullptr && !m_RestPose )
            m_RestPose = animator->GetAuthoringPose(); // the animator has just been built: the rest pose
        StepSimulation();

        PreviewEnvironment::ApplyTo( *m_Preview, m_Assets );
        m_Preview->Update( m_RenderSize.x, m_RenderSize.y );
    }

    void PhysicsAssetEditorDocument::DrawToolbar()
    {
        const bool running = m_Simulation.IsRunning();
        if ( ImGui::Button( running ? "Stop" : "Simulate" ) )
        {
            if ( running )
                StopSimulation();
            else
                StartSimulation();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Save" ) )
            (void)SaveDocument();
        ImGui::SameLine();
        ImGui::TextDisabled( "%s%s", m_MeshName.c_str(),
                             GetDiskState() == DiskState::Dirty ? "  (modified)" : "" );
        if ( !m_LastRefusal.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", m_LastRefusal.c_str() );
        if ( !m_DescriptionError.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "%s", m_DescriptionError.c_str() );
    }

    void PhysicsAssetEditorDocument::DrawBoneTree()
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( animator == nullptr || !m_Working )
        {
            ImGui::TextDisabled( "Starting the preview..." );
            return;
        }
        const auto& skeleton = animator->GetSkeleton();
        const auto& bones    = skeleton.GetBones();
        for ( const SkeletonTreeRow& row : BuildSkeletonTreeRows( skeleton, {}, {} ) )
        {
            const std::string& bone    = bones[row.Bone].Name;
            const bool         hasBody = FindBody( *m_Working, bone ) != nullptr;
            ImGui::Indent( static_cast<float>( row.Depth ) * 10.0f + 1.0f );
            const std::string label = std::format( "{}{}##bone{}", hasBody ? "[body] " : "", bone, row.Bone );
            if ( !hasBody )
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            if ( ImGui::Selectable( label.c_str(), bone == m_SelectedBone ) )
            {
                m_SelectedBone   = bone;
                m_EditingDetails = false;
            }
            if ( !hasBody )
                ImGui::PopStyleColor();
            ImGui::Unindent( static_cast<float>( row.Depth ) * 10.0f + 1.0f );
        }
    }

    void PhysicsAssetEditorDocument::DrawBodyDetails( const Physics::PhysicsAssetBody& current )
    {
        if ( !m_EditingDetails || m_BodyEdit.Bone != current.Bone )
        {
            m_BodyEdit         = current;
            m_BodyEulerDegrees = glm::degrees( glm::eulerAngles( current.Rotation ) );
        }
        bool       active = false, commit = false;
        const auto track = [&]()
        {
            active |= ImGui::IsItemActive();
            commit |= ImGui::IsItemDeactivatedAfterEdit();
        };

        ImGui::SeparatorText( "Body" );
        int shape = static_cast<int>( m_BodyEdit.Shape );
        if ( ImGui::Combo( "Shape", &shape, kShapeNames.data(), static_cast<int>( kShapeNames.size() ) ) )
        {
            m_BodyEdit.Shape = static_cast<Physics::PhysicsBodyShape>( shape );
            commit           = true;
        }
        if ( m_BodyEdit.Shape != Physics::PhysicsBodyShape::Box )
        {
            ImGui::DragFloat( "Radius (cm)", &m_BodyEdit.Radius, 0.1f, 0.0f, 0.0f, "%.2f" );
            track();
        }
        if ( m_BodyEdit.Shape == Physics::PhysicsBodyShape::Capsule )
        {
            ImGui::DragFloat( "Length (cm)", &m_BodyEdit.Length, 0.1f, 0.0f, 0.0f, "%.2f" );
            track();
        }
        if ( m_BodyEdit.Shape == Physics::PhysicsBodyShape::Box )
        {
            ImGui::DragFloat3( "Size (cm)", &m_BodyEdit.BoxExtents.x, 0.1f, 0.0f, 0.0f, "%.2f" );
            track();
        }
        ImGui::DragFloat3( "Center (cm)", &m_BodyEdit.Center.x, 0.1f, 0.0f, 0.0f, "%.2f" );
        track();
        if ( ImGui::DragFloat3( "Rotation (deg)", &m_BodyEulerDegrees.x, 0.5f, 0.0f, 0.0f, "%.1f" ) )
            m_BodyEdit.Rotation = glm::quat( glm::radians( m_BodyEulerDegrees ) );
        track();
        ImGui::DragFloat( "Mass (kg, 0 = density)", &m_BodyEdit.MassKg, 0.1f, 0.0f, 0.0f, "%.2f" );
        track();
        if ( m_BodyEdit.MassKg <= 0.0f )
        {
            ImGui::DragFloat( "Density (g/cm3)", &m_BodyEdit.DensityGramsPerCm3, 0.01f, 0.0f, 0.0f, "%.3f" );
            track();
        }
        ImGui::Text( "Mass used: %.2f kg", Physics::PhysicsBodyMassKg( m_BodyEdit ) );

        m_EditingDetails = active;
        if ( commit )
            Report( CommitBody( m_Working, m_BodyEdit ), "Body" );
    }

    void PhysicsAssetEditorDocument::DrawJointDetails( const Physics::PhysicsAssetConstraint& current )
    {
        if ( !m_EditingDetails )
            m_JointLimitsDegrees = { current.Swing1LimitDegrees, current.Swing2LimitDegrees,
                                     current.TwistLimitDegrees };
        bool       active = false, commit = false;
        const auto track = [&]()
        {
            active |= ImGui::IsItemActive();
            commit |= ImGui::IsItemDeactivatedAfterEdit();
        };
        ImGui::SeparatorText( "Joint" );
        ImGui::Text( "%s -> %s", current.ParentBone.c_str(), current.ChildBone.c_str() );
        ImGui::DragFloat( "Swing 1 (deg)", &m_JointLimitsDegrees.x, 0.5f, 0.0f, 180.0f, "%.1f" );
        track();
        ImGui::DragFloat( "Swing 2 (deg)", &m_JointLimitsDegrees.y, 0.5f, 0.0f, 180.0f, "%.1f" );
        track();
        ImGui::DragFloat( "Twist (deg)", &m_JointLimitsDegrees.z, 0.5f, 0.0f, 180.0f, "%.1f" );
        track();
        m_EditingDetails = m_EditingDetails || active;
        if ( commit )
            Report( CommitJointLimits( m_Working, current.ChildBone, m_JointLimitsDegrees.x,
                                       m_JointLimitsDegrees.y, m_JointLimitsDegrees.z ),
                    "Joint" );
    }

    void PhysicsAssetEditorDocument::DrawDetails()
    {
        if ( !m_Working )
            return;
        if ( m_SelectedBone.empty() )
        {
            ImGui::TextDisabled( "Select a bone in the tree." );
            return;
        }
        ImGui::TextUnformatted( m_SelectedBone.c_str() );
        // Copies: a commit replaces the working copy, which would leave a reference into it dangling.
        const auto*                                    body  = FindBody( *m_Working, m_SelectedBone );
        const auto*                                    joint = FindJoint( *m_Working, m_SelectedBone );
        std::optional<Physics::PhysicsAssetBody>       bodyCopy;
        std::optional<Physics::PhysicsAssetConstraint> jointCopy;
        if ( body != nullptr )
            bodyCopy.emplace( *body );
        if ( joint != nullptr )
            jointCopy.emplace( *joint );

        ImGui::BeginDisabled( m_Simulation.IsRunning() );
        const bool wasEditing = m_EditingDetails;
        m_EditingDetails      = false;
        if ( bodyCopy )
        {
            // The body's fields decide whether the buffers are being edited; the joint's add to it.
            m_EditingDetails = wasEditing;
            DrawBodyDetails( *bodyCopy );
        }
        else
            ImGui::TextDisabled( "No body on this bone." );
        if ( jointCopy )
            DrawJointDetails( *jointCopy );
        else if ( bodyCopy )
            ImGui::TextDisabled( "No joint: this body simulates free of its parent." );
        ImGui::EndDisabled();
    }

    void PhysicsAssetEditorDocument::DrawBodies( const glm::vec2& origin, const glm::vec2& size )
    {
        const auto* animator = m_Preview ? m_Preview->GetAnimator() : nullptr;
        if ( animator == nullptr )
            return;
        RebuildDescriptionIfChanged( animator->GetSkeleton() );

        // Simulating: the running ragdoll as it was built at Start, at the bodies' simulated transforms. Stopped:
        // the working copy's bodies at the pose the preview shows.
        const bool                                 running = m_Simulation.IsRunning();
        const Physics::RagdollDesc&                desc    = running ? m_Simulation.Description() : m_Description;
        std::vector<Physics::RagdollPartTransform> parts;
        if ( running )
            parts = m_Simulation.Parts();
        else if ( auto posed = Physics::RagdollPartsFromPose( desc, animator->GetSkeleton(),
                                                              animator->GetLocalPose(), glm::mat4( 1.0f ) ) )
            parts = posed.ExtractValue();
        if ( parts.size() != desc.Parts.size() )
            return;

        const Projector project{ m_Preview->GetViewProjection() * m_Preview->GetTargetTransform(), origin, size };
        ImDrawList*     draw = ImGui::GetWindowDrawList();
        for ( std::size_t i = 0; i < parts.size(); ++i )
        {
            const auto&     part = desc.Parts[i];
            const glm::mat4 body =
                 glm::translate( glm::mat4( 1.0f ), parts[i].Position ) * glm::mat4_cast( parts[i].Rotation );
            const glm::mat4 shape = body * glm::translate( glm::mat4( 1.0f ), part.ShapeOffset ) *
                                    glm::mat4_cast( part.ShapeRotation );
            const ImU32 colour =
                 part.Bone == m_SelectedBone ? IM_COL32( 255, 200, 40, 255 ) : IM_COL32( 90, 220, 255, 200 );
            DrawShape( *draw, project, shape, part, colour );
        }
    }

    void PhysicsAssetEditorDocument::OnUIRender()
    {
        // No ImGui::Begin: EditorLayer's document loop wraps this in Begin/End.
        m_DrewThisFrame = true;

        if ( !m_Unavailable.empty() )
        {
            ImGui::TextWrapped( "%s", m_Unavailable.c_str() );
            return;
        }

        DrawToolbar();

        constexpr float kTreeWidth    = 220.0f;
        constexpr float kDetailsWidth = 300.0f;
        const ImVec2    avail         = ImGui::GetContentRegionAvail();
        const float     spacing       = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2    view( std::max( avail.x - kTreeWidth - kDetailsWidth - 2.0f * spacing, 1.0f ),
                              std::max( avail.y, 1.0f ) );
        m_RenderSize = glm::uvec2( static_cast<uint32_t>( view.x ), static_cast<uint32_t>( view.y ) );

        if ( ImGui::BeginChild( "##bonetree", ImVec2( kTreeWidth, view.y ), ImGuiChildFlags_Borders ) )
            DrawBoneTree();
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##physicsview", view ) )
        {
            if ( !m_Preview || !m_UIHelper )
                ImGui::TextDisabled( "Starting the preview..." );
            else
            {
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                (void)m_Preview->Draw( *m_UIHelper, view, PreviewInteraction::Interactive );
                DrawBodies( glm::vec2( origin.x, origin.y ), glm::vec2( view.x, view.y ) );
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if ( ImGui::BeginChild( "##physicsdetails", ImVec2( kDetailsWidth, view.y ) ) )
            DrawDetails();
        ImGui::EndChild();
    }

    SubjectEditorRegistry::PathOpenOutcome RequestPhysicsAssetDocument( Assets::AssetManager*        assets,
                                                                        const std::string&           path,
                                                                        const SubjectEditorRegistry& editors )
    {
        using Outcome = SubjectEditorRegistry::PathOpenOutcome;
        std::error_code ec;
        if ( assets == nullptr || std::filesystem::path( path ).extension() != Physics::kPhysicsAssetExtension ||
             !std::filesystem::exists( path, ec ) )
            return Outcome::NotMine;

        auto asset = assets->FindByPath<Assets::PhysicsAsset>( path );
        if ( !asset )
            asset = assets->CreateAsset<Assets::PhysicsAsset>( path );
        if ( !asset )
        {
            LOG_ERROR( "[Assets] '{}' could not be registered as a physics asset — no editor was opened.", path );
            return Outcome::Failed;
        }
        if ( const auto loaded = asset->EnsureLoaded( *assets ); !loaded )
        {
            LOG_ERROR( "[Assets] '{}' would not load as a physics asset — no editor was opened: {}", path,
                       loaded.GetError() );
            return Outcome::Failed;
        }

        const auto handle = asset->GetMetadata().Handle;
        if ( const auto opened = Core::RequestOpenAsset( assets->FindMetadataByHandle( handle ), handle, editors );
             !opened.IsSuccess() )
        {
            LOG_ERROR( "[Assets] '{}': {}", path, opened.GetError() );
            return Outcome::Failed;
        }
        return Outcome::Requested;
    }

    Common::ResultStr<std::filesystem::path> CreatePhysicsAssetForMesh( Assets::AssetManager&        assets,
                                                                        const std::filesystem::path& meshPath )
    {
        using Path = std::filesystem::path;
        std::string error;
        const auto  mesh = LoadSkinnedMesh( assets, meshPath, error );
        if ( !mesh )
            return Common::MakeError<Path>( error );
        const auto skeletonGuid = mesh->GetSkeleton();
        if ( skeletonGuid.IsNull() )
            return Common::MakeFormattedError<Path>( "skeletal mesh '{}' names no skeleton — assign one first",
                                                     meshPath.generic_string() );
        const auto skeletonAsset = SkeletonSlots::LoadSkeleton( assets, skeletonGuid );
        if ( !skeletonAsset )
            return Common::MakeFormattedError<Path>( "skeletal mesh '{}': {}", meshPath.generic_string(),
                                                     skeletonAsset.GetError() );
        const Animation::Skeleton* skeleton = skeletonAsset.GetValue()->GetSkeleton();
        if ( skeleton == nullptr )
            return Common::MakeFormattedError<Path>( "skeletal mesh '{}': its skeleton has no bones loaded",
                                                     meshPath.generic_string() );

        auto data = Physics::GeneratePhysicsAsset( *skeleton, mesh->GetVertices(), skeletonGuid );
        if ( !data )
            return Common::MakeFormattedError<Path>( "skeletal mesh '{}': {}", meshPath.generic_string(),
                                                     data.GetError() );

        // UE names it <Mesh>_PhysicsAsset beside the mesh, and never writes over an asset that is there.
        const std::string stem = meshPath.stem().string() + "_PhysicsAsset";
        std::error_code   ec;
        Path              target = meshPath.parent_path() / ( stem + Physics::kPhysicsAssetExtension );
        for ( int n = 1; std::filesystem::exists( target, ec ); ++n )
            target = meshPath.parent_path() / std::format( "{}{}{}", stem, n, Physics::kPhysicsAssetExtension );

        if ( const auto saved = Assets::PhysicsAsset::Save( target, data.GetValue() ); !saved )
            return Common::MakeFormattedError<Path>( "'{}' could not be written: {}", target.generic_string(),
                                                     saved.GetError() );
        return Common::MakeSuccess( target );
    }
} // namespace Desert::Editor
