// THE POINTER-OWNERSHIP CENSUS.
//
// The owner asked for an audit of where `shared_ptr`, `unique_ptr` and raw pointers are used and which
// of them each place should be. The failure mode of that request is five hundred lines of taste, so
// every verdict here is the answer to two FACTUAL questions instead:
//
//   Q1. Who is OBLIGED to destroy this object?  One known owner -> unique_ptr. Several owners whose
//       deaths are not ordered -> shared_ptr. No owner, only a watcher -> a raw pointer or a weak_ptr,
//       and then Q2 is compulsory.
//   Q2. Can the observed die before the observer?  Yes and unguarded -> that is a DEFECT, not a style.
//       No -> the row must name WHAT guarantees it.
//
// The subject is the pointer-typed DATA MEMBER (pointer_ownership_scan.hpp says why), the verdicts are
// in pointer_ownership_register.hpp, and this file is what makes them fail.
//
// WHY A CENSUS AND NOT A DOCUMENT IN `Docs/`. A report goes stale in silence; this goes red. A pointer
// member added tomorrow has no row, and `EveryRawPointerMemberNamesItsGuard` names it with its file and
// line and asks its author the two questions above.

#include "pointer_ownership_register.hpp"
#include "pointer_ownership_scan.hpp"

#include <Engine/Graphic/Render2D/Render2DExecutorRetire.hpp>

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Tests::PointerCensus;
    namespace Text = Desert::Tests::ConsumerText;

    const std::vector<Member>& Members()
    {
        static const std::vector<Member> members = []
        {
            const std::string root = RepoRoot();
            return root.empty() ? std::vector<Member>{} : ScanMembers( root );
        }();
        return members;
    }

    int CountOf( Form f )
    {
        int n = 0;
        for ( const Member& m : Members() )
            n += m.Kind == f ? 1 : 0;
        return n;
    }

    const Row* FindRow( const Member& m )
    {
        for ( const Row& r : Register() )
            if ( m.Class == r.Class && m.Name == r.Member && m.File == r.File )
                return &r;
        return nullptr;
    }

    std::string ReadRepoFile( const char* relative )
    {
        return Text::StripCommentsAndLiterals( ReadAll( fs::path( RepoRoot() ) / relative ) );
    }
} // namespace

// The listing the register was WRITTEN from, and the one the next stage will widen it from. Disabled
// because it asserts nothing; run it with
//   ./PointerOwnership --gtest_also_run_disabled_tests --gtest_filter=*Dump*
// after adding a tree to ScannedTrees(), and every new member arrives with its file, line and
// declaration ready to be answered for.
TEST( PointerOwnership, DISABLED_DumpEveryMember )
{
    ASSERT_FALSE( RepoRoot().empty() );
    int counts[4] = { 0, 0, 0, 0 };
    for ( const Member& m : Members() )
    {
        ++counts[static_cast<int>( m.Kind )];
        std::printf( "%s|%d|%s|%s|%s|%s\n", FormName( m.Kind ), m.Line, m.File.c_str(), m.Class.c_str(),
                     m.Name.c_str(), m.Decl.c_str() );
    }
    std::printf( "TOTAL %zu unique=%d weak=%d shared=%d raw=%d\n", Members().size(), counts[0], counts[1],
                 counts[2], counts[3] );
}

