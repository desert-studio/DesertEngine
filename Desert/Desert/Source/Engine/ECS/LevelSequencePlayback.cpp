#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <Engine/ECS/Components.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <algorithm>
#include <charconv>
#include <format>

namespace Desert::ECS
{
    namespace
    {
        namespace T = Animation::Timeline;

        [[nodiscard]] entt::entity EntityOf( const T::ResolvedBinding& target )
        {
            return static_cast<entt::entity>( static_cast<uint32_t>( target.Handle ) );
        }

        [[nodiscard]] const char* KindName( const T::EvaluatedValue& value )
        {
            constexpr const char* kNames[] = { "float", "vector", "rotation", "transform", "bool" };
            return kNames[value.index()];
        }
    } // namespace

    LevelSequenceEntityHost::LevelSequenceEntityHost( entt::registry& registry, const LevelSequenceComponent& component )
         : m_Registry( registry ), m_Component( component )
    {
    }

    std::optional<T::ResolvedBinding> LevelSequenceEntityHost::Resolve( const T::Binding& binding )
    {
        if ( binding.Kind != T::BindingKind::Entity )
            return std::nullopt;

        Common::UUID wanted = Common::UUID::Null();
        for ( const LevelSequenceBindingOverride& over : m_Component.BindingOverrides )
            if ( over.Binding == binding.Guid )
                wanted = over.Entity;
        if ( wanted.IsNull() )
        {
            uint64_t   value = 0;
            const auto end   = binding.Locator.data() + binding.Locator.size();
            const auto read  = std::from_chars( binding.Locator.data(), end, value );
            if ( read.ec != std::errc() || read.ptr != end || value == 0 )
                return std::nullopt;
            wanted = Common::UUID( value );
        }

        for ( const auto entity : m_Registry.view<UUIDComponent>() )
            if ( static_cast<uint64_t>( m_Registry.get<UUIDComponent>( entity ).UUID ) == static_cast<uint64_t>( wanted ) )
                return T::ResolvedBinding{ static_cast<uint64_t>( static_cast<uint32_t>( entity ) ) };
        return std::nullopt;
    }

    void LevelSequenceEntityHost::Apply( const T::ResolvedBinding& target, std::string_view property,
                                         const T::EvaluatedValue& value )
    {
        const entt::entity entity = EntityOf( target );
        if ( !m_Registry.valid( entity ) )
        {
            m_Refusals.push_back( std::format( "track '{}': its entity no longer exists", property ) );
            return;
        }

        const auto refuse = [&]
        {
            m_Refusals.push_back( std::format( "track '{}' ({}): no entity property of that name and kind", property,
                                               KindName( value ) ) );
        };

        if ( property == "Visible" )
        {
            const bool* visible = std::get_if<bool>( &value );
            if ( visible == nullptr )
                return refuse();
            auto& component = m_Registry.has<VisibilityComponent>( entity )
                                   ? m_Registry.get<VisibilityComponent>( entity )
                                   : m_Registry.emplace<VisibilityComponent>( entity );
            component.Visible = *visible;
            return;
        }

        if ( !m_Registry.has<TransformComponent>( entity ) )
        {
            m_Refusals.push_back( std::format( "track '{}': its entity has no Transform", property ) );
            return;
        }
        auto& transform = m_Registry.get<TransformComponent>( entity );

        if ( property == "Transform" )
        {
            const auto* bone = std::get_if<Animation::BoneTransform>( &value );
            if ( bone == nullptr )
                return refuse();
            transform.Translation = bone->Translation;
            transform.Rotation    = glm::eulerAngles( bone->Rotation );
            transform.Scale       = bone->Scale;
        }
        else if ( property == "Translation" || property == "Location" || property == "Scale" )
        {
            const auto* vector = std::get_if<glm::vec3>( &value );
            if ( vector == nullptr )
                return refuse();
            ( property == "Scale" ? transform.Scale : transform.Translation ) = *vector;
        }
        else if ( property == "Rotation" )
        {
            const auto* rotation = std::get_if<glm::quat>( &value );
            if ( rotation == nullptr )
                return refuse();
            transform.Rotation = glm::eulerAngles( *rotation );
        }
        else
            refuse();
    }

