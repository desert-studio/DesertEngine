#pragma once

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AssetBase.hpp>

#include <memory>

namespace Desert::Assets
{
    /// The format's own constant, re-exported so an asset-layer caller need not reach into the animation
    /// namespace for it. ONE definition — see Engine/Animation/Graph/AnimGraph.hpp for why it lives there.
    inline constexpr std::string_view kAnimGraphExtension = Animation::Graph::kAnimGraphExtension;

    /**
     * @brief An ANIMATION STATE MACHINE on disk, in the engine's ONE asset system.
     *
     * WHAT THIS FIXES, and it is the half of §5.1 that the shader graph's was the mirror of. The graph
     * used to be a JSON STRING INSIDE THE ENTITY (`AnimationComponentSer::GraphJson`), so it had the
     * right window — a document, with a subject and a liveness answer — and the wrong storage: two
     * characters could not share one walk graph, and copying it meant copying a blob. The shader graph
     * had exactly the opposite pair, a file with no identity. Both are cured by the same thing: an asset
     * type and a handle.
     *
     * THE PARSED GRAPH IS SHARED, NOT COPIED, and that is the load-bearing decision here. `GetGraph()`
     * hands out the asset's OWN `shared_ptr`, so every entity that names this file points at one object:
     * an edit made in the Anim Graph window of character A is the graph character B is evaluating on the
     * next frame, with no publish step and no second copy to fall out of date. The evaluators stay
     * per-entity (each holds its own copy and its own live parameter values), which is what keeps two
     * characters on one graph in two different states.
     *
     * THE REVISION IS THE ASSET'S, NOT THE COMPONENT'S. `AnimationComponent::GraphRevision` used to be
     * bumped by whoever edited the graph — which could only ever bump the component in front of the
     * editor, so a shared graph would have re-synced ONE of its entities. The counter belongs to the
     * thing that changes.
     */
    class AnimGraphAsset final : public AssetBase
    {
    public:
        AnimGraphAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. Missing, empty or malformed is an ERROR carrying the reason — never
        /// a quietly substituted empty graph, which would leave a character standing still while its scene
        /// file plainly names a state machine.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Graph != nullptr;
        }

        /// The asset's OWN graph object — see the class note: callers SHARE it rather than copying it.
        /// Null until a successful Load.
        [[nodiscard]] const std::shared_ptr<Animation::Graph::AnimGraph>& GetGraph() const
        {
            return m_Graph;
        }

        /// What to show in a slot: the graph's own `Name` when it has one, the file's stem when it does not.
        [[nodiscard]] const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /// Bumped by every successful Load AND by `MarkEdited` below. `AnimationECSSystem` holds the
        /// revision each entity's evaluator was built at, so one number re-syncs every entity that names
        /// this graph — which is the whole point of the graph being one object.
        [[nodiscard]] uint32_t GetRevision() const
        {
            return m_Revision;
        }

        /// "The object you are holding was just changed." Called by the editor after any structural edit.
        ///
        /// IT TAKES NO GRAPH, deliberately: the caller already holds the very object this asset owns, so a
        /// parameter here would be a second way to set the graph and an invitation to hand over a copy —
        /// which is exactly the drift the shared pointer exists to prevent.
        void MarkEdited()
        {
            ++m_Revision;
            if ( m_Graph )
            {
                m_DisplayName = m_Graph->Name.empty() ? m_Metadata.Filepath.stem().string() : m_Graph->Name;
            }
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::AnimGraph;
        }

        /// Writes a graph to disk, creating the directory if needed. Static because saving is what CREATES
        /// an asset: writing through an instance would mean an instance had to exist for a file that does
        /// not.
        static Common::BoolResultStr Save( const Common::Filepath&            filepath,
                                           const Animation::Graph::AnimGraph& graph );

    private:
        std::shared_ptr<Animation::Graph::AnimGraph> m_Graph;
        std::string                                  m_DisplayName;
        uint32_t                                     m_Revision = 0;
    };
} // namespace Desert::Assets