// ------------------------------------------------------------------------------------------------
// The number
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, TheScanFindsTheCensusedPopulation )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository from the working directory";

    // A census that finds nothing to census has stopped working, and it fails in the confident voice it
    // uses when a tree really is clean. This is the line that tells the two apart.
    ASSERT_FALSE( Members().empty() ) << "the scan found no pointer members at all -- the reader is blind, "
                                         "and every assertion below is vacuous.";

    // MEASURED, not estimated, and measured with THIS scanner. Run over the whole tree by widening
    // ScannedTrees() (Desert/Desert/Source, Desert/Common/Source, Editor/Source, Runtime/Source) it
    // THE WHOLE TREE IS NOW THE SCOPE — Desert/Desert/Source, Desert/Common/Source, Editor/Source and
    // Runtime/Source — so there is no longer an unscanned half in which a raw pointer can appear without
    // owing an answer. 783 members, and the three stages that got here are still visible in the register's
    // section headers because the ARGUMENTS differ by tree: in Graphic the form answers about half the
    // rows by itself, in the editor almost every row is a construction order, and in the engine core
    // almost every row is a back-pointer closed by containment.
    //
    // The number fell from 787 to 783 across the three stages, and both moves were the fixes rather than
    // the scan: eight raw members became co-owned handles (A8-3), a dead class, a dead accessor, a
    // write-only set of panel pointers, a stack-address drag target and LayerStack's un-owned layer
    // vector all went away, and MaterialSlotBinding brought two new ones in.
    //
    // THIS NUMBER HAS MOVED TWICE AND BOTH MOVES WERE THE CENSUS BEING WRONG, not the tree changing.
    // Neither is written off, because a census whose number drifts without an account is a census nobody
    // can use:
    //
    //   738 -> 761. The scope was picked with a throwaway prototype that skipped every CONTAINER of raw
    //   pointers -- `std::vector<IProperty*> m_RegisteredProperties`, `std::unordered_map<uint32_t,
    //   Image*> m_BoundInputs`, `std::vector<RenderCommand*> m_Commands`. A container of raw pointers
    //   raises exactly the same two questions as one raw pointer, and three of those turned out to carry
    //   load-bearing arguments.
    //
    //   A8-1 then moved it by -2 in these trees, and that one is NOT a blind spot: Render2D's three
    //   executor caches used to be `unordered_map<const void*, unique_ptr<MaterialExecutor>>` and are now
    //   maps of a small struct, so the ownership question moved off three container members and onto the
    //   ONE `CachedExecutor::Executor` inside them, where there is exactly one answer to give. The `const
    //   void*` key is no longer a member the scan can see; its argument lives on Render2D::m_Backdrop's
    //   row and in Render2DExecutorRetire.hpp, which is the file that decides its lifetime.
    //
    //   761 -> 787. The scanner knew `std::shared_ptr` and not the project's own ALIASES for it, so
    //   `MaterialInstancePtr m_X` and `DescriptorSetLayoutRef m_Y` were counted as NOTHING AT ALL --
    //   seventeen members in these three trees alone were invisible, among them the four
    //   `MaterialExecutor::m_*PropertiesStorage` vectors that forty-six of this register's rows rest on.
    //   It was found the only way a blind spot ever is: A8-3 converted eight raw members to co-owned
    //   handles and the total FELL by five instead of holding. The alias list is now derived from the
    //   tree (see DeclaredAliases), not typed.
    //   783 -> 784, AND THIS ONE IS THE CENSUS EARNING ITS KEEP. G17 landed a hand-written keyword scan
    //   in DShaderParser to replace fifteen unconditional std::regex passes, and its rule table brought a
    //   `const char* Replacement` with it. That branch was cut BEFORE this census covered the whole tree,
    //   so nobody on it was ever asked the two questions -- and the integration is exactly where an
    //   un-owed pointer would otherwise have slipped in unremarked. The answer is the easy kind (a string
    //   literal in a `static const` table, so the language closes both questions), which is the point: the
    //   gate does not care whether the answer is hard, only that one exists.
    //
    //   784 -> 786, and it earned its keep a second time in the same way. O1 added
    //   Graphic::kCloudUnreadSlots — the register of cloud parameter-block slots no shader reads, one row
    //   per slot with the reason in it — and its row type carries two `const char*`. This suite named both
    //   with their file and line before anyone thought about them; the answer is again the easy kind
    //   (string literals in an `inline constexpr std::array`), and again the value is that a raw pointer
    //   could not be ADDED without someone being asked. Note the shape of the thing it caught: a table
    //   written to make an exception explainable, which would itself have been an unexplained pointer.
    //
    //   786 -> 787, and the interesting half of this one is the SHARED count going DOWN. U7-2 made the UI
    //   editor a document over one entity's UICanvasComponent, and a document holds its scene WEAKLY: a
    //   closed scene is one of the ways its subject dies, and a shared_ptr would hide that death and leak
    //   the level with it (the argument is written out at AnimGraphPanel::m_Scene). So UIEditorPanel::m_Scene
    //   moved shared -> weak, which is 317 -> 316 and 34 -> 35 with the total unchanged by that move. The
    //   +1 is the class constant `kComponentTypeName` — the literal the registration and the Details button
    //   both read so the two cannot spell the subject's facet differently — and it is the same easy answer
    //   the two documents beside it give: a string literal in static storage, owned by nobody and outliving
    //   everything.
    //
    //   787 -> 789, same task and the same two movements. The Sequencer became a document over TWO subject
    //   types — the rig's SkinnedMeshComponent and a UI element's UIAnimComponent, which are two different
    //   kinds of thing and cannot share a key — so it carries two of those class constants instead of one,
    //   and its scene went shared -> weak with the other documents'.
    //   789 -> 791 ПРИ СВЕДЕНИИ, и ни одна из двух веток не угадала: У7-2 пришёл с 789
    //   (328/315/110/36), И14+М12 с 788 (325/319/110/34), а сумма дала 791 (328/317/110/36).
    //   Слияние двух переписей — это НЕ выбор одной стороны: каждая измеряла своё дерево, и верно
    //   только третье число, которого не видел никто. Оно ПОЛУЧЕНО ЗАПУСКОМ переписи на сведённом
    //   дереве, а не выведено арифметикой из двух отчётов — арифметика здесь и была бы подгонкой.
    //
    //   791 -> 805 with O1-E, the authored cloud medium, and every one of the fourteen is accounted for
    //   by what that mechanism IS. O1-E branched from the 789 tree and reported 803; the extra two are
    //   the merge's own +2 above, not anything of O1-E's — which is why neither branch's number is the
    //   merged tree's. 805 was PREDICTED from that reasoning and then CONFIRMED BY RUNNING the census on
    //   the merged tree before this merge was committed. The prediction is not the evidence; the run is.
    //   Had they disagreed, the run would have won and the reasoning above would have been the thing to
    //   fix.
    //
    //   NINE RAW, and all nine are string literals in static tables: the Volume domain's two registers
    //   (which material properties a medium graph may read, and which it deliberately may not, with the
    //   reason) and the emitter's table of the five functions a medium compiles to. Same easy answer as
    //   every other table entry above them.
    //
    //   THREE SHARED, and they are the point of the design rather than a detail. A cloud material's
    //   authored medium produces a NEW compiled program on every edit of its graph, and each one owns
    //   VkShaderModules and descriptor set layouts. ShaderService therefore holds variants only WEAKLY,
    //   and VolumetricCloudRenderer's three shared_ptrs are the strong references — dropping them is what
    //   frees the modules. A service-owned cache would have grown by one program per edit for the life of
    //   the session and released none.
    //
    //   TWO WEAK, which are the other half of that arrangement: ShaderService::m_ShaderAssets (the asset
    //   manager owns the assets; this service must not extend their life to compile a variant later) and
    //   VariantEntry::Program (the cache above).
    //
    //   805 -> 807 with O1-F, the authored medium's ShadowRay input, and both are RAW: the two string
    //   literals of ShadowRayScope — the register saying in which of the medium's five outputs the flag
    //   means anything, and which GLSL entry point a shadow march calls to get there. Same static-table
    //   answer as the nine O1-E added beside them, and the same reason there is a register at all rather
    //   than a pair of names in an `if`: the suite that derives the set from the shader tree needs
    //   something to compare against. Nothing else of O1-F is a pointer — the flag itself is a float on a
    //   GLSL struct, which no C++ census can see.
    //
    //   807 -> 815 with O1-G-2, the authored medium's own parameters and images. FIVE RAW, and every one
    //   of them is a borrowed image or buffer crossing a seam that already existed: the medium's images on
    //   the cloud renderer (m_MediumImages) and on the bake payload, its parameter buffer and its images on
    //   the bake's argument pack, and the Compiler's set of node addresses inside the graph emitter. THREE
    //   SHARED, which are the parameter buffer itself in the three places a non-persistent storage buffer
    //   has to exist separately — the march, the shadow map and the sky's own bake — for the reason the
    //   layer's packed block is already tripled: one buffer holds one set of bytes per (frame x renderer
    //   slot), not per pass.
    //
    //   815 -> 816 with Ю2, and it is ONE RAW: CommandHistory::StringCommand::m_Target. The undo stack
    //   grew a second property-edit command because a std::string field cannot take the byte one — the
    //   entry stores the object's REPRESENTATION and restoring it hands the live string a heap pointer
    //   the edit already freed, which seventeen reflected string fields could reach. The new command
    //   stores the VALUE and assigns it back. Its pointer is the same kind of pointer ByteCommand's is
    //   (into a live component's field) under the same guard (IsVolatile, so DropVolatile drops it), so
    //   it adds a row rather than a question.
    //
    //   816 -> 819 with D34, and all THREE are raw. One is a host reference: AssetPreloader now takes the
    //   AnimationLibrary it publishes clips to, because the library was the one content index a HOST filled
    //   rather than the scan — the editor with its own loop in the wrong place, the packaged game with no
    //   loop at all and every character in its bind pose. The other two are the fix to what that made
    //   reachable: Animator's bone->track memo was keyed on the clip's ADDRESS alone, and an asset unload +
    //   reload leaves that address alone while freeing the Tracks vector the memo points into, so the memo
    //   now carries the storage it was built from (TracksData, never dereferenced) beside the pointers it
    //   guards (ByBone). The old m_TrackBinding row's argument for why the key was safe is retracted in
    //   place rather than deleted — it was wrong in exactly the direction that cost a segfault.
    //
    //   819 -> 823 with U10 (UI introspection). Three raw, and all three are IDENTITIES rather than
    //   accesses: two copies of DrawCommand::Texture carried into the probe so the panel can print WHICH
    //   texture broke a batch, and the editor's probe registry keyed on a Scene's address. The fourth is
    //   the shared Scene the UI Debugger panel holds like every other scene-bound panel.
    //   823 -> 830 with Ю11 (materials on a UI element). Three raw: the material's identity in the batch
    //   command, the copy of it the probe prints, and the view's pointer to the cache that owns them.
    //   Two shared (the cache's target framebuffer and each entry's pipeline) and two unique (each
    //   entry's runtime material, and the shared error entry) are ownership and answer for themselves.
    //
    //   830 -> 824 with Г25, and this one goes DOWN: the procedural grass generator was removed whole,
    //   because grass becomes a mesh ASSET scattered by the Foliage tool. Six members of TerrainRenderer
    //   went with it — five shared (the grass graphics pipeline, the baked clump atlas, the cull compute
    //   pipeline, the compacted visible-clump buffer and the indirect-args buffer) and one unique (the
    //   grass DataDrivenMaterial). None was raw, so none held a row in the register, and the two questions
    //   above are answered by the removal itself: nothing is obliged to destroy an object that is never
    //   created. The count is the whole evidence that the members left with the feature rather than being
    //   orphaned inside a class that no longer draws them.
    //   824 -> 822 with Ю14 (multi-channel text), and this one goes DOWN by two raw pointers, both in
    //   the font baker. `RawGlyph::Bitmap` is gone because stb no longer allocates the glyph bitmap —
    //   the multi-channel field is generated into a std::vector the RawGlyph owns — and `Placed::G` is
    //   gone because the packer now stores the glyph's INDEX rather than its address, which is also the
    //   answer to the second question: an index cannot dangle when the vector it indexes reallocates.
    //   One raw pointer was added in its place, `Msdf::EdgePoint::NearEdge`, and it has a row.
    //
    //   824 -> 828 with Г26, and all FOUR are shared members of MeshRenderer: the (Instanced x GBuffer)
    //   shader, its pipeline, its MaterialPBR and that material's instance. They are the deferred twins
    //   of the four (Instanced x Forward) members already censused two lines apart in the same class,
    //   and they exist because the G-buffer pass had no instanced cell at all -- which is why every
    //   InstancedStaticMesh entity was dropped there in silence. Q1: the shader and the pipeline are
    //   owned by the shader service and the pipeline cache and merely HELD here, exactly as the forward
    //   pair is; the material and its instance are created here and outlive nothing. Q2: none is
    //   deleted by this class; none is raw, so none takes a row in the register.
    //   -> +1 with Ю15: ONE member, `Localization::m_Language`, which points at a row of the constexpr
    //   locale table (Engine/Localization/LocaleFormat.cpp) and is registered as StaticStorage. Both
    //   questions are answered by the language: nobody allocated the table, and nobody can destroy it.
    //   The source-language constant beside it was written as `const char*` and would have been a second
    //   row; it is a `std::string_view` instead, which is why this is +1 and not +2.
    //
    //   FOUR BRANCHES PREDICTED THIS NUMBER TODAY AND EACH WAS RIGHT ONLY AGAINST ITS OWN HEAD. The value
    //   below was read off a run of the merged tree, as it must be.
    //   824 -> 823 with A1 (the pose substrate), and it goes DOWN by one RAW. Animator::TrackBinding::ByBone
    //   was a vector of BoneTrack pointers into the clip's own storage, held safe by a rebind discipline; it
    //   is now a vector of track INDICES. Both questions are answered by the type rather than by a rule
    //   somebody has to keep: an index cannot point at freed memory, and the generation stamp the clip now
    //   carries (AnimationClip::TrackRevision) closes the case the address comparison could not see — an
    //   unload and reload of the same size, which the allocator satisfies from the very block it just freed,
    //   leaving data() and size() both unchanged across a complete replacement of the list.
    //
    //   AND THE SUM IS NEITHER SIDE'S ARITHMETIC. dev said 830 members / 358 raw and A1 said 823 / 353;
    //   each was correct about its own head and both are wrong here. The merged tree is 829 / 357 —
    //   dev's 830 minus the single raw member A1 retired (TrackBinding::ByBone), because A1's own 823
    //   was measured against a head that predated Ю14, Г26 and Ю15 entirely. Subtracting seven from
    //   830, or adding six to 823, would each have produced a plausible wrong number.
    //
    //   THIS IS THE FIFTH CONSECUTIVE MERGE IN WHICH THIS CENSUS CONFLICTED, and the fifth in which no
    //   arithmetic on the two branch values predicted the result: 826 vs 825 -> 827, 319 vs 325 -> 323,
    //   359 vs 352 -> 357, 357 vs 355 -> 358, and now 358 vs 353 -> 357. The five numbers below were
    //   READ OFF A RUN of this merge, which is the only way this file has ever been right.
    //
    //   -> +4 with Ю16 (830 -> 834), and every one of the four is named because a count nobody can
    //   account for is a count somebody will "adjust":
    //     * Raw +1    UIViewContext::RenderTextures -- registered below as ObservedContainsUs.
    //     * Shared +1 UIRenderTextureCache::Capture::Scene, the captured world.
    //     * Unique +2 UIRenderTextureCache::Capture::Renderer (the SceneRenderer holding the renderer
    //                 slot) and RuntimeLayer::m_UIRenderTextures (the cache itself, held by pointer there
    //                 because that header forward-declares the Render2D namespace).
    //   EditorUIPass::m_RenderTextures adds nothing: it is held BY VALUE, which is exactly what makes the
    //   lifetime argument for the raw row above hold.
    //
    //   -> AND AGAIN WITH Ю16, THE SIXTH IN A ROW: dev said 829 / 357, Ю16 said 834 / 359, the merge is
    //   833 / 358. Ю16 branched before А1 landed, so its absolute count still contains the row А1
    //   retired. Six merges have now made the same point precisely enough to state it: a branch's
    //   DELTA survives the merge and its TOTAL does not, so the total is the one thing that cannot be
    //   carried over — and it is the only thing written in this file. That is why it is read off a run
    //   every time, and why no arithmetic on the two branch values has ever predicted it.
    //
    //   -> +1 with A3 (833 -> 834), and it is named for the same reason: Unique +1 for
    //   `Animator::m_Controls`, the vector of skeletal controls the pose pipeline runs. `unique_ptr`
    //   because a control is polymorphic and holds bone indices resolved against THIS rig, and the
    //   Animator is its one owner — so it answers both questions in its type and owes no row below.
    //   The number is READ OFF THIS BRANCH'S RUN; per the six merges above, it will not survive the
    //   merge and must be re-read there.
    //   -> +1 with A11 (837 -> 838), Unique, and it owes no row for the reason A3's did not:
    //   `Animator::m_Rig` is the optional control-rig stage (T5.4) and the Animator is its one owner.
    //   `ControlRigStage` itself adds NO pointer member of any form -- it holds its `ControlHierarchy` by
    //   value and takes the skeleton, the pose and the component view per call, which is the same refusal
    //   to store a lifetime T5.1 and T5.3 made. Read off THIS branch's run; per the six merges above it
    //   will not survive the merge and must be re-read there.
    //   -> +3 with A10 (834 -> 837), all three Raw and all three CallScoped: `ControlKeyTarget`'s
    //   {Hierarchy, Skeleton, Clip}. T5.3's keyer is deliberately stateless about all three -- the pack
    //   is what lets it be -- so the three rows share one argument rather than inventing three. Read off
    //   THIS branch's run; per the six merges above it will not survive the merge and must be re-read.
    //   -> +2 with A12 (838 -> 840), one Raw and one Shared, and they are two different arguments.
    //   The Raw is `AnimationECSSystem::m_AssetManager`: the system now resolves a ControlRigComponent's
    //   handle to a parsed `.derig`, and it takes the manager the same way it already takes the animation
    //   library -- by the host's own guarantee, with a register row of its own beside that one. The Shared
    //   is `ControlRigPanel::m_Scene`, which is what every panel in this editor holds and needs no new
    //   argument. Read off THIS branch's run; per the merges above it will not survive the merge and must
    //   be re-read there.
    //   -> +3 with A15 (840 -> 843), two Raw and one Shared, and all three come from the anim graph
    //   becoming a `.danimgraph` asset. The two Raw are `AnimGraphPanel::m_AssetManager` and
    //   `AnimationComponentWidget::m_AssetManager`: the graph is a FILE now, so the window that edits one
    //   has to resolve a handle to save it and the Details slot has to offer the project's graphs — both
    //   take the manager the way every other panel and widget in this editor takes it, with a register row
    //   of its own. The Shared is `AnimGraphAsset::m_Graph`, and it is shared ON PURPOSE rather than
    //   incidentally: it is the one object every entity naming that file points at, which is what makes
    //   an edit reach all of them instead of one. Read off THIS branch's run; per the merges above it will
    //   not survive the merge and must be re-read there.
    //   -> +1 with B2 (843 -> 844), one Raw, and it is `LoadTimingScope::m_Parent`. The world
    //   programme's synchronous-load detector times nested loads by having each scope hand its duration
    //   up to the one enclosing it, and the enclosing scope is reached by the only thing that can name a
    //   stack object: its address. Its row argues the guard from the LIFO order of the call stack, the
    //   thread-local stack of open scopes, and the four deleted copy/move operators -- the last of which
    //   is what makes "one parent per scope" a property of the type rather than of the caller.
    //   -> +4 with B6 (844 -> 848), ALL FOUR Shared, and all four are the demand-driven asset model.
    //   `AssetRef<T>::m_Payload` is the reference type's whole state: a consumer holding one that says
    //   Ready must be able to use what it names for as long as it holds it, and the service that filled
    //   it may be cleared by a project close in between -- several owners whose deaths are not ordered,
    //   which is Q1's shared answer rather than a habit. `Record::Payload` and `LoaderState::Live` in
    //   AsyncAssetLoader.cpp are the KEEP-ALIVE itself: a worker thread is inside `Load()` on that asset
    //   while the main thread may be releasing the request, so the object has exactly two owners whose
    //   order is not knowable, which is the textbook case. And `CloudNoiseService::Entry::Source` is the
    //   announced-but-unread asset the service can later ask to be read -- held by the AssetManager and
    //   by this service, neither of which outlives the other by construction.
    //   and +1 Unique with the same task (848 -> 849): `AsyncAssetLoader::m_State`. The loader's queues
    //   began as a file-local `static`, which compiles and works and made every method of the class
    //   `static`-able -- clang-tidy said so before a reader would have, and a singleton whose methods are
    //   all static is a namespace wearing a class. The state is the loader's now, held behind a pointer
    //   only so the header carries no mutex and no map. One owner with a known lifetime, which is Q1's
    //   unique answer.
    //   and A25 moves it again, +2 Raw and +1 Unique (849 -> 852). The two Raw are one map and one
    //   reference to a map, both keyed by clip address and both covered by `m_TrackBinding`'s own
    //   argument -- see their rows. The Unique is `Animator::m_Retarget`: the source rig and its
    //   retargeter, held behind a pointer because null IS the answer to "does this entity retarget",
    //   with no second flag to disagree with it, and because a `Skeleton` value member would cost every
    //   Animator in the project four vectors for a feature most of them do not use. One owner with a
    //   known lifetime, which is Q1's unique answer.
    //   and +1 Raw with T2.4 (367 -> 368, 852 -> 853): `ContentKindSpec::Root`. The content census that
    //   replaced seventeen hand-written (root, extension) pairs in AssetPreloader holds the live path
    //   constant by ADDRESS, not by value, so a row follows a `SetProjectRoot` remap — the same choice,
    //   for the same reason, as `PackagedTree::Tree`, which is the row above it in the register.
    //
    //   and +1 Raw with A28 (368 -> 369, 853 -> 854): `ControlKeyTarget::AuthoredPose`. The keyer learned
    //   to key BONES as well as controls, and a bone's value lives in the Animator's authoring buffer
    //   rather than in the control hierarchy — so the argument pack grew a fourth member with the same
    //   call-scoped guard as its three neighbours. It is a pointer and not a value for the reason
    //   `EndInteraction` exists: §971 keys the value the drag ENDED at, which means reading the buffer at
    //   the commit rather than remembering a copy from the write.
    //
    //   and +4 Raw with A29 (369 -> 373, 854 -> 858): the pose/clip undo transaction and the command it
    //   pushes, two pointers each. All four take ByteCommand's guard rather than a stronger one, and the
    //   reason is a FACT about the types and not a preference: `AnimationComponent::Animator` is a
    //   `unique_ptr`, so there is no `weak_ptr` to observe it with, and the clip lives inside an
    //   `AnimationAsset` an eviction may unload — which is the argument `ControlKeyTarget` already makes
    //   at its own declaration when it refuses to store one. The command reports `IsVolatile()`, so
    //   `DropVolatile` drops it on every structural change and every selection change.
    //
    //   and +2 Raw with A32 (373 -> 375, 858 -> 860): `SequencerPanel::SectionTarget`, which is what a
    //   section edit acts on -- the animator and the clip, resolved together from one entity. CallScoped
    //   and not the transaction's volatile guard, and the difference is the point: this struct is built
    //   BY VALUE per call and never stored, so the frame's own structure closes Q2. The two pointers that
    //   DO outlive the frame are the four A29 rows above, and they pay for it with `IsVolatile()`.
    //
    //   THESE TWO ROWS ARRIVED ON DIFFERENT BRANCHES AND BOTH EDITED THIS NUMBER. Each was green
    //   against its own base (365 -> 367 and 365 -> 366) and the sum is neither; a merge that took
    //   either side whole would have been a number that compiles, passes review, and is wrong. The
    //   count is derived from the rows, so the rows are what to read when it moves.
    //   and +2 Raw with A33 (375 -> 377, 860 -> 862): `UIClipCommand::m_Clip` and
    //   `UIClipEditTransaction::m_Clip`, the UI timeline's own undo entry and the transaction that pushes
    //   it. Same guard and same drop as the four A29 rows, for a hazard that is if anything plainer: the
    //   pointer is INTO an entt pool, so the address can die while the entity lives. The transaction adds
    //   one guarantee the pose one does not -- the panel compares Subject() with the component it
    //   resolved this frame and abandons the entry when they differ.
    //   and +1 Raw with A33's second half (377 -> 378, 862 -> 863): `ControlPoseCommand::m_Hierarchy`.
    //   The control drag had NO undo entry at all -- LightGizmoRenderer wrote m_ControlPoseAtGrab and
    //   m_ControlDragOwner and read neither, under a comment saying an entry was pushed from them. The
    //   UUID goes with this change too, and it moves NO number here -- a Common::UUID is not a pointer
    //   and was never in this census, which is exactly why nothing went red while it sat there unread.
    //   and +2 Raw, +1 Shared, +1 Unique with Г28: `MeshRenderer` stopped accumulating every instanced
    //   batch into one triple of scratch vectors and now keeps one `InstancedBatchSet` per RECORDING
    //   material, because a batch has to be drawn with its own `.demat`'s (Instanced x pass) material or
    //   it loses every texture that material names. The two Raw are that set's `Mat` and `Inst`; the
    //   Shared is `m_InstancedVariantInstances`; the Unique is `m_ScratchInstSets`, and it is a
    //   `vector<unique_ptr<...>>` RATHER THAN a `vector<T>` for a reason this register cares about: the
    //   accumulation hands out a pointer to one set and goes on to create others, so a growing vector of
    //   values would move the pointee under a live pointer.
    //
    //   and +3 Raw / +2 Unique with U9, where a Scene stopped holding ONE renderer and one camera and
    //   started holding a LIST of views. Every line of it is a member, not a number:
    //     gone   Scene::m_SceneRenderer (Raw), Scene::m_MainCamera (Weak), Scene::m_ActiveCamera (Shared);
    //     new    SceneViewList::View::Renderer (Raw) and ::Camera (Shared) -- the same two values, now
    //            one pair PER VIEW instead of one pair per scene;
    //     new    ExternalPassContext::Renderer (Raw) -- a pass runs once per view and has to know which
    //            one it is drawing into;
    //     new    ViewportPanel::m_ViewRenderer (Raw), EditorLayer::SceneViewport::Viewport (Raw),
    //            SceneViewport::Renderer (Unique), the m_ExtraViewports element (Unique), and
    //            SceneViewport::Scene (Weak).
    //   Shared and Weak do not move for U9: each gained exactly what it lost.
    //
    //   THE TWO ARRIVED ON DIFFERENT BRANCHES AND BOTH EDITED THESE NUMBERS. Each was green against its
    //   own base and the sum is neither; the totals below are DERIVED from the rows above (Raw
    //   378+2+3, Shared 330+1+0, Unique 117+1+2, Weak unmoved) and then CONFIRMED by running, never
    //   pasted from one side. Same shape as the merge two days ago, and the reason the register pins
    //   rows rather than a count: a number can be fixed by editing the number.
    //     E4 (2026-09-23) added FOUR raw members and was green against its own base, while dev was green
    //     against itself -- the sum was neither, and the integrator pushed it red. Exactly the failure
    //     this comment already described one merge earlier, which is the point: the register survives it
    //     because a row cannot be satisfied by editing a number. The four are the two literal columns of
    //     GizmoIconRow, ViewportCameraPresetRow::Name, and LightGizmoRenderer::m_UIHelper. Raw 383+4.
    //     WP6 (2026-09-23) added three raw members -- PhysicsBodyLifetime::m_World and ::m_Registry, and
    //     AttachmentSystem::m_HookedRegistry, each with a row -- and one unique_ptr,
    //     PhysicsECSSystem::m_Lifetime. Raw 387+3, Unique 120+1.
    //     M1 (2026-09-23) removed two: ModelingPanel's ToolBtn table (Icon, Name) went with the five
    //     placeholder Create buttons it described. Raw 390-2.
    //   and +2 Unique with SP1 (878 -> 880 after WP6 and M1): the start-up splash, owned by `Sandbox::m_Splash`
    //   from before the renderer exists until the editor layer takes it (`EditorLayer::m_Splash`). One object
    //   handed over once -- a move, never a second owner -- so Unique is the honest form and neither owes a row.
    //   and WP5b (2026-09-24) added WorldStreamer::m_Scene and ::m_Assets (two rows) and two unique_ptrs,
    //   EditorLayer::m_WorldStreamer and RuntimeLayer::m_WorldStreamer, each the one owner of the streamer of
    //   the world it plays. Raw 388+2, Unique 123+2.
    //     ENV1 (2026-09-24) added one raw member, SampledCube::Cube, with its row: the cubemap preview's
    //     resolver now answers the cube AND the look it is read with. Raw 388+1.
    //     M4 (2026-09-23) added three shared_ptr<const Geometry::EditMesh> members, and each is shared ON
    //     PURPOSE: the mesh is IMMUTABLE once on a component, so the undo record holding the old one by
    //     reference IS the snapshot, with no copy. StaticMeshComponent::EditableMesh (the entity's source of
    //     truth), PolyEditTool::m_DragBefore (the mesh a drag started from) and the EditMeshCommand's
    //     m_Before/m_After declaration. Shared 331+3. PolyEdit's picking target holds the ENTITY, not a
    //     StaticMeshComponent*, so Raw does not move: that pointer would have been into an entt pool.
    //   LS-4 (2026-09-24) added three: the landscape tile's heightmap as it travels to the terrain pass,
    //   DrawLandscapeTileCommand::Heightmap and TerrainDrawData::Heightmap (raw, frame-scoped, each with a
    //   row), and its owner, LandscapeECSSystem::TileGpu::Heightmap (shared). Raw 389+2, Shared 331+1.
    //   M13 (2026-09-24) added two shared_ptr<const Geometry::EditMesh>, shared for M4's reason (the mesh is
    //   immutable once on a component): MeshElementSelection::m_Mesh, the mesh the element selection was
    //   last checked against - its IDENTITY is how an edit is noticed and the selection pruned - and the
    //   Select Elements tool's per-frame Target::Mesh. The tool's painter holds its draw list by reference,
    //   so Raw does not move. Shared 335+2.
    //   M14 (2026-09-24) added two: EditMeshOperations' LayerRecord::Layer (raw, call-scoped, with a row) -
    //   an attribute layer of the mesh the operation is building - and EditMeshCommand::m_Alongside
    //   (unique), the selection change undone and redone with a mesh operation as one step. Raw 391+1,
    //   Unique 123+1.
    EXPECT_EQ( CountOf( Form::Raw ), 394 );
    EXPECT_EQ( CountOf( Form::Shared ), 337 );
    EXPECT_EQ( CountOf( Form::Unique ), 126 );
    EXPECT_EQ( CountOf( Form::Weak ), 38 );
    EXPECT_EQ( (int)Members().size(), 895 )
         << "the population moved. That is not a number to adjust -- it means a pointer member was added "
            "or removed, and the two questions at the top of this file are owed an answer for it.";
}

