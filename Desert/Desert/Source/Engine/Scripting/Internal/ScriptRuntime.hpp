#pragma once

// INTERNAL scripting runtime — included ONLY by Engine/Scripting/*.cpp translation units.
// Pulls sol2/Lua (heavy) and defines the pieces every binding module shares: the ScriptEntity
// handle, the engine Impl, and the binder entry points. The rest of the engine still sees only
// ScriptEngine.hpp (PIMPL).
//
// BINDING ARCHITECTURE: the core (ScriptEngine.cpp) owns the VM and registers only the base
// Entity usertype (identity + transform + messaging). Every DOMAIN registers its own API in its
// own translation unit by EXTENDING the Lua state (adding Entity methods / global tables) — the
// script core has no knowledge of swimming, materials or water. Adding a new scripting domain =
// adding one Register*Bindings file and one call in the ctor; nothing else changes.

#include "../ScriptEngine.hpp"

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Math/Ray.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// sol2 + Lua are pulled in ONLY here (PIMPL) — heavy headers stay out of the rest of the engine.
#include <sol/sol.hpp>

#include <optional>
#include <unordered_map>

namespace Desert::Scripting
{
    // The per-entity Lua environment map type: entt handle -> one sandbox env PER SCRIPT SLOT. Declared here
    // so the bound ScriptEntity can reach sibling entities' scripts for cross-entity messaging (entity:call,
    // which broadcasts to every slot of the target).
    using EnvMap = std::unordered_map<uint32_t, std::vector<sol::environment>>;

        // Maps script-friendly key names to engine key codes. Single characters cover the
        // whole alphabet + digits ("G", "5"); named keys cover the common gameplay set.
        inline std::optional<Common::KeyCode> KeyFromName( const std::string& name )
        {
            using K = Common::KeyCode;
            if ( name.size() == 1 )
            {
                const char c = name[0];
                if ( c >= 'A' && c <= 'Z' )
                    return static_cast<K>( c );
                if ( c >= 'a' && c <= 'z' )
                    return static_cast<K>( c - 'a' + 'A' );
                if ( c >= '0' && c <= '9' )
                    return static_cast<K>( c ); // KeyCode::D0..D9 == ASCII '0'..'9'
            }
            if ( name == "Space" ) return K::Space;
            if ( name == "Shift" || name == "LeftShift" ) return K::LeftShift;
            if ( name == "Ctrl" || name == "LeftControl" ) return K::LeftControl;
            if ( name == "Alt" || name == "LeftAlt" ) return K::LeftAlt;
            if ( name == "Tab" ) return K::Tab;
            if ( name == "Enter" ) return K::Enter;
            if ( name == "Escape" ) return K::Escape;
            if ( name == "Left" ) return K::Left;
            if ( name == "Right" ) return K::Right;
            if ( name == "Up" ) return K::Up;
            if ( name == "Down" ) return K::Down;
            return std::nullopt;
        }

        // The keys Input.wasPressed() edge-tracks each frame (NewInputFrame). Mirrors
        // KeyFromName's coverage: full alphabet + digits + the named gameplay keys.
        inline const std::vector<Common::KeyCode>& TrackedKeys()
        {
            static const std::vector<Common::KeyCode> keys = []
            {
                std::vector<Common::KeyCode> v;
                for ( int c = 'A'; c <= 'Z'; ++c )
                    v.push_back( static_cast<Common::KeyCode>( c ) );
                for ( int c = '0'; c <= '9'; ++c )
                    v.push_back( static_cast<Common::KeyCode>( c ) );
                using K = Common::KeyCode;
                for ( K k : { K::Space, K::LeftShift, K::LeftControl, K::LeftAlt, K::Tab, K::Enter,
                              K::Escape, K::Left, K::Right, K::Up, K::Down } )
                    v.push_back( k );
                return v;
            }();
            return keys;
        }

        // Generic entity handle bound to Lua. The SAME usertype is `self`, World.find/spawn/raycast results, etc.
        // Transform/name/destroy work on any entity; the character methods (move/jump/look) no-op without a
        // CharacterControllerComponent. `scene`+`envs` let it reach components and sibling scripts.
        struct ScriptEntity
        {
            entt::entity handle = entt::null;
            Core::Scene* scene  = nullptr;
            EnvMap*      envs   = nullptr;

