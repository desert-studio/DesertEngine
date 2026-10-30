#pragma once
/**
 * @file GameplayDebugger.hpp
 * @brief The gameplay debugger (UE's GameplayDebugger plugin): a key shows an overlay about one entity of the
 *        world - its target - made of categories, each toggled by a digit.
 *
 * ── WHAT IT IS ───────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Every Scene owns one (UE: the local player's AGameplayDebuggerCategoryReplicator). The ` key (GraveAccent; UE
 * binds ') shows and hides it, digits 1-9 toggle the categories while it shows. The target is the editor's
 * selection when the editor pushed one, else what the view's centre looks at (a ray from the active camera,
 * Scene::Raycast); with nothing under the centre the last target stays, as UE keeps its debug actor.
 *
 * Each frame it is shown: every enabled category collects when its interval ran out (or the target changed),
 * then draws its last collected data - shapes into the scene's DebugDraw for the frame, text into one screen
 * panel at the view's top-left: a header naming the target, the category list ("1 Basic [on]"), then each enabled
 * category's lines. The panel is DebugDraw's screen panel, drawn by the view's DebugDrawRenderer, not ImGui, so a
 * Play window without the editor shows the same thing.
 *
 * ── NOT IN A PLAYER'S BUILD ──────────────────────────────────────────────────────────────────────────────────
 *
 * As UE compiles the plugin out of Shipping, this whole file is DESERT_DEV_INSTRUMENTS only; Scene has no
 * debugger in a player's build.
 */

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

#include <Common/Core/UUID.hpp>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Desert::Input
{
    class LocalPlayerInput;
}

namespace Desert::Debug
{
    class GameplayDebugger
    {
    public:
        /// One registered category, its state and its last collected data.
        struct CategorySlot
        {
            std::unique_ptr<IGameplayDebuggerCategory> Category;
            bool                                       Enabled             = true;
            float                                      SecondsSinceCollect = 0.0f;
            bool                                       Collected           = false; // Data holds a collection
            GameplayDebuggerCategoryData               Data;
        };

        /// With the engine's categories registered: Basic, Physics, Script (digits 1-3).
        GameplayDebugger();

        /// UE IGameplayDebugger::RegisterCategory: appended, so its digit is its place in the list.
        void AddCategory( std::unique_ptr<IGameplayDebuggerCategory> category );

        void               SetActive( bool active );
        [[nodiscard]] bool IsActive() const
        {
            return m_Active;
        }

        /// false when no category has that name.
        bool SetCategoryEnabled( std::string_view name, bool enabled );
        /// The digit's category (0-based place); false past the end.
        bool                                           ToggleCategory( size_t index );
        [[nodiscard]] const std::vector<CategorySlot>& GetCategories() const
        {
            return m_Categories;
        }

        /// The editor's selection (UE: the debugger follows the selected actor in the editor), pushed by the
        /// editor every frame; std::nullopt = nothing selected, the view's centre picks.
        void SetSelectedEntity( std::optional<Common::UUID> entity )
        {
            m_Selected = entity;
        }
        /// The entity shown now (std::nullopt when nothing was ever targeted or the target is gone).
        [[nodiscard]] std::optional<Common::UUID> GetTarget() const
        {
            return m_Target;
        }

        /// Once per scene update, after the scripts: reads @p input's keys (nullptr outside Play - the debugger is
        /// then switched by calls only), picks the target, collects and draws. @p advancesFrame false (a pause)
        /// leaves last frame's drawing standing, as DebugDraw keeps one-frame items then.
        void Tick( Core::Scene& scene, const Input::LocalPlayerInput* input, float deltaSeconds,
                   bool advancesFrame );

    private:
        void ReadKeys( const Input::LocalPlayerInput& input );
        void PickTarget( const Core::Scene& scene );
        void Draw( Core::Scene& scene ) const;

        std::vector<CategorySlot>   m_Categories;
        bool                        m_Active = false;
        std::optional<Common::UUID> m_Selected;
        std::optional<Common::UUID> m_Target;
        std::optional<Common::UUID> m_CollectedTarget; // the target the slots' Data describe
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
