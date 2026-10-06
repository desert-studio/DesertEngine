#pragma once

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RDG-FAULT1 - fault isolation of the render graph (UE: a broken pass is a check() at AddPass / Compile in a
// development build; a shipping engine keeps the frame alive). Seen live 2026-10-05: one pass failing a binding
// check (glass bound u_ShadowMap0..3, which its shader does not declare) failed the WHOLE graph - nothing drew and
// the same error went to the log every frame, flooding the editor's Logs panel.
//
// The decided shape (one owner per responsibility):
//   * a PASS never decides what happens to the frame. Its setup records declaration errors against itself, its
//     binding blocks are validated against the shader before anything is recorded (ValidatePassBindings), and its
//     exec returns a BoolResultStr. Each of the three becomes a PassFault of that pass;
//   * the GRAPH decides (Builder::Compile, Builder::Execute): a faulted pass is removed as if it had not been
//     added; every pass that can no longer get a defined input is culled with it (FaultCulled); a surviving pass
//     that reads an output the faulted pass alone produced reads the resource's declared FaultDefault (a system
//     texture) instead; an external whose FaultPolicy is FrameFatal that loses its writer makes the fault
//     FRAME-level (FrameFault) - the only case in which the frame does not draw the graph;
//   * the REPORTER logs: one line per (graph, pass, reason) when it appears or changes, one line when it is gone.
//     Nothing else in the graph logs a pass fault.
namespace Desert::Graphic::RDG
{
    // Where a pass fault was found. The earlier, the cheaper: Declaration and Validation are found by Compile,
    // before any command of the graph is recorded, so the pass contributes nothing (no barrier, no render pass,
    // no half-recorded draw). Execution is found by Execute after the pass began recording (see LateFault).
    enum class PassFaultStage : uint8_t
    {
        Declaration, // its setup declared something malformed (PassBuilder::Read/Write/ColorTarget/... errors)
        Validation,  // a binding block does not match its shader (ValidatePassBindings)
        Execution,   // its exec lambda returned an error
        Dependency,  // not broken itself: an input it needs has no surviving producer and no FaultDefault
    };

    std::string_view GetPassFaultStageName( PassFaultStage stage );

    // One faulted pass of one compile / execute. @p Reason is STABLE across frames for the same defect: it names
    // passes, resources and shader slots, never a frame number, a pointer or a time. The reporter keys on it, so a
    // reason that varied per frame would be reported every frame (the flood this task removes).
    struct PassFault
    {
        uint32_t       Pass = 0; // AddPass index
        std::string    PassName;
        PassFaultStage Stage = PassFaultStage::Declaration;
        std::string    Reason;
        // Set for Stage == Dependency: the AddPass index of the faulted pass the dependency chain starts at, so a
        // cascade is reported as ONE root cause ("'Glass' faulted; culled with it: 'GlassComposite'").
        std::optional<uint32_t> RootPass;
    };

    // What a SURVIVING reader of a resource sees when the only pass that defined the contents it reads was removed
    // by a fault (UE: GSystemTextures black / white dummies). Declared by the resource's creator with
    // Builder::SetFaultDefault, because only the producer knows what "nothing" means for its output: an SSAO term
    // defaults to White (no occlusion), bloom and a light-shaft term to Black (adds nothing). None (the default):
    // the readers are culled too (PassFaultStage::Dependency) - a guessed default is a silent fallback.
    // Buffers have no system default: a buffer input lost to a fault always culls its readers.
    enum class FaultDefault : uint8_t
    {
        None,
        Black,     // SystemTextures::Black
        White,     // SystemTextures::White
        BlackCube, // SystemTextures::BlackCube
    };

    // The clear an attachment LOADED from a lost transient gets instead (DefaultSubstitution::AttachmentCleared):
    // Black 0,0,0,1; White 1,1,1,1; BlackCube 0,0,0,1. None has no clear (its readers are culled).
    ClearValue GetFaultDefaultClear( FaultDefault value );

    // What losing every writer of an EXTERNAL resource to a fault means (Builder::SetFaultPolicy). Removing a pass
    // leaves an external with the contents it entered the graph with, which is right for most of them and wrong
    // for two kinds, so the owner of the external says which kind it is:
    //   * KeepsContents (the default): the previous contents are still a defined picture (a scene colour another
    //     graph already wrote, an engine image). The frame continues;
    //   * InvalidateHistory: a history the NEXT frame reads (TAA, auto-exposure, temporal clouds). The frame
    //     continues; the external is listed in ExecuteReport::InvalidatedExternals and its owner resets it before
    //     the next frame reads it (UE: a camera cut resets the history), instead of blending stale contents;
    //   * FrameFatal: the frame's output (the swapchain image, the image an editor viewport presents). Its
    //     entering contents are undefined (a just-acquired swapchain image), so there is no defined picture
    //     without its writer: the fault becomes a FrameFault.
    enum class ExternalFaultPolicy : uint8_t
    {
        KeepsContents,
        InvalidateHistory,
        FrameFatal,
    };

    // A surviving reader reading a FaultDefault in place of a lost transient. Compile adds the read of the system
    // texture to the reader (so it gets its barrier and is in GraphView::Resources as used) and PassContext::
    // GetTexture on @p Original from @p ReaderPass returns the binding of @p Replacement. An attachment the reader
    // LOADED from the lost transient is not substituted (a system texture cannot be rendered into): its load
    // becomes a Clear to the default's value (Black: 0,0,0,1; White: 1,1,1,1), which is the same picture.
    struct DefaultSubstitution
    {
        uint32_t     ReaderPass        = 0;                // AddPass index
        uint32_t     Original          = kInvalidResource; // the transient whose producer was removed
        uint32_t     Replacement       = kInvalidResource; // the system texture's resource index in this graph
        FaultDefault Default           = FaultDefault::None;
        bool         AttachmentCleared = false; // the attachment-load case above: no Replacement, a Clear
    };

    // The frame cannot be drawn by this graph: a FrameFatal external lost its writer, or the graph itself (not a
    // pass) is malformed (a resource declared wrong, an aliasing plan the backend refused). The graph records
    // nothing (Compile-time) or ends what it opened (Execute-time). The caller (VulkanRenderer::ExecuteGraph) then
    // clears every image in @p Externals to opaque black and presents, so the swapchain protocol (acquire ->
    // present) and the frame cadence stay intact and the window shows black, not the last frame and not garbage.
    struct FrameFault
    {
        std::string           Reason;
        std::vector<uint32_t> Externals;  // FrameFatal externals left without a defined picture
        std::vector<uint32_t> RootPasses; // the faulted passes that caused it (empty: the graph itself is broken)
    };

    // The result of one Builder::Execute, kept by the builder (Builder::GetExecuteReport). An error returned by
    // Execute is exactly a FrameFault; every pass-level fault is in Faults and the frame was drawn without it.
    struct ExecuteReport
    {
        std::vector<PassFault>    Faults;      // every faulted pass, any stage, AddPass order
        std::vector<uint32_t>     FaultCulled; // AddPass indices removed with them (includes the faulted ones)
        std::vector<uint32_t>     InvalidatedExternals; // InvalidateHistory externals whose writer was removed
        std::optional<FrameFault> Frame;
    };

    // RDG-FAULT1. The ONE place a pass fault is logged (UE: a "log once" keyed per pass). Owned per backend
    // (IBackend::GetPassFaultReporter) so it outlives graphs: graphs are rebuilt every frame and a reporter owned
    // by one would forget what it already said.
    //
    // Key = (graph name, pass name). Per key the reporter remembers the reason it last logged:
    //   * a key with no remembered reason, or a different reason -> one line at ERROR
    //     ("RDG graph '<graph>' pass '<pass>' <stage>: <reason>"), cascades named in the same line;
    //   * the same reason again -> nothing (the defect is still there and was already said);
    //   * a remembered key the graph executed WITHOUT faulting -> one line at INFO
    //     ("RDG graph '<graph>' pass '<pass>' recovered") and the key is forgotten, so a relapse is reported
    //     again.
    // Recovery is judged per graph: a graph that did not execute this frame (a closed preview) keeps its keys and
    // says nothing. A pass that was not ADDED to an executed graph is recovered too (its feature was turned off).
    // A FrameFault is reported the same way under the pass name "<frame>".
    // Who calls it: Builder::Execute, once per execute, after the frame's faults are final (late faults included).
    // The sink is the engine logger in the product and a line collector in the suite.
    class PassFaultReporter
    {
    public:
        enum class Severity : uint8_t
        {
            Error,     // a new or changed fault
            Recovered, // a fault that is gone
        };
        using Sink = std::function<void( Severity severity, std::string_view line )>;

        explicit PassFaultReporter( Sink sink );

        // @p graph: Builder::GetName(); @p addedPasses: the names of every pass the graph added this execute (to
        // judge recovery); @p report: that execute's faults. Returns the number of lines logged by this call.
        uint32_t Report( std::string_view graph, std::span<const std::string_view> addedPasses,
                         const ExecuteReport& report );

        // Faults currently remembered (logged and not yet recovered), over every graph.
        uint32_t GetActiveCount() const;
        uint32_t GetLinesLogged() const;

    private:
        Sink m_Sink;
        // (graph, pass) -> the reason last logged for it.
        std::map<std::pair<std::string, std::string>, std::string, std::less<>> m_Active;
        uint32_t                                                                m_LinesLogged = 0;
    };
} // namespace Desert::Graphic::RDG
