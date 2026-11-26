// THE LEVEL SEQUENCE'S BONE ROWS (SEQ-BONE-KEYS) — UE's FK Control Rig track in the Sequencer: a row per keyed
// bone nested under its actor, "+ Track ▸ Bone", the controls' Location / Rotation / Scale and the section's
// Blend and Weight as document properties, "Key Bone", and the viewport's bone gizmo keying under Auto Key.
//
// Every edit is one undo step through the document's transaction (m_LevelEdit) and every rule lives in the
// engine half (ECS/LevelSequenceBoneKeys.hpp): this file draws and routes.

#include "LevelBoneRows.hpp"
#include "SequencerPanel.hpp"

#include <Editor/Core/GizmoState.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/ToastManager.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Assets/LevelSequenceAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/LevelSequenceBoneKeys.hpp>
#include <Engine/ECS/UUIDComponent.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace Desert::Editor
{
    namespace LevelBoneEdit
    {
        namespace LevelTL = Animation::Timeline;

        namespace
        {
            constexpr std::array<std::string_view, 5> kFields = { "Location", "Rotation", "Scale", "Weight",
                                                                  "Blend" };

            EditableProperty Vector3( std::string name, std::string label, const std::string& group,
                                      const glm::vec3& value )
            {
                EditableProperty property;
                property.Name       = std::move( name );
                property.Label      = std::move( label );
                property.Group      = group;
                property.Type       = "float3";
                property.Components = 3;
                property.Value      = { value.x, value.y, value.z, 0.0F };
                return property;
            }
        } // namespace

        std::string PropertyName( const std::string_view actor, const std::string_view bone,
                                  const std::string_view field )
        {
            return std::format( "{}{}.{}.{}", kPrefix, actor, bone, field );
        }

        Common::ResultStr<Address> Parse( const LevelTL::Sequence& sequence, const std::string_view name )
        {
            if ( !name.starts_with( kPrefix ) )
                return Common::MakeFormattedError<Address>(
                     "'{}' is not a bone property (Bone.<actor>.<bone>.<field>)", name );
            const std::string_view rest = name.substr( kPrefix.size() );
            const size_t           last = rest.rfind( '.' );
            if ( last == std::string_view::npos )
                return Common::MakeFormattedError<Address>( "'{}' names no field", name );
            const std::string_view field = rest.substr( last + 1 );
            if ( std::ranges::find( kFields, field ) == kFields.end() )
                return Common::MakeFormattedError<Address>(
                     "'{}': a bone has no field '{}' (Location, Rotation, Scale, Weight, Blend)", name, field );
            const std::string_view actorAndBone = rest.substr( 0, last );
            // The actor label is matched against the sequence's actors, so a label holding a dot still reads.
            for ( const LevelTL::Binding& actor : sequence.Bindings )
            {
                if ( actor.Kind != LevelTL::BindingKind::Entity || actor.Label.empty() ||
                     !actorAndBone.starts_with( actor.Label ) || actorAndBone.size() <= actor.Label.size() + 1 ||
                     actorAndBone[actor.Label.size()] != '.' )
                    continue;
                Address address;
                address.Actor      = actor.Guid;
                address.ActorLabel = actor.Label;
                address.BoneName   = std::string( actorAndBone.substr( actor.Label.size() + 1 ) );
                address.Field      = std::string( field );
                address.Bone       = ECS::FindBoneBinding( sequence, actor.Guid, address.BoneName );
                return Common::MakeSuccess( std::move( address ) );
            }
            return Common::MakeFormattedError<Address>( "'{}': no actor of this sequence is named so", name );
        }

        std::vector<EditableProperty> Describe( const LevelTL::Sequence&     sequence,
                                                const Animation::FrameNumber tick )
        {
            std::vector<EditableProperty> out;
            for ( const LevelTL::Binding& bone : sequence.Bindings )
            {
                if ( bone.Kind != LevelTL::BindingKind::Bone )
                    continue;
                const LevelTL::Binding* actor = LevelTL::FindBinding( sequence, bone.Parent );
                if ( actor == nullptr )
                    continue;
                const std::string group = std::format( "Bone {} {}", actor->Label, bone.Locator );
                if ( const auto value = ECS::BoneChannelValue( sequence, bone.Guid, tick ) )
                {
                    const Animation::BoneTransform& v = value.GetValue();
                    out.push_back( Vector3( PropertyName( actor->Label, bone.Locator, "Location" ),
                                            "Location (cm)", group, v.Translation ) );
                    out.push_back( Vector3( PropertyName( actor->Label, bone.Locator, "Rotation" ),
                                            "Rotation (deg)", group, EulerDegrees( v.Rotation ) ) );
                    out.push_back(
                         Vector3( PropertyName( actor->Label, bone.Locator, "Scale" ), "Scale", group, v.Scale ) );
                }
                if ( const auto blend = ECS::BoneSectionBlendOf( sequence, bone.Guid ) )
                {
                    EditableProperty weight;
                    weight.Name       = PropertyName( actor->Label, bone.Locator, "Weight" );
                    weight.Label      = "Weight";
                    weight.Group      = group;
                    weight.Type       = "float";
                    weight.Components = 1;
                    weight.Value[0]   = blend.GetValue().Weight;
                    out.push_back( std::move( weight ) );
                    EditableProperty type;
                    type.Name       = PropertyName( actor->Label, bone.Locator, "Blend" );
                    type.Label      = "Blend (0 Absolute, 1 Additive)";
                    type.Group      = group;
                    type.Type       = "int";
                    type.Components = 1;
                    type.Min        = 0.0F;
                    type.Max        = 1.0F;
                    type.Value[0]   = blend.GetValue().Blend == LevelTL::SectionBlendType::Additive ? 1.0F : 0.0F;
                    out.push_back( std::move( type ) );
                }
            }
            return out;
        }

        glm::vec3 EulerDegrees( const glm::quat& rotation )
        {
            return glm::degrees( glm::eulerAngles( rotation ) );
        }

        glm::quat FromEulerDegrees( const glm::vec3& degrees )
        {
            return glm::quat( glm::radians( degrees ) );
        }
    } // namespace LevelBoneEdit

    namespace
    {
        namespace LevelTL = Animation::Timeline;

        /// A refusal, said where the person looks (a toast) and where a script reads (the Output Log).
        Common::BoolResultStr Refused( const std::string_view what, const std::string& why )
        {
            LOG_WARN( "[Sequencer] {} refused: {}", what, why );
            ToastManager::Push( std::format( "{} refused: {}", what, why ), ToastLevel::Error, 6.0f );
            return Common::MakeFormattedError<bool>( "{} refused: {}", what, why );
        }

        /// The actor entity, its animator and the bone index a Bone binding names in @p registry.
        struct BoneTarget
        {
            entt::entity               Entity   = entt::null;
            Animation::Animator*       Animator = nullptr;
            uint32_t                   Index    = 0;
        };

        Common::ResultStr<BoneTarget> ResolveBone( const LevelTL::Sequence& sequence, entt::registry& registry,
                                                   const LevelTL::BindingGuid& bone )
        {
            const LevelTL::Binding* bound = LevelTL::FindBinding( sequence, bone );
            if ( bound == nullptr || bound->Kind != LevelTL::BindingKind::Bone )
                return Common::MakeError<BoneTarget>( "the binding is not a bone of this sequence" );
            const auto entity = ECS::BoneActorEntity( sequence, registry, bound->Parent );
            if ( !entity )
                return Common::MakeFormattedError<BoneTarget>( "{}: {}", bound->Locator, entity.GetError() );
            const auto* animation = registry.try_get<ECS::AnimationComponent>( entity.GetValue() );
            if ( animation == nullptr || !animation->Animator )
                return Common::MakeFormattedError<BoneTarget>( "{}: the actor has no Animator", bound->Locator );
            const auto index = animation->Animator->GetSkeleton().FindBoneIndex( bound->Locator );
            if ( !index )
                return Common::MakeFormattedError<BoneTarget>( "{}: the actor's skeleton has no such bone",
                                                               bound->Locator );
            return Common::MakeSuccess( BoneTarget{ entity.GetValue(), animation->Animator.get(), *index } );
        }
    } // namespace

    Common::BoolResultStr SequencerPanel::AddLevelBoneTrack( const LevelTL::BindingGuid& actor,
                                                             const std::string_view      bone )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return Refused( "Add Bone Track", "the Level Sequence or its scene is gone" );
        LevelTL::Sequence& sequence = asset->EditSequence();
        // Checked before the undo step opens, so a refusal leaves no empty step behind.
        LevelTL::Sequence probe = sequence;
        if ( const auto added = ECS::AddBoneBinding( probe, scene->GetRegistry(), actor, bone ); !added )
            return Refused( "Add Bone Track", added.GetError() );
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto added = ECS::AddBoneBinding( sequence, scene->GetRegistry(), actor, bone ); !added )
            return Refused( "Add Bone Track", added.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SequencerPanel::RemoveLevelBoneTrack( const LevelTL::BindingGuid& bone )
    {
        const auto asset = ResolveLevelAsset();
        if ( !asset )
            return Refused( "Remove Bone Track", "the Level Sequence is gone" );
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto removed = ECS::RemoveBoneBinding( asset->EditSequence(), bone ); !removed )
            return Refused( "Remove Bone Track", removed.GetError() );
        if ( m_LevelSelBone == bone )
            SelectLevelBone( bone ); // picking the selected row again gives the gizmo back
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SequencerPanel::KeyLevelBone( const LevelTL::BindingGuid& bone )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return Refused( "Key Bone", "the Level Sequence or its scene is gone" );
        LevelTL::Sequence& sequence = asset->EditSequence();
        auto&              registry = scene->GetRegistry();
        const auto         target   = ResolveBone( sequence, registry, bone );
        if ( !target )
            return Refused( "Key Bone", target.GetError() );
        const std::string& name = LevelTL::FindBinding( sequence, bone )->Locator;
        // What the viewer sees is what gets keyed — KeyLevelTransform's rule, for a bone.
        const auto shown = ECS::BonePose( registry, target.GetValue().Entity, name );
        const auto under = ECS::BonePreLayerPose( registry, target.GetValue().Entity, name );
        if ( !shown )
            return Refused( "Key Bone", shown.GetError() );
        if ( !under )
            return Refused( "Key Bone", under.GetError() );
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto keyed = ECS::SetBoneKey( sequence, bone, m_LevelTick, shown.GetValue(), under.GetValue() );
             !keyed )
            return Refused( "Key Bone", keyed.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SequencerPanel::SetLevelBoneProperty( const std::string&        name,
                                                                const std::vector<float>& value )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return Refused( name, "the Level Sequence or its scene is gone" );
        const LevelTL::Sequence& current  = asset->GetSequence();
        auto&                    registry = scene->GetRegistry();
        const auto               parsed   = LevelBoneEdit::Parse( current, name );
        if ( !parsed )
            return Refused( name, parsed.GetError() );
        const LevelBoneEdit::Address& address = parsed.GetValue();
        if ( address.Bone.IsNull() )
        {
            // No track: say first whether the rig has the bone at all, then that it is not tracked.
            const auto actor = ECS::BoneActorEntity( current, registry, address.Actor );
            if ( !actor )
                return Refused( name, actor.GetError() );
            if ( const auto pose = ECS::BonePose( registry, actor.GetValue(), address.BoneName ); !pose )
                return Refused( name, pose.GetError() );
            return Refused( name,
                            std::format( "{} of {} has no bone track (Add Bone Track {} {})", address.BoneName,
                                         address.ActorLabel, address.ActorLabel, address.BoneName ) );
        }
        const auto target = ResolveBone( current, registry, address.Bone );
        if ( !target )
            return Refused( name, target.GetError() );
        const auto under = ECS::BonePreLayerPose( registry, target.GetValue().Entity, address.BoneName );
        if ( !under )
            return Refused( name, under.GetError() );

        if ( address.Field == "Weight" || address.Field == "Blend" )
        {
            if ( value.size() != 1 )
                return Refused( name, std::format( "takes one number, not {}", value.size() ) );
            const auto blend = ECS::BoneSectionBlendOf( current, address.Bone );
            if ( !blend )
                return Refused( name, blend.GetError() );
            LevelTL::SectionBlendType type   = blend.GetValue().Blend;
            float                     weight = blend.GetValue().Weight;
            if ( address.Field == "Weight" )
                weight = value[0];
            else if ( value[0] == 0.0F || value[0] == 1.0F )
                type =
                     value[0] == 1.0F ? LevelTL::SectionBlendType::Additive : LevelTL::SectionBlendType::Absolute;
            else
                return Refused( name, std::format( "{} is no blend (0 Absolute, 1 Additive)", value[0] ) );
            const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
            if ( const auto set = ECS::SetBoneSectionBlend( asset->EditSequence(), address.Bone, type, weight,
                                                            under.GetValue() );
                 !set )
                return Refused( name, set.GetError() );
            return BOOLSUCCESS;
        }

        if ( value.size() != 3 )
            return Refused( name, std::format( "takes three numbers, not {}", value.size() ) );
        // The channel value at the playhead, one part replaced; with no section there yet, the shown pose.
        Animation::BoneTransform channel;
        if ( const auto at = ECS::BoneChannelValue( current, address.Bone, m_LevelTick ) )
            channel = at.GetValue();
        else if ( const auto shown = ECS::BonePose( registry, target.GetValue().Entity, address.BoneName ) )
            channel = shown.GetValue();
        else
            return Refused( name, shown.GetError() );
        const glm::vec3 v( value[0], value[1], value[2] );
        if ( address.Field == "Location" )
            channel.Translation = v;
        else if ( address.Field == "Rotation" )
            channel.Rotation = LevelBoneEdit::FromEulerDegrees( v );
        else
            channel.Scale = v;
        const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
        if ( const auto keyed =
                  ECS::SetBoneChannelKey( asset->EditSequence(), address.Bone, m_LevelTick, channel );
             !keyed )
            return Refused( name, keyed.GetError() );
        return BOOLSUCCESS;
    }

    void SequencerPanel::AppendLevelBoneActions( std::vector<DocumentAction>& actions )
    {
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return;
        const LevelTL::Sequence& sequence = asset->GetSequence();
        auto&                    registry = scene->GetRegistry();
        for ( const LevelTL::Binding& actor : sequence.Bindings )
        {
            if ( actor.Kind != LevelTL::BindingKind::Entity )
                continue;
            const auto entity = ECS::BoneActorEntity( sequence, registry, actor.Guid );
            if ( !entity )
                continue;
            // UE: "+ Track ▸ Control Rig ▸ FK" offers the rig's bones; a bound bone is keyed, not added.
            for ( const std::string& bone : ECS::ActorBoneNames( registry, entity.GetValue() ) )
            {
                const LevelTL::BindingGuid bound = ECS::FindBoneBinding( sequence, actor.Guid, bone );
                if ( bound.IsNull() )
                {
                    actions.push_back( DocumentAction{ std::format( "Add Bone Track {} {}", actor.Label, bone ),
                                                       [this, guid = actor.Guid, name = bone]
                                                       { (void)AddLevelBoneTrack( guid, name ); } } );
                    continue;
                }
                actions.push_back( DocumentAction{ std::format( "Key Bone {} {}", actor.Label, bone ),
                                                   [this, bound] { (void)KeyLevelBone( bound ); } } );
                actions.push_back( DocumentAction{ std::format( "Remove Bone Track {} {}", actor.Label, bone ),
                                                   [this, bound] { (void)RemoveLevelBoneTrack( bound ); } } );
            }
        }
    }

    void SequencerPanel::DrawLevelBoneMenu( const LevelTL::Sequence& sequence, const LevelTL::BindingGuid& actor )
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;
        auto&      registry = scene->GetRegistry();
        const auto entity   = ECS::BoneActorEntity( sequence, registry, actor );
        if ( !entity )
            return;
        const auto bones = ECS::ActorBoneNames( registry, entity.GetValue() );
        if ( bones.empty() || !ImGui::BeginMenu( ICON_MDI_BONE " Bone" ) )
            return;
        for ( const std::string& bone : bones )
            if ( ImGui::MenuItem( bone.c_str(), nullptr, false,
                                  ECS::FindBoneBinding( sequence, actor, bone ).IsNull() ) )
                (void)AddLevelBoneTrack( actor, bone );
        ImGui::EndMenu();
    }

    void SequencerPanel::DrawLevelBoneRows( LevelTL::Sequence& sequence, const LevelTL::BindingGuid& actor,
                                            const float contentX0, const float laneX0, const float laneW )
    {
        // Copied: a row's button may remove a binding and reallocate the vector.
        std::vector<LevelTL::Binding> bones;
        for ( const LevelTL::Binding& binding : sequence.Bindings )
            if ( binding.Kind == LevelTL::BindingKind::Bone && binding.Parent == actor )
                bones.push_back( binding );
        const float rowH = ImGui::GetFrameHeight();
        for ( const LevelTL::Binding& bone : bones )
        {
            ImGui::PushID( bone.Locator.c_str() );
            const float rowY = ImGui::GetCursorScreenPos().y;
            ImGui::SetCursorScreenPos( ImVec2( contentX0 + 16.0f, rowY ) );
            const bool picked = m_LevelSelBone == bone.Guid;
            // The row's name picks the bone for the viewport's bone gizmo (UE: selecting a control).
            if ( ImGui::Selectable( std::format( ICON_MDI_BONE " {}##LevelBone", bone.Locator ).c_str(), picked,
                                    ImGuiSelectableFlags_None,
                                    ImVec2( std::max( 40.0f, m_LevelNameWidth - 20.0f ), 0.0f ) ) )
                SelectLevelBone( bone.Guid );
            SameLineAtLevelControls( contentX0 );
            if ( ImGui::SmallButton( ICON_MDI_KEY " Key" ) )
                (void)KeyLevelBone( bone.Guid );
            if ( const auto blend = ECS::BoneSectionBlendOf( sequence, bone.Guid ) )
            {
                ImGui::SameLine();
                int type = blend.GetValue().Blend == LevelTL::SectionBlendType::Additive ? 1 : 0;
                ImGui::SetNextItemWidth( 90.0f );
                if ( ImGui::Combo( "##LevelBoneBlend", &type, "Absolute\0Additive\0" ) )
                    (void)SetLevelBoneProperty(
                         LevelBoneEdit::PropertyName( LevelTL::FindBinding( sequence, actor )->Label, bone.Locator,
                                                      "Blend" ),
                         { static_cast<float>( type ) } );
                ImGui::SameLine();
                // Committed on Enter: InputFloat keeps its own text while it is edited, so one edit is one step.
                float weight = blend.GetValue().Weight;
                ImGui::SetNextItemWidth( 56.0f );
                if ( ImGui::InputFloat( "##LevelBoneWeight", &weight, 0.0f, 0.0f, "%.2f",
                                        ImGuiInputTextFlags_EnterReturnsTrue ) )
                    (void)SetLevelBoneProperty(
                         LevelBoneEdit::PropertyName( LevelTL::FindBinding( sequence, actor )->Label, bone.Locator,
                                                      "Weight" ),
                         { weight } );
            }
            ImGui::SameLine();
            if ( ImGui::SmallButton( ICON_MDI_DELETE ) )
                (void)RemoveLevelBoneTrack( bone.Guid );
            DrawLevelKeyLane( sequence, bone.Guid, laneX0, laneW, rowY, rowH );
            ImGui::SetCursorScreenPos( ImVec2( contentX0, rowY + rowH + 4.0f ) );
            ImGui::PopID();
        }
    }

    void SequencerPanel::SelectLevelBone( const LevelTL::BindingGuid& bone )
    {
        auto& live = Core::ActiveAuthoringContext();
        if ( m_LevelSelBone == bone )
        {
            // Picking the picked row again gives the gizmo back to the object.
            m_LevelSelBone  = {};
            m_LevelBoneHeld = false;
            live.Focus( m_AuthoringOwner, m_Authoring );
            (void)live.SetMode( m_AuthoringOwner, m_Authoring, Core::AuthoringMode::Object );
            return;
        }
        const auto asset = ResolveLevelAsset();
        const auto scene = m_Scene.lock();
        if ( !asset || !scene )
            return;
        auto&      registry = scene->GetRegistry();
        const auto target   = ResolveBone( asset->GetSequence(), registry, bone );
        if ( !target )
        {
            (void)Refused( "Select Bone", target.GetError() );
            return;
        }
        const auto* uuid = registry.try_get<ECS::UUIDComponent>( target.GetValue().Entity );
        if ( uuid == nullptr )
            return;
        // The viewport's bone gizmo poses the SELECTED entity's selected bone in Pose mode (GizmoController::
        // RenderBone): the actor is selected and this document's context is about it.
        Core::SelectionManager::SetSelected( uuid->UUID );
        m_Authoring.Entity = uuid->UUID;
        live.Focus( m_AuthoringOwner, m_Authoring );
        const auto mode = live.SetMode( m_AuthoringOwner, m_Authoring, Core::AuthoringMode::Pose );
        const auto pick = live.SetSelectedBone( m_AuthoringOwner, m_Authoring, target.GetValue().Index );
        if ( !mode || !pick )
        {
            LOG_WARN( "[Sequencer] bone selection refused: {}", !mode ? mode.GetError() : pick.GetError() );
            return;
        }
        m_LevelSelBone  = bone;
        m_LevelBoneHeld = false;
    }

    void SequencerPanel::UpdateLevelBoneGizmo( LevelTL::Sequence& sequence )
    {
        if ( m_LevelSelBone.IsNull() )
            return;
        const auto scene = m_Scene.lock();
        if ( !scene )
            return;
        auto&       registry = scene->GetRegistry();
        const auto  target   = ResolveBone( sequence, registry, m_LevelSelBone );
        const auto& live     = Core::ActiveAuthoringContext();
        const auto* uuid     = target ? registry.try_get<ECS::UUIDComponent>( target.GetValue().Entity ) : nullptr;
        // The row is picked only while the editor still poses that bone: another surface taking the context,
        // or the bone track gone, drops the pick.
        if ( uuid == nullptr || !live.IsPoseAuthoring() || live.Entity() != uuid->UUID ||
             live.SelectedBoneIndex() != static_cast<int>( target.GetValue().Index ) )
        {
            m_LevelSelBone  = {};
            m_LevelBoneHeld = false;
            return;
        }
        const bool held      = Core::GizmoState::PoseInteraction();
        const bool releasing = m_LevelBoneHeld && !held;
        m_LevelBoneHeld      = held;
        if ( held )
            return;
        Animation::Animator* animator = target.GetValue().Animator;
        if ( releasing && m_LevelRecord )
        {
            // UE Auto Key: the release of a control keys it, one undo step.
            const std::string& name  = LevelTL::FindBinding( sequence, m_LevelSelBone )->Locator;
            const auto         under = ECS::BonePreLayerPose( registry, target.GetValue().Entity, name );
            // The buffer itself, not its matrix: the matrix round trip is not the identity (Animator.hpp).
            const Animation::BoneTransform dragged = animator->GetAuthoringPose()[target.GetValue().Index];
            if ( !under )
                (void)Refused( "Auto Key Bone", under.GetError() );
            else
            {
                const ScopedSequenceEdit undoStep( m_LevelEdit, LevelOwner() );
                if ( const auto keyed =
                          ECS::SetBoneKey( sequence, m_LevelSelBone, m_LevelTick, dragged, under.GetValue() );
                     !keyed )
                    (void)Refused( "Auto Key Bone", keyed.GetError() );
            }
        }
        // Between drags the gizmo's buffer is the pose on screen, so a grab starts from what the viewer sees
        // (the authoring pose is a separate buffer, Animator::ApplyLocalPose).
        const size_t         count = animator->GetSkeleton().GetBones().size();
        Animation::LocalPose shown( count );
        for ( uint32_t i = 0; i < count; ++i )
            shown[i] = animator->GetPublishedLocal( i );
        if ( const auto seeded = animator->SetAuthoringPose( shown ); !seeded )
            LOG_WARN( "[Sequencer] the bone gizmo cannot start from the shown pose: {}", seeded.GetError() );
    }
} // namespace Desert::Editor