// ------------------------------------------------------------------------------------------------
// Every raw pointer answers Q2
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, EveryRawPointerMemberNamesItsGuard )
{
    ASSERT_FALSE( Members().empty() );

    int unlisted = 0;
    for ( const Member& m : Members() )
    {
        if ( m.Kind != Form::Raw )
            continue;
        if ( FindRow( m ) != nullptr )
            continue;
        ++unlisted;
        ADD_FAILURE() << m.File << ":" << m.Line << " " << m.Class << "::" << m.Name << " (" << m.Decl
                      << ")\nis a raw pointer member with no row in the register. A raw pointer answers NEITHER "
                         "ownership question by itself, so add a row to pointer_ownership_register.hpp saying:\n"
                         "  (1) who is obliged to destroy the pointee, and\n"
                         "  (2) what stops it dying before this object does.\n"
                         "If the answer to (2) is 'nothing', that is a defect: fix it, or file it as Guard::Debt "
                         "with the task that owns the fix.";
    }
    EXPECT_EQ( unlisted, 0 );
}

TEST( PointerOwnership, TheRegisterDescribesMembersThatStillExist )
{
    // The other direction, and the one a census normally loses first: a row whose member was renamed or
    // deleted goes on asserting nothing while looking like coverage. DeviceLostCensus lost sight this
    // way, which is why the check is here rather than assumed.
    ASSERT_FALSE( Members().empty() );

    for ( const Row& r : Register() )
    {
        const bool alive = std::any_of(
             Members().begin(), Members().end(), [&r]( const Member& m )
             { return m.Kind == Form::Raw && m.File == r.File && m.Class == r.Class && m.Name == r.Member; } );
        EXPECT_TRUE( alive ) << r.File << " " << r.Class << "::" << r.Member
                             << " has a register row but the scan no longer finds the member. Delete the "
                                "row, or find out why the scan stopped seeing it.";
    }
}

