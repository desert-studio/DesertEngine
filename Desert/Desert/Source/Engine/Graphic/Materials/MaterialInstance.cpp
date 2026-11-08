#include "MaterialInstance.hpp"
#include "Material.hpp"

#include <Engine/Core/RenderTask.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // THE INSTANCES WRITTEN WHILE A RENDER JOB WAS IN FLIGHT (MaterialInstance, THE RENDER COPY). A write from
        // the tick no longer waits for the job that reads the render copy (it used to join it: the fence of
        // OVERLAP-ON-2): the instance is listed here and published at the next hand-off. The writer may be the
        // game thread or a worker of the tick, the destructor any thread, so the list is under a mutex; `Any`
        // lets the hand-off and the destructor skip the lock when nothing is pending.
        struct PendingRenderWrites
        {
            std::mutex                     Mutex;
            std::vector<MaterialInstance*> Instances;
            std::atomic<bool>              Any{ false };
        };

        // Never destroyed: an instance may outlive static destruction (a shared_ptr held by another static).
        PendingRenderWrites& PendingWrites()
        {
            static auto* const pending = new PendingRenderWrites();
            return *pending;
        }

        // MaterialInstance::RenderPublishGeneration: any thread may publish (the render task's own writes, the
        // hand-off), a view reads it at its sync.
        std::atomic<uint64_t>& RenderPublishCounter() noexcept
        {
            static std::atomic<uint64_t> counter{ 1 };
            return counter;
        }
        void NoteRenderPublish() noexcept
        {
            RenderPublishCounter().fetch_add( 1, std::memory_order_relaxed );
        }
    } // namespace

    uint64_t MaterialInstance::RenderPublishGeneration() noexcept
    {
        return RenderPublishCounter().load( std::memory_order_relaxed );
    }

    void MaterialInstance::SetTwoSidedOverride( const std::optional<bool> twoSided ) noexcept
    {
        if ( m_TwoSidedOverride == twoSided )
            return;
        m_TwoSidedOverride = twoSided;
        NoteRenderPublish();
    }

    MaterialInstance::MaterialInstance( Material* parentMaterial, const std::string& name )
         : m_ParentMaterial( parentMaterial ), m_Name( name )
    {
        DESERT_VERIFY( parentMaterial, "Parent material cannot be null" );
    }

    MaterialInstance::~MaterialInstance()
    {
        // `Any` is false unless some instance is pending; when THIS one is, the thread dropping its last owner
        // sees the store that listed it (the owners' release/acquire), so a false here means not listed.
        PendingRenderWrites& pending = PendingWrites();
        if ( !pending.Any.load( std::memory_order_acquire ) )
            return;
        const std::lock_guard<std::mutex> lock( pending.Mutex );
        if ( !m_bRenderPending )
            return;
        const auto at = std::find( pending.Instances.begin(), pending.Instances.end(), this );
        if ( at != pending.Instances.end() )
            pending.Instances.erase( at );
        m_bRenderPending = false;
    }

    void MaterialInstance::ParametersWritten()
    {
        // No job in flight: nothing reads the render copy concurrently (serial mode, the replay after a view's
        // Join, a preview, a bake), publish now - the order every caller had before the render task. The render
        // task's own write is sequential with its own reads. A worker of the render job would be deferred one
        // hand-off (none writes a parameter: the writers are the tick, the interface and the asset services).
        const ::Desert::Engine::RenderTask& task = ::Desert::Engine::RenderTask::Get();
        if ( !task.InFlight() || ::Desert::Engine::RenderTask::OnRenderThread() )
        {
            Publish();
            return;
        }
        PendingRenderWrites&              pending = PendingWrites();
        const std::lock_guard<std::mutex> lock( pending.Mutex );
        if ( m_bRenderPending )
            return;
        m_bRenderPending = true;
        pending.Instances.push_back( this );
        pending.Any.store( true, std::memory_order_release );
    }

    void MaterialInstance::Publish()
    {
        m_RenderProperties.CopyFrom( m_Properties );
        NoteRenderPublish();
        m_bNeedsApply = true;
        // A child binds from its own copy, but re-applies after its parent's write (today's propagation).
        for ( auto& child : m_ChildInstances )
            child->m_bNeedsApply = true;
    }

    void MaterialInstance::PublishPending()
    {
        PendingRenderWrites& pending = PendingWrites();
        if ( !pending.Any.load( std::memory_order_acquire ) )
            return;
        // A view recorded inside a job (InFlight on the render task): the game thread may be writing the game
        // copies right now, so the next hand-off on the game thread publishes them.
        if ( ::Desert::Engine::RenderTask::Get().InFlight() )
            return;
        const std::lock_guard<std::mutex> lock( pending.Mutex );
        for ( MaterialInstance* instance : pending.Instances )
        {
            instance->m_bRenderPending = false;
            instance->Publish();
        }
        pending.Instances.clear();
        pending.Any.store( false, std::memory_order_release );
    }

    float MaterialInstance::GetFloat( const std::string& name, float defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<float>( value ) )
            return std::get<float>( value );
        if ( std::holds_alternative<int>( value ) )
            return static_cast<float>( std::get<int>( value ) );
        return defaultValue;
    }

    int MaterialInstance::GetInt( const std::string& name, int defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<int>( value ) )
            return std::get<int>( value );
        if ( std::holds_alternative<float>( value ) )
            return static_cast<int>( std::get<float>( value ) );
        return defaultValue;
    }

    bool MaterialInstance::GetBool( const std::string& name, bool defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<bool>( value ) )
            return std::get<bool>( value );
        return defaultValue;
    }

    glm::vec2 MaterialInstance::GetVec2( const std::string& name, const glm::vec2& defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<glm::vec2>( value ) )
            return std::get<glm::vec2>( value );
        return defaultValue;
    }

    glm::vec3 MaterialInstance::GetVec3( const std::string& name, const glm::vec3& defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<glm::vec3>( value ) )
            return std::get<glm::vec3>( value );
        return defaultValue;
    }

    glm::vec4 MaterialInstance::GetVec4( const std::string& name, const glm::vec4& defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<glm::vec4>( value ) )
            return std::get<glm::vec4>( value );
        return defaultValue;
    }

    glm::mat4 MaterialInstance::GetMat4( const std::string& name, const glm::mat4& defaultValue ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<glm::mat4>( value ) )
            return std::get<glm::mat4>( value );
        return defaultValue;
    }

    void* MaterialInstance::GetTexture( const std::string& name ) const
    {
        auto value = ResolveProperty( name );
        if ( std::holds_alternative<void*>( value ) )
            return std::get<void*>( value );
        return nullptr;
    }

    void MaterialInstance::SetFloat( const std::string& name, float value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Float );
        ParametersWritten();
    }

    void MaterialInstance::SetInt( const std::string& name, int value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Int );
        ParametersWritten();
    }

    void MaterialInstance::SetBool( const std::string& name, bool value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Bool );
        ParametersWritten();
    }

    void MaterialInstance::SetVec2( const std::string& name, const glm::vec2& value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Vec2 );
        ParametersWritten();
    }

    void MaterialInstance::SetVec3( const std::string& name, const glm::vec3& value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Vec3 );
        ParametersWritten();
    }

    void MaterialInstance::SetVec4( const std::string& name, const glm::vec4& value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Vec4 );
        ParametersWritten();
    }

    void MaterialInstance::SetMat4( const std::string& name, const glm::mat4& value )
    {
        m_Properties.SetProperty( name, value, MaterialPropertyType::Mat4 );
        ParametersWritten();
    }

    void MaterialInstance::SetTexture( const std::string& name, void* texture )
    {
        m_Properties.SetProperty( name, texture, MaterialPropertyType::Texture );
        ParametersWritten();
    }

    void
    MaterialInstance::SetParameters( const std::vector<std::pair<std::string, MaterialPropertyValue>>& params )
    {
        for ( const auto& [name, value] : params )
        {
            // Auto-detect type (simplified - you'd want proper type detection)
            if ( std::holds_alternative<float>( value ) )
                SetFloat( name, std::get<float>( value ) );
            else if ( std::holds_alternative<int>( value ) )
                SetInt( name, std::get<int>( value ) );
            else if ( std::holds_alternative<bool>( value ) )
                SetBool( name, std::get<bool>( value ) );
            else if ( std::holds_alternative<glm::vec3>( value ) )
                SetVec3( name, std::get<glm::vec3>( value ) );
            else if ( std::holds_alternative<void*>( value ) )
                SetTexture( name, std::get<void*>( value ) );
        }
    }

    void MaterialInstance::SetParameters( const MaterialPropertySet& properties )
    {
        m_Properties.CopyFrom( properties );
        ParametersWritten();
    }

    MaterialInstancePtr MaterialInstance::CreateChildInstance( const std::string& name )
    {
        auto child              = std::make_shared<MaterialInstance>( m_ParentMaterial, name );
        child->m_ParentInstance = shared_from_this();
        child->m_Properties.CopyFrom( m_Properties );
        // A fresh instance: no job holds it yet, so its render copy is written here, not published later.
        child->m_RenderProperties.CopyFrom( m_Properties );
        NoteRenderPublish();
        m_ChildInstances.push_back( child );
        return child;
    }

    bool MaterialInstance::HasParameter( const std::string& name ) const
    {
        return m_Properties.HasProperty( name );
    }

    std::optional<glm::vec4> MaterialInstance::GetOverrideAsVec4( const std::string& name ) const
    {
        if ( !m_Properties.HasProperty( name ) )
            return std::nullopt;
        const MaterialPropertyValue value = m_Properties.GetProperty( name );
        if ( const auto* f = std::get_if<float>( &value ) )
            return glm::vec4( *f, 0.0f, 0.0f, 0.0f );
        if ( const auto* i = std::get_if<int>( &value ) )
            return glm::vec4( static_cast<float>( *i ), 0.0f, 0.0f, 0.0f );
        if ( const auto* b = std::get_if<bool>( &value ) )
            return glm::vec4( *b ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f );
        if ( const auto* v2 = std::get_if<glm::vec2>( &value ) )
            return glm::vec4( *v2, 0.0f, 0.0f );
        if ( const auto* v3 = std::get_if<glm::vec3>( &value ) )
            return glm::vec4( *v3, 0.0f );
        if ( const auto* v4 = std::get_if<glm::vec4>( &value ) )
            return *v4;
        return std::nullopt;
    }

    void MaterialInstance::ClearOverride( const std::string& name )
    {
        if ( !m_Properties.RemoveProperty( name ) )
            return;
        ParametersWritten();
    }

    bool MaterialInstance::SetParamFromVec4( const std::string& name, const glm::vec4& value )
    {
        // Resolve the param's type so the vec4 is unpacked correctly. When this instance has never held the
        // param — the common case for a bare runtime instance created from a primitive + MaterialComponent
        // override (e.g. the Cornell Box walls) — fall back to the parent instance's declared type, then to
        // Vec4 (store the full value losslessly). Previously this bailed when the property was absent, which
        // silently DROPPED every such override, so those walls lost their authored albedo/roughness.
        MaterialPropertyType type = MaterialPropertyType::Vec4;
        if ( m_Properties.HasProperty( name ) )
        {
            type = m_Properties.GetPropertyType( name );
        }
        else if ( auto parent = m_ParentInstance.lock(); parent && parent->m_Properties.HasProperty( name ) )
        {
            type = parent->m_Properties.GetPropertyType( name );
        }
        else if ( m_ParentMaterial && m_ParentMaterial->GetMaterialExecutor() &&
                  m_ParentMaterial->GetMaterialExecutor()->GetShader() )
        {
            // Last authority before the lossy vec4 fallback: the shader SCHEMA's declared type.
            // Without this, a param set before it ever existed on the instance was stored as a
            // raw vec4 and silently changed type (GetFloat then read .x of a vec4 forever).
            using VT = ::Desert::Core::Formats::ShaderValueType;
            for ( const auto& p :
                  m_ParentMaterial->GetMaterialExecutor()->GetShader()->GetProgramMeta().Params )
            {
                if ( p.Name != name || p.IsTexture )
                    continue;
                switch ( p.Type )
                {
                    case VT::Float:  type = MaterialPropertyType::Float; break;
                    case VT::Float2: type = MaterialPropertyType::Vec2;  break;
                    case VT::Float3: type = MaterialPropertyType::Vec3;  break;
                    case VT::Float4: type = MaterialPropertyType::Vec4;  break;
                    case VT::Int:    type = MaterialPropertyType::Int;   break;
                    default:         type = MaterialPropertyType::Vec4;  break;
                }
                break;
            }
        }

        switch ( type )
        {
            case MaterialPropertyType::Float:
                SetFloat( name, value.x );
                return true;
            case MaterialPropertyType::Int:
                SetInt( name, static_cast<int>( value.x ) );
                return true;
            case MaterialPropertyType::Bool:
                SetBool( name, value.x != 0.0f );
                return true;
            case MaterialPropertyType::Vec2:
                SetVec2( name, glm::vec2( value ) );
                return true;
            case MaterialPropertyType::Vec3:
                SetVec3( name, glm::vec3( value ) );
                return true;
            case MaterialPropertyType::Vec4:
                SetVec4( name, value );
                return true;
            default: // Mat4 / Texture / Invalid — not vec4-packable; keep the raw vec4 so nothing is lost
                SetVec4( name, value );
                return true;
        }
    }

    void MaterialInstance::Apply()
    {
        if ( !m_bNeedsApply )
            return;

        if ( m_ParentMaterial )
            m_ParentMaterial->Bind( this );

        m_bNeedsApply = false;
    }

    MaterialPropertyValue MaterialInstance::ResolveProperty( const std::string& name ) const
    {
        // Check local override first
        if ( m_Properties.HasProperty( name ) )
            return m_Properties.GetProperty( name );

        // Check parent instance
        auto parent = m_ParentInstance.lock();
        if ( parent )
            return parent->ResolveProperty( name );

        return float( 0.0f );
    }

    bool MaterialInstance::IsTwoSided() const
    {
        // Up the instance chain to the first override; the root instance answers from its material.
        std::shared_ptr<const MaterialInstance> held;
        const MaterialInstance*                 at = this;
        while ( true )
        {
            if ( at->m_TwoSidedOverride.has_value() )
                return *at->m_TwoSidedOverride;
            auto parent = at->m_ParentInstance.lock();
            if ( parent == nullptr )
                return at->m_ParentMaterial != nullptr && at->m_ParentMaterial->IsTwoSided();
            held = std::move( parent );
            at   = held.get();
        }
    }
} // namespace Desert::Graphic