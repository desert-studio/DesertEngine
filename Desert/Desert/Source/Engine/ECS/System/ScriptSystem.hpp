#pragma once

#include <Engine/ECS/System/System.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Input.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/UI/UIDataStore.hpp>

#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::ECS
{
    // Runs entity Lua scripts (Play only): loads each ScriptComponent's file once (OnStart) then calls
    // OnUpdate(dt) per frame. Also owns the per-frame INPUT plumbing the scripts read — cursor capture and the
    // mouse delta — which used to live in PhysicsECSSystem (it now only executes the move intent the scripts
    // set). Left Alt toggles the cursor free so you can click Stop / the editor UI.
    // DOES NOT HONOUR VisibilityComponent, AND MUST NOT: it runs gameplay Lua and owns the frame's cursor
    // capture. A hidden entity whose script stops running is a behaviour change, not a drawing one.
    // Verdict and mutation gate: Desert/Tests/Engine/VisibilityHonoured.
    class ScriptSystem final : public System
    {
    public:
        explicit ScriptSystem( Core::Scene* scene, Assets::AssetManager* assetManager = nullptr )
             : m_Scene( scene ), m_Engine( scene, assetManager )
        {
        }

        ~ScriptSystem() override
        {
            // Don't leave a dangling on_destroy listener pointing at this (freed) system.
            if ( m_HookedRegistry )
                m_HookedRegistry->on_destroy<ScriptComponent>().disconnect( this );
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer&,
                     const Common::Timestep& ts ) override
        {
            // Release an entity's Lua envs the instant its ScriptComponent dies (entity destroyed, component
            // removed, or scene cleared). Without this the env map leaks AND — since entt recycles entity ids
            // — a NEW entity could inherit a destroyed one's stale env. Event-driven (entt keeps the connection
            // across registry.clear(), so wire it once; re-arm only if the registry object itself changes).
            EnsureDestroyHook( registry );

            using SceneState = Core::Scene::SceneState;
            const bool playing = m_Scene && m_Scene->GetState() == SceneState::Play;

            if ( !playing )
            {
                if ( m_CursorLocked ) // give the cursor back when not playing
                {
                    Input::Mouse::Get().SetCursorMode( Input::MouseState::Visible );
                    m_CursorLocked = false;
                }
                m_LookSuspended = false; // next Play starts captured again
                return;
            }

            // Hot-reload: a saved .lua re-loads its slots live (same mtime-polling approach as
            // the material/shader watcher). Throttled; scripts keep running between polls.
            PollScriptFiles( registry, ts );

            // Advance edge-detection state so Input.wasPressed() fires exactly on the press transition.
            m_Engine.NewInputFrame();

            // ---- Per-frame input plumbing exposed to scripts (Input.mouseDelta()) ----
            // ESCAPE TOGGLES the cursor free/captured (edge-detected) — so you can always free it to click
            // Stop / the UI. (Left Alt is avoided: on Windows it grabs the window's system menu and is
            // unreliable in GLFW.) Captured = smooth unbounded look.
            const bool toggleDown = Input::Keyboard::IsKeyPressed( Common::KeyCode::Escape );
            if ( toggleDown && !m_AltPrev )
                m_LookSuspended = !m_LookSuspended;
            m_AltPrev = toggleDown;

            const bool wantLock = !m_LookSuspended;
            bool       toggled  = false;
            if ( wantLock != m_CursorLocked )
            {
                Input::Mouse::Get().SetCursorMode( wantLock ? Input::MouseState::Locked
                                                            : Input::MouseState::Visible );
                m_CursorLocked = wantLock;
                toggled        = true; // GLFW recenters on lock → skip this frame's delta to avoid a jump
            }

            const auto      mp = Input::Mouse::Get().GetMousePosition();
            const glm::vec2 mouseNow( mp.first, mp.second );
            const glm::vec2 delta = ( wantLock && !toggled ) ? ( mouseNow - m_LastMouse ) : glm::vec2( 0.0f );
            m_LastMouse           = mouseNow;
            m_Engine.SetFrameMouseDelta( delta.x, delta.y );

            // ---- Run the scripts (each entity may run several script SLOTS, like UE ActorComponents) ----
            auto view = registry.view<ScriptComponent>();
            for ( auto entity : view )
            {
                auto&          sc = view.get<ScriptComponent>( entity );
                const uint32_t id = static_cast<uint32_t>( entity );

                // Keep the engine's per-entity env list in lockstep with the component's slot count (a removed
                // slot drops its env, so indices stay aligned).
                m_Engine.TrimSlots( id, static_cast<uint32_t>( sc.Scripts.size() ) );

                for ( uint32_t slot = 0; slot < sc.Scripts.size(); ++slot )
                {
                    auto& script = sc.Scripts[slot];
                    if ( script.ScriptKey.empty() )
                        continue;

                    if ( !script.Started )
                    {
                        script.Started = true; // set first so a load error doesn't retry-spam every frame
                        // The KEY names the script; the PATH is what an ifstream can open, and the two
                        // differ in a packaged game because ASSETS_PATH is remapped there. Resolved once,
                        // here, and both the loader and its error message get the same answer - an error
                        // naming the key would send the reader looking for a file spelled "assets:...".
                        const std::string file   = script.ResolvedPath().generic_string();
                        auto              loaded = m_Engine.LoadEntityScript( id, slot, file );
                        if ( !loaded )
                        {
                            LOG_ERROR( "[Script] failed to load '{}' (from '{}'): {}", file, script.ScriptKey,
                                       loaded.GetError() );
                            continue;
                        }
                        m_Engine.ApplyProperties( id, slot, script.Properties ); // editor overrides -> env
                        m_Engine.CallStart( id, slot );
                    }
                    // Re-apply every frame so editing a property in Details updates the running script LIVE.
                    m_Engine.ApplyProperties( id, slot, script.Properties );
                    m_Engine.CallUpdate( id, slot, ts.GetSeconds() );
                }

                // Dispatch animation notifies (footstep, hit-frame, ...) queued this frame by the Animator to
                // EVERY started slot as OnAnimationNotify(name), then clear so each fires exactly once.
                if ( registry.has<AnimationComponent>( entity ) )
                {
                    auto& anim = registry.get<AnimationComponent>( entity );
                    if ( !anim.PendingNotifies.empty() )
                    {
                        for ( const auto& notify : anim.PendingNotifies )
                            for ( uint32_t slot = 0; slot < sc.Scripts.size(); ++slot )
                                if ( !sc.Scripts[slot].ScriptKey.empty() && sc.Scripts[slot].Started )
                                    m_Engine.CallAnimationNotify( id, slot, notify );
                        anim.PendingNotifies.clear();
                    }
                }
            }

            // Deliver whatever the canvas raised this frame (button actions, pointer events, drops) to
            // every script defining OnUIMessage. Drained here rather than pushed by the UI, so the canvas
            // stays unaware that scripting exists — and so a message queued while paused isn't lost.
            for ( const std::string& msg : UI::UIMessageQueue::Get().Drain() )
                m_Engine.BroadcastUIMessage( msg );

            // Fire due Timer.after callbacks (scheduled by OnStart/OnUpdate/earlier timers). Game
            // time only — pausing Play pauses the timers because this Update early-outs above.
            m_Engine.TickTimers( ts.GetSeconds() );

            // A script may have requested cursor lock/unlock (Input.lockCursor/showCursor). Apply it so it
            // cooperates with the Escape toggle (also keeps m_LookSuspended in sync for the next Escape press).
            if ( auto req = m_Engine.ConsumeCursorLockRequest() )
            {
                m_LookSuspended = !*req;
                if ( *req != m_CursorLocked )
                {
                    Input::Mouse::Get().SetCursorMode( *req ? Input::MouseState::Locked
                                                            : Input::MouseState::Visible );
                    m_CursorLocked = *req;
                }
            }
        }

    private:
        // Polls the mtimes of every script file referenced by a running slot; on change, flags
        // the slot for re-load (Started=false -> next frame: fresh env + OnStart + properties).
        // Errors surface through the normal load path (Logs panel) and never kill the session.
        void PollScriptFiles( entt::registry& registry, const Common::Timestep& ts )
        {
            m_ScriptPollAccum += ts.GetSeconds();
            if ( m_ScriptPollAccum < 0.7f )
                return;
            m_ScriptPollAccum = 0.0f;

            auto view = registry.view<ScriptComponent>();
            for ( auto entity : view )
            {
                auto& sc = view.get<ScriptComponent>( entity );
                for ( auto& script : sc.Scripts )
                {
                    if ( script.ScriptKey.empty() )
                        continue;

                    // The mtime is a property of the FILE, so it is probed on the resolved path; the map
                    // is keyed on the KEY, which is the one spelling that does not change under a project
                    // remap - two slots naming one script share a row whichever root is mounted.
                    std::error_code ec;
                    const auto      mtime = std::filesystem::last_write_time( script.ResolvedPath(), ec );
                    if ( ec )
                        continue;

                    auto it = m_ScriptTimes.find( script.ScriptKey );
                    if ( it == m_ScriptTimes.end() )
                    {
                        m_ScriptTimes[script.ScriptKey] = mtime; // baseline
                        continue;
                    }
                    if ( it->second == mtime )
                        continue;
                    it->second = mtime;

                    script.Started = false; // re-load + OnStart on the next frame
                    LOG_INFO( "[HotReload] Script '{}' reloading", script.ScriptKey );
                }
            }
        }

        // Connects the on_destroy<ScriptComponent> listener to `registry` once (re-arms if the registry object
        // changes, e.g. a brand-new scene). Cheap no-op on every subsequent frame.
        void EnsureDestroyHook( entt::registry& registry )
        {
            if ( m_HookedRegistry == &registry )
                return;
            if ( m_HookedRegistry )
                m_HookedRegistry->on_destroy<ScriptComponent>().disconnect( this );
            registry.on_destroy<ScriptComponent>().connect<&ScriptSystem::OnScriptComponentDestroyed>( this );
            m_HookedRegistry = &registry;
        }

        void OnScriptComponentDestroyed( entt::registry&, entt::entity entity )
        {
            m_Engine.Release( static_cast<uint32_t>( entity ) );
        }

    private:
        Core::Scene*            m_Scene = nullptr;
        Scripting::ScriptEngine m_Engine;
        entt::registry*         m_HookedRegistry = nullptr; // registry our on_destroy listener is wired to

        glm::vec2 m_LastMouse{ 0.0f, 0.0f };
        bool      m_CursorLocked  = false;
        bool      m_LookSuspended = false;
        bool      m_AltPrev       = false;

        // Script hot-reload state (mtime polling, throttled).
        std::unordered_map<std::string, std::filesystem::file_time_type> m_ScriptTimes;
        float                                                            m_ScriptPollAccum = 0.0f;
    };
} // namespace Desert::ECS