            entt::registry& Reg() const { return scene->GetRegistry(); }

            bool Valid() const
            {
                return scene && handle != entt::null && Reg().valid( handle );
            }

            std::string Name() const
            {
                if ( Valid() && Reg().has<ECS::TagComponent>( handle ) )
                    return Reg().get<ECS::TagComponent>( handle ).Tag;
                return {};
            }

            ECS::TransformComponent* Transform() const
            {
                if ( Valid() && Reg().has<ECS::TransformComponent>( handle ) )
                    return &Reg().get<ECS::TransformComponent>( handle );
                return nullptr;
            }

            std::tuple<float, float, float> GetPosition() const
            {
                if ( auto* t = Transform() )
                    return { t->Translation.x, t->Translation.y, t->Translation.z };
                return { 0.0f, 0.0f, 0.0f };
            }
            void SetPosition( float x, float y, float z )
            {
                if ( auto* t = Transform() )
                    t->Translation = { x, y, z };
            }
            void Translate( float dx, float dy, float dz )
            {
                if ( auto* t = Transform() )
                    t->Translation += glm::vec3( dx, dy, dz );
            }
            std::tuple<float, float, float> GetRotation() const // euler radians
            {
                if ( auto* t = Transform() )
                    return { t->Rotation.x, t->Rotation.y, t->Rotation.z };
                return { 0.0f, 0.0f, 0.0f };
            }
            void SetRotation( float x, float y, float z )
            {
                if ( auto* t = Transform() )
                    t->Rotation = { x, y, z };
            }
            std::tuple<float, float, float> GetScale() const
            {
                if ( auto* t = Transform() )
                    return { t->Scale.x, t->Scale.y, t->Scale.z };
                return { 1.0f, 1.0f, 1.0f };
            }
            void SetScale( float x, float y, float z )
            {
                if ( auto* t = Transform() )
                    t->Scale = { x, y, z };
            }

            // World-space direction vectors derived from the euler rotation (same quat convention as
            // the transform matrix). forward = -Z, right = +X.
            std::tuple<float, float, float> Forward() const
            {
                if ( auto* t = Transform() )
                {
                    const glm::vec3 f = glm::quat( t->Rotation ) * glm::vec3( 0.0f, 0.0f, -1.0f );
                    return { f.x, f.y, f.z };
                }
                return { 0.0f, 0.0f, -1.0f };
            }
            std::tuple<float, float, float> Right() const
            {
                if ( auto* t = Transform() )
                {
                    const glm::vec3 r = glm::quat( t->Rotation ) * glm::vec3( 1.0f, 0.0f, 0.0f );
                    return { r.x, r.y, r.z };
                }
                return { 1.0f, 0.0f, 0.0f };
            }

            float DistanceTo( ScriptEntity other ) const
            {
                auto* a = Transform();
                auto* b = other.Transform();
                return ( a && b ) ? glm::distance( a->Translation, b->Translation ) : -1.0f;
            }

            // Remove this entity (and its subtree) from the scene. Releases its script env too.
            void Destroy()
            {
                if ( !Valid() )
                    return;
                if ( envs )
                    envs->erase( static_cast<uint32_t>( handle ) );
                scene->DestroyEntity( ECS::Entity{ handle, Reg() } );
                handle = entt::null;
            }

            // Cross-entity message: invoke a named function in THIS entity's scripts, passing args (the caller
            // usually sends `self`). BROADCASTS to every script slot that defines `fn` (an entity may run many
            // behaviors). The mechanism behind OnInteract: the instigator raycasts then target:call().
            void Call( const std::string& fn, sol::variadic_args va )
            {
                if ( !envs )
                    return;
                auto it = envs->find( static_cast<uint32_t>( handle ) );
                if ( it == envs->end() )
                    return;
                for ( sol::environment& env : it->second )
                {
                    sol::protected_function f = env[fn];
                    if ( !f.valid() )
                        continue;
                    sol::protected_function_result r = f( sol::as_args( va ) );
                    if ( !r.valid() )
                    {
                        sol::error err = r;
                        LOG_ERROR( "[Lua] {} error: {}", fn, err.what() );
                    }
                }
            }