    void LevelSequenceEntityHost::Fire( const T::FiredEvent& event )
    {
        m_Fired.push_back( event.Event.Key != nullptr ? event.Event.Key->Name : std::string() );
    }

    void LevelSequenceEntityHost::SetCamera( const std::optional<T::ResolvedBinding>& camera )
    {
        if ( !camera )
        {
            m_CameraCut.reset();
            return;
        }
        const entt::entity entity = EntityOf( *camera );
        if ( !m_Registry.valid( entity ) || !m_Registry.has<CameraComponent>( entity ) )
        {
            m_Refusals.push_back( "Camera Cut: the bound entity has no Camera component" );
            m_CameraCut.reset();
            return;
        }
        m_CameraCut = entity;
    }

    void LevelSequenceEntityHost::PlayAnimation( const T::ResolvedBinding&, const T::AnimationSample& sample )
    {
        // Driving an entity's Animator from a sequence section is the Animator host's seam (Animator.hpp is
        // outside this change); until it lands the section is REFUSED by name, not dropped.
        m_Refusals.push_back( std::format( "Animation section of clip {}: an entity's Animator is not driven by a "
                                           "LevelSequence yet",
                                           Common::Content::AssetGuidToText( sample.Clip ) ) );
    }

    LevelSequencePlayback::LevelSequencePlayback( const T::Sequence& sequence )
         : Player( sequence.TickRate, sequence.Start, sequence.End ), Evaluator( sequence )
    {
    }

    LevelSequenceStep StepLevelSequence( entt::registry& registry, const LevelSequenceComponent& component,
                                         LevelSequencePlayback& playback, const T::TimeStep& step )
    {
        playback.Evaluator.Evaluate( step, playback.Frame );
        LevelSequenceEntityHost host( registry, component );
        LevelSequenceStep       out;
        out.Report      = playback.Evaluator.Apply( playback.Frame, host );
        out.CameraCut   = host.CameraCut();
        out.Refusals    = host.Refusals();
        out.FiredEvents = host.FiredEvents();
        return out;
    }
    std::vector<std::string> TakeNewLevelSequenceErrors( LevelSequenceActorState& state, const LevelSequenceStep& step )
    {
        std::vector<std::string> fresh;
        const auto               take = [&]( std::string message )
        {
            if ( state.Reported.insert( message ).second )
                fresh.push_back( std::move( message ) );
        };
        for ( const auto& label : step.Report.Unresolved )
            take( std::format( "binding '{}' names no entity of this scene; its tracks are not applied", label ) );
        for ( const auto& refusal : step.Refusals )
            take( refusal );
        return fresh;
    }

    std::optional<entt::entity> LevelSequenceViewTarget( LevelSequenceActorState& state, const LevelSequenceStep& step,
                                                         entt::entity current )
    {
        if ( step.CameraCut )
        {
            if ( !state.CutInForce )
            {
                state.TargetBeforeCut = current;
                state.CutInForce      = true;
            }
            return *step.CameraCut;
        }
        if ( !state.CutInForce )
            return std::nullopt;
        state.CutInForce = false;
        return state.TargetBeforeCut;
    }
    std::vector<const T::Binding*> OverridableBindings( const T::Sequence&            sequence,
                                                        const LevelSequenceComponent& component )
    {
        std::vector<const T::Binding*> bindings;
        for ( const T::Binding& binding : sequence.Bindings )
        {
            if ( binding.Kind != T::BindingKind::Entity )
                continue;
            const bool overridden = std::any_of(
                 component.BindingOverrides.begin(), component.BindingOverrides.end(),
                 [&]( const LevelSequenceBindingOverride& over ) { return over.Binding == binding.Guid; } );
            if ( !overridden )
                bindings.push_back( &binding );
        }
        return bindings;
    }
} // namespace Desert::ECS
