#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Editor
{
    // THE DICTIONARY, AND THE ONLY ONE. Every command this editor can be asked to perform without a mouse: the
    // tool panels, the open documents, the entities in the open scene, the menu bar, the openable assets, the
    // focused document's preview viewpoints, and the plain actions.
    //
    // The command palette and the CONTROL CHANNEL build their list from this same registry and call these same
    // closures. That is the whole design of the channel: everything a person can reach with Ctrl+P, an agent can
    // reach by naming a group and a label, by construction rather than by anybody maintaining a second list. See
    // Editor/Core/Control/ControlDispatch.hpp.
    //
    // UE's FUICommandList shape: an ordered list of PROVIDERS, each living next to its subject (the Landscape
    // group in Panels/Landscape, the Modeling group in Panels/Modeling, ...) and registered by whoever owns that
    // subject. Build() calls them in registration order, so the order of the groups in the palette is the order
    // of Register() calls.
    //
    // Built on demand — when the palette opens, or when a request arrives — never per frame. THAT SENTENCE USED TO
    // BE FALSE: DrawCommandPalette rebuilt it on every frame the overlay was up, and the dictionary walks the
    // scene's entities, the levels on disk and every openable file under the content root. See
    // EditorLayer::DrawCommandPalette for what makes rebuilding on OPEN correct rather than a snapshot going
    // stale.
    class CommandRegistry
    {
    public:
        using Provider = std::function<void( std::vector<PaletteCommand>& )>;

        // `owner` names who registered the provider (a module, a panel); it is what a reader of a
        // duplicate or a missing group looks up. Order of calls = order of groups in the palette.
        void Register( std::string owner, Provider provider );
        // Runs once at the start of every Build(), before any provider: the place for a census several providers
        // read (the content root's files), so it is taken once per build and never depends on provider order.
        void OnBuildBegin( std::function<void()> prelude );
        // Appends every provider's entries to `out`, in registration order.
        // A provider's wall time in one Build (the preludes under "preludes"): the slowest first, so a
        // dictionary that costs a frame names its owner.
        struct ProviderCost
        {
            std::string Owner;
            double      Ms = 0.0;
        };
        void Build( std::vector<PaletteCommand>& out, std::vector<ProviderCost>* costs = nullptr ) const;

        // ONE GROUP'S SLICE OF THE DICTIONARY (WQH p2): the preludes, then only the providers that produced
        // @p group on the last full Build, in registration order - so the entries, and which of two equal
        // addresses wins, are the ones a full Build would give. UE's FUICommandList binds a command to its
        // context once instead of re-enumerating every context per invocation; here the provider -> groups index
        // is learnt from the full Builds (the palette's, a refused `run`'s). A control `run` rebuilt all ~9200
        // entries per request (~0.5 s of its frame) to read one.
        // Returns false and appends nothing when no full Build has seen @p group yet: the caller builds whole
        // (and that Build teaches the index). A slice that does not hold the wanted label is not proof it is
        // absent - a provider may have started producing the group since - so a caller refusing an address
        // confirms against a full Build first.
        bool BuildGroup( const std::string& group, std::vector<PaletteCommand>& out,
                         std::vector<ProviderCost>* costs = nullptr ) const;

        [[nodiscard]] size_t Size() const
        {
            return m_Providers.size();
        }

    private:
        struct Entry
        {
            std::string Owner;
            Provider    Append;
        };
        std::vector<Entry>                 m_Providers;
        std::vector<std::function<void()>> m_Preludes;
        // group -> the providers (indices into m_Providers, ascending) that produced it on the last full Build.
        mutable std::unordered_map<std::string, std::vector<std::size_t>> m_GroupProviders;

        void RunPreludes( std::vector<ProviderCost>* costs ) const;
    };
} // namespace Desert::Editor