            static void UpsertComponentParam( ECS::MaterialComponent& mc, const std::string& name,
                                              const glm::vec4& v )
            {
                for ( auto& p : mc.Params )
                    if ( p.Name == name )
                    {
                        p.Value = v;
                        return;
                    }
                mc.Params.push_back( { name, v } );
            }

            // ── Material access (the unified material protocol: params by shader-schema name) ──
            // PBR path: writes go STRAIGHT to the slot-0 runtime MaterialInstance (live override, no
            // per-frame re-apply channel — the authored material slots stay the source of truth).
            // Custom Shader Override path: the MaterialComponent params ARE the draw state consumed
            // per draw by the generic path. Before the first instance build (Init() runs before the
            // mesh system's first tick) the write is stashed on the component; MeshECSSystem applies
            // it once at build time and clears it.
            void SetMaterialParam( const std::string& name, float x, sol::optional<float> y,
                                   sol::optional<float> z, sol::optional<float> w )
            {
                if ( !Valid() )
                    return;

                // w defaults to 1 so `self:setMaterialParam("AlbedoColor", r, g, b)` reads as a colour.
                const glm::vec4 v( x, y.value_or( 0.0f ), z.value_or( 0.0f ), w.value_or( 1.0f ) );

                // Custom-shader entities keep the component channel (it is their material state).
                if ( Reg().has<ECS::MaterialComponent>( handle ) )
                {
                    auto& mc = Reg().get<ECS::MaterialComponent>( handle );
                    if ( !mc.ShaderName.empty() && mc.ShaderName != "StaticMeshPBR" &&
                         mc.ShaderName != "SkinnedMeshPBR" )
                    {
                        UpsertComponentParam( mc, name, v );
                        return;
                    }
                }

                // PBR path: live write on the slot-0 instance.
                if ( Reg().has<ECS::StaticMeshComponent>( handle ) )
                {
                    auto& smc = Reg().get<ECS::StaticMeshComponent>( handle );
                    if ( !smc.RuntimeMaterialInstances.empty() && smc.RuntimeMaterialInstances[0] )
                    {
                        smc.RuntimeMaterialInstances[0]->SetParamFromVec4( name, v );
                        return;
                    }
                }

                // Instances not built yet — stash as a one-shot seed MeshECSSystem consumes at build.
                if ( !Reg().has<ECS::MaterialComponent>( handle ) )
                    Reg().emplace<ECS::MaterialComponent>( handle );
                UpsertComponentParam( Reg().get<ECS::MaterialComponent>( handle ), name, v );
            }

            std::tuple<float, float, float, float> GetMaterialParam( const std::string& name ) const
            {
                if ( !Valid() )
                    return { 0.0f, 0.0f, 0.0f, 0.0f };

                // Component channel first: custom-shader state and not-yet-consumed seeds live here.
                if ( Reg().has<ECS::MaterialComponent>( handle ) )
                {
                    const auto& mc = Reg().get<ECS::MaterialComponent>( handle );
                    for ( const auto& p : mc.Params )
                        if ( p.Name == name )
                            return { p.Value.x, p.Value.y, p.Value.z, p.Value.w };
                }

                // PBR path: read the live override off the slot-0 instance.
                if ( Reg().has<ECS::StaticMeshComponent>( handle ) )
                {
                    const auto& smc = Reg().get<ECS::StaticMeshComponent>( handle );
                    if ( !smc.RuntimeMaterialInstances.empty() && smc.RuntimeMaterialInstances[0] )
                    {
                        const auto& props =
                             smc.RuntimeMaterialInstances[0]->GetPropertySet().GetProperties();
                        for ( const auto& [pname, prop] : props )
                        {
                            if ( pname != name || !prop.bIsOverridden )
                                continue;
                            if ( const auto* f = std::get_if<float>( &prop.Value ) )
                                return { *f, 0.0f, 0.0f, 0.0f };
                            if ( const auto* v2 = std::get_if<glm::vec2>( &prop.Value ) )
                                return { v2->x, v2->y, 0.0f, 0.0f };
                            if ( const auto* v3 = std::get_if<glm::vec3>( &prop.Value ) )
                                return { v3->x, v3->y, v3->z, 1.0f };
                            if ( const auto* v4 = std::get_if<glm::vec4>( &prop.Value ) )
                                return { v4->x, v4->y, v4->z, v4->w };
                        }
                    }
                }
                return { 0.0f, 0.0f, 0.0f, 0.0f };
            }

