#pragma once

// THE REGISTER: for every RAW pointer member the scan finds, the answer to the two questions.
//
// The forms answer for themselves and get no row here:
//
//   * `unique_ptr` — Q1 is in the type. One owner, named, and the destructor is not optional. Q2 cannot
//     arise because there is no observer.
//   * `weak_ptr`   — Q2 is in the type. The observed may die whenever it likes; `lock()` is the answer.
//   * `shared_ptr` — Q1 says "several owners and their deaths are unordered". Whether that claim is TRUE
//     is a separate question and it is a question of COST, not of correctness: a `shared_ptr` where a
//     `unique_ptr` would do costs an atomic pair and a false impression, never a crash. Those are counted
//     and argued in the suite, not here.
//
// A RAW POINTER MEMBER IS THE ONLY FORM THAT ANSWERS NEITHER QUESTION, so every one of them owes a row,
// and a row that cannot name a guard is a defect rather than a style. That is the whole register.

#include <string>
#include <vector>

namespace Desert::Tests::PointerCensus
{
    // The guarantees this tree actually has. Each is a FACT about the code, not a preference, and each
    // is stated so that the next reader can check it rather than trust it.
    enum class Guard
    {
        // The pointee has static storage duration — a string literal, or a table of them. It outlives
        // every object in the process, so Q2 is closed by the language.
        StaticStorage,

        // The pointee is owned by a smart pointer (or an arena) that is a MEMBER OF THE SAME OBJECT.
        // Observer and observed are destroyed together and in that order, so Q2 is closed by the layout.
        // The row must also say why the address is stable: a `vector<shared_ptr<T>>` does not move its
        // pointees when it grows; a `vector<T>` does, and would make this guard false.
        OwnedByThisObject,

        // A BACK-POINTER: the pointee owns (directly or through one link) the object holding it. It
        // cannot be destroyed without destroying the holder first, so Q2 is closed by containment.
        ObservedContainsUs,

        // AN ARGUMENT PACK. The struct is built at a call site and consumed inside that call; every
        // pointer in it comes from a local or a member of the caller, which outlives the call by
        // construction. Q2 is closed by the call stack.
        CallScoped,

        // Recorded during one frame and consumed before that frame ends, with the pointee owned for at
        // least the frame by a source the row names. Q2 is closed by the frame's own structure — and
        // this guard is only honest when NOTHING inside the window can free the pointee, which is
        // exactly where the register's `Debt` rows come from.
        FrameScoped,

        // NEVER DEREFERENCED. The pointer's VALUE is an identity — a cache key, a batch discriminator.
        // Q2 does not arise for a value. But identity is not free: an address is REUSED after a free,
        // so a row with this guard must also say what stops a recycled address from being mistaken for
        // the old object.
        IdentityOnly,

        // THIS RAW POINTER OWNS. Named rather than converted, because the thing it owns is not a C++
        // object with a destructor (a VMA allocation handle, a byte range inside a mapping). Q1 is "this
        // class", and the row must name where the release happens.
        OwningRaw,

        // The holder re-points it from a live owner before every dereference. Q2 is closed by the
        // CALLER'S DISCIPLINE and not by the type — which is a weaker guarantee than the ones above and
        // is recorded as such: it is one refactor away from being false, and the row says what would
        // break it.
        ReboundBeforeEveryUse,

        // A POINTER-TO-MEMBER, which is not the address of an object at all: it is an offset within a
        // type, resolved against an instance the caller supplies. There is nothing for it to outlive, so
        // Q2 does not apply — and it is listed rather than skipped because the scan cannot tell the two
        // spellings apart and a member the census silently drops is the failure this whole suite exists
        // to prevent.
        MemberOffset,

        // THE POINTEE BELONGS TO A HOST WHOSE LIFETIME STRICTLY ENCLOSES OURS, and the row names the
        // order that makes it so. This is the editor's commonest answer and it is WEAKER than
        // ObservedContainsUs: containment is structural, an enclosing lifetime is a fact about
        // construction and destruction order that a future edit can move. Where that order is C++ member
        // declaration order, `EditorLayerDeclaresItsHostsBeforeItsPanels` asserts it.
        HostOutlivesUs,

        // THE POINTEE REMOVES ITSELF FROM THIS CONTAINER IN ITS OWN DESTRUCTOR. A self-registering list:
        // the constructor pushes `this`, the destructor erases it, so the container can never name a dead
        // object however the object is destroyed. Stronger than any caller-side discipline, and it is why
        // such a type must not be copyable or movable — a copy never registers yet erases the original's
        // entry on destruction.
        SelfDeregistering,

        // Q2 IS "YES, AND NOTHING STOPS IT". A defect, not a taste. The row MUST name the task that owns
        // the fix — an exception with no task name is unreadable in a month, which is why
        // ConfigOwnership's debt register carries the same rule and the same check.
        Debt
    };

    struct Row
    {
        const char* File;   // repository-relative, as the scan reports it
        const char* Class;  // the class or struct the member is declared in
        const char* Member; // the member's name
        Guard       How;
        const char* Why;       // the argument, in one sentence; the long form is the paragraph it cites
        const char* Task = ""; // required, and only meaningful, for Guard::Debt
    };

    // ----------------------------------------------------------------------------------------------
    // The arguments that more than one row rests on. A family shares one argument because it IS one
    // argument; writing it out 46 times would not make it truer and would hide the rows that differ.
    // ----------------------------------------------------------------------------------------------

    // THE MATERIAL PROPERTY CACHES (46 rows). A material subclass caches
    // `Get<Texture2DProperty>( "u_X" )`, which is `.get()` on a `shared_ptr` held by
    // `MaterialExecutor::m_Texture2DPropertiesStorage`. That executor is a `unique_ptr` MEMBER of the
    // same material (Material.hpp:156), so the observed cannot outlive the observer. Three facts make
    // the address stable, and `MaterialPropertyStorageIsAddressStable` checks all three:
    //   * the storage is `std::vector<std::shared_ptr<T>>` — growing it moves the handles, never the
    //     property objects;
    //   * `InitializeProperties()` has exactly one caller and it is the executor's constructor;
    //   * nothing anywhere clears, erases from or resizes a `*PropertiesStorage`.
    inline constexpr const char* kWhyMaterialProperty =
         "cached from the material's own MaterialExecutor (a unique_ptr member of this same object); the "
         "property lives in a vector<shared_ptr<T>> filled once in the executor's constructor and never "
         "cleared, so the pointee's address is stable and its death is this object's death";

    // THE PER-FRAME RENDER PAYLOADS. A struct filled by the collector and read by the pass, inside one
    // `Scene::UpdateSceneFrame`. Every pointer in it names a GPU resource owned by a render system of
    // the SAME SceneRenderer, or an asset held by a service for longer than the frame.
    inline constexpr const char* kWhyFramePayload =
         "filled by this frame's collector and read by this frame's pass; the pointee is owned for at "
         "least the frame by a render system of the same SceneRenderer";

    // THE ARGUMENT PACKS. Built at a call site, passed by const reference, dead at the semicolon.
    inline constexpr const char* kWhyArgumentPack =
         "an argument pack built at the call site and consumed inside that call; every pointer in it "
         "comes from a local or a member of the caller";