TEST( PointerOwnership, EveryRowCarriesAnArgument )
{
    for ( const Row& r : Register() )
    {
        EXPECT_GT( std::string( r.Why ).size(), 30u )
             << r.Class << "::" << r.Member
             << " has no argument. A row without one is a name in a list, which is what this census "
                "exists instead of.";
        if ( r.How != Guard::Debt )
            continue;

        // A DEBT WITH NO TASK NAME IS UNREADABLE IN A MONTH. The same rule ConfigOwnership enforces on
        // its own debt register, for the same reason.
        EXPECT_FALSE( std::string( r.Task ).empty() )
             << r.Class << "::" << r.Member
             << " is recorded as a live lifetime defect and names no task. Name the task that owns the "
                "fix, or fix it here.";
    }
}

// ------------------------------------------------------------------------------------------------
// The guards that rest on a checkable fact are checked
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, MaterialPropertyStorageIsAddressStable )
{
    // 46 of the 322 rows rest on ONE argument: a material's cached `Texture2DProperty*` cannot dangle
    // because the property lives in the material's own executor. That argument has three legs and all
    // three are facts about the source, so all three are checked here rather than believed.
    ASSERT_FALSE( RepoRoot().empty() );

    const std::string executorHpp =
         ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/MaterialExecutor.hpp" );
    const std::string executorCpp =
         ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/MaterialExecutor.cpp" );
    const std::string materialHpp = ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Materials/Material.hpp" );

    // (1) The storage holds HANDLES, not objects. `std::vector<T>` would move every property when it
    //     grew, and all 46 cached pointers would dangle on the next `emplace`.
    EXPECT_NE( executorHpp.find( "using PropertyStorage = std::vector<std::shared_ptr<T>>" ), std::string::npos )
         << "MaterialExecutor::PropertyStorage is no longer a vector of shared_ptr. If the properties "
            "are stored BY VALUE, every material's cached property pointer dangles the moment the "
            "vector grows.";

    // (2) The material owns the executor, so the observed dies with the observer.
    EXPECT_NE( materialHpp.find( "std::unique_ptr<MaterialExecutor> m_MaterialExecutor" ), std::string::npos )
         << "Material no longer owns its MaterialExecutor by unique_ptr; the 46 cached property pointers "
            "lose the reason they cannot outlive their pointee.";

    // (3) The table is built ONCE, in the constructor, and never rebuilt. A shader hot-reload that
    //     called InitializeProperties() again would leave every cached pointer pointing at a released
    //     property.
    int initializeCalls = 0;
    for ( std::size_t at : Text::WordPositions( executorCpp, "InitializeProperties" ) )
    {
        // The definition itself (`void MaterialExecutor::InitializeProperties()`) is not a call.
        const std::string before = executorCpp.substr( at < 64 ? 0 : at - 64, at < 64 ? at : 64 );
        if ( before.find( "MaterialExecutor::" ) != std::string::npos )
            continue;
        ++initializeCalls;
    }
    EXPECT_EQ( initializeCalls, 1 ) << "InitializeProperties() is called " << initializeCalls
                                    << " times. It must be called exactly once, from the constructor: a "
                                       "second call rebuilds the property table under 46 cached pointers.";

    for ( const char* mutation : { "PropertiesStorage.clear", "PropertiesStorage.erase",
                                   "PropertiesStorage.resize", "PropertiesStorage.pop_back" } )
    {
        EXPECT_EQ( executorCpp.find( mutation ), std::string::npos )
             << "MaterialExecutor now does `" << mutation
             << "`. Removing a property releases the object 46 materials hold a raw pointer to.";
        EXPECT_EQ( executorHpp.find( mutation ), std::string::npos );
    }
}