            // Undo every script-set param: clears the component channel (custom-shader state +
            // unconsumed seeds) AND resets the slot-0 instance overrides, so the entity renders
            // with its authored slot materials again.
            void ClearMaterialParams()
            {
                if ( !Valid() )
                    return;
                if ( Reg().has<ECS::MaterialComponent>( handle ) )
                {
                    auto& mc = Reg().get<ECS::MaterialComponent>( handle );
                    mc.Params.clear();
                    mc.Textures.clear();
                }
                if ( Reg().has<ECS::StaticMeshComponent>( handle ) )
                {
                    auto& smc = Reg().get<ECS::StaticMeshComponent>( handle );
                    if ( !smc.RuntimeMaterialInstances.empty() && smc.RuntimeMaterialInstances[0] )
                        smc.RuntimeMaterialInstances[0]->ResetOverrides();
                }
            }

            // Assign a surface shader by name ("" -> back to the PBR slot materials). Same routing
            // as the editor's Shader Override section.
            void SetShader( const std::string& name )
            {
                if ( !Valid() )
                    return;
                if ( !Reg().has<ECS::MaterialComponent>( handle ) )
                    Reg().emplace<ECS::MaterialComponent>( handle );
                Reg().get<ECS::MaterialComponent>( handle ).ShaderName = name;
            }

            std::string GetShader() const
            {
                if ( Valid() && Reg().has<ECS::MaterialComponent>( handle ) )
                    return Reg().get<ECS::MaterialComponent>( handle ).ShaderName;
                return {};
            }

            // Socket-attach this entity to a bone of `target` (UE-style): adds a SocketAttachmentComponent so
            // AttachmentSystem makes us follow that bone each frame. e.g. weapon:attachTo(player, "hand_r").
            void AttachTo( ScriptEntity target, const std::string& bone )
            {
                if ( !Valid() || !target.Valid() )
                    return;
                Common::UUID targetId;
                if ( target.Reg().has<ECS::UUIDComponent>( target.handle ) )
                    targetId = target.Reg().get<ECS::UUIDComponent>( target.handle ).UUID;
                if ( !Reg().has<ECS::SocketAttachmentComponent>( handle ) )
                    Reg().emplace<ECS::SocketAttachmentComponent>( handle );
                auto& sa    = Reg().get<ECS::SocketAttachmentComponent>( handle );
                sa.Target   = targetId;
                sa.BoneName = bone;
            }

            void Detach()
            {
                if ( Valid() && Reg().has<ECS::SocketAttachmentComponent>( handle ) )
                    Reg().remove<ECS::SocketAttachmentComponent>( handle );
            }

            ECS::CharacterControllerComponent* Character() const
            {
                if ( Valid() && Reg().has<ECS::CharacterControllerComponent>( handle ) )
                    return &Reg().get<ECS::CharacterControllerComponent>( handle );
                return nullptr;
            }

            // forward = W/S axis, right = D/A axis (each -1..1), speed in m/s.
            void Move( float forward, float right, float speed )
            {
                if ( auto* cc = Character() )
                {
                    cc->MoveInput    = { right, forward };
                    cc->DesiredSpeed = speed;
                }
            }

            void Jump( float strength )
            {
                if ( auto* cc = Character() )
                {
                    cc->JumpRequested = true;
                    cc->JumpStrength  = strength;
                }
            }

            bool IsOnGround() const
            {
                auto* cc = Character();
                return cc ? cc->OnGround : false;
            }

            // Swimming (buoyancy): the script toggles this when the body crosses the water surface. `vertical`
            // is the swim up/down intent (+1 up, -1 down) applied while swimming.
            void SetSwimming( bool on )
            {
                if ( auto* cc = Character() )
                    cc->Swimming = on;
            }
            void Swim( float vertical )
            {
                if ( auto* cc = Character() )
                    cc->SwimVertical = vertical;
            }
            bool IsSwimming() const
            {
                auto* cc = Character();
                return cc ? cc->Swimming : false;
            }

