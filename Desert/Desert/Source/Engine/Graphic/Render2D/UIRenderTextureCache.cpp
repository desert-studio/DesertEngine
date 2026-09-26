#include "UIRenderTextureCache.hpp"

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Render2D/UIRenderTextureView.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Desert::Graphic::Render2D
{
    namespace
    {
        // Say it once per element per stretch of the same reason, not once per frame.
        //
        // A UI element is walked every frame it is on screen, so a refusal that logged unconditionally
        // would be sixty identical lines a second — which is the same as no line at all, because the
        // reader stops looking at the log. Keyed by the REASON and not by a bool, so an element that goes
        // from "no free slot" to "that file does not parse" says the second thing.
        bool ShouldSay( std::unordered_map<entt::entity, std::string>& said, entt::entity element,
                        const std::string& reason )
        {
            auto& previous = said[element];
            if ( previous == reason )
            {
                return false;
            }
            previous = reason;
            return true;
        }
    } // namespace

    UIRenderTextureCache::UIRenderTextureCache()  = default;
    UIRenderTextureCache::~UIRenderTextureCache() = default;

    void UIRenderTextureCache::Reset()
    {
        if ( m_Captures.empty() )
        {
            m_Demanded.clear();
            m_Refused.clear();
            return;
        }

        // THE GPU MAY STILL BE EXECUTING AGAINST THESE. Each capture owns framebuffers, pipelines and
        // descriptor pools that a submitted frame can be reading; the editor's own preview widgets wait
        // here for the same reason (PreviewViewport's destructor). Once for the whole batch rather than
        // once per capture — an idle is the most expensive thing in this file.
        Renderer::GetInstance().WaitDeviceIdle();
        const std::size_t released = m_Captures.size();
        m_Captures.clear();
        m_Demanded.clear();
        m_Refused.clear();
        LOG_INFO( "[UI] render-texture cache reset: {} capture(s) destroyed, {} view(s) returned", released,
                  released );
    }

    UIRenderTextureCache::Capture* UIRenderTextureCache::Build( entt::entity element, const Demand& demand,
                                                                Assets::AssetManager& assetManager )
    {
        if ( demand.ScenePath.empty() )
        {
            if ( ShouldSay( m_Refused, element, "empty" ) )
            {
                LOG_WARN( "[UI] render-texture element {} names no scene, so it draws the magenta error "
                          "fill. Put a .desce path in its Scene field.",
                          static_cast<uint32_t>( element ) );
            }
            return nullptr;
        }

        // ASKED BEFORE ANYTHING IS BUILT, through the shared byte rule (Engine/Core/ViewBudget.hpp) and not a
        // comparison written out here: the Details preview and the thumbnail service ask the same question
        // with different entitlements. What is asked for is RequestUIRenderTextureView's, and the renderer
        // below is built from the same request.
        const UIRenderTextureViewRequest request = RequestUIRenderTextureView( demand.Width, demand.Height );
        if ( const auto may = MayCreateView( request.Who, "UI render texture " + demand.ScenePath, request.Profile,
                                             request.Extent );
             !may )
        {
            if ( ShouldSay( m_Refused, element, "budget" ) )
            {
                LOG_WARN( "[UI] render-texture element {} draws the magenta error fill: {}. Hide or remove "
                          "another render-texture element, or close a view, and it builds on the next frame.",
                          static_cast<uint32_t>( element ), may.GetError() );
            }
            return nullptr;
        }

        // ASKED BEFORE THE RENDERER IS ALLOCATED, for the same reason RuntimeLayer asks before it tears
        // down the running scene: a file that will not load must cost nothing. A SceneRenderer built here
        // and thrown away would have taken — and returned — a slot, and on the way would have allocated
        // its cascades.
        const auto json = Common::Utils::FileSystem::ReadFileContent( demand.ScenePath );
        if ( !json )
        {
            if ( ShouldSay( m_Refused, element, "read:" + demand.ScenePath ) )
            {
                LOG_WARN( "[UI] render-texture element {} draws the magenta error fill: {}",
                          static_cast<uint32_t>( element ), json.GetError() );
            }
            return nullptr;
        }
        if ( const auto loadable = Core::ParseLoadableScene( demand.ScenePath, json.GetValue() ); !loadable )
        {
            if ( ShouldSay( m_Refused, element, "parse:" + demand.ScenePath ) )
            {
                LOG_WARN( "[UI] render-texture element {} draws the magenta error fill: {}",
                          static_cast<uint32_t>( element ), loadable.GetError() );
            }
            return nullptr;
        }

        Capture capture;
        // kPreviewShadowQuality, not the scene budget: this is 21 MB of cascade attachments against 335 MB,
        // and the number is per capture. An element a few hundred pixels across that paid the viewport's
        // shadow budget would make six of them cost 2 GB of shadow maps alone. Passed to the CONSTRUCTOR
        // because MeshRenderer allocates from it inside Init() — a value arriving later is read by nothing.
        capture.Renderer = std::make_unique<SceneRenderer>( request.Extent, request.Profile );
        capture.Scene    = std::make_shared<Core::Scene>( "UIRenderTexture", capture.Renderer.get() );

        // THE WORLD NEEDS SOMETHING TO COLLECT IT. A Core::Scene adds no ECS systems of its own, so
        // without this line the capture's render graph runs every pass it has, into the right
        // framebuffer, with empty queues — and the element shows one flat colour that is identical for
        // every scene it names. Measured before this line existed; see SceneRenderCollectors.hpp.
        Core::AddSceneRenderCollectors( *capture.Scene );

        const Core::SceneSerializer serializer( capture.Scene.get(), &assetManager );
        if ( const auto loaded = serializer.DeserializeFromJson( json.GetValue(), demand.ScenePath ); !loaded )
        {
            if ( ShouldSay( m_Refused, element, "load:" + demand.ScenePath ) )
            {
                LOG_WARN( "[UI] render-texture element {} draws the magenta error fill: {}",
                          static_cast<uint32_t>( element ), loaded.GetError() );
            }
            return nullptr;
        }
        if ( const auto inited = capture.Scene->Init(); !inited )
        {
            if ( ShouldSay( m_Refused, element, "init:" + demand.ScenePath ) )
            {
                LOG_WARN( "[UI] render-texture element {} draws the magenta error fill, its scene failed to "
                          "initialise: {}",
                          static_cast<uint32_t>( element ), inited.GetError() );
            }
            return nullptr;
        }

        // PLAY, and it is what makes the element show the shot the author framed. In Edit a scene is
        // viewed through the engine's own EditorCamera and its CameraComponents are read by nothing
        // (Scene::UpdateActiveCameraSource) — so an authored camera would have been inert, and the widget
        // would always look from the engine's default eye. Play is also what makes the world LIVE, which
        // is the whole difference between this and a thumbnail.
        // THE SKY HAS TO BE ASKED TO BAKE, and a scene file cannot ask. SkyAtmosphereComponent::RequestBake
        // is a runtime request and is not serialized, so a world loaded from disk arrives with it false —
        // while every other offscreen scene in this repository sets it by hand right here
        // (PreviewViewport, AssetThumbnailRenderer, PhotogrammetryPanel, and EditorLayer for the level it
        // creates). Without it the capture's DeferredLighting has no environment to shade the ambient
        // with and the element shows a flat grey rectangle that looks exactly the same whatever scene it
        // names — measured, before this line existed.
        for ( const auto entity : capture.Scene->GetRegistry().view<ECS::SkyAtmosphereComponent>() )
        {
            capture.Scene->GetRegistry().get<ECS::SkyAtmosphereComponent>( entity ).RequestBake = true;
        }

        capture.Scene->SetState( Core::Scene::SceneState::Play );
        capture.Scene->Resize( demand.Width, demand.Height );
        capture.ScenePath = demand.ScenePath;
        capture.Width     = demand.Width;
        capture.Height    = demand.Height;

        m_Refused.erase( element );
        LOG_INFO( "[UI] render-texture element {} took a renderer slot for '{}' at {}x{} ({} of {} now in "
                  "use)",
                  static_cast<uint32_t>( element ), demand.ScenePath, demand.Width, demand.Height,
                  SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );

        const auto inserted = m_Captures.emplace( element, std::move( capture ) );
        return &inserted.first->second;
    }

    void UIRenderTextureCache::Tick( Assets::AssetManager& assetManager, const Common::Timestep& ts )
    {
        // 1. GIVE BACK WHAT IS NO LONGER ON SCREEN — first, so the slots it frees are available to the
        //    elements built in step 2 of this same tick. An element that appears in the frame another one
        //    disappears from therefore does not have to wait a frame for the slot.
        //
        //    "No longer on screen" is "the last walk did not ask about it", and that is the only evidence
        //    there is: nothing tells this cache that an element scrolled out of a clipped list or lost its
        //    Visible bit. A capture whose scene or size changed is also dropped here, because a target is
        //    sized at creation and a path is what it was built from.
        //    A SIZE CHANGE IS NOT A REASON TO DESTROY ANYTHING, and getting that wrong would have been
        //    expensive: an element whose rect moves by one pixel — a tween, a window drag, a layout group
        //    reflowing — would have had its whole scene RELOADED FROM DISK, every frame it moved, with the
        //    renderer slot released and re-claimed in between. Scene::Resize is the supported operation and
        //    is what the editor's own viewport does on a drag; only the SCENE changing is a rebuild,
        //    because the path is what the capture was made from.
        std::vector<entt::entity> stale;
        for ( const auto& [element, capture] : m_Captures )
        {
            const auto demand = std::find_if( m_Demanded.begin(), m_Demanded.end(),
                                              [e = element]( const Demand& d ) { return d.Element == e; } );
            if ( demand == m_Demanded.end() || demand->ScenePath != capture.ScenePath )
            {
                stale.push_back( element );
            }
        }
        if ( !stale.empty() )
        {
            // One idle for the batch — see Reset().
            Renderer::GetInstance().WaitDeviceIdle();
            for ( const entt::entity element : stale )
            {
                m_Captures.erase( element );
                LOG_INFO( "[UI] render-texture element {} released its renderer slot ({} of {} now in use)",
                          static_cast<uint32_t>( element ), SceneRenderer::GetLiveRendererCount() - 1,
                          EngineContext::kMaxRendererSlots );
            }
        }

        // 2. BUILD WHAT IS NEWLY ON SCREEN, and 3. RENDER EVERY LIVE ONE. One pass, because a capture
        //    built this tick must also be rendered this tick — otherwise its first frame is magenta for a
        //    reason that no longer holds, which reads as a refusal that is not one.
        for ( const Demand& demand : m_Demanded )
        {
            const entt::entity element = demand.Element;
            auto               it      = m_Captures.find( element );
            Capture* capture = it == m_Captures.end() ? Build( element, demand, assetManager ) : &it->second;
            if ( capture == nullptr )
            {
                continue; // refused, and already named in the log with its numbers
            }

            // Follow the element's rect. On an ACTUAL change only: Resize recreates framebuffers behind a
            // device idle, which is the most expensive thing this loop can do, and calling it every frame
            // with the same numbers would pay for it on every frame of every element.
            if ( capture->Width != demand.Width || capture->Height != demand.Height )
            {
                capture->Scene->Resize( demand.Width, demand.Height );
                capture->Width  = demand.Width;
                capture->Height = demand.Height;
            }

            // Recorded into the HOST's current frame command buffer and submitted when that frame ends,
            // which is why this function may only be called before any pass is open. The scene opens and
            // closes its own renderer inside this call, so a refusal cannot leave that buffer holding
            // half a pass.
            //
            // Latched by MESSAGE, not by a flag, for the reason the refusal log above it is: an element
            // whose frame fails every frame must say so once, and must say so again when the reason
            // changes.
            if ( const auto frame = capture->Scene->OnUpdate( ts ); !frame )
            {
                if ( ShouldSay( m_Refused, element, "frame:" + frame.GetError() ) )
                {
                    LOG_ERROR( "[UI] render-texture element {} skipped a frame: {}",
                               static_cast<uint32_t>( element ), frame.GetError() );
                }
                continue;
            }
        }

        // The demand is spent. The walk that runs later this frame refills it, and the next Tick reads
        // what that walk asked for — so an element stops being demanded exactly one tick after the last
        // walk that drew it.
        m_Demanded.clear();
    }

    const void* UIRenderTextureCache::ResolveRenderTexture( entt::entity                                element,
                                                            const ::Desert::UI::UIRenderTextureRequest& request )
    {
        // RECORDING THE DEMAND IS THE FIRST THING AND IT HAPPENS EVEN WHEN THE ANSWER IS NULL. A refused
        // element that stopped being demanded would never be retried, so a shortage that ended — a scene
        // view closed, another element hidden — would leave the magenta up forever with nothing in the log
        // to say why it did not come back.
        //
        // Appended in the order the walk reaches the elements, which is the order the frame draws them.
        // A repeat within one frame cannot happen — the walk visits an element once — but a host that
        // walked twice without a Tick between would otherwise grow this without bound, so the row is
        // replaced rather than added when it is already there.
        const auto seen = std::find_if( m_Demanded.begin(), m_Demanded.end(),
                                        [element]( const Demand& d ) { return d.Element == element; } );
        Demand     row{ .Element   = element,
                        .ScenePath = std::string( request.ScenePath ),
                        .Width     = request.WidthPx,
                        .Height    = request.HeightPx };
        if ( seen == m_Demanded.end() )
        {
            m_Demanded.push_back( std::move( row ) );
        }
        else
        {
            *seen = std::move( row );
        }

        const auto it = m_Captures.find( element );
        if ( it == m_Captures.end() )
        {
            return nullptr;
        }

        // A capture whose SCENE no longer matches the request is still drawn for one more frame rather
        // than blinking magenta: the next Tick rebuilds it. A stale picture for one frame is the right
        // answer to an authoring change; a magenta flash while somebody edits the path is not. A size
        // mismatch is not even that — Tick resizes the capture in place and the picture stays live.
        const auto& image = it->second.Scene->GetFinalImage();
        return image.get();
    }
} // namespace Desert::Graphic::Render2D