TEST( PointerOwnership, TheUniformImageKeepsTheDescriptorAndNotTheImage )
{
    // The shape this audit wants preferred, asserted where it already exists: a class that must survive
    // an image it does not own keeps a COPY OF THE VALUE it needs (the descriptor, snapshotted at set
    // time) rather than a handle on somebody else's lifetime. Г12 removed the pointers; this is what
    // stops them coming back.
    ASSERT_FALSE( RepoRoot().empty() );

    for ( const char* header :
          { "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp",
            "Desert/Desert/Source/Engine/ShaderResources/API/Vulkan/VulkanUniformImageCube.hpp" } )
    {
        const std::string src = ReadRepoFile( header );
        EXPECT_NE( src.find( "VkDescriptorImageInfo m_DescriptorInfo" ), std::string::npos )
             << header << " must keep the descriptor it copied out at SetImage time.";
        EXPECT_EQ( src.find( "Graphic::Image2D* m_Image" ), std::string::npos )
             << header << " has taken a raw pointer to an image it does not own back as a member.";
        EXPECT_EQ( src.find( "Graphic::ImageCube* m_Image" ), std::string::npos ) << header;
    }
}

TEST( PointerOwnership, NoRawPointerMemberIsDeletedByItsHolder )
{
    // AN OWNING RAW POINTER WEARING AN OBSERVER'S CLOTHES. `delete m_Something` in a class whose
    // `m_Something` is a raw pointer member means Q1's answer is "this class" and the type does not say
    // so — which is the one case where the audit's verdict is mechanical: it should be a unique_ptr.
    // The two legitimate owning raws in this tree hold VMA handles, which have no C++ destructor and
    // are released through the allocator; neither is `delete`d.
    ASSERT_FALSE( Members().empty() );

    // THE MEMBER IS DECLARED IN THE HEADER AND DELETED IN THE .cpp, so scanning only the declaring file
    // sees nothing. Caught by the mutation check for this very test: a `delete m_Backdrop;` planted in
    // Render2D.cpp left it green. The class's own translation unit is where a class frees its own
    // members, so both halves of the pair are read.
    std::map<std::string, std::vector<std::string>> byFile;
    for ( const Member& m : Members() )
    {
        if ( m.Kind != Form::Raw )
            continue;
        byFile[m.File].push_back( m.Name );
        if ( m.File.size() > 4 && m.File.compare( m.File.size() - 4, 4, ".hpp" ) == 0 )
        {
            const std::string sibling = m.File.substr( 0, m.File.size() - 4 ) + ".cpp";
            if ( fs::exists( fs::path( RepoRoot() ) / sibling ) )
                byFile[sibling].push_back( m.Name );
        }
    }

    for ( const auto& [file, names] : byFile )
    {
        const std::string src = ReadRepoFile( file.c_str() );
        for ( std::size_t at : Text::WordPositions( src, "delete" ) )
        {
            const std::size_t i    = Text::SkipSpace( src, at + 6 );
            const std::string what = Text::IdentAt( src, i );
            for ( const std::string& name : names )
            {
                if ( what != name )
                    continue;
                ADD_FAILURE() << file << ":" << LineOf( src, at ) << " deletes its own raw pointer member `"
                              << name
                              << "`. That is sole ownership spelled without saying so -- make it a "
                                 "std::unique_ptr, which cannot be forgotten on an early return.";
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------
// The shared_ptr half: a claim of shared ownership, and what it costs when it is not true
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, SharedOwnershipIsTheMajorityAndThatIsTheMeasuredAnswer )
{
    // THE AUDIT'S LARGEST SINGLE RESULT IS A REFUSAL, and it is recorded here so the next person does
    // not re-derive it. Roughly two fifths of the members in these trees are `shared_ptr`, and for the GPU
    // resources that is the CORRECT form rather than a habit: an Image2D is held at once by the
    // framebuffer that allocated it, by the descriptor sets that sample it and by the deletion queue
    // that outlives both, and no two of those have an ordered death. Converting them to `unique_ptr`
    // would not be a cleanup, it would be a use-after-free.
    //
    // What the audit did NOT find is worth saying plainly: not one `shared_ptr` member in these trees
    // sits in a per-frame hot loop where the atomic refcount pair is measurable. The cost of a wrong
    // `shared_ptr` here is a false impression of shared ownership, and the register's job is to make
    // the true owner findable instead of mass-replacing them for uniformity -- churn that would hide
    // the seven real findings in a diff of two hundred files.
    // 324 -> 326 with Ю11's UIMaterialCache: its target framebuffer and each entry's pipeline. Both are
    // genuinely shared -- a Framebuffer is held by the scene that made it and by every pipeline compiled
    // against its render pass, and a GraphicsPipeline by the cache entry and by the specification it was
    // created from -- so they belong on this side of the census rather than being narrowed for tidiness.
    // 326 -> 321 with Г25: the five GPU resources of the procedural grass generator, removed with it.
    // 321 -> 319 with Ю13: NOT a conversion of anything. Two members named `Asset` whose types are a
    // `std::string` and an `AssetHandle` were being counted here because the alias match ran over the
    // member's name as well as its type; they are not pointers and never were.
    // 319 -> 323 with Г26: the (Instanced x GBuffer) shader, pipeline, material and material instance --
    // the deferred twins of the forward instanced four that were already on this side.
    // NEITHER BRANCH'S TOTAL SURVIVES THE MERGE: Ю13 alone says 319, Г26 alone says 325, and the tree
    // says 323. Read off a run; see the arithmetic at TheScanFindsTheCensusedPopulation.
    // 323 -> 324 with Ю16: UIRenderTextureCache::Capture::Scene, the world one UI element shows.
    // 324 -> 325 with A12: ControlRigPanel::m_Scene, which is what every panel in this editor holds.
    // 325 -> 326 with A15: AnimGraphAsset::m_Graph. Shared BY DESIGN and not by habit — every entity that
    // names one `.danimgraph` holds this same object, so an edit in the graph window is the graph all of
    // them evaluate next frame. A unique_ptr here, or a copy per entity, would compile and would restore
    // the per-entity blob that schema step 21 removed.
    // 326 -> 330 with B6: the four members of the demand-driven asset model. Three of them (the
    // reference's payload, the request record's payload, the loader's live map) are shared because a
    // WORKER THREAD is inside the asset while the main thread may be dropping the request -- two owners
    // whose deaths cannot be ordered, which is the one case Q1 answers with shared_ptr and not a
    // preference. The fourth, an announced-but-unread asset in the noise service, is co-held with the
    // AssetManager. See the arithmetic at TheScanFindsTheCensusedPopulation.
    // 330 -> 331 with Г28: MeshRenderer::m_InstancedVariantInstances, one MaterialInstance per instanced
    // material variant. Shared because MaterialInstancePtr is the engine's one spelling of an instance
    // handle and every other holder of one is a shared_ptr too; what makes it safe is not the count but
    // the stamp check that empties the map when MaterialService retires the materials they point at.
    // 331 -> 334 with M4: StaticMeshComponent::EditableMesh, PolyEditTool::m_DragBefore and the
    // EditMeshCommand's m_Before/m_After. Shared BY DESIGN: the EditMesh is immutable once set on a
    // component, so the undo record keeping the old one by reference is the snapshot, with no copy and
    // nothing able to change it under the history. See TheScanFindsTheCensusedPopulation.
    // 331 -> 332 with LS-4: LandscapeECSSystem::TileGpu::Heightmap, a GPU image — the case this test's
    // opening paragraph describes (the cache, the descriptor sets and the deletion queue all hold it).
    // 334 -> 335: M4's three and LS-4's one, merged 2026-09-24.
    // 335 -> 337 with M13: MeshElementSelection::m_Mesh and the Select Elements tool's Target::Mesh, the
    // same immutable EditMesh - see TheScanFindsTheCensusedPopulation.
    EXPECT_EQ( CountOf( Form::Shared ), 337 );
    EXPECT_GT( CountOf( Form::Shared ), CountOf( Form::Unique ) + CountOf( Form::Weak ) );
}

TEST( PointerOwnership, NoECSSystemHandsARenderCommandAnAddress )
{
    // THE RULE A8-3 COST, WRITTEN WHERE IT CAN FAIL. A render command is recorded by an ECS system and
    // read later — after every remaining system has run, ScriptSystem and its user Lua among them, and
    // after the command buffer has been executed. Anything the command holds must therefore be either a
    // value or a CO-OWNED handle; the address of a component member is neither, and it is the shape that
    // put five members of this register in debt.
    //
    // The check is deliberately blunt: an argument to `Emplace<...>` may not begin with `&`. It catches
    // `&mesh.RuntimeSlotPtrs` and `&ism.InstanceTransforms`, which is what it is for, and it also catches
    // the address of a LOCAL — which is equally wrong here for the same reason and equally worth a
    // conversation. Measured on the fixed tree: sixteen Emplace sites across eight systems, zero
    // addresses.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const fs::path systems = fs::path( root ) / "Desert/Desert/Source/Engine/ECS/System";
    int            sites   = 0;

    std::error_code ec;
    for ( auto it = fs::recursive_directory_iterator( systems, ec );
          !ec && it != fs::recursive_directory_iterator(); ++it )
    {
        const fs::path& path = it->path();
        if ( path.extension() != ".hpp" && path.extension() != ".cpp" )
            continue;

        const std::string src = ReadRepoFile( fs::relative( path, root ).generic_string().c_str() );
        for ( std::size_t at : Text::WordPositions( src, "Emplace" ) )
        {
            const std::size_t open = src.find( '(', at );
            if ( open == std::string::npos || src.find( '<', at ) > open )
                continue;
            ++sites;

            int depth = 0;
            for ( std::size_t i = open; i < src.size(); ++i )
            {
                if ( src[i] == '(' )
                    ++depth;
                else if ( src[i] == ')' && --depth == 0 )
                    break;
                if ( depth != 1 || ( src[i] != '(' && src[i] != ',' ) )
                    continue;

                const std::size_t arg = Text::SkipSpace( src, i + 1 );
                if ( arg >= src.size() || src[arg] != '&' )
                    continue;
                if ( arg + 1 >= src.size() || !Text::IsIdentChar( src[arg + 1] ) )
                    continue;

                ADD_FAILURE()
                     << fs::relative( path, root ).generic_string() << ":" << LineOf( src, arg )
                     << " hands a render command the ADDRESS of `" << Text::IdentAt( src, arg + 1 )
                     << "`.\nA command outlives the system that recorded it: every later ECS system runs "
                        "before it is executed, ScriptSystem among them, and that one runs user Lua which "
                        "can add or destroy entities. Pass a value, or a co-owned handle "
                        "(Graphic::MaterialSlotBinding is the one this rule was written for).";
            }
        }
    }

    EXPECT_GT( sites, 8 ) << "the scan found almost no Emplace sites -- either the commands are recorded "
                             "some other way now, or this check has stopped looking at anything.";
}

// ------------------------------------------------------------------------------------------------
// The fact three Debt rows rest on, instantiated rather than reasoned about
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, EnttComponentAddressesAreNotStable )
{
    // FIVE OF THE REGISTER'S ROWS SAY "the address of a std::vector member of an ECS component", and
    // whether that is a defect turns entirely on one property of the vendored entt: does a component
    // keep its address when the pool changes? THE ANSWER IS NO, and it is asserted here rather than
    // read off a header, because the header is the thing that would change under us.
    //
    // `storage<Entity, Type>` keeps `std::vector<object_type> instances` (entt.hpp) — components live
    // BY VALUE in a flat vector. `emplace` push_backs, so growth moves every component; `erase` is
    // swap-and-pop, so destroying one entity moves ANOTHER component over it. Newer entt uses paged
    // storage and would make both of these stable; this test is what will say so on the day the
    // submodule moves.
    //
    // Why it matters here and not merely in the abstract: MeshECSSystem records `&mesh.RuntimeSlotPtrs`
    // into the render command buffer, ScriptSystem is registered AFTER it and runs Lua (`World
    // .spawnMarker` adds a StaticMeshComponent, `entity:destroy()` removes one), and the commands are
    // only executed and dereferenced afterwards. The two ends of that window are in different files and
    // nothing between them says the pointer must survive it.
    struct Probe
    {
        std::vector<int> Slots;
        int              Filler = 0;
    };

    entt::registry reg;

    // Growth. Reserving would only postpone it; the point is that nothing in the engine reserves.
    const auto first                  = reg.create();
    reg.emplace<Probe>( first ).Slots = { 1, 2, 3 };
    const void* before                = &reg.get<Probe>( first ).Slots;

    bool moved = false;
    for ( int i = 0; i < 64 && !moved; ++i )
    {
        const auto e                  = reg.create();
        reg.emplace<Probe>( e ).Slots = { i };
        moved                         = ( &reg.get<Probe>( first ).Slots ) != before;
    }
    EXPECT_TRUE( moved )
         << "adding components no longer moves the ones already in the pool. That would make five of "
            "this register's Debt rows obsolete -- check whether the ECS storage became paged, and if "
            "it did, close A8-3 with this test as the evidence.";

    // Swap-and-pop. The LAST component in the pool is moved over the erased slot, so a pointer to the
    // last one is stolen from under its holder even though that entity was never touched.
    entt::registry two;
    const auto     keep               = two.create();
    const auto     tail               = two.create();
    two.emplace<Probe>( keep ).Slots  = { 1 };
    two.emplace<Probe>( tail ).Slots  = { 2, 3 };
    const std::vector<int>* tailSlots = &two.get<Probe>( tail ).Slots;

    two.destroy( keep );
    ASSERT_TRUE( two.valid( tail ) ) << "the surviving entity must still be valid";
    EXPECT_NE( &two.get<Probe>( tail ).Slots, tailSlots )
         << "destroying ANOTHER entity no longer moves this one's component. Same question as above.";
}

TEST( PointerOwnership, EditorLayerDeclaresItsHostsBeforeItsPanels )
{
    // TWENTY ROWS OF THE EDITOR HALF REST ON ONE FACT: a panel holding `AssetManager*` or
    // `AnimationLibrary*` cannot outlive what it points at, because EditorLayer declares those members
    // BEFORE m_Panels and C++ destroys members in reverse declaration order. That is not a property of
    // the panels, it is a property of ONE LINE ORDER in one header — the weakest kind of guarantee in
    // this register, and the easiest to break by moving a member while tidying up. So it is asserted.
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const std::string src = ReadRepoFile( "Editor/Source/EditorLayer.hpp" );
    ASSERT_FALSE( src.empty() );

    const std::size_t panels = src.find( "PanelRegistry m_Panels" );
    ASSERT_NE( panels, std::string::npos ) << "EditorLayer no longer declares m_Panels -- the twenty "
                                              "Guard::HostOutlivesUs rows that cite this order need "
                                              "re-deriving, not a renamed search string.";

    for ( const char* host :
          { "std::shared_ptr<Assets::AssetManager>", "m_AnimationLibrary", "OpenDocuments m_OpenDocuments" } )
    {
        const std::size_t at = src.find( host );
        ASSERT_NE( at, std::string::npos ) << host << " is no longer a member of EditorLayer.";
        EXPECT_LT( at, panels )
             << host
             << " is now declared AFTER m_Panels, so it is destroyed BEFORE the panels that point at it. "
                "Every panel holding a raw pointer to it is then reading freed memory during its own "
                "destructor. Move it back above m_Panels, or give the panels a weak handle.";
    }

    // AND THE SAME ORDER FOR DOCUMENTS, WHICH THIS TEST DID NOT COVER. Half a dozen rows in the register
    // are about panels that are no longer panels: U7 moved the anim graph and the particle editor into
    // m_OpenDocuments and U7-2 moved the sequencer and the UI editor, and a DOCUMENT holding
    // `AnimationLibrary*` is guarded by its host preceding m_OpenDocuments — not by the host preceding
    // m_Panels, which is a different member and could satisfy the loop above while this failed.
    //
    // The rows said "before m_Panels" for a whole release after their subject stopped being a panel. They
    // happened to be true, because m_OpenDocuments is itself above m_Panels; a true sentence about the
    // wrong container is exactly the guarantee that stops holding the day somebody reorders one of the
    // three, and nothing would have gone red.
    const std::size_t documents = src.find( "OpenDocuments m_OpenDocuments" );
    ASSERT_NE( documents, std::string::npos );
    for ( const char* host : { "std::shared_ptr<Assets::AssetManager>", "m_AnimationLibrary" } )
    {
        const std::size_t at = src.find( host );
        ASSERT_NE( at, std::string::npos ) << host << " is no longer a member of EditorLayer.";
        EXPECT_LT( at, documents )
             << host
             << " is now declared AFTER m_OpenDocuments, so it is destroyed BEFORE the documents that point "
                "at it. Every document holding a raw pointer to it is then reading freed memory during its "
                "own destructor. Move it back above m_OpenDocuments, or give the documents a weak handle.";
    }
}

// ------------------------------------------------------------------------------------------------
// A8-1: the condition a cache eviction has to respect, as an assertion rather than a comment
// ------------------------------------------------------------------------------------------------

TEST( PointerOwnership, Render2DExecutorRetirementRespectsFramesInFlight )
{
    using Desert::Graphic::Render2D::MayRetireExecutor;

    // The three Render2D executor caches are keyed by a texture ADDRESS and, until A8-1, nothing ever
    // removed an entry: every viewport resize and every new texture left a MaterialExecutor and its
    // descriptor set behind for the life of the process. The reason the obvious `cache.clear()` was
    // REFUSED rather than written is the whole content of this test — destroying an executor destroys
    // descriptor sets a submitted frame may still be reading, which corrupts a frame instead of crashing
    // a process. So the window is the condition, and here it is where it can fail.
    constexpr uint32_t kWindow = 9; // 3 frames in flight x 3 slots, a plausible DirtyLifetime()

    // Nothing may be retired inside the window, and the EDGE belongs to the GPU: an entry last used
    // exactly `window` frames ago is still reachable by the oldest frame in flight.
    for ( uint64_t age = 0; age <= kWindow; ++age )
        EXPECT_FALSE( MayRetireExecutor( 100, 100 + age, kWindow ) )
             << "an executor last used " << age << " frames ago was retired with a window of " << kWindow
             << ". A frame recorded against it may still be in flight, and destroying its descriptor set "
                "corrupts that frame rather than failing loudly.";

    EXPECT_TRUE( MayRetireExecutor( 100, 100 + kWindow + 1, kWindow ) )
         << "nothing is ever retired, which is the leak this was written to close.";
    EXPECT_TRUE( MayRetireExecutor( 0, 1000, kWindow ) );

    // A counter that has not moved (the first frames of a process, or two reads inside one frame) must
    // not retire anything -- and must not underflow while deciding, which is what an unsigned subtraction
    // written the obvious way does.
    EXPECT_FALSE( MayRetireExecutor( 100, 100, kWindow ) );
    EXPECT_FALSE( MayRetireExecutor( 100, 50, kWindow ) );

    // A window of zero would mean "retire on the next frame", which is inside the in-flight range for
    // every real configuration. It is not reachable through PropertyDirty::DirtyLifetime() -- that
    // function floors frames-in-flight at 3 -- and the sweep must not invent its own number.
    const std::string src = ReadRepoFile( "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" );
    EXPECT_NE( src.find( "PropertyDirty::DirtyLifetime()" ), std::string::npos )
         << "Render2D::RetireUnusedExecutors no longer takes its window from the material properties' "
            "own frame window. A literal here is a second answer to 'how long does a frame live', and the "
            "two would drift.";
    EXPECT_NE( src.find( "MayRetireExecutor(" ), std::string::npos )
         << "the sweep no longer goes through the tested predicate.";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
