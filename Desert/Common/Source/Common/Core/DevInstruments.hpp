#pragma once

/**
 * @file DevInstruments.hpp
 * @brief THE ONE QUESTION EVERY DEVELOPMENT INSTRUMENT IN THIS ENGINE ASKS: am I in a build a player
 *        will run?
 *
 * ── WHY THIS HEADER EXISTS ────────────────────────────────────────────────────────────────────────
 *
 * The engine grew a lot of machinery whose only reader is a developer: a frame capture on the player
 * host (`--shot`), the Optick profiler and the GPU timestamp readout, a draw-call counter, a per-frame
 * memory watch, and a per-asset synchronous-load ledger. Until the `Shipping` configuration existed,
 * every one of them was compiled into the player binary in BOTH configurations — not switched off,
 * COMPILED IN — and four of the five arrived within one week. A surface that grows by one instrument
 * per task and is never cut is a surface nobody can state the size of.
 *
 * ── WHY A MACRO AND NOT A RUNTIME FLAG ───────────────────────────────────────────────────────────
 *
 * `--shot` is already "off unless you ask for it", and that is exactly the property that is not enough:
 * a flag belongs to the RUN, so anyone holding the binary can turn it on, and the cost of the code
 * (the memory watch samples every frame, the load ledger takes a lock on every asset read) is paid
 * whether or not anybody asked. Code that is not in the binary can be neither switched on nor paid for.
 *
 * ── HOW A SITE USES IT, AND WHY THE SPELLING IS FIXED ────────────────────────────────────────────
 *
 *     #if DESERT_DEV_INSTRUMENTS
 *         ... the instrument ...
 *     #endif
 *
 * Always `#if DESERT_DEV_INSTRUMENTS`, never `#ifdef DESERT_CONFIG_SHIPPING` spelled out at the site.
 * Two reasons, and the second is the load-bearing one:
 *
 *   1. It names the QUESTION ("is this a development build") rather than the answer's cause ("which
 *      premake configuration am I"), so a site reads as what it is.
 *   2. It is ONE token, so it can be counted. Desert/Tests/Runtime/ShippingBoundary is a source-text
 *      census that walks the instruments' own files and asserts each one is behind this macro — and a
 *      census can only pin a spelling it can grep for. `#ifdef DESERT_CONFIG_SHIPPING`, `#if
 *      !defined(DESERT_CONFIG_SHIPPING)` and `#ifndef DESERT_CONFIG_SHIPPING` are three spellings of
 *      one thing, and the census would have to know all three plus the next one somebody invents.
 *
 * `#if` and not `#ifdef` is deliberate too: the macro is ALWAYS defined, to 1 or to 0. A misspelling
 * under `#ifdef` is silently false — which for this macro means "shipping", i.e. the instrument
 * disappears from development builds and nobody notices for a week. Under `#if` a misspelling is 0 as
 * well, but `-Wundef` can see it, and the census asserts the definition exists exactly once.
 *
 * ── WHAT IS DELIBERATELY *NOT* BEHIND IT (the register, and its reasons) ─────────────────────────
 *
 * Five things in the engine look like development instruments and are NOT cut by this boundary. They
 * are listed here rather than left to be discovered, because "we also meant to remove those" is the
 * promise this whole mechanism exists to stop being made:
 *
 *   - `Engine/Graphic/ResourceLedger.hpp` — it is the OWNERSHIP mechanism (ResourceOwnership is RAII
 *     over device allocations, referenced by 27 engine files), not only a readout. Cutting it changes
 *     who frees what, which is not a boundary question.
 *   - `Engine/Graphic/DebugViewState.hpp` — `SceneRenderer` *chooses its render path* from it
 *     (SceneRenderer.cpp). Removing it would change the picture, and a boundary that changes the
 *     picture is not a boundary.
 *   - `Engine/Graphic/Materials/Debug/MaterialDebugLine.hpp` — and with it the PIPELINES. MEASURED, on
 *     the first packaged game ever started from this repository (Docs/World/Shots/B9): a Shipping build
 *     creates 38 Vulkan pipelines at startup and at least four of them exist only for a developer —
 *     `DebugLinePipeline`, `StaticMeshWireframe`, `OverdrawPipeline`, `OverdrawResolvePipeline`, plus
 *     the selection-outline family (`SilhouettePipeline`, `SilhouetteSkinnedPipeline`, `JFA_*`) which
 *     draws the EDITOR's selection highlight and has no caller in a player at all. That is startup time
 *     and device memory spent on nothing, and it is a task with a number attached rather than a line to
 *     add here — a pipeline set is chosen by the render graph, not by an `#if`.
 *   - `Engine/Core/EngineStats.hpp` — frame time and FPS, updated every frame in `Application::Run`,
 *     read only by the editor's HUD. Cheap, but it is an instrument, and cheap is not a reason.
 *   - ImGui, and `Layer::OnImGuiRender` — the player's LOADING SCREEN is drawn through that call
 *     (RuntimeLayer::OnImGuiRender -> Render2D). The name says debug UI; the path says product.
 *
 * Also not cut, and deliberately so: the `DebugName` strings handed to Vulkan objects (~20 sites). They
 * are what makes a device-lost report name the pipeline that died, which is a PLAYER's bug report, not a
 * developer's convenience. Validation layers are already Debug-only (VulkanContext.cpp).
 *
 * Each of these is a task, not an oversight, and the census names them so the next reader inherits the
 * reason instead of the surprise.
 */

// Set by BuildScripts/Configurations.lua at WORKSPACE scope, so no project can fail to receive it.
#if defined( DESERT_CONFIG_SHIPPING )
#define DESERT_DEV_INSTRUMENTS 0
#else
#define DESERT_DEV_INSTRUMENTS 1
#endif