            // Yaw turns the whole entity (body + child camera follow through the hierarchy).
            void AddYaw( float radians )
            {
                if ( auto* t = Transform() )
                    t->Rotation.y += radians;
            }

            // Pitch tilts the child camera only (look up/down), clamped to avoid flipping over.
            void AddCameraPitch( float radians )
            {
                if ( !Valid() || !Reg().has<ECS::RelationshipComponent>( handle ) )
                    return;
                auto& reg = Reg();
                for ( entt::entity child : reg.get<ECS::RelationshipComponent>( handle ).Children )
                {
                    if ( reg.has<ECS::CameraComponent>( child ) && reg.has<ECS::TransformComponent>( child ) )
                    {
                        auto& rot = reg.get<ECS::TransformComponent>( child ).Rotation;
                        rot.x     = glm::clamp( rot.x + radians, glm::radians( -85.0f ), glm::radians( 85.0f ) );
                        return;
                    }
                }
            }
        };

    struct ScriptEngine::Impl
    {
        sol::state             Lua;
        Core::Scene*           Scene  = nullptr;
        Assets::AssetManager*  Assets = nullptr;
        EnvMap                 Envs;
        float                  MouseDx = 0.0f;
        float                  MouseDy = 0.0f;

        // Input.wasPressed() edge state (down this frame & up last frame), refreshed by NewInputFrame().
        std::unordered_map<int, bool> KeyDownPrev;
        std::unordered_map<int, bool> KeyEdge;

        std::optional<bool> CursorLockRequest; // set by Input.lockCursor()/showCursor(), consumed by ScriptSystem

        // Last OnUpdate error per (entity, slot): a runtime error repeats every frame — log it
        // ONCE until it changes or the script reloads, so the Logs panel stays readable.
        std::unordered_map<uint64_t, std::string> LastUpdateError;

        // Timer.after(seconds, fn) scheduler. Each pending timer is OWNED by the (entity, slot)
        // that scheduled it, so a reload/release of that slot cancels its timers (a stale callback
        // must never fire into a replaced env). CurrentOwner is set by the engine right before it
        // enters script code (OnStart/OnUpdate/a firing timer) — that's how Timer.after knows who
        // is scheduling, and why a timer callback can re-arm itself.
        struct PendingTimer
        {
            uint64_t                Owner = 0; // SlotKey of the scheduling (entity, slot)
            float                   Remaining = 0.0f;
            sol::protected_function Fn;
        };
        std::vector<PendingTimer> Timers;
        uint64_t                  CurrentOwner = 0;

        static uint64_t SlotKey( uint32_t entity, uint32_t slot )
        {
            return ( static_cast<uint64_t>( entity ) << 32 ) | slot;
        }

        // Build a Lua-bound handle for an entt entity (carries the scene + env map so its methods work).
        ScriptEntity MakeEntity( entt::entity h ) { return ScriptEntity{ h, Scene, &Envs }; }
    };

    // ── Domain binders (one TU each; see the note above) ─────────────────────────────
    void RegisterLogBindings( ScriptEngine::Impl& impl );       // log(), Log.info/warn/error
    void RegisterEntityCoreBindings( ScriptEngine::Impl& impl );// base Entity usertype
    void RegisterCharacterBindings( ScriptEngine::Impl& impl ); // move/jump/look/swim (gameplay)
    void RegisterMaterialBindings( ScriptEngine::Impl& impl );  // unified material protocol
    void RegisterInputBindings( ScriptEngine::Impl& impl );     // Input table
    void RegisterTimerBindings( ScriptEngine::Impl& impl );     // Timer.after scheduler
    void RegisterWorldBindings( ScriptEngine::Impl& impl );     // World table (find/spawn/raycast/water)
    void RegisterReflectionBindings( ScriptEngine::Impl& impl ); // auto component access from reflection
    void RegisterAudioBindings( ScriptEngine::Impl& impl );      // Audio.play / Audio.stopAll
    void RegisterUIBindings( ScriptEngine::Impl& impl );         // ui.set/get/send + OnUIMessage bridge
    void RegisterLocalizationBindings( ScriptEngine::Impl& impl ); // loc.text/plural/number/money/date
} // namespace Desert::Scripting
