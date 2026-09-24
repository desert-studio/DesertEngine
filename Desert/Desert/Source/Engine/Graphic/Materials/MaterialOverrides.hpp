#pragma once

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // Per-draw overrides for a data-driven material: named shader params (vec4) + named texture-slot bindings
    // (texture asset handle). These two ALWAYS travel together through the whole submission chain (command ->
    // SceneRenderer -> renderer), so they're one type instead of two parallel vectors duplicated everywhere.
    struct MaterialOverrides
    {
        std::vector<std::pair<std::string, glm::vec4>> Params;   // shader param name -> value
        // slot name -> asset handle: every sampler, cloud-asset and shader slot of the material chain
        // (MaterialData::ForEachSlotHandle), each already folded from what the file states.
        std::vector<std::pair<std::string, uint64_t>> Textures;
    };

    /**
     * @brief How many of a vec4's four lanes a parameter actually MEANS.
     *
     * A material stores every value as a vec4 and the unused lanes are whatever the writer left there, so
     * "is this number readable" is a question about the lanes the reader is going to use and about no
     * others. Spelled as a type rather than as an int because the readable-check below is the only thing
     * that takes it and a bare `3` at that call site says nothing.
     */
    enum class MaterialValueLanes : int
    {
        One   = 1,
        Two   = 2,
        Three = 3,
        Four  = 4
    };

    /**
     * @brief THE ONE DECISION: a number a material carries is READABLE, or it is REFUSED BY NAME.
     *
     * WHAT THIS REPLACES, and why it is one function rather than fifteen guards. Every reader of a cloud
     * material clamped its value into range and trusted the clamp — `std::clamp( v, lo, hi )` is
     * `v < lo ? lo : hi < v ? hi : v`, every comparison against a NaN is false, and the NaN therefore
     * comes out of the clamp unchanged and goes to the GPU. Task O11 found this on one knob and guarded
     * that knob; O13 measured the same shape on four more in `ApplyCloudMaterialToBakeParams` alone, and
     * the medium resolver hands a raw vec4 straight to the parameter block with no clamp at all. Fifteen
     * sites of one shape is a missing abstraction, not fifteen oversights, so the guard moved to where a
     * file's number ENTERS the engine: every reader now asks this, and none of them re-decides.
     *
     * WHY REFUSAL MEANS "KEEP THE DEFAULT" AND NOT "SUBSTITUTE A ZERO". The caller leaves the field
     * holding what the schema said, which is the value an unauthored material already draws with — so an
     * unreadable number produces the sky the shader's own defaults produce, named in the log, instead of a
     * black frame or a hang. Contract §1.4: it is a refusal with the name and the value in it, never a
     * silent substitution.
     *
     * HOW A NON-FINITE NUMBER GETS INTO A MATERIAL AT ALL, measured rather than assumed. Not through the
     * file: `.demat` and `.desce` are read by rfl::json over yyjson with no read flags, and yyjson with
     * flag 0 REFUSES `NaN`, `Infinity` and an overflowing literal like `1e400` — the whole document fails
     * to parse, which is already a named refusal. The reachable source is the EDITOR: every float row is
     * an ImGui drag, ImGui's text entry parses with `sscanf( buf, "%f", … )`, and `%f` accepts `nan`,
     * `inf` and `1e40`. So the value arrives through `MaterialService::ResolveOverrides` from a material
     * a person edited, which is exactly the path this guard sits on.
     *
     * SAID ONCE PER NAME. The cloud resolve runs every frame (the artist dragging a slider must see the
     * sky move in the same frame), so an unguarded LOG_ERROR here is sixty lines a second. The register is
     * the parameter's name, which is what a reader needs and what does not grow.
     *
     * @param name   the parameter's name, as the material and the shader schema both spell it
     * @param value  the stored vec4
     * @param lanes  how many lanes the caller is about to read
     * @return true when every lane the caller will read is finite
     */
    /**
     * @brief The question without the diagnostic: is every lane the caller will read a number at all?
     *
     * Split out because two callers ask it for opposite reasons and only one of them is a render path.
     * The reader below answers "may I use this" and says so in the log once per name; the WRITER
     * (SurfaceMaterialAsset::Save) answers "may this be written down" and has to name the parameter in a
     * refusal the caller shows to a person, so a log line is the wrong channel for it. One predicate, so
     * the two cannot disagree about what a number is.
     */
    [[nodiscard]] inline bool MaterialValueIsFinite( const glm::vec4& value, MaterialValueLanes lanes ) noexcept
    {
        for ( int lane = 0; lane < static_cast<int>( lanes ); ++lane )
            if ( !std::isfinite( value[lane] ) )
                return false;
        return true;
    }

    [[nodiscard]] inline bool MaterialValueIsReadable( std::string_view name, const glm::vec4& value,
                                                       MaterialValueLanes lanes )
    {
        for ( int lane = 0; lane < static_cast<int>( lanes ); ++lane )
        {
            if ( std::isfinite( value[lane] ) )
                continue;

            static std::mutex            said;
            static std::set<std::string> alreadySaid;
            {
                const std::lock_guard<std::mutex> lock( said );
                if ( !alreadySaid.insert( std::string( name ) ).second )
                    return false;
            }

            LOG_ERROR( "[Material] parameter '{}' carries {} in component {}, which is not a number the "
                       "renderer can use — a clamp does not repair it, every comparison against it is "
                       "false. The parameter keeps its shader default for the rest of this session.",
                       name, value[lane], lane );
            return false;
        }
        return true;
    }

    /**
     * @brief The same decision for a parameter the reader turns into an int.
     *
     * A SEPARATE FUNCTION BECAUSE THE FAILURE IS A DIFFERENT ONE. `static_cast<int32_t>( x )` on a value
     * outside int32's range is undefined behaviour in C++ before any clamp downstream can see it, and a
     * NaN is only the loudest member of that set — `1e30` is finite, readable and just as undefined. So
     * the range is part of the question here and is not part of it for a float.
     */
    [[nodiscard]] inline bool MaterialValueIsReadableAsInt( std::string_view name, float value )
    {
        constexpr float kLowest  = -2147483648.0f; // exactly representable
        constexpr float kLargest = 2147483520.0f;  // the largest float below 2^31

        if ( std::isfinite( value ) && value >= kLowest && value <= kLargest )
            return true;

        static std::mutex            said;
        static std::set<std::string> alreadySaid;
        {
            const std::lock_guard<std::mutex> lock( said );
            if ( !alreadySaid.insert( std::string( name ) ).second )
                return false;
        }

        LOG_ERROR( "[Material] parameter '{}' carries {}, which no 32-bit integer can hold — the cast "
                   "alone would be undefined before any range check could see it. The parameter keeps "
                   "its shader default for the rest of this session.",
                   name, value );
        return false;
    }
} // namespace Desert::Graphic