    // ----------------------------------------------------------------------------------------------
    // THE 137 ROWS. Sorted by file and line, which is the order the scan reports them in.
    // ----------------------------------------------------------------------------------------------
    //
    // The table is kept out of the formatter's hands: one row is three lines — where, what, why — and
    // reflowing it packs several rows onto a line and makes the 145 unreadable as a list. The directive
    // must be exactly this string; trailing text after "off" makes clang-format ignore it.
    // clang-format off
    inline const std::vector<Row>& Register()
    {
        static const std::vector<Row> rows = {
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp",
          "OutputBinding", "Image", Guard::ReboundBeforeEveryUse,
          "an entry of m_BoundOutputs, which is never cleared -- not even by Release() -- so after a "
          "renderer resize it names last frame's image; every dispatch site re-Sets it in the same "
          "function immediately before dispatching, and that is the whole guarantee" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp",
          "VulkanPipelineCompute", "m_BoundInputs", Guard::ReboundBeforeEveryUse,
          "the map is NEVER cleared and the pipelines are long-lived members, so a stale entry survives "
          "a resize that destroyed the image it names; it is dereferenced (dynamic_cast reads the vtable) "
          "only from RecordDescriptorsAndDispatch, and every call site re-Sets in the same function first" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanPipelineCompute.hpp",
          "VulkanPipelineCompute", "m_BoundStorageBuffers", Guard::ReboundBeforeEveryUse,
          "same as m_BoundInputs: never cleared, and safe only because every dispatch site re-Sets the "
          "binding from the live owner in the same function before dispatching" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanQueue.hpp",
          "VulkanQueue", "m_SwapChain", Guard::ObservedContainsUs,
          "the swapchain creates and owns the queue wrapper; the queue cannot outlive it" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp",
          "VulkanSwapChain", "m_CaptureAllocation", Guard::OwningRaw,
          "a VMA allocation handle this class allocates and frees itself (there is no C++ object to hold); "
          "released by TakeCapturedFrameRGBA8 and, since A8, by Release() as well -- before that a capture "
          "recorded on a frame that then resized or shut down leaked a full frame of GPU_TO_CPU memory" },
        { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp",
          "VulkanSwapChain", "m_VmaAllocation", Guard::OwningRaw,
          "VMA allocation handles for the capture buffers, allocated and freed by this class" },
        { "Desert/Desert/Source/Engine/Graphic/AtmosphereEnv.hpp",
          "AtmosphereEnv", "AerialPerspectiveVolume", Guard::FrameScoped,
          "an opaque per-frame handle; the SkyboxRenderer of THIS SceneRenderer owns the image and refills the struct every frame (documented at the member)" },
        { "Desert/Desert/Source/Engine/Graphic/AtmosphereEnv.hpp",
          "AtmosphereEnv", "DistantSkyLight", Guard::FrameScoped,
          "an opaque per-frame handle; the SkyboxRenderer of THIS SceneRenderer owns the image and refills the struct every frame (documented at the member)" },
        { "Desert/Desert/Source/Engine/Graphic/AtmosphereEnv.hpp",
          "AtmosphereEnv", "TransmittanceLut", Guard::FrameScoped,
          "an opaque per-frame handle; the SkyboxRenderer of THIS SceneRenderer owns the image and refills the struct every frame (documented at the member)" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudEnvironmentBake", "Modelling", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudEnvironmentBake", "AuthoredAtlas", Guard::FrameScoped,
          "the atlas is a PROCESS-WIDE service's image, and what makes this frame payload safe is that "
          "the VolumetricCloudRenderer filling it CO-OWNS that image for as long as it holds it (A8-2); "
          "before that, a second live SceneRenderer could free it mid-frame" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudEnvironmentBake", "SkyOcclusionVolume", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "Medium", Guard::CallScoped,
          "points at CloudEnvironmentBake::Medium, a value member of the bake the caller holds on its own "
          "stack across the submit-and-wait this pack is an argument to" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "Params", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "Authored", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "Modelling", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "AuthoredAtlas", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "SkyOcclusionVolume", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "DistantSkyLight", Guard::CallScoped,
          kWhyArgumentPack },
        // О1-G-2: THE AUTHORED MEDIUM'S OWN IMAGES AND PARAMETER BLOCK, crossing the same seam as the
        // three above and guarded by the same two things.
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudEnvironmentBake", "MediumImages", Guard::FrameScoped,
          "the medium's images are the TEXTURE service's, and this frame payload is safe for the reason "
          "the noise volumes beside it are: VolumetricCloudRenderer::ResolveMediumValues re-resolves them "
          "every frame from the material's handles and the bake is issued inside that same frame, so no "
          "entry here outlives the resolve that produced it" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "MediumParams", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudEnvironmentBake.hpp",
          "CloudBakeBinding", "MediumImages", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.hpp",
          "VolumetricCloudRenderer", "m_MediumImages", Guard::FrameScoped,
          "borrowed from the texture service and rebuilt from scratch by ResolveMediumValues once per "
          "frame, before any pass reads it. Nothing here survives a frame boundary, so an image the "
          "service released between frames cannot be bound: the vector is assigned, not patched" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "Compiler", "touchedParams", Guard::ObservedContainsUs,
          "addresses of nodes in the Document the Compiler holds a reference to and never mutates; the "
          "set is read within CompileToDShader, which the document outlives by construction" },
        // THE REGISTER OF PARAMETER-BLOCK SLOTS NO SHADER READS (O1). Both members are string literals in
        // an `inline constexpr std::array`, so the pointees live in the binary's read-only data and Q2 is
        // closed by the language — the same argument, and the same guard, as SkyPresetEntry::Name below.
        // Q1: nobody destroys them, because nobody allocated them.
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudPayload.hpp",
          "CloudUnreadSlot", "Member", Guard::StaticStorage,
          "a string literal in a constexpr table of the block's unread slots" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudPayload.hpp",
          "CloudUnreadSlot", "Reason", Guard::StaticStorage,
          "a string literal in a constexpr table of the block's unread slots" },
        { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudShadowPayload.hpp",
          "CloudShadowInput", "Map", Guard::FrameScoped,
          "SceneRenderer gathers ONE of these per frame and hands the same one to the deferred, the PBR "
          "and the terrain materials; the map is owned by the VolumetricCloudRenderer of that same "
          "SceneRenderer, and null is the ordinary state every consumer already tests" },
        { "Desert/Desert/Source/Engine/Graphic/Environment/SkyLook.hpp",
          "SampledCube", "Cube", Guard::CallScoped,
          "the answer of a resolver the editor's cubemap preview calls EVERY frame and consumes in the same "
          "call (EditorCubemapPreviewPass); the cube is resolved through the image service each time and "
          "never held, so an asset reload between frames cannot leave it dangling" },
        { "Desert/Desert/Source/Engine/Graphic/ExternalRenderPass.hpp",
          "ExternalPassContext", "Camera", Guard::FrameScoped,
          "per-frame data handed to an editor-registered pass when the render graph executes it; the "
          "target and its depth belong to the SceneRenderer running the graph" },
        { "Desert/Desert/Source/Engine/Graphic/ExternalRenderPass.hpp",
          "ExternalPassContext", "Target", Guard::FrameScoped,
          "per-frame data handed to an editor-registered pass when the render graph executes it; the "
          "target and its depth belong to the SceneRenderer running the graph" },
        { "Desert/Desert/Source/Engine/Graphic/ExternalRenderPass.hpp",
          "ExternalPassContext", "Depth", Guard::FrameScoped,
          "per-frame data handed to an editor-registered pass when the render graph executes it; the "
          "target and its depth belong to the SceneRenderer running the graph" },
        { "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
          "Rule", "Replacement", Guard::StaticStorage,
          "a string literal in a function-local `static const Rule kRules[]`; the table and every literal in it have static storage duration, so nothing can outlive them" },
        { "Desert/Desert/Source/Engine/Localization/LocalizationService.hpp",
          "Localization", "m_Language", Guard::StaticStorage,
          "a row of the `constexpr std::array` of languages in Engine/Localization/LocaleFormat.cpp, "
          "reached only through FindLocale, which returns either a pointer into that table or nullptr. "
          "The table has static storage duration, so Q2 is closed by the language; Q1 does not arise "
          "because nobody allocated it. Never null after Get() — the constructor resolves the source "
          "language and says so loudly if that row has been deleted" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "kNeverAttempted", Guard::StaticStorage,
          "a string literal held by a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "kReleased", Guard::StaticStorage,
          "a string literal held by a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "kMovedOut", Guard::StaticStorage,
          "a string literal held by a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "m_Allocation", Guard::OwningRaw,
          "the VMA allocation this guard is holding open; released in Release() and in the destructor, which is the type's whole purpose" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "m_Bytes", Guard::OwningRaw,
          "the mapped byte range, valid exactly while m_Allocation is mapped and nulled by the same Release()" },
        { "Desert/Desert/Source/Engine/Graphic/MappedMemory.hpp",
          "MappedMemory", "m_Reason", Guard::StaticStorage,
          "always one of the constexpr literals above, so the refusal text cannot outlive its own storage" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Clouds/MaterialCloudComposite.hpp",
          "MaterialCloudComposite", "m_ScatterTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Clouds/MaterialCloudComposite.hpp",
          "MaterialCloudComposite", "m_GuideTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Debug/MaterialOverdrawResolve.hpp",
          "MaterialOverdrawResolve", "m_Overdraw", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialCopy.hpp",
          "MaterialCopy", "m_Input", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "DeferredShadowInput", "CascadeVP", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "DeferredEnvironmentInput", "Irradiance", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "DeferredEnvironmentInput", "Prefiltered", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "DeferredEnvironmentInput", "BrdfLut", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_GBufferB", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_GBufferC", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_GBufferEmissive", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_SSAO", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_GI", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_EnvIrradiance", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_EnvSpecular", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp",
          "MaterialDeferredLighting", "m_BrdfLut", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp",
          "MaterialGIResolve", "m_Normal", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp",
          "MaterialGIResolve", "m_WorldPos", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp",
          "MaterialGIResolve", "m_RSMAlbedo", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp",
          "MaterialGIResolve", "m_RSMNormal", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp",
          "MaterialGIResolve", "m_RSMWorldPos", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSAO.hpp",
          "MaterialSSAO", "m_Pos", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSAO.hpp",
          "MaterialSSAO", "m_Normal", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSR", "m_Albedo", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSR", "m_Normal", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSR", "m_WorldPos", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSR", "m_SceneColor", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSRResolve", "m_Trace", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSRResolve", "m_History", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSRResolve", "m_WorldPos", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSRComposite", "m_SSR", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialSSR.hpp",
          "MaterialSSRComposite", "m_Normal", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Fog/MaterialHeightFog.hpp",
          "MaterialHeightFog", "m_FogTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/MaterialInstance.hpp",
          "MaterialSlotBinding", "Slots", Guard::OwnedByThisObject,
          "the parallel `Owned` vector in the SAME object holds a shared_ptr to every instance this view "
          "names, and the two are only ever built together; that is the whole purpose of the type, and it "
          "is why the render path may hold this view when it may not hold the component's" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Material.hpp",
          "Material", "m_RegisteredProperties", Guard::OwnedByThisObject,
          "every entry is the address of an MPROPERTY member of this same object, registered by that member's own registrar sub-object at construction" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/MaterialInstance.hpp",
          "MaterialInstance", "m_ParentMaterial", Guard::ReboundBeforeEveryUse,
          "the parent CAN die first -- MaterialService::Invalidate graveyards it -- and the guard is the "
          "invalidation stamp: MeshECSSystem compares MaterialService's version and clears the "
          "component's RuntimeMaterialInstances before any use, so no instance outlives its parent. "
          "Clear() does not bump that stamp, but its only caller is Renderer::Shutdown" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp",
          "MaterialPBRBase", "kShadowBlockName", Guard::StaticStorage,
          "a shader block name, a string literal in a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp",
          "MaterialPBRBase", "kEnvIrradianceName", Guard::StaticStorage,
          "a shader block name, a string literal in a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp",
          "MaterialPBRBase", "kEnvSpecularName", Guard::StaticStorage,
          "a shader block name, a string literal in a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp",
          "MaterialPBRBase", "kBrdfLutName", Guard::StaticStorage,
          "a shader block name, a string literal in a constexpr static" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "Camera", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "PointLights", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "SpotLights", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "DirectionLights", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "CascadeViewProj", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "IrradianceMap", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "PrefilteredMap", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp",
          "PBRSceneFrame", "BrdfLut", Guard::CallScoped,
          "PBRSceneFrame is always a function-local consumed by ApplyTo in the same scope; every field points at a member of the SceneRenderer or MeshRenderer that built it" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/JumpFloodMaterials.hpp",
          "MaterialJFAInit", "m_MaskTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/JumpFloodMaterials.hpp",
          "MaterialJFAStep", "m_InputTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialFXAA.hpp",
          "MaterialFXAA", "m_InputTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialJFAComposite.hpp",
          "MaterialJFAComposite", "m_SceneTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAAEdges", "m_Color", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAAWeights", "m_Edges", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAAWeights", "m_Area", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAAWeights", "m_Search", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAABlend", "m_Color", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAABlend", "m_Blend", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAABlend", "m_Edges", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp",
          "MaterialSMAABlend", "m_Area", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp",
          "MaterialTonemap", "m_BloomTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp",
          "MaterialTonemap", "m_AvgLuminance", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp",
          "MaterialTonemap", "m_LightShaftTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp",
          "MaterialTonemap", "m_LensFlareTexture", Guard::OwnedByThisObject,
          kWhyMaterialProperty },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Properties/Texture2DProperty.hpp",
          "Texture2DProperty", "m_Texture", Guard::ReboundBeforeEveryUse,
          "the image is not owned here; its DESCRIPTOR is copied out by UniformImage2D at SetImage2D time, and every consumer re-binds from the live framebuffer attachment each frame before the material is applied" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp",
          "TextureCubeProperty", "m_Texture", Guard::ReboundBeforeEveryUse,
          "same as Texture2DProperty::m_Texture: not owned, re-bound from the live owner before every apply" },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp",
          "UpdateMaterialSkyboxInfo", "Camera", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp",
          "MaterialSkybox", "m_CubeMapTexture", Guard::OwnedByThisObject,
          "as the material-property family, except that MaterialSkybox holds its executor by shared_ptr "
          "rather than unique_ptr -- which only strengthens the argument: the property cannot outlive "
          "this object because this object holds a reference to the executor that owns it" },
        { "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp",
          "Key", "Shader", Guard::IdentityOnly,
          "the pipeline cache keys on the ADDRESS of the shader/framebuffer/render pass; a recycled address cannot collide because a pipeline is only reachable through the spec that still holds shared_ptrs to all three" },
        { "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp",
          "Key", "Framebuffer", Guard::IdentityOnly,
          "the pipeline cache keys on the ADDRESS of the shader/framebuffer/render pass; a recycled address cannot collide because a pipeline is only reachable through the spec that still holds shared_ptrs to all three" },
        { "Desert/Desert/Source/Engine/Graphic/PipelineCache.hpp",
          "Key", "Renderpass", Guard::IdentityOnly,
          "the pipeline cache keys on the ADDRESS of the shader/framebuffer/render pass; a recycled address cannot collide because a pipeline is only reachable through the spec that still holds shared_ptrs to all three" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp",
          "DrawGenericMeshCommand", "Mesh", Guard::FrameScoped,
          "the mesh is held by the ECS component, the primitive factory's process-wide table or MeshService for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp",
          "DrawGenericMeshCommand", "DirectTexture", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawMeshCommand.hpp",
          "DrawStaticMeshCommand", "Mesh", Guard::FrameScoped,
          "the mesh is held by the ECS component, the primitive factory's process-wide table or MeshService for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawMeshCommand.hpp",
          "DrawInstancedStaticMeshCommand", "Mesh", Guard::FrameScoped,
          "the mesh is held by the ECS component or the primitive factory's process-wide table for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawSkinnedMeshCommand.hpp",
          "DrawSkinnedMeshCommand", "Mesh", Guard::FrameScoped,
          "held by the ECS component for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawSlotMaterialMeshCommand.hpp",
          "DrawSlotMaterialMeshCommand", "Mesh", Guard::FrameScoped,
          "held by the ECS component or the primitive factory for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawSlotMaterialMeshCommand.hpp",
          "DrawSlotMaterialMeshCommand", "SlotMaterial", Guard::FrameScoped,
          "a Material owned by MaterialService or by the SceneRenderer for longer than the frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawLandscapeTileCommand.hpp",
          "DrawLandscapeTileCommand", "Heightmap", Guard::FrameScoped,
          "the tile's R16 copy, held by LandscapeECSSystem's per-entity cache for longer than the frame; a re-upload drops the old image into the allocator's per-frame deletion queue, never frees it under a recorded frame" },
        { "Desert/Desert/Source/Engine/Graphic/Render/Commands/DrawTerrainCommand.hpp",
          "DrawTerrainCommand", "SplatMap", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Render/RenderCommandBuffer.hpp",
          "RenderCommandBuffer", "m_Commands", Guard::OwnedByThisObject,
          "the commands are placement-new'd into this object's own paged arena; pages are never freed mid-frame and Clear() runs the virtual destructor of every one before rewinding" },
        { "Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.hpp",
          "DrawCommand", "Texture", Guard::IdentityOnly,
          "a batch discriminator: consecutive primitives with the same value extend one draw. It is also the key of Render2D's executor caches, and THAT use is what makes address recycling matter -- see the Render2D rows" },
        { "Desert/Desert/Source/Engine/Graphic/Render2D/DrawList2D.hpp",
          "DrawCommand", "Material", Guard::FrameScoped,
          "the UI material this batch is filled with, as a UIMaterialCache::Entry address. Unlike the "
          "Texture row beside it this IS dereferenced -- Render2D::Flush reads the entry's pipeline and "
          "its runtime material -- so identity alone would not close Q2. The frame does: the entry is "
          "owned by the UIMaterialCache inside the Render2D that will Flush this very list, the walk that "
          "recorded the command stamped the entry with the current frame through Resolve(), and "
          "RetireUnused() (which runs AFTER the last draw of the same Flush) refuses to erase an entry "
          "any frame in flight could still be reading -- the identical rule, and the identical window, as "
          "Render2D's executor caches" },
        { "Desert/Desert/Source/Engine/UI/UIIntrospection.hpp",
          "UIBatchInfo", "Material", Guard::IdentityOnly,
          "a COPY of DrawCommand::Material taken for display, on exactly the terms of the Texture copy "
          "below it: the panel prints it as an address so an author can tell two material batches apart, "
          "nothing dereferences it, and UIFrameProbe::Reset drops the whole list at the start of every "
          "capture so the value never outlives the frame it came from" },
        // FOUND BY Ю13 AND NOT BY Ю13'S CODE. This member is a raw pointer and has been one all along;
        // the scan called it a `shared_ptr` because `Assets::Asset` is an alias for one and the alias was
        // matched against the whole declaration, MEMBER NAME INCLUDED — so a member literally named
        // `Asset` answered to it. The scan looks at the TYPE now (pointer_ownership_scan.hpp), which is
        // what made this row necessary: a genuine raw pointer had been sitting on the shared side of an
        // ownership census, never asked either question.
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp",
          "SlotRow", "Asset", Guard::CallScoped,
          "an argument pack. DrawElementRows builds a SlotRow from a `shared_ptr` LOCAL it is still "
          "holding, hands it to DrawSlotRow and lets both die at the end of that iteration; the co-owner "
          "is alive for the whole of the call, so the pointee cannot be freed inside it. Null is legal and "
          "means the element has no material of its own, which the row draws as 'Engine default material'" },

        // Ю13's three. All of them point INTO THE SERVICE'S OWN CACHE, which is a function-local static
        // reached through ResourceRegistry — i.e. an object with static storage duration that the whole
        // process outlives, released only by ResourceRegistry::ClearAll from Renderer::Shutdown, inside
        // main. They are re-derived at the top of EVERY canvas walk from the canvas's handle, so a theme
        // that was hot-reloaded, evicted or repointed between two frames is never seen through a stale
        // pointer: the walk asks the service again rather than remembering the answer.
        { "Desert/Desert/Source/Engine/UI/UIStyleResolver.hpp",
          "ElementStyle", "m_Theme", Guard::ReboundBeforeEveryUse,
          "the flattened theme this element resolves through, owned by Runtime::UIThemeService (a "
          "function-local static released by ResourceRegistry::ClearAll inside main). An ElementStyle is "
          "built per element per frame from CanvasStyle, which is itself rebuilt at the top of every "
          "RenderCanvas2D from the canvas's own handle, so the pointer is at most one walk old and cannot "
          "name an entry a hot reload replaced. Null is a legal value and means 'this canvas has no "
          "theme', which every query answers by returning the element's own authored value" },
        { "Desert/Desert/Source/Engine/UI/UIStyleResolver.hpp",
          "ElementStyle", "m_Table", Guard::ObservedContainsUs,
          "the style's per-slot binding table, a value MEMBER of the UIThemeRuntime m_Theme points at — "
          "so it lives exactly as long as that entry does and the two cannot disagree about lifetime. "
          "Null means the theme declares no style of that name, which the walk reports once and answers "
          "by falling back to the element's own authored values" },
        { "Desert/Desert/Source/Engine/UI/UIStyleResolver.hpp",
          "CanvasStyle", "m_Theme", Guard::ReboundBeforeEveryUse,
          "the same service-owned entry as ElementStyle::m_Theme above, held for the length of ONE canvas "
          "walk: RenderCanvas2D constructs a CanvasStyle from UIThemeService::Get at the top of the walk "
          "and the object dies with the walk, which is what makes a theme switch reach the very next "
          "frame without any invalidation to remember" },
        { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp",
          "UIViewContext", "Materials", Guard::ObservedContainsUs,
          "where this view's UI materials come from, as an IUIMaterialSource. The one implementation is "
          "the UIMaterialCache that is a MEMBER of the Render2D backend the view's host owns alongside "
          "the context and hands to every RenderCanvas2D call it makes; the backend cannot be destroyed "
          "while the host that owns both is still walking. Null is a legal value and means the walk has "
          "no GPU backend at all (a unit test), which the walk REPORTS rather than treating as 'no "
          "materials today'" },
        { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp",
          "UIViewContext", "RenderTextures", Guard::ObservedContainsUs,
          "where this view's render-texture elements get their worlds from, as an IUIRenderTextureSource. "
          "Q1: nobody destroys the pointee through this member -- the one implementation, "
          "Render2D::UIRenderTextureCache, is a BY-VALUE member of the same object that owns this context "
          "(EditorUIPass in the editor, RuntimeLayer in the game), so the two die together and neither can "
          "outlive the other. Q2: the host hands this pointer to every RenderCanvas2D call it makes and is "
          "the thing walking, so the backend cannot be gone while the walk is running. Null is a legal "
          "value and means the walk has no GPU backend at all (a unit test), which the element REPORTS "
          "with the magenta error fill rather than drawing nothing"},
        { "Desert/Desert/Source/Engine/UI/UIIntrospection.hpp",
          "UIBatchInfo", "Texture", Guard::IdentityOnly,
          "a COPY of DrawCommand::Texture taken for display: the probe prints it as an address so an "
          "author can tell two batches apart, and nothing dereferences it. Address recycling cannot "
          "mislead here the way it can in Render2D's executor caches, because the value never outlives "
          "the frame it was copied from -- UIFrameProbe::Reset drops the whole batch list at the start "
          "of every capture, and a capture only happens while the panel is open" },
        { "Desert/Desert/Source/Engine/UI/UIIntrospection.hpp",
          "UIElementCost", "Texture", Guard::IdentityOnly,
          "the same value again, for the one batch a measured element landed in. Same argument, and the "
          "same window: the measurement is taken inside one Capture and replaced by the next request" },
        { "Editor/Source/Editor/Core/UIProbeRegistry.hpp",
          "UIProbeRegistry", "m_Sinks", Guard::IdentityOnly,
          "keyed on a Scene's address, compared and never dereferenced -- the routing question is only "
          "'is this the same document'. Recycling is answered by EditorUIPass's destructor, which calls "
          "Forget( scene.get() ) as it unregisters the pass, so a slot cannot outlive its scene and a "
          "reopened document at the same address cannot inherit the closed one's numbers" },
        { "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.hpp",
          "Render2D", "m_WhiteImage", Guard::FrameScoped,
          "resolved from ImageService, whose only release path is Renderer::Shutdown -- terminal, and after the last Flush" },
        { "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.hpp",
          "Render2D", "m_Backdrop", Guard::ReboundBeforeEveryUse,
          "BackdropBlurRenderer::Resize does destroy the image this equals, but the UI pass calls "
          "SetBackdrop from the live pyramid before every Flush and Flush is the only reader. It is also "
          "the KEY of m_GlassExecutors, and A8-1 established that a stale key is a LEAK and not a dangle: "
          "ExecutorFor re-points the entry's Texture2DProperty with SetImage before every use, so the "
          "stale address is overwritten before anything reads it. The leak is closed by "
          "RetireUnusedExecutors, whose window is asserted rather than described" },
        { "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp",
          "ExternalPassSystem", "m_Renderer", Guard::ObservedContainsUs,
          "the SceneRenderer owns its render systems, so it cannot be destroyed while one of them is alive" },
        { "Desert/Desert/Source/Engine/Graphic/SkyPresets.hpp",
          "SkyPresetEntry", "Name", Guard::StaticStorage,
          "a string literal in a constexpr preset table" },
        { "Desert/Desert/Source/Engine/Graphic/SwapChain.hpp",
          "SwapChain", "m_Window", Guard::ObservedContainsUs,
          "the platform window owns the swapchain and declares m_GLFWWindow BEFORE m_SwapChain, so the "
          "swapchain is destroyed first; nothing in the tree calls glfwDestroyWindow at all. Note the "
          "member's const is decorative -- the one use site casts it away for InitSurface" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/RenderSystem.hpp",
          "RenderSystem", "m_SceneRenderer", Guard::ObservedContainsUs,
          "the SceneRenderer owns its render systems" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/RenderSystem.hpp",
          "RenderSystem", "m_RenderGraphBuilder", Guard::ObservedContainsUs,
          "the builder is a member of the SceneRenderer that owns this system; note the sibling m_TargetFramebuffer is a weak_ptr, because THAT one is not owned by the renderer" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "MeshRenderData", "Mesh", Guard::CallScoped,
          "a temporary aggregate consumed synchronously by MeshRenderer::SubmitMesh" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "StaticMeshRenderData", "Mesh", Guard::FrameScoped,
          "held by the ECS component, the primitive factory or MeshService for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "SkinnedMeshRenderData", "Mesh", Guard::FrameScoped,
          "held by the ECS component for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "SkinnedMeshRenderData", "Material", Guard::FrameScoped,
          "a Material owned by the MeshRenderer itself" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "SkinnedMeshRenderData", "Instance", Guard::OwnedByThisObject,
          "selected out of the MaterialSlots binding carried in the SAME struct, which co-owns it; before "
          "A8-3 this row read 'a heap MaterialInstance behind a shared_ptr in the component', and that was "
          "the register being too generous with itself -- the component's shared_ptr dies with the entity, "
          "and Lua can destroy the entity while this queue is waiting to be drawn" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "InstancedMeshRenderData", "Mesh", Guard::FrameScoped,
          "held by the ECS component or the primitive factory for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "GenericMeshRenderData", "Mesh", Guard::FrameScoped,
          "held by the ECS component or the primitive factory for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "GenericMeshRenderData", "SlotMaterial", Guard::FrameScoped,
          "a Material owned by MaterialService or the SceneRenderer for longer than the frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "GenericMeshRenderData", "DirectTexture", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "ObjDraw", "Obj", Guard::OwnedByThisObject,
          "points into m_StaticQueue, which is fully populated before the pass that builds these and is not pushed to during it" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "ObjDraw", "Inst", Guard::OwnedByThisObject,
          "selected out of the binding held by the StaticMeshRenderData this ObjDraw points at, and that "
          "queue entry co-owns it for as long as the frame's passes run (A8-3)" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "InstancedDraw", "Mesh", Guard::FrameScoped,
          "held by the ECS component or the primitive factory for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "InstancedBatchSet", "Mat", Guard::FrameScoped,
          "the material that will RECORD this set's draws, and it is owned for longer than the pass by "
          "one of exactly two holders: MaterialService (an asset's (Instanced x pass) variant) or the "
          "MeshRenderer itself (m_StaticInstancedMaterial / m_InstancedGBufferMaterial, both shared_ptr "
          "members). The service is the side that can retire one, and it cannot do so inside the window: "
          "Invalidate MOVES a material to the graveyard and CollectGarbage destroys it only at a frame "
          "start, after WaitDeviceIdle -- while these sets are filled and drained entirely inside one "
          "DrawStaticMeshes call" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "InstancedBatchSet", "Inst", Guard::OwnedByThisObject,
          "the MaterialInstance to Bind Mat with, and every one of them is held by a MeshRenderer member "
          "for longer than the pass: m_InstancedVariantInstances (a MaterialInstancePtr per asset variant, "
          "dropped WHOLE when MaterialService's invalidation stamp moves, which is what stops an instance "
          "outliving the material it points at) or m_StaticInstancedInstance / m_InstancedGBufferInstance. "
          "Unlike ObjDraw::Inst it is NOT selected out of an entity's slot binding, so no entity's death "
          "can reach it" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "GenericDraw", "Data", Guard::OwnedByThisObject,
          "points into m_GenericQueue, fully populated before the pass that builds these" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "GenericDraw", "Material", Guard::FrameScoped,
          "a DataDrivenMaterial owned by MaterialService or the SceneRenderer for longer than the frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "ShadowBatch", "Mesh", Guard::FrameScoped,
          "held by the ECS component or the primitive factory for the whole frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "MeshRenderer", "m_ScratchShadowSingles", Guard::OwnedByThisObject,
          "a reused scratch vector of pointers into m_StaticQueue, cleared and refilled inside one pass" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp",
          "MeshRenderer", "m_ScratchGenericRows", Guard::OwnedByThisObject,
          "a reused scratch vector; the DataDrivenMaterial* in it are owned by MaterialService for longer than the frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.hpp",
          "FrameEmitter", "Gpu", Guard::OwnedByThisObject,
          "points into ParticleRenderer::m_Emitters, an unordered_map -- NODE-BASED, so the insert that "
          "PrepareFrame can perform while it is already pushing these pointers cannot move the pointee; "
          "the only erase is OnSceneReplaced's clear(), which clears m_FrameEmitters first" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.hpp",
          "SkyboxRenderer", "m_ActiveCamera", Guard::ReboundBeforeEveryUse,
          "re-pointed by PrepareCamera from SceneRenderer::BeginScene at the top of every frame, before any pass that reads it; the camera itself is a persistent member of Scene that Scene::Clear does not touch" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.cpp",
          "Group", "Material", Guard::FrameScoped,
          "a per-frame grouping key; the DataDrivenMaterial is owned by MaterialService for longer than the frame" },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.hpp",
          "TerrainDrawData", "SplatMap", Guard::FrameScoped,
          kWhyFramePayload },
        { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.hpp",
          "TerrainDrawData", "Heightmap", Guard::FrameScoped,
          "copied from DrawLandscapeTileCommand::Heightmap for the same frame; same owner, LandscapeECSSystem's cache" },

        // ------------------------------------------------------------------------------------------
        // STAGE 2: Editor/Source. The teamlead called this the harder half and the reason is in the
        // answers: in the graphics trees half the rows are closed by the FORM, while here almost every
        // one is a fact about the editor's own construction order, and a panel's lifetime is tangled
        // with the six renderer slots (a preview must be DESTROYED to give its slot back).
        // ------------------------------------------------------------------------------------------
        { "Editor/Source/Editor/Core/CommandHistory.hpp",
          "ByteCommand", "m_Target", Guard::ReboundBeforeEveryUse,
          "the undo stack DROPS every pointer-based entry whenever its target may have died -- CommandHistory::DropVolatile, called by OnStructuralChange from all thirteen structural commands and again whenever the selected entity changes -- and Clear() runs on scene load and on entering Play. The mechanism is written down at the call site and names the same entt pool relocation A8-3 was about. NAMED RESIDUAL: CreateNewEntity is also called from about twenty-five places in EditorLayer.cpp that do not go through SceneCommands, and whether each of them ends up dropping (by selecting the new entity, or by Clear()) is not something any one place asserts" },
        { "Editor/Source/Editor/Core/CommandHistory.hpp",
          "StringCommand", "m_Target", Guard::ReboundBeforeEveryUse,
          "the same guard as ByteCommand above, for the same reason and by the same mechanism: it reports IsVolatile() and DropVolatile therefore drops it whenever its target may have died. It exists as a SECOND command because a std::string field cannot take the byte entry -- restoring a snapshot of the object's representation hands the live string a heap pointer the edit already freed -- so the value is stored and assigned back. The pointer is to the string MEMBER of a live component, exactly as ByteCommand's is to the field's bytes, and inherits ByteCommand's NAMED RESIDUAL about the CreateNewEntity call sites that bypass SceneCommands" },
        { "Editor/Source/Editor/Core/Commands/PoseEditTransaction.hpp",
          "ClipPoseCommand", "m_Animator", Guard::ReboundBeforeEveryUse,
          "the third entry to take ByteCommand's guard, by the same mechanism and for a reason the type could not avoid: the Animator is a unique_ptr MEMBER of AnimationComponent, so there is no weak_ptr to take and no handle to re-resolve from. It reports IsVolatile(), so CommandHistory::DropVolatile drops it whenever the selected entity changes or any structural command runs. It carries one guarantee the two byte commands do not: Apply() refuses when the authoring pose is no longer the length this entry was recorded against, and CommandHistory::Undo discards a command that reports failure -- so an entry surviving onto a DIFFERENT rig of the same-sized skeleton is the residual, not one surviving onto a destroyed one" },
        { "Editor/Source/Editor/Core/Commands/PoseEditTransaction.hpp",
          "ClipPoseCommand", "m_Clip", Guard::ReboundBeforeEveryUse,
          "the same guard and the same drop as m_Animator above. The clip lives inside an AnimationAsset that an eviction may unload, and the honest form of 'does this pointer outlive that' is the one Animation::ControlKeyTarget already gives at its declaration -- it does not, which is why the keyer takes its target PER CALL and never stores it. This command is the one place that must hold the clip across frames (an undo entry is by definition about a past edit), and volatility is what pays for it" },
        { "Editor/Source/Editor/Core/Commands/PoseEditTransaction.hpp",
          "PoseEditTransaction", "m_Animator", Guard::ReboundBeforeEveryUse,
          "written by Begin and cleared by End or Cancel, so it is only non-null while an interaction is OPEN -- a gizmo drag or a widget being held, which is a span of frames inside one document's OnUIRender. The transaction is a member of SequencerPanel, which is closed by the editor's own liveness sweep on the same frame its subject dies (IsSubjectAlive), and the same OnUIRender that resolves the entity is the only writer of this field. WHAT WOULD BREAK IT: a second surface calling Begin with an animator it did not resolve this frame" },
        { "Editor/Source/Editor/Core/Commands/PoseEditTransaction.hpp",
          "PoseEditTransaction", "m_Clip", Guard::ReboundBeforeEveryUse,
          "the same span and the same writer as m_Animator above: both are set together by Begin, which refuses unless both are present, and cleared together by Cancel. The clip is re-resolved from the animator every frame by the panel (GetCurrentClip) and handed in, so the stored one is never older than the interaction" },
        { "Editor/Source/Editor/Core/Commands/PoseEditTransaction.hpp",
          "ControlPoseCommand", "m_Hierarchy", Guard::ReboundBeforeEveryUse,
          "the same guard and the same drop as ClipPoseCommand's two pointers above: it reports IsVolatile(), so CommandHistory::DropVolatile takes it on every structural command and every selection change, and Clear() takes it on scene load and on entering Play. The hierarchy belongs to a ControlRigStage owned by the Animator, which is a unique_ptr member of AnimationComponent -- so there is nothing to re-resolve it from at this layer, exactly as for the animator itself. It carries one guarantee of its own: Apply() refuses when the control index is no longer inside the rig, and CommandHistory::Undo discards a command that reports failure, so an entry surviving onto a SMALLER rig is dropped rather than dereferenced -- an entry surviving onto a same-sized DIFFERENT rig is the residual" },
        { "Editor/Source/Editor/Core/Commands/UIClipEdit.hpp",
          "UIClipCommand", "m_Clip", Guard::ReboundBeforeEveryUse,
          "the fifth entry to take ByteCommand's guard, and the one whose hazard is the plainest: this is a raw pointer at the UIAnimData INSIDE a live component, and entt relocates a pool when it grows, so the address can die while the ENTITY it belongs to is perfectly alive. There is no handle to re-resolve from at this layer -- the command deliberately knows nothing about scenes or UUIDs, which is what lets its suite run with no registry at all -- so it reports IsVolatile() and CommandHistory::DropVolatile drops it on every structural command and every selection change. Apply() also refuses on a null clip and CommandHistory::Undo discards an entry that reports failure, so an entry that somehow survived is dropped rather than dereferenced" },
        { "Editor/Source/Editor/Core/Commands/UIClipEdit.hpp",
          "UIClipEditTransaction", "m_Clip", Guard::ReboundBeforeEveryUse,
          "written by Begin and cleared by End or Cancel, so it is only non-null while an interaction is OPEN -- an ImGui widget being held, which is a span of frames inside one document's OnUIRender. The transaction is a member of SequencerPanel, whose subject is swept by the editor's own liveness check (IsSubjectAlive) on the frame it dies, and DrawUITracks compares Subject() against the component it just resolved every frame and abandons the entry when the two disagree -- which is the check that covers the pool relocation above, because a moved component is a DIFFERENT address and not a dead one" },
        { "Editor/Source/Editor/Core/CommandLine.hpp",
          "CommandLineFlag", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Core/CommandLine.hpp",
          "CommandLineFlag", "ExampleValue", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Core/CommandLine.hpp",
          "CommandLineFlag", "AlsoNeeds", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Core/CommandPalette.hpp",
          "PaletteHit", "Command", Guard::CallScoped,
          "RankPaletteCommands takes the command list by const reference and its hits point into it; the caller owns the list for at least the call" },
        { "Editor/Source/Editor/Core/Control/ControlProtocol.hpp",
          "OpSpec", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Core/Control/ControlProtocol.hpp",
          "SubjectSpec", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Core/DocumentWell.hpp",
          "DocumentWell", "m_Documents", Guard::HostOutlivesUs,
          "EditorLayer holds OpenDocuments BY VALUE and declares it before m_DocumentWell, so it is destroyed after; the well is handed it by reference in an in-class initializer and never re-points. Note the well deliberately remembers documents by SubjectId, not by pointer, so the container emptying does not reach it" },
        { "Editor/Source/Editor/Core/EditorResources.hpp",
          "EditorResources", "s_RegularFont", Guard::HostOutlivesUs,
          "an ImFont is owned by ImGui's font atlas, which is built before the first panel draws and destroyed with the ImGui context after the last one; every getter also falls back to the regular font, so a null here degrades rather than crashes" },
        { "Editor/Source/Editor/Core/EditorResources.hpp",
          "EditorResources", "s_BoldFont", Guard::HostOutlivesUs,
          "an ImFont is owned by ImGui's font atlas, which is built before the first panel draws and destroyed with the ImGui context after the last one; every getter also falls back to the regular font, so a null here degrades rather than crashes" },
        { "Editor/Source/Editor/Core/EditorResources.hpp",
          "EditorResources", "s_ExtraBoldFont", Guard::HostOutlivesUs,
          "an ImFont is owned by ImGui's font atlas, which is built before the first panel draws and destroyed with the ImGui context after the last one; every getter also falls back to the regular font, so a null here degrades rather than crashes" },
        { "Editor/Source/Editor/Core/EditorResources.hpp",
          "EditorResources", "s_IconFont", Guard::HostOutlivesUs,
          "an ImFont is owned by ImGui's font atlas, which is built before the first panel draws and destroyed with the ImGui context after the last one; every getter also falls back to the regular font, so a null here degrades rather than crashes" },
        { "Editor/Source/Editor/Core/EditorResources.hpp",
          "EditorResources", "s_BigIconFont", Guard::HostOutlivesUs,
          "an ImFont is owned by ImGui's font atlas, which is built before the first panel draws and destroyed with the ImGui context after the last one; every getter also falls back to the regular font, so a null here degrades rather than crashes" },
        { "Editor/Source/Editor/Core/GizmoIconSet.hpp",
          "GizmoIconRow", "File", Guard::StaticStorage,
          "a string literal in the constexpr std::array kGizmoIcons, whose position IS its enumerator; the table and every literal in it have static storage duration" },
        { "Editor/Source/Editor/Core/GizmoIconSet.hpp",
          "GizmoIconRow", "Label", Guard::StaticStorage,
          "a string literal in the constexpr std::array kGizmoIcons, whose position IS its enumerator; the table and every literal in it have static storage duration" },
        { "Editor/Source/Editor/Core/OpenDocuments.hpp",
          "DocumentOpenResult", "Document", Guard::CallScoped,
          "the result is a function-local at its one call site and aliases an element of OpenDocuments::m_Documents (a vector of unique_ptr, so the pointee survives a reallocation). NAMED RESIDUAL: this field is never read anywhere in the tree -- only Outcome is -- which is the same write-only shape as the two dead accessors this task removed, and closing it is a decision about the API rather than about a lifetime" },
        { "Editor/Source/Editor/Core/SubjectEditorRegistry.hpp",
          "Registration", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Packaging/PackageTarget.hpp",
          "TargetPlatformInfo", "DisplayName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Packaging/PackageTarget.hpp",
          "TargetPlatformInfo", "RuntimeBinary", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Packaging/PackageTarget.hpp",
          "TargetPlatformInfo", "LauncherName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Packaging/PackageTarget.hpp",
          "TargetPlatformInfo", "BuildScript", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Packaging/PackageTarget.hpp",
          "TargetPlatformInfo", "NotHereReason", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Desert/Common/Source/Common/Content/ContentKinds.hpp",
          "ContentKindSpec", "Root", Guard::StaticStorage,
          "the live namespace-scope path constant in Common/Core/Constants.hpp -- a pointer rather than a copy ON PURPOSE, for exactly PackagedTree::Tree's reason below: the row follows a SetProjectRoot remap instead of freezing the dev-time value, which is what lets one census serve the cook, both hosts, the standalone tool and the CI gate" },
        { "Editor/Source/Editor/Packaging/PackagedContentTrees.hpp",
          "PackagedTree", "Tree", Guard::StaticStorage,
          "the live namespace-scope path constant in Common/Core/Constants.hpp -- a pointer rather than a copy ON PURPOSE, so the tree follows a SetProjectRoot remap instead of freezing the dev-time value" },
        { "Editor/Source/Editor/Packaging/PackagedContentTrees.hpp",
          "PackagedTree", "PakKey", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.hpp",
          "AnimGraphPanel", "kComponentTypeName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.hpp",
          "AnimGraphPanel", "m_Library", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_OpenDocuments -- this is a DOCUMENT, so m_Panels is the wrong container to cite (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts both orders rather than trusting either" },
        { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.hpp",
          "AnimGraphPanel", "m_Context", Guard::OwningRaw,
          "ed::CreateEditor in this panel's constructor, ed::DestroyEditor in its destructor. A C API handle with no C++ destructor, so a unique_ptr would need a custom deleter to say the same thing; it is named as owning instead" },
        { "Editor/Source/Editor/Panels/Animation/AnimLayersPanel.hpp",
          "AnimLayersPanel", "m_Library", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Clouds/CloudLayoutPanel.hpp",
          "CloudLayoutPanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "BodyField", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "BodyField", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "BodyField", "Member", Guard::MemberOffset,
          "a pointer to a member of the recipe/params struct this table drives -- an offset, not an address" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpField", "Suffix", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpField", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpField", "Member", Guard::MemberOffset,
          "a pointer to a member of the recipe/params struct this table drives -- an offset, not an address" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpVector", "Suffix", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpVector", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp",
          "LumpVector", "Member", Guard::MemberOffset,
          "a pointer to a member of the recipe/params struct this table drives -- an offset, not an address" },
        { "Editor/Source/Editor/Panels/Clouds/CloudModellingVolumePanel.hpp",
          "CloudModellingVolumePanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp",
          "RecipeField", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp",
          "RecipeField", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp",
          "RecipeField", "Member", Guard::MemberOffset,
          "a pointer to a member of the recipe/params struct this table drives -- an offset, not an address" },
        { "Editor/Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp",
          "CloudNoiseVolumePanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "Member", Guard::MemberOffset,
          "a pointer to a member of the recipe/params struct this table drives -- an offset, not an address" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "Format", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "SectionBefore", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.cpp",
          "ShapeField", "Tooltip", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudTypePanel.hpp",
          "CloudTypePanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Clouds/CloudsPanel.hpp",
          "CloudsPanel", "kPanelName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Clouds/CloudsPanel.hpp",
          "CloudsPanel", "m_Documents", Guard::HostOutlivesUs,
          "the same OpenDocuments member of EditorLayer, declared before m_Panels, so every panel is destroyed first; shutdown also empties the container before clearing the panels, which is a different thing from destroying it" },
        { "Editor/Source/Editor/Panels/Collections/CollectionsPanel.hpp",
          "CollectionsPanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Debug/ShaderLibraryPanel.hpp",
          "ShaderLibraryPanel", "m_SelectedShader", Guard::HostOutlivesUs,
          "ShaderService owns every Shader by shared_ptr for the session and this panel only ever selects one the service just handed it. RESIDUAL, named rather than hidden: a shader hot-reload that REPLACED the object would leave this pointing at the old one, and nothing here would notice" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "DirectoryInformation", "Parent", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "DirectoryInformation", "Children", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_CurrentDir", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_BaseProjectDir", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_NextDirectory", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_PreviousDirectory", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_BreadCrumbData", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_CurrentSelected", Guard::OwnedByThisObject,
          "every node lives in this panel's own m_Directories, an unordered_map of shared_ptr, so the nodes outlive any view into them for as long as the panel lives; the one place that erases nodes (the in-place rescan) remembers the selection BY PATH and re-resolves it afterwards, which is the only reason a raw view is safe across a refresh" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/FileExplorer/FileExplorerPanel.hpp",
          "FileExplorerPanel", "m_SubjectEditors", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/LuaConsole/LuaConsolePanel.hpp",
          "LuaConsolePanel", "m_Scene", Guard::HostOutlivesUs,
          "EditorLayer::m_PrimaryScene holds the scene for the layer's life and the panels are destroyed before it. NAMED RESIDUAL, and it is a correctness bug rather than a lifetime one: this panel does not override IPanel::SetScene, so the active-scene fanout is a silent no-op for it and the console goes on executing against the primary scene after the editor has switched views" },
        { "Editor/Source/Editor/Panels/LuaConsole/LuaConsolePanel.hpp",
          "LuaConsolePanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Modeling/ModelingPanel.cpp",
          "Cat", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Modeling/ModelingPanel.cpp",
          "Cat", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.hpp",
          "NodeGraphPanel", "m_Context", Guard::OwningRaw,
          "ed::CreateEditor in this panel's constructor, ed::DestroyEditor in its destructor. A C API handle with no C++ destructor, so a unique_ptr would need a custom deleter to say the same thing; it is named as owning instead" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "Compiler", "nodeByPin", Guard::CallScoped,
          "a function-local compiler over a `const Document&` it holds by reference; every Node* points into that document's node vector and the compile is synchronous" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "Compiler", "varOf", Guard::CallScoped,
          "same as nodeByPin: keyed by nodes of the document being compiled" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "Compiler", "state", Guard::CallScoped,
          "same as nodeByPin: keyed by nodes of the document being compiled" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "PinRef", "Owner", Guard::CallScoped,
          "resolved from the document during one compile and used inside it" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "NodeSpec", "Kind", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "NodeSpec", "Title", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "PinSpec", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "VolumeParam", "SchemaName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "VolumeParam", "Expression", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "VolumeParam", "Units", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "VolumeParamOutOfScope", "SchemaName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "VolumeParamOutOfScope", "Reason", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "ShadowRayScope", "OutputPin", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.hpp",
          "ShadowRayScope", "EntryPoint", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "MediumFunction", "Signature", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "MediumFunction", "Pin", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/NodeGraph/ShaderGraph.cpp",
          "MediumFunction", "Fallback", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Particles/ParticleEditorPanel.cpp",
          "Preset", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Particles/ParticleEditorPanel.hpp",
          "ParticleEditorPanel", "kComponentTypeName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp",
          "CmdPreset", "name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp",
          "CmdPreset", "cmd", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp",
          "PhotogrammetryPanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.cpp",
          "AddCategory", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.cpp",
          "AddCategory", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "AnimationLibrary", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "UIHelper", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "Preview", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "PreviewUI", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "PreviewUsed", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp",
          "ComponentEditContext", "FieldFilter", Guard::CallScoped,
          "a per-frame argument pack: ComponentEditor::MakeContext builds it by value and it is consumed inside the same Render call. The PreviewViewport it names IS destroyed to hand a renderer slot back, but only from OnPreUpdate -- never inside a UI frame -- and the context is rebuilt from the editor's freshly stamped pointer every UI frame" },
        { "Editor/Source/Editor/Panels/PropertyEditor/PropertyEditorBuilder.cpp",
          "CategoryBucket", "Fields", Guard::StaticStorage,
          "a FieldInfo belongs to the generated reflection table for its type (Engine/Generated/Reflection.gen.cpp), which has static storage duration; the buckets are rebuilt each draw" },
        { "Editor/Source/Editor/Panels/PropertyEditor/PropertyEditorBuilder.cpp",
          "CategoryBucket", "Advanced", Guard::StaticStorage,
          "a FieldInfo belongs to the generated reflection table for its type (Engine/Generated/Reflection.gen.cpp), which has static storage duration; the buckets are rebuilt each draw" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/EntityTypeCensus.hpp",
          "EntityTypeInfo", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp",
          "ArchetypeDef", "Category", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp",
          "ArchetypeDef", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp",
          "ArchetypeDef", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp",
          "AddCategory", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp",
          "AddCategory", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.cpp",
          "Row", "Name", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.cpp",
          "Row", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.cpp",
          "PinnedRow", "Object", Guard::CallScoped,
          "a function-local vector rebuilt from the registry every draw and consumed in the same function; Object is the component address the widget registry just resolved for this entity" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.cpp",
          "PinnedRow", "Type", Guard::StaticStorage,
          "the generated reflection TypeInfo for the component, static storage duration" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.cpp",
          "PinnedRow", "Field", Guard::StaticStorage,
          "a FieldInfo inside that same generated table" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.hpp",
          "ComponentEditor", "m_Preview", Guard::ReboundBeforeEveryUse,
          "ScenePropertiesPanel::ReleasePreview destroys the preview to return its renderer slot, and this member is left pointing at it -- but SetPreview re-stamps it one line before every Render, and ReleasePreview has no call site inside a UI frame. The guarantee is that ordering, not the type" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.hpp",
          "ComponentEditor", "m_ThumbnailUI", Guard::ReboundBeforeEveryUse,
          "destroyed alongside the preview by ReleasePreview and re-stamped by the same SetPreview call; same ordering guarantee as m_Preview" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.hpp",
          "ComponentEditor", "m_PreviewUsed", Guard::HostOutlivesUs,
          "points at ScenePropertiesPanel::m_PreviewActive, a plain member of the panel that owns this editor" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditor.hpp",
          "ComponentEditor", "m_AnimationLibrary", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/AnimationComponentWidget.hpp",
          "AnimationComponentWidget", "m_AnimationLibrary", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/Helper/MeshDetailsWidget.hpp",
          "Context", "RuntimeMesh", Guard::CallScoped,
          "built and consumed in one statement pair at each of its two call sites; MeshDetailsWidget::Show stores nothing, and there is no yield point between the assignments and the call" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/Helper/MeshDetailsWidget.hpp",
          "Context", "Entity", Guard::CallScoped,
          "built and consumed in one statement pair at each of its two call sites; MeshDetailsWidget::Show stores nothing, and there is no yield point between the assignments and the call" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/Helper/MeshDetailsWidget.hpp",
          "Context", "Scene", Guard::CallScoped,
          "built and consumed in one statement pair at each of its two call sites; MeshDetailsWidget::Show stores nothing, and there is no yield point between the assignments and the call" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp",
          "MaterialHost", "Slots", Guard::CallScoped,
          "HostOf returns a stack value naming the ECS component vector for the frame's selected entity, and MaterialComponentWidget::Render consumes it within the call. NAMED RESIDUAL: this is the address of an entt component member, which is exactly what A8-3 was about -- it is safe here only because nothing in this file mutates the registry structurally, which is an invariant of the code as written rather than a guarantee" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp",
          "MaterialHost", "Mesh", Guard::CallScoped,
          "the component's RuntimeMesh or a MeshService mesh, consumed inside the same Render call" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp",
          "MaterialComponentWidget", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/PrefabComponentWidget.hpp",
          "PrefabComponentWidget", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/StaticMeshComponent.hpp",
          "StaticMeshComponentWidget", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/StaticMeshComponent.hpp",
          "StaticMeshComponentWidget", "m_Ctx", Guard::CallScoped,
          "the widget is a temporary constructed and destroyed inside the draw lambda that was handed the context, so it cannot outlive it" },
        { "Editor/Source/Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp",
          "ScenePropertiesPanel", "m_AnimationLibrary", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SequencerPanel", "kSkeletalComponentTypeName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SequencerPanel", "kUIComponentTypeName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SectionTarget", "Animator", Guard::CallScoped,
          "ResolveSectionTarget builds this struct BY VALUE and hands it back to one call; every user is a "
          "local that lives for the length of one section edit inside one OnUIRender, and nothing is stored. "
          "Both pointers are resolved from the same entity in the same expression -- the animator out of the "
          "AnimationComponent's unique_ptr, the clip out of that animator -- so the two cannot come from "
          "different frames. WHAT WOULD BREAK IT: a caller keeping the struct as a MEMBER, which is exactly "
          "what PoseEditTransaction is for and why this one is not" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SectionTarget", "Clip", Guard::CallScoped,
          "the same span and the same resolution as Animator above. The clip is Animator::GetCurrentClip's, "
          "const-cast for editing (the cast the dope sheet documents at length): an eviction can unload the "
          "AnimationAsset behind it, so it is honest only while nothing yields -- which is what CallScoped "
          "means here, and why the undo entry that must outlive the frame is VOLATILE instead" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SequencerPanel", "m_Library", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_OpenDocuments -- this is a DOCUMENT, so m_Panels is the wrong container to cite (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts both orders rather than trusting either" },
        { "Editor/Source/Editor/Panels/Animation/AnimGraphPanel.hpp",
          "AnimGraphPanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns the manager as a shared_ptr member and hands the raw pointer to the factory "
          "that builds this document; documents live in EditorDocuments, which EditorLayer also owns and "
          "which is destroyed before the manager it was given. It is also allowed to be NULL -- a host "
          "with no manager simply cannot resolve the graph, and ResolveAsset says so through the window's "
          "status line instead of dereferencing" },
        { "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/AnimationComponentWidget.hpp",
          "AnimationComponentWidget", "m_AssetManager", Guard::HostOutlivesUs,
          "the widget is constructed and destroyed inside ONE Details frame, from a shared_ptr the "
          "registration locks for exactly that call (ComponentEditContext::AssetManager is a weak_ptr and "
          "the lock is what makes the raw pointer valid for the whole of it). Null when no manager exists, "
          "and every use checks -- the same shape the three widgets below hold it in" },
        { "Editor/Source/Editor/Panels/Sequencer/SequencerPanel.hpp",
          "SequencerPanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_OpenDocuments -- this is a DOCUMENT, so m_Panels is the wrong container to cite (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts both orders rather than trusting either" },
        { "Editor/Source/Editor/Panels/UI/UIEditorPanel.hpp",
          "UIEditorPanel", "kComponentTypeName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/UI/UIElementCatalog.hpp",
          "UIElementEntry", "ComponentType", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/UI/UIElementCatalog.hpp",
          "UIElementEntry", "EntityName", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/UI/UIElementCatalog.hpp",
          "UIElementEntry", "Icon", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/UI/UIElementCatalog.hpp",
          "UIElementEntry", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/Validation/SceneValidationPanel.hpp",
          "SceneValidationPanel", "m_Assets", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.hpp",
          "LightGizmoRenderer", "m_UIHelper", Guard::HostOutlivesUs,
          "ViewportPanel owns the helper as a unique_ptr and DECLARES IT BEFORE m_LightGizmoRenderer (ViewportPanel.hpp:314 before :315), so C++ destroys the renderer first and the helper outlives every use of it; the renderer is handed .get() once in ViewportPanel.cpp:200 and never re-points" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp",
          "ViewportCameraPresetRow", "Name", Guard::StaticStorage,
          "a string literal in the constexpr std::array kViewportCameraPresets, which is dense and in enum order (static_assert'ed); the table and every literal in it have static storage duration" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp",
          "Axis", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp",
          "Tip", "Label", Guard::StaticStorage,
          "a string literal, held by a constexpr/static table entry" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.hpp",
          "ViewportPanel", "s_Live", Guard::SelfDeregistering,
          "the constructor pushes `this` and the out-of-line destructor erases it, so neither close path can leave a dead entry; A8-2 deleted the copy and move operations, which were the one way to break that pairing" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.hpp",
          "ViewportPanel", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer owns it and declares it BEFORE m_Panels, so the panels are destroyed first (members die in reverse declaration order); EditorLayerDeclaresItsHostsBeforeItsPanels asserts that order rather than trusting it" },
        { "Editor/Source/EditorLayer.hpp",
          "EditorLayer", "m_Application", Guard::HostOutlivesUs,
          "the Application creates the layer and destroys it as part of its own teardown, so it cannot go first" },
        { "Editor/Source/EditorLayer.hpp",
          "EditorLayer", "m_FileExplorerPanel", Guard::ReboundBeforeEveryUse,
          "an alias into m_Panels, nulled by A8-2 at m_Panels.Clear(); before that it stayed non-null and stale for the rest of OnDetach, which made its one caller's null check protection against the wrong thing" },
        { "Desert/Desert/Source/Engine/Graphic/ExternalRenderPass.hpp",
          "ExternalPassContext", "Renderer", Guard::CallScoped,
          "the context is a local built inside the render graph's pass lambda and consumed by Execute within that call; the pointer is the ExternalPassSystem's own m_Renderer, i.e. the renderer that is recording -- which is what lets a pass ask what THIS view is showing instead of asking the scene, whose answer is view 0's" },
        { "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.hpp",
          "ViewportPanel", "m_ViewRenderer", Guard::HostOutlivesUs,
          "null for the primary viewport (which is always view 0); otherwise the renderer owned by the EditorLayer::SceneViewport that opened this panel. CloseSceneViewport removes the PANEL from m_Panels first and destroys the renderer last, so the panel cannot outlive it -- the same order CloseSceneView uses for a document, and for the same reason" },
        { "Editor/Source/EditorLayer.hpp",
          "SceneViewport", "Viewport", Guard::ReboundBeforeEveryUse,
          "the same alias-into-m_Panels as SceneDocument::Viewport above and it is nulled on the same terms: CloseSceneViewport removes the panel and nulls this before anything else, and OnDetach's m_Panels.Clear() is followed by the teardown loop" },
        { "Editor/Source/EditorLayer.hpp",
          "SceneDocument", "Viewport", Guard::ReboundBeforeEveryUse,
          "the panel is owned by EditorLayer::m_Panels and this is an alias into it, so the guarantee is that every path destroying the panel NULLS the alias first. CloseSceneView always did; OnDetach did not, and A8-2 added it -- the pointers were dangling but unread between m_Panels.Clear() and the loop that tears the documents down" },

        // ------------------------------------------------------------------------------------------
        // STAGE 3: Engine/{Core,ECS,Animation,Physics,Scripting,Text,UI}, Common, Platform, Runtime.
        // The remainder, and the tree where the FORM answers least often of all three -- almost every
        // row here is a back-pointer whose safety is a containment or a construction order.
        // ------------------------------------------------------------------------------------------
        { "Desert/Common/Source/Common/Core/AssetHandle.hpp",
          "PathRoot", "Root", Guard::StaticStorage,
          "a namespace-scope path constant in Common/Core/Constants.hpp -- a pointer rather than a copy so a SetProjectRoot remap is followed rather than frozen" },
        { "Desert/Common/Source/Common/Core/AutoRegistry.hpp",
          "AutoRegistry", "m_Instances", Guard::SelfDeregistering,
          "the constructor pushes `this`, the destructor erases it, and copy and move are already deleted -- the complete form of this pattern, and the one ViewportPanel::s_Live was missing until A8-2" },
        { "Desert/Common/Source/Common/Core/JobSystem.cpp",
          "Guard", "Share", Guard::CallScoped,
          "a scope guard over a RangeShare that lives in the enclosing ParallelFor frame" },
        { "Desert/Common/Source/Common/Core/JobSystem.hpp",
          "InlineJob", "m_Ops", Guard::CallScoped,
          "the operations table of the job being run, owned by the caller for the duration of the submit" },
        { "Desert/Common/Source/Common/Core/Memory/Buffer.hpp",
          "Buffer", "Data", Guard::OwningRaw,
          "`new std::byte[]` in Allocate, `delete[]` in Release -- an owning raw byte block, and the type is the engine's hand-rolled buffer rather than a container on purpose (it is handed to Vulkan and to memcpy by size)" },
        { "Desert/Common/Source/Common/Core/Memory/CommandBuffer.hpp",
          "CommandBuffer", "m_CommandBuffer", Guard::OwningRaw,
          "the command arena this object allocates and frees itself" },
        { "Desert/Common/Source/Common/Core/Memory/CommandBuffer.hpp",
          "CommandBuffer", "m_CommandBufferPtr", Guard::OwnedByThisObject,
          "a write cursor into m_CommandBuffer above -- the same allocation, in the same object" },
        { "Desert/Common/Source/Common/Core/Memory/not_null.hpp",
          "not_null", "m_Ptr", Guard::CallScoped,
          "not_null<T> is a PARAMETER type: it asserts non-nullness at construction and says nothing about lifetime, by design. Whoever passes one owes the callee a pointer that outlives the call, which is the same contract a reference carries" },
        { "Desert/Common/Source/Common/Core/Profiler.hpp",
          "Profiler", "m_GpuSink", Guard::HostOutlivesUs,
          "installed by SetGpuSink from the renderer that owns the sink, and the profiler is a process-wide singleton, so the ORDER is the wrong way round: the sink dies first. Every read goes through GetGpuSink and is null-checked, and the renderer clears it on teardown -- the guarantee is that clearing, not the type" },
        { "Desert/Common/Source/Common/Core/Profiler.hpp",
          "ScopedTimer", "m_Name", Guard::StaticStorage,
          "the scope name, a string literal at every use (the macro stringizes it)" },
        { "Desert/Common/Source/Common/Core/Profiler.hpp",
          "GpuScopedTimer", "m_Sink", Guard::CallScoped,
          "read from the profiler at construction and used only within the scope the timer measures" },
        { "Desert/Common/Source/Common/Core/Reflection.hpp",
          "FieldMeta", "displayName", Guard::StaticStorage,
          "reflection metadata: a string literal in the generated table" },
        { "Desert/Common/Source/Common/Core/Reflection.hpp",
          "FieldMeta", "category", Guard::StaticStorage,
          "reflection metadata: a string literal in the generated table" },
        { "Desert/Common/Source/Common/Core/Reflection.hpp",
          "FieldMeta", "shaderUniform", Guard::StaticStorage,
          "reflection metadata: a string literal in the generated table" },
        { "Desert/Common/Source/Common/Utilities/CameraCapture.hpp",
          "CameraCapture", "m_Impl", Guard::OwningRaw,
          "the platform capture object this class news and deletes; a void* because the implementation type is Objective-C on macOS and must not reach the header" },
        { "Desert/Desert/Source/Engine/Animation/AnimationLibrary.hpp",
          "AnimationLibrary", "m_AssetManager", Guard::HostOutlivesUs,
          "EditorLayer declares the AssetManager before the library and destroys it after" },
        { "Desert/Desert/Source/Engine/Assets/SyncLoadLedger.hpp",
          "LoadTimingScope", "m_Parent", Guard::HostOutlivesUs,
          "the enclosing load scope on THIS thread's stack. Scopes are created and destroyed in LIFO order "
          "-- each is a local in the body of an AssetBase::Load() that a load one level up is inside -- so "
          "the parent's lifetime strictly encloses the child's by the shape of the call stack. Two things "
          "make that hold rather than merely look true: the stack of open scopes is THREAD-LOCAL, so a load "
          "on the preloader's thread can never take the address of a scope on the hot-reload watcher's; and "
          "the type is neither copyable nor movable (all four operators deleted), because a copy would give "
          "two scopes one parent and a move would leave a live pointer to a husk. Dereferenced exactly once, "
          "in the destructor, to add this scope's duration to the parent's child-time" },
        { "Desert/Desert/Source/Engine/Assets/AssetPreloader.hpp",
          "AssetPreloader", "m_AnimationLibrary", Guard::HostOutlivesUs,
          "the library the scan publishes clips to. Taken as a REFERENCE by the constructor, so it can never "
          "be null, and both hosts that own one declare it BEFORE their preloader -- EditorLayer.hpp and "
          "RuntimeLayer.hpp both say so at the declaration, because members are destroyed in reverse "
          "declaration order and that order is the whole guarantee" },
        { "Desert/Desert/Source/Engine/Animation/Animator.hpp",
          "ClipPlayback", "Clip", Guard::HostOutlivesUs,
          "an AnimationClip inside an AnimationLibrary entry; the library outlives the animator that plays it" },
        { "Desert/Desert/Source/Engine/Animation/Animator.hpp",
          "Animator", "m_TrackBinding", Guard::IdentityOnly,
          "a memo keyed BY CLIP ADDRESS. This row used to claim the key was safe because the animator only "
          "ever memoises a clip it is currently playing and holding through the library -- and that argument "
          "was WRONG in the one direction it needed to be right: the danger is not a recycled clip address, it "
          "is the SAME clip address whose Tracks vector has been freed and reallocated under it by an asset "
          "unload + reload (D34, a segfault in lower_bound). The value type now carries the storage it was "
          "built from and is rebuilt when that storage moves; see TrackBinding::TracksData below" },
        { "Desert/Desert/Source/Engine/Animation/Animator.hpp",
          "Animator", "m_SourceTrackBinding", Guard::IdentityOnly,
          "A25: the same memo, built against the SOURCE rig when a retarget is attached. A SECOND MAP and "
          "not a second entry in the first, because the key is the clip and one clip is legally sampled on "
          "both rigs in the same frame; one map would hand back a binding built for the wrong bone order. "
          "Every word of m_TrackBinding's row above applies to it unchanged -- same key, same value type, "
          "same storage check -- and it is additionally cleared by AttachRetarget/DetachRetarget, because "
          "the rig it was built against is exactly what those two change" },
        { "Desert/Desert/Source/Engine/Animation/Animator.hpp",
          "RigSampling", "Binding", Guard::HostOutlivesUs,
          "A25: a REFERENCE to one of the two maps above, inside a struct that exists only as a function "
          "argument. It is a parameter pack in the shape of a type -- constructed by TargetSampling() or "
          "SourceSampling(), passed down the three sampling functions, destroyed when they return -- so the "
          "Animator that owns the map is, by construction, the caller several frames of stack below it. The "
          "scanner reads the map's KEY as the raw pointer; the reference itself cannot outlive the call" },
        // TrackBinding::ByBone HAS NO ROW ANY MORE, and its removal is the point. It was a
        // `std::vector<const BoneTrack*>` guarded by ReboundBeforeEveryUse — a discipline, and disciplines
        // are what the two questions at the top of this file exist to be suspicious of. It now holds track
        // INDICES, so there is no pointer to dangle and nothing to rebind: the worst a stale entry can do
        // is name the wrong track, which the revision check turns into a rebuild. A raw pointer deleted is
        // worth more than a raw pointer argued for.
        { "Desert/Desert/Source/Engine/Animation/Animator.hpp",
          "TrackBinding", "TracksData", Guard::IdentityOnly,
          "NOT DEREFERENCED, EVER. It is clip->Tracks.data() as it stood when the binding was built, kept only "
          "to be compared against the clip's current data() -- an identity, exactly like Mouse::m_Window. That "
          "comparison is what makes the ByBone pointers below safe, and it is the whole fix: the clip keeps one "
          "address for its asset's life while AnimationAsset::Unload frees its Tracks and Load allocates a new "
          "vector, so an address-only key handed back pointers into returned memory" },
        { "Desert/Desert/Source/Engine/Animation/Graph/AnimGraph.hpp",
          "Result", "Current", Guard::HostOutlivesUs,
          "a State inside the AnimGraph asset the result was produced from; the graph outlives one evaluation" },
        // A10 (T5.3): THE KEYING TARGET. Three pointers in one pack, and the pack exists PRECISELY so that
        // the keyer stores none of them -- `ControlKeyer`'s only members are a bool and a vector of
        // indices. A keyer holding a clip would owe this register an argument about outliving an asset
        // unload, and the honest form of that argument is "it does not"; the same reasoning is why
        // `ControlHierarchy::Evaluate` takes the pose per call rather than in its constructor.
        { "Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.hpp",
          "ControlKeyTarget", "Hierarchy", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.hpp",
          "ControlKeyTarget", "Skeleton", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.hpp",
          "ControlKeyTarget", "Clip", Guard::CallScoped,
          kWhyArgumentPack },
        // A28: the fourth member of the same pack, added when the keyer learned to key BONES. It is the
        // Animator's authoring buffer (`Animator::GetAuthoringPose`), and it is here rather than a pose
        // taken by value for the reason `EndInteraction` exists at all: the value keyed is READ AT THE
        // COMMIT and not remembered at the write, so the keyer has to be able to look at the buffer when
        // the drag ends. Same guard as its three neighbours -- the panel builds the pack inside the frame
        // that uses it and the Animator it points into is the one it just drew.
        { "Desert/Desert/Source/Engine/Animation/Rig/ControlKeyer.hpp",
          "ControlKeyTarget", "AuthoredPose", Guard::CallScoped,
          kWhyArgumentPack },
        { "Desert/Desert/Source/Engine/Core/Input.hpp",
          "Mouse", "m_Window", Guard::IdentityOnly,
          "the GLFW window handle, kept as `const void*` so Engine/Core does not include GLFW; it is passed back to the platform layer, never dereferenced here" },
        { "Desert/Desert/Source/Engine/Core/RendererSlotPool.hpp",
          "RendererSlotLease", "m_Pool", Guard::HostOutlivesUs,
          "the pool is a process-wide singleton and the lease is the RAII object that returns the slot to it; a lease outliving its pool would mean returning a slot to a dead pool, and the pool is created before any renderer and destroyed after the last one" },
        { "Desert/Desert/Source/Engine/Core/SceneViewList.hpp",
          "View", "Renderer", Guard::HostOutlivesUs,
          "REPLACES Scene::m_SceneRenderer, which was the same pointer when a scene could have only one view (U9). The renderer is owned by whoever opened the view -- EditorLayer::m_SceneRenderer for the main viewport, EditorLayer::SceneViewport::Renderer for a second angle, the panel itself for a preview or a thumbnail -- and the ORDER that makes this safe is named in one place per owner: CloseSceneViewport calls Scene::RemoveView BEFORE destroying the renderer, and a whole document is torn down scene-first. What would break it is destroying a renderer while its view is still in the list, which is also what would leak its slot" },
        { "Desert/Desert/Source/Engine/Core/Serialize/SceneSerializer.hpp",
          "SceneSerializer", "m_Scene", Guard::CallScoped,
          "the serializer is a function-local at every call site, built around a scene the caller holds" },
        { "Desert/Desert/Source/Engine/Core/Serialize/SceneSerializer.hpp",
          "SceneSerializer", "m_AssetManager", Guard::CallScoped,
          "same: a function-local serializer's argument" },
        { "Desert/Desert/Source/Engine/ECS/Entity.hpp",
          "Entity", "m_Registry", Guard::HostOutlivesUs,
          "ECS::Entity is a HANDLE, copied by value everywhere, and the registry it names is a member of the Scene. Every entity in the tree is obtained from a scene and used within that scene's life; a stored Entity outliving its scene would be the defect, and the register cannot see that from here -- it is why ScriptEntity below carries a validity check instead of trusting the handle" },
        { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp",
          "AnimationECSSystem", "m_AssetManager", Guard::HostOutlivesUs,
          "the same host that owns m_AnimationLibrary next door owns this: both layers hold the manager as a "
          "shared_ptr member declared BEFORE the scene that carries the systems, so the scene and its systems "
          "are destroyed first. It is also allowed to be NULL -- a host that builds no manager simply has no "
          "control rigs, and SyncControlRig says so once instead of dereferencing" },
        { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp",
          "AnimationECSSystem", "m_AnimationLibrary", Guard::HostOutlivesUs,
          "EditorLayer owns the library and hands it in; the systems are torn down with their scene before the layer releases it" },
        { "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp",
          "AttachmentSystem", "m_Scene", Guard::ObservedContainsUs,
          "Scene owns its systems in a vector<unique_ptr<ECS::System>>, so the scene cannot be destroyed while one of them is alive to read this" },
        { "Desert/Desert/Source/Engine/ECS/System/AttachmentSystem.hpp",
          "AttachmentSystem", "m_HookedRegistry", Guard::ObservedContainsUs,
          "the registry is a member of the Scene that owns this system; the destructor disconnects through it, and a changed registry is detected by comparison before any dereference" },
        { "Desert/Desert/Source/Engine/ECS/System/AudioECSSystem.hpp",
          "AudioECSSystem", "m_Scene", Guard::ObservedContainsUs,
          "Scene owns its systems in a vector<unique_ptr<ECS::System>>, so the scene cannot be destroyed while one of them is alive to read this" },
        { "Desert/Desert/Source/Engine/ECS/System/LocomotionSystem.hpp",
          "LocomotionSystem", "m_Scene", Guard::ObservedContainsUs,
          "Scene owns its systems in a vector<unique_ptr<ECS::System>>, so the scene cannot be destroyed while one of them is alive to read this" },
        { "Desert/Desert/Source/Engine/ECS/System/MeshECSSystem.hpp",
          "SlotDraw", "Mat", Guard::FrameScoped,
          "a per-frame slot record; the Material is owned by MaterialService for longer than the frame" },
        { "Desert/Desert/Source/Engine/ECS/System/PhysicsECSSystem.hpp",
          "PhysicsECSSystem", "m_Scene", Guard::ObservedContainsUs,
          "Scene owns its systems in a vector<unique_ptr<ECS::System>>, so the scene cannot be destroyed while one of them is alive to read this" },
        { "Desert/Desert/Source/Engine/ECS/System/PhysicsBodyLifetime.hpp",
          "PhysicsBodyLifetime", "m_World", Guard::HostOutlivesUs,
          "PhysicsECSSystem declares m_Lifetime AFTER m_World, so it is destroyed first, and resets it explicitly before m_World->Shutdown(); the listener can never release into a dead world" },
        { "Desert/Desert/Source/Engine/ECS/System/PhysicsBodyLifetime.hpp",
          "PhysicsBodyLifetime", "m_Registry", Guard::ObservedContainsUs,
          "the registry is a member of the Scene that owns the PhysicsECSSystem that owns this; Detach() in the destructor disconnects through it while it is alive" },
        { "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp",
          "ScriptSystem", "m_Scene", Guard::ObservedContainsUs,
          "Scene owns its systems in a vector<unique_ptr<ECS::System>>, so the scene cannot be destroyed while one of them is alive to read this" },
        { "Desert/Desert/Source/Engine/ECS/System/ScriptSystem.hpp",
          "ScriptSystem", "m_HookedRegistry", Guard::ObservedContainsUs,
          "the registry is a member of the Scene that owns this system, so it outlives the system by containment; the pointer exists to tell whether the hooks are still installed on the SAME registry, which is a comparison and not a dereference" },
        { "Desert/Desert/Source/Engine/Geometry/EditMeshOperations.cpp",
          "LayerRecord", "Layer", Guard::CallScoped,
          "an attribute layer of the result mesh CutAndStitch builds; the record is a local of that call, the mesh is a local declared before it, and no layer is added or removed while the call runs (nothing there calls SetUVLayerCount or Enable/Disable)" },
        { "Desert/Desert/Source/Engine/Geometry/ProceduralCharacterFactory.cpp",
          "JointDef", "Name", Guard::StaticStorage,
          "a joint name literal in the factory's static table" },
        { "Desert/Desert/Source/Engine/Geometry/SkinnedMesh.hpp",
          "SkinnedMesh", "m_Skeleton", Guard::HostOutlivesUs,
          "the component that owns the mesh also owns the Skeleton through RuntimeSkeleton and says so at the member; for a cooked asset the SkeletonAsset holds it. Both outlive the mesh" },
        { "Desert/Desert/Source/Engine/Physics/PhysicsWorld.cpp",
          "PhysicsWorld", "Bodies", Guard::HostOutlivesUs,
          "JPH::BodyInterface belongs to the Jolt PhysicsSystem this struct wraps, and is handed out by reference from it; it is valid exactly while that system is" },
        { "Desert/Desert/Source/Engine/Reflection/ReflectionTypes.hpp",
          "FieldInfo", "StructType", Guard::StaticStorage,
          "the TypeInfo of a nested struct, in the same generated reflection table as the field itself" },
        { "Desert/Desert/Source/Engine/Runtime/Services/Material/MaterialService.hpp",
          "MaterialService", "m_BuiltToAsset", Guard::IdentityOnly,
          "a reverse index keyed BY MATERIAL ADDRESS, never dereferenced; entries are removed when the material is graveyarded, which is what stops a recycled address from answering for the old one" },
        { "Desert/Desert/Source/Engine/Runtime/Services/Video/VideoService.hpp",
          "VideoPlayback", "Plm", Guard::OwningRaw,
          "a pl_mpeg decoder this playback creates with plm_create_with_filename and destroys with plm_destroy; a C handle with no C++ destructor" },
        { "Desert/Desert/Source/Engine/Scripting/Internal/ScriptRuntime.hpp",
          "ScriptEntity", "scene", Guard::HostOutlivesUs,
          "the scene that owns the ScriptSystem that owns the Lua state these proxies live in; a proxy cannot outlive the state, and the state cannot outlive the scene. The ENTITY it names can die, which is why every method begins with Valid()" },
        { "Desert/Desert/Source/Engine/Scripting/Internal/ScriptRuntime.hpp",
          "ScriptEntity", "envs", Guard::HostOutlivesUs,
          "the per-entity environment map, a member of the ScriptSystem in the same chain as `scene`" },
        { "Desert/Desert/Source/Engine/Scripting/Internal/ScriptRuntime.hpp",
          "ScriptEngine", "Scene", Guard::HostOutlivesUs,
          "same chain as ScriptEntity::scene" },
        { "Desert/Desert/Source/Engine/Scripting/Internal/ScriptRuntime.hpp",
          "ScriptEngine", "Assets", Guard::HostOutlivesUs,
          "the AssetManager, held for the editor session and outliving every scene in it" },
        { "Desert/Desert/Source/Engine/Scripting/ReflectionBindings.cpp",
          "ReflectedComponentEntry", "Name", Guard::StaticStorage,
          "the component's Lua-visible name, a literal in the bindings table" },
        { "Desert/Desert/Source/Engine/Scripting/ReflectionBindings.cpp",
          "ReflectedComponentEntry", "TypeName", Guard::StaticStorage,
          "the component's Lua-visible name, a literal in the bindings table" },
        { "Desert/Desert/Source/Engine/Scripting/ReflectionBindings.cpp",
          "ComponentProxy", "scene", Guard::HostOutlivesUs,
          "same chain as ScriptEntity::scene" },
        { "Desert/Desert/Source/Engine/Scripting/ReflectionBindings.cpp",
          "ComponentProxy", "entry", Guard::StaticStorage,
          "an entry of the static bindings table built once at registration" },
        { "Desert/Desert/Source/Engine/Scripting/ReflectionBindings.cpp",
          "ComponentProxy", "type", Guard::StaticStorage,
          "the generated reflection TypeInfo for that component" },
        { "Desert/Desert/Source/Engine/Text/Msdf.cpp",
          "EdgePoint", "NearEdge", Guard::CallScoped,
          "the edge that won this texel's channel, inside the Shape the caller owns for the whole "
          "generation; the EdgePoint itself dies at the end of the texel" },
        { "Desert/Desert/Source/Engine/Text/Msdf.cpp",
          "EdgePoint", "NearEdge", Guard::CallScoped,
          "the edge that won this texel's channel, inside the Shape the caller owns for the whole "
          "generation; the EdgePoint dies at the end of the texel" },
        { "Desert/Desert/Source/Engine/UI/UICanvasContext.hpp",
          "UIViewContext", "Registry", Guard::FrameScoped,
          "the scene registry, handed to the UI pass for one frame and never stored past it" },
        { "Desert/Desert/Source/Platform/MacOS/MacOSWindow.hpp",
          "MacOSWindow", "m_GLFWWindow", Guard::OwningRaw,
          "glfwCreateWindow here and NOTHING ANYWHERE DESTROYS IT: there is no glfwDestroyWindow in the tree and both platform files carry a live `// TODO: glfwTerminate on system shutdown`. So this is an owning raw pointer that never releases -- reclaimed by the process exit, which is why it has never been felt. Named here rather than fixed: the TODO predates this audit and closing it is a window-lifetime task, not a pointer-form one" },
        { "Desert/Desert/Source/Platform/Windows/WindowsWindow.hpp",
          "WindowsWindow", "m_GLFWWindow", Guard::OwningRaw,
          "glfwCreateWindow here and NOTHING ANYWHERE DESTROYS IT: there is no glfwDestroyWindow in the tree and both platform files carry a live `// TODO: glfwTerminate on system shutdown`. So this is an owning raw pointer that never releases -- reclaimed by the process exit, which is why it has never been felt. Named here rather than fixed: the TODO predates this audit and closing it is a window-lifetime task, not a pointer-form one" },
        { "Runtime/Source/RuntimeLayer.hpp",
          "RuntimeLayer", "m_Application", Guard::HostOutlivesUs,
          "the Application owns the layer stack that owns this layer; it cannot go first" },

        };
        return rows;
    }
    // clang-format on
} // namespace Desert::Tests::PointerCensus
