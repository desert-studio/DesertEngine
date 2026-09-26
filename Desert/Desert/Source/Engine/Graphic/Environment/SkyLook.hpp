#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * @brief THE AUTHORED LOOK OF AN HDR SKY — the three knobs that turn one `.hdr` file into one
     *        scene's sky, carried as ONE value from the component to every place the sky is sampled.
     *
     * WHY A STRUCT RATHER THAN THREE ARGUMENTS. Every one of these has to reach every reader of the
     * environment — the visible background, the diffuse irradiance, the GGX-prefiltered specular — and
     * those readers must agree. Three loose floats is three chances for a new knob to be threaded into
     * some of them; a struct means adding a field here is the whole change on the CPU side.
     *
     * APPLIED WHERE THE CUBES ARE SAMPLED, NOT WHERE THEY ARE BUILT. The cubes are baked ONCE per `.hdr`,
     * in the panorama's own orientation at unit gain, and the look turns and scales the LOOKUP
     * (Shaders/Common/SkyLook.glslh). It used to be baked in, and every slider value was a new bake:
     * the device idled for the whole convolution chain and a new cache file landed in
     * Cooked/EnvironmentCache for each value the owner dragged through — the "каждый раз когда я кручу
     * ротацию env оно сразу запекается" complaint. Rotation about the up axis commutes with both
     * convolutions (irradiance(R n) of the unturned sky IS the irradiance at n of the turned one, and the
     * same holds for the prefilter's lobe), and gain is linear, so sampling-time application gives the
     * same picture for the price of one rotation per fetch.
     *
     * THE DEFECT THE BAKED FORM EXISTED TO PREVENT STAYS UNREACHABLE, by a different mechanism: the old
     * `SkyboxComponent::Intensity` reached the backdrop and nothing else. Now every program that declares
     * an environment cube must declare `SkyLookUB` and read through the shared text — a census over the
     * shader sources (Desert/Tests/Engine/SkyPanorama) makes a consumer that forgets it a red test.
     */
    struct SkyLook
    {
        /// Turn the sky about the world's up axis, in DEGREES. Answers "the sun baked into this
        /// panorama is not where my directional light is" without re-authoring the file.
        float RotationDegrees = 0.0f;

        /// Linear multiplier on the sky's radiance. The one knob that already existed, now reaching the
        /// lighting as well as the backdrop.
        float Intensity = 1.0f;

        /// Linear colour the sky is graded through. White is the file as authored.
        glm::vec3 Tint = glm::vec3( 1.0f );

        /// (cos, sin) of the rotation, evaluated once on the CPU rather than per fetch in every shader.
        [[nodiscard]] glm::vec2 YawCosSin() const
        {
            const float yaw = glm::radians( RotationDegrees );
            return glm::vec2( std::cos( yaw ), std::sin( yaw ) );
        }

        /// What the shader multiplies radiance by. Tint and intensity are ONE number on the GPU and two
        /// knobs in the editor on purpose — the shader has no use for the distinction and a second
        /// uniform is a second thing to forget to bind.
        [[nodiscard]] glm::vec3 Gain() const
        {
            return Tint * Intensity;
        }

        [[nodiscard]] bool operator==( const SkyLook& ) const = default;
    };

    /// `SkyLookUB` as every environment-sampling shader declares it (two vec4s: std140 and std430 agree).
    /// Identity is (1, 0) and white — the value a procedural sky and "no sky" both bind, because a
    /// declared-but-unwritten uniform is whatever the last writer left there.
    struct SkyLookGPU
    {
        glm::vec4 YawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f }; // xy = (cos yaw, sin yaw)
        glm::vec4 Gain{ 1.0f, 1.0f, 1.0f, 1.0f };      // rgb = tint * intensity
    };
    static_assert( sizeof( SkyLookGPU ) == 32, "SkyLookUB is two vec4s in every shader that declares it" );

    [[nodiscard]] inline SkyLookGPU ToGPU( const SkyLook& look )
    {
        const glm::vec2 cs   = look.YawCosSin();
        const glm::vec3 gain = look.Gain();
        return SkyLookGPU{ glm::vec4( cs.x, cs.y, 0.0f, 0.0f ), glm::vec4( gain, 1.0f ) };
    }

    /// The block's name, shared by the binders and the shader census.
    inline constexpr const char* kSkyLookBlockName = "SkyLookUB";

    class ImageCube;

    /// A cube AND how to read it, for a consumer that is handed one rather than resolving an
    /// Environment itself (the editor's cubemap-on-a-ball preview). One value, so a resolver cannot
    /// return the cube and forget the look the scene draws it with.
    struct SampledCube
    {
        const ImageCube* Cube = nullptr;
        SkyLook          Look{};
        // The mip read with textureLod: the skybox viewer walks the prefiltered chain with it. 0 elsewhere.
        float Lod = 0.0f;
    };
} // namespace Desert::Graphic
