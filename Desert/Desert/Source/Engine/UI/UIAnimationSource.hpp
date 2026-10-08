#pragma once

// WHERE THE WALK'S KEYED CLIPS COME FROM. A UI clip (UE's UWidgetAnimation) is evaluated once per view frame,
// before any canvas is walked, and every element then folds in what the frame computed for it — a pre-pass
// rather than a per-element lookup because a clip on one element may drive another element that is walked
// first. The walk does not know what a clip IS: it asks this source to step for the frame and then asks it,
// per element, for that element's sample. The engine's answer (UI/Ecs/UIAnimationPlayback.hpp) evaluates the
// scene's UIAnimComponent sequences on the Timeline core; a test or another host hands in its own.
//
// ONE PER VIEW, OWNED BY THE VIEW. Each view evaluates for itself — the driving viewport steps the playheads,
// a preview only reads them — so two views must never share one frame's results. The view is COPIED (an
// introspection probe walks a copy), and the copy must not write into the original's results, so the view
// owns its source and a copy Clone()s it. The host's resources (UICanvasResources.hpp) create it, because the
// resources are what knows which backend the view draws.

#include <Engine/UI/UITree.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace Desert::UI
{
    /// What the clips of the scene add to one element this frame — the same three quantities a tween drives.
    struct UIClipSample
    {
        glm::vec2 Offset = glm::vec2( 0.0F );
        glm::vec2 Size   = glm::vec2( 0.0F );
        glm::vec4 Tint   = glm::vec4( 1.0F );
        /// "Reveal" (UIPath): the keyed fraction of the line's length, REPLACING the authored one while the
        /// clip drives it. Unset = the clip does not touch it.
        std::optional<float> Reveal;
        /// "HazeAmplitude" (UIRetainer): the keyed heat-haze amplitude in design px, REPLACING the authored one
        /// while the clip drives it. Unset = the clip does not touch it.
        std::optional<float> HazeAmplitude;
    };

    /// One view frame's step, as BeginUIFrame hands it to the source.
    struct UIAnimationStep
    {
        float DtSeconds = 0.0F;
        /// Does this view drive the scene's shared playheads (UIViewContext::DrivesSceneAnimation)?
        bool Advance = true;
        /// Is the view showing a GAME world (UIViewContext::GameWorld)? Only a game world starts AutoPlay.
        bool GameWorld = true;
    };

    class IUIAnimationSource
    {
    public:
        virtual ~IUIAnimationSource() = default;

        /// A source with the same results, for a copy of the view that must not write into this one's.
        [[nodiscard]] virtual std::unique_ptr<IUIAnimationSource> Clone() const = 0;

        /// Step (when @p step.Advance) and evaluate every clip of @p scene, replacing the previous frame's
        /// results. Once per view frame, before any canvas is walked.
        virtual void Evaluate( const IUITree& scene, const UIAnimationStep& step ) = 0;

        /// Forget everything — the view was pointed at another scene, whose entity ids mean something else.
        virtual void Reset() = 0;

        /// What the clips add to @p element this frame; nullptr when no clip drives it.
        [[nodiscard]] virtual const UIClipSample* Sample( NodeId element ) const = 0;
    };

    /// The view's owned source with VALUE semantics: copying the view clones it, so a copy evaluates into
    /// results of its own. Never empty — the view's constructor fills it from its resources.
    class UIAnimationSourceSlot
    {
    public:
        explicit UIAnimationSourceSlot( std::unique_ptr<IUIAnimationSource> source )
             : m_Source( std::move( source ) )
        {
        }
        UIAnimationSourceSlot( const UIAnimationSourceSlot& other ) : m_Source( other.m_Source->Clone() )
        {
        }
        UIAnimationSourceSlot& operator=( const UIAnimationSourceSlot& other )
        {
            if ( this != &other )
                m_Source = other.m_Source->Clone();
            return *this;
        }
        UIAnimationSourceSlot( UIAnimationSourceSlot&& ) noexcept            = default;
        UIAnimationSourceSlot& operator=( UIAnimationSourceSlot&& ) noexcept = default;
        ~UIAnimationSourceSlot()                                             = default;

        [[nodiscard]] IUIAnimationSource& Get() const
        {
            return *m_Source;
        }

    private:
        std::unique_ptr<IUIAnimationSource> m_Source;
    };
} // namespace Desert::UI
