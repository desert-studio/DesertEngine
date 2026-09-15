#pragma once

namespace Desert::Core
{
    class Scene;

    /**
     * @brief Give @p scene the ECS systems that turn its components into render data, in the one order
     *        they are allowed to run in.
     *
     * WHY THIS EXISTS, AND IT IS NOT TIDYING. A Core::Scene has NO systems of its own. Its constructor
     * adds none and Init() adds none, so a freshly built world renders its clear colour and says nothing
     * — no error, no warning, a picture that is a single flat value. Five call sites hand-copied the list
     * (EditorLayer::BuildSceneSystems, RuntimeLayer::Init, PreviewViewport, AssetThumbnailRenderer,
     * PhotogrammetryPanel), which is five places that have to remember a thing whose omission is silent.
     *
     * MEASURED, Ю16: the sixth caller — the UI render-texture cache — omitted it, and the symptom was a
     * uniform image whose mean, min and max were the same three numbers for EVERY scene it was pointed
     * at, at every size, in Edit and in Play, deferred and forward, with and without AA. The render
     * graph was correct throughout: nine passes, all with a cached VkRenderPass, all bound to that
     * renderer's own framebuffer. They simply had nothing to draw, because nothing had collected it.
     * That cost most of a task to find, which is the argument for this function existing.
     *
     * WHAT IS IN IT AND WHAT IS NOT. Exactly the systems that READ components and EMIT render data, and
     * exactly those that need nothing the host owns. The rest — animation (needs the host's
     * AnimationLibrary), scripts (the AssetManager), physics, locomotion, audio — are BEHAVIOUR, they
     * need a service this function has no way to obtain, and a world in a 200-pixel widget has no
     * business running a second physics simulation. A host that wants them adds them after this call, in
     * the order it already used: animation, then AttachmentSystem, then scripts, then physics.
     *
     * THE ORDER IS PART OF THE ANSWER. TimeOfDayECSSystem writes the atmosphere sun's transform, which
     * the sky collector, the light collector and the shadow path all read in the SAME frame — so it runs
     * before them or the sun they see is one frame old.
     */
    void AddSceneRenderCollectors( Scene& scene );
} // namespace Desert::Core
