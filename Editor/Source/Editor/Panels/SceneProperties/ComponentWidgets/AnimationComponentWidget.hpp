#pragma once

#include "IComponentWidget.hpp"

#include <Engine/Assets/Mesh/AnimationAsset.hpp>

#include <vector>

namespace Desert::Editor
{
    class AnimationComponentWidget final : public IComponentWidget
    {
    public:
        // THE ASSET MANAGER IS BACK, AND THIS TIME IT IS ASSIGNED AND READ. It used to be a constructor
        // parameter and a `m_AssetManager` member that the constructor never connected — an UNINITIALISED
        // pointer for as long as the class existed, which `-Wunused-private-field` is what finally said so
        // — and it was removed rather than fixed because nothing here needed one. Something does now: the
        // anim graph is a `.danimgraph` asset, so this widget offers the project's graphs in a slot and
        // writes a new one, and both of those are questions only the manager can answer. It is stored as
        // a raw pointer taken from the context's weak_ptr at the call site, for the lifetime the other
        // widgets assume: the widget is constructed and destroyed inside one Details frame.
        AnimationComponentWidget( const Animation::AnimationLibrary* animationLibrary,
                                  Assets::AssetManager*              assetManager );

        bool CanRemove() const override
        {
            return false;
        }

        void Render( ECS::Entity& entity, ::Desert::Core::Scene* scene = nullptr ) override;

    private:
        // AnimGraph (Phase 4) authoring UI: parameters (with live value controls), states (name/clip/loop/speed
        // + entry), and per-state transitions (target + blend + exit-time + parameter conditions).
        // @p entity is here for ONE reason: the "Open in Anim Graph" button opens a DOCUMENT, and a
        // document is asked for by subject — AnimGraphPanel::SubjectFor( the entity's UUID ). The old
        // button called a static RequestOpen() that meant "reveal the one Anim Graph window", which is all
        // there was to say while the panel was a singleton.
        void RenderAnimGraph( ECS::Entity& entity, ECS::AnimationComponent& animation,
                              const std::vector<Assets::Asset<Assets::AnimationAsset>>& clips );

        // Writes a starter `.danimgraph` under the project's AnimGraphs/ folder, registers it, points
        // @p animation at it and opens the document. EVERY STEP IS CHECKED and a failure is reported with
        // the path: a "New" that silently produced no file would leave the slot empty and the author
        // guessing, which is the empty successful answer the contract forbids.
        void CreateAnimGraphAsset( ECS::Entity& entity, ECS::AnimationComponent& animation,
                                   const std::vector<Assets::Asset<Assets::AnimationAsset>>& clips,
                                   Assets::AssetManager*                                     assets );

    private:
        const Animation::AnimationLibrary* m_AnimationLibrary;
        Assets::AssetManager*              m_AssetManager;
    };
} // namespace Desert::Editor