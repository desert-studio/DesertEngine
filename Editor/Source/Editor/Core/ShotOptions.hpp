#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace Desert::Editor
{
    // Command-line SCREENSHOT MODE: load a scene, point the camera, render a fixed number of frames, write
    // the viewport to a PNG and exit.
    //
    // It exists because the alternative is worse. The most expensive rendering defects this engine has
    // shipped — a tonemapper that was the identity, a radiance five thousand times too large, a shadow map
    // marching the far side of the planet — are ones that a person looking at the screen catches in a
    // minute, and the people looking at this screen are outnumbered by the parameters. This turns "does it
    // look right" from a question someone has to be present to answer into one command.
    //
    // The frame count matters and is not decoration: a temporally accumulating pass needs several frames to
    // converge, so a shot taken on frame one is a picture of the dither.
    //
    // MOTION, and why a still is not enough. Under a FIXED camera a temporal resolve's reprojection is
    // exact by construction — every pixel finds its own history at its own texel — so a still can never
    // exercise the parts of that stage that exist for a moving one: the disocclusion fallback, the
    // neighbourhood clamp. `--camera-to` / `--look-to` interpolate the camera across the warm-up frames and
    // `--shot-sequence` writes the frames out instead of only the last, which is what turns those into
    // things that can be measured rather than predicted.
    //
    // MOTION OF THE WORLD, which is a second thing entirely and was missing until `--play`. Moving the
    // camera moves the camera; it does not move the sky. `Scene::OnUpdate` hands the gameplay systems a ZERO
    // timestep outside Play (Engine/Core/Scene.cpp), so in every headless capture this programme has ever
    // taken the wind had advanced by exactly nothing — measured, not assumed: 90 frames against 900 differed
    // by 12.9 % of pixels at max 10/255, which is the noise floor of temporal convergence and not the 810 m
    // of drift 27 seconds at the default 3000 units/s would produce. Everything whose mechanism is "the
    // world moves under a temporal buffer" — cloud advection, particles, foliage, animation, physics —
    // therefore never reached a verification frame at all. `--play` runs the gameplay clock during the
    // capture; the frame is then taken after N seconds of SIMULATED time rather than after N frames of a
    // frozen one.

    // A camera pose along the shot path: where the eye is and where it points. Not normalized — the
    // consumer normalizes, exactly as it does for the `--look` it replaces.
    struct ShotCamera
    {
        glm::vec3 Position;
        glm::vec3 Forward;
    };

    // Interpolate two view DIRECTIONS along the shortest arc at constant angular velocity.
    //
    // Constant angular velocity is the point, not a flourish. The thing these paths are built to measure is
    // frame-to-frame difference under smooth motion; a component-wise lerp of two directions sweeps fastest
    // in the middle of the turn and slowest at its ends, which would put a hump in the middle of every
    // measurement that belongs to the interpolator rather than to the renderer.
    //
    // The result is a DIRECTION and its length is not part of the contract — exactly like the `--look` it
    // interpolates, which the documentation has always said need not be normalized. What IS part of the
    // contract: at the endpoints, and wherever the two directions coincide, the input comes back as the
    // same float it went in as.
    inline glm::vec3 ShotSlerpDirection( const glm::vec3& from, const glm::vec3& to, float t )
    {
        // The endpoints come back EXACTLY as they were given, ahead of any arithmetic that could move them
        // in the last bit. This is not tidiness: a still shot places its camera at t = 0, and `--look`
        // must reach `SnapToDirection` bit for bit as it did before this path existed. The rest of a shot
        // is reproducible too — measured, three runs to one md5 — but for a reason that had nothing to do
        // with care: gameplay time did not advance at all, so there was no wall clock in the picture to be
        // exact about. `--play` is the run where there IS one, and it answers with a fixed step for this
        // same reason.
        if ( t <= 0.0f )
            return from;
        if ( t >= 1.0f )
            return to;

        const float fromLength = glm::length( from );
        const float toLength   = glm::length( to );
        // A zero direction is not a direction. Nothing sensible can be interpolated from or to it, so the
        // one that IS a direction is returned unchanged rather than producing a NaN pose.
        if ( fromLength < 1e-6f )
            return to;
        if ( toLength < 1e-6f )
            return from;

        const glm::vec3 a = from / fromLength;
        const glm::vec3 b = to / toLength;

        const float cosAngle = glm::clamp( glm::dot( a, b ), -1.0f, 1.0f );

        // Nearly parallel: the arc is shorter than the precision of the axis that would describe it, and
        // the slerp denominator sin(angle) is on its way to zero. `from` UNCHANGED, not its normalized
        // form — `--camera-to` with no `--look-to` interpolates a position along a fixed aim, and that aim
        // has to survive the trip as the same float it arrived as, or a pure translation quietly becomes a
        // translation plus a hundredth of a degree of pan.
        if ( cosAngle > 0.999999f )
            return from;

        // Exactly opposed. There is no shortest arc — every half-turn is as short as every other — so one
        // is chosen rather than left to a cross product that is the zero vector here. Rodrigues about an
        // axis perpendicular to `a`, with the perpendicular taken from whichever world axis `a` leans on
        // least so the cross product is never degenerate.
        if ( cosAngle < -0.999999f )
        {
            const glm::vec3 leastAligned =
                 ( std::fabs( a.x ) < 0.9f ) ? glm::vec3( 1.0f, 0.0f, 0.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
            const glm::vec3 axis  = glm::normalize( glm::cross( a, leastAligned ) );
            const float     angle = 3.14159265358979323846f * t;
            return a * std::cos( angle ) + glm::cross( axis, a ) * std::sin( angle );
        }

        const float angle    = std::acos( cosAngle );
        const float sinAngle = std::sin( angle );
        return ( a * std::sin( ( 1.0f - t ) * angle ) + b * std::sin( t * angle ) ) / sinAngle;
    }

    struct ShotOptions
    {
        static ShotOptions& Get()
        {
            static ShotOptions s;
            return s;
        }

        std::string Scene;  // --scene <path.desce>, overrides the project's default scene
        std::string Output; // --shot <out.png>; empty = no single final frame
        int         Frames = 90;

        bool      HasCamera = false;
        glm::vec3 Position{ 0.0f, 200.0f, 0.0f }; // --camera x,y,z   (world units)
        glm::vec3 Forward{ 0.0f, 0.30f, -1.0f };  // --look   x,y,z   (need not be normalized)

        // The far end of the path. Held with their own flags rather than defaulted to Position/Forward at
        // parse time because the flags may arrive in any order: `--camera-to` before `--camera` must still
        // end at the position `--camera` names, not at the built-in default it was holding when parsed.
        bool      HasPositionTo = false;
        glm::vec3 PositionTo{ 0.0f, 0.0f, 0.0f }; // --camera-to x,y,z
        bool      HasForwardTo = false;
        glm::vec3 ForwardTo{ 0.0f, 0.0f, -1.0f }; // --look-to   x,y,z

        std::string Sequence;          // --shot-sequence <dir>; every Nth frame written there
        int         SequenceEvery = 1; // --shot-every N

        // --gpu-profile: switch GPU timestamps ON for this run and write the profiler's CPU and GPU
        // per-pass table to the log at the end of it.
        //
        // It has to do BOTH, because timestamps are off by default (they inflate the frame by ~8 % on
        // MoltenVK — Common/Core/Profiler.hpp GpuEnabled()). A headless shot draws no ImGui, so the
        // Profiler panel is unreachable there and this flag is the only way in. An ordinary capture is
        // therefore an uninstrumented capture, which is the number a budget decision should be taken on.
        bool GpuProfile = false;

        // --no-gpu-timing: dump the table but leave the timestamps off, so the log carries CPU columns
        // and no GPU ones. Its reason to exist is the A/B that prices the instrumentation: one binary,
        // one scene, the feature the only thing that differs.
        bool GpuTiming = true;

        // --gpu-profile-frame-only: time the whole command buffer and NOT the individual passes. Two
        // timestamps a frame instead of ~80. This is what prices the per-pass marks against the frame
        // bracket rather than against nothing, and it is also a legitimate lightweight mode: GPU frame
        // time for almost no cost.
        bool GpuFrameOnly = false;

        // --play: run the scene in Play during the capture, so gameplay time advances and the world moves
        // under the camera instead of standing still (see the header comment).
        //
        // OFF by default and that is load-bearing: every frame this programme has captured, and every
        // byte-for-byte comparison taken against one, was taken on a frozen world. A flag that changed
        // what a plain `--shot` does would invalidate the whole existing corpus at once.
        bool Play = false;

        // --- The pointer a headless capture has, and does not otherwise have (Ю12) -------------------
        //
        // WHY THIS EXISTS. `--shot` runs with no human and therefore with no cursor, so everything the UI
        // does in RESPONSE to a pointer — a tooltip appearing after a hover delay and flipping at the edge
        // of the view, a context menu opening at the click and stacking a submenu beside it — was
        // unobservable in a frame. Not "hard to photograph": there was no arrangement of flags that could
        // produce the picture at all, so the strongest evidence available for the whole overlay state
        // machine was a unit test, and this project's four most expensive defects all shipped tested.
        //
        // The pointer is in FRAMEBUFFER pixels, top-left origin — the same space the canvas walk uses, and
        // the same numbers the log prints for the trace resolution. It is honoured only during a capture,
        // for the same reason --play is: a flag that changed what the interactive editor does would be a
        // second way into a mode the toolbar does not know about.
        bool      HasUIPointer = false;
        glm::vec2 UIPointer{ 0.0f, 0.0f }; // --ui-pointer x,y

        // Which button is HELD for the capture. The press edge is derived inside the UI frame exactly as it
        // is for a real mouse, so the first frame of the capture is the press and the rest are the hold —
        // which is what opens a context menu and then leaves it open to be photographed.
        enum class UIButtonHeld
        {
            None,
            Left,
            Right
        };
        UIButtonHeld UIPress = UIButtonHeld::None; // --ui-press left|right

        // The gameplay step a `--play` capture advances by PER RENDERED FRAME. Fixed, and a constant rather
        // than a flag, for two separate reasons:
        //
        //   * DETERMINISM. The wall-clock step the editor normally runs on makes the simulated duration a
        //     function of how fast the machine happened to draw; two runs of one command would put the wind
        //     in different places and no capture could be compared with another. A verification tool that
        //     does not repeat is not a verification tool. With this constant, N rendered frames are N/60
        //     seconds of world on any machine, under any load.
        //   * The step must stay REALISTIC, and a knob invites the opposite. Fast-forwarding by taking
        //     bigger steps would reach 30 seconds cheaply and destroy the very thing being looked at: a
        //     temporal resolve reprojects between consecutive frames, so a tenth-of-a-second step rejects
        //     its history every frame and the capture converges to nothing. 60 Hz is what the world it is
        //     imitating runs at. Simulate longer by rendering more frames — `--shot-frames`, whose meaning
        //     ("how many frames are drawn") is left exactly as it was.
        static constexpr float PlayStepSeconds = 1.0f / 60.0f;
        // Headless capture mode at all — either flavour of output activates it. `--shot-sequence` alone is
        // a legitimate run: a motion study wants the frames and has no use for a designated last one.
        bool Active() const
        {
            return !Output.empty() || !Sequence.empty();
        }

        // Whether THIS run advances gameplay time. `--play` outside a capture is not a mode: the editor's
        // own Play button is that, and honouring the flag in a headful session would mean a second way into
        // Play that the toolbar does not know about.
        bool PlayActive() const
        {
            return Play && Active();
        }

        // The world time a capture of @p frames frames has advanced by, in seconds. The relation that makes
        // the flag usable — "the shot is taken after N seconds" is `--shot-frames N*60` — and the one a
        // report quotes, so it is computed in one place rather than multiplied out by each caller.
        float SimulatedSeconds( int frames ) const
        {
            if ( !PlayActive() || frames <= 0 )
                return 0.0f;
            return static_cast<float>( frames ) * PlayStepSeconds;
        }

        // The timestep the frame should be driven by, given the wall-clock one the application measured.
        //
        // A one-line function so that the rule the whole tool rests on is a pure one and can be asserted
        // instead of argued: under `--play` the answer does not depend on the clock AT ALL — that is what
        // makes two runs of one command the same frame — and otherwise it is the measured value returned on
        // the exact float, which is what makes every capture taken before this flag existed still valid.
        float FrameSeconds( float wallClockSeconds ) const
        {
            return PlayActive() ? PlayStepSeconds : wallClockSeconds;
        }

        // Whether the camera MOVES. False is the whole of the existing behaviour: the pose is placed once
        // and never touched again, which is what keeps an old `--camera`/`--look` shot identical.
        bool HasMotion() const
        {
            return HasPositionTo || HasForwardTo;
        }

        // Where along the path frame @p frame sits, in [0, 1]. The first rendered frame is at 0 and the
        // last at 1, so `--camera-to` names the pose the captured frame is actually taken from.
        float Parameter( int frame ) const
        {
            if ( Frames <= 1 )
                return 0.0f;
            return glm::clamp( static_cast<float>( frame ) / static_cast<float>( Frames - 1 ), 0.0f, 1.0f );
        }

        // The pose at normalized time @p t. With no motion flags this is the constant (Position, Forward)
        // for every t, on the exact float — the relation that makes an existing still shot the same shot
        // it was, and asserted by test rather than argued for.
        ShotCamera CameraAt( float t ) const
        {
            const glm::vec3 positionEnd = HasPositionTo ? PositionTo : Position;
            const glm::vec3 forwardEnd  = HasForwardTo ? ForwardTo : Forward;
            return ShotCamera{ glm::mix( Position, positionEnd, t ),
                               ShotSlerpDirection( Forward, forwardEnd, t ) };
        }
    };
} // namespace Desert::Editor
