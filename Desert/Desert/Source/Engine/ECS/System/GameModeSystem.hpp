#pragma once

#include <Engine/Core/GameMode.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/System.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::ECS
{
    // Ticks the played world's GameMode (Core/GameMode.hpp): the respawn delay counts down in game time, and the
    // restart spawns the level's Default Pawn prefab at its PlayerStart (Core::SpawnPawnPrefabAt). Registered
    // right AFTER ScriptSystem by both hosts, so a gameMode.kill made by a script this frame is seen this frame;
    // the events it raises reach the scripts at the start of the next ScriptSystem update.
    class GameModeSystem final : public System
    {
    public:
        GameModeSystem( Core::Scene* scene, Assets::AssetManager* assetManager )
             : m_Scene( scene ), m_AssetManager( assetManager )
        {
        }

        void Update( entt::registry&, Graphic::Render::RenderCommandBuffer&, const Common::Timestep& ts ) override
        {
            if ( m_Scene == nullptr || m_AssetManager == nullptr || !m_Scene->TicksGameplay() )
                return;
            const Assets::AssetManager& assets = *m_AssetManager;
            const auto                  ticked = m_Scene->GetGameMode().Tick(
                 *m_Scene, ts.GetSeconds(),
                 [&assets]( Core::Scene& scene, const glm::mat4& at ) -> Common::ResultStr<entt::entity>
                 {
                     const auto pawn = Core::SpawnPawnPrefabAt( scene, assets, at );
                     if ( !pawn )
                         return Common::MakeError<entt::entity>( pawn.GetError() );
                     return Common::MakeSuccess( pawn.GetValue().GetHandle() );
                 } );
            if ( !ticked )
                LOG_ERROR( "[GameMode] {}", ticked.GetError() );
        }

    private:
        Core::Scene*          m_Scene        = nullptr;
        Assets::AssetManager* m_AssetManager = nullptr;
    };
} // namespace Desert::ECS
