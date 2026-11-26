#pragma once

// THE SHIPPED QUALITY TABLE, read by a suite instead of copied into it. A test that wrote "Low = 16 samples" by
// hand went on checking 16 after Scalability.json moved on; these read the file the packager ships
// (Editor/Resources/Config/Scalability.json), as scalability_contract_test does.

#include "scratch_dir.hpp"

#include <Common/Settings/Scalability.hpp>
#include <Engine/Graphic/Clouds/CloudQuality.hpp>

#include <fstream>
#include <sstream>

namespace Desert::TestSupport
{
    // The shipped table, parsed under every loader rule; the error names why it does not load.
    [[nodiscard]] inline Common::ResultStr<Common::Scalability::ScalabilityTable> ShippedScalabilityTable()
    {
        const std::ifstream in( RepositoryRoot() / "Editor/Resources/Config/Scalability.json", std::ios::binary );
        std::ostringstream  text;
        text << in.rdbuf();
        return Common::Scalability::ScalabilityTable::Parse( text.str() );
    }

    // The cloud cost the shipped table's Effects @p level runs (Graphic::CloudQualityOf of its three rows).
    [[nodiscard]] inline Graphic::CloudQualityScale
    ShippedCloudQuality( const Common::Scalability::ScalabilityTable& table,
                         const Common::Scalability::Level             level )
    {
        using Common::Scalability::Parameter;
        Common::Scalability::ResolvedQuality quality;
        for ( const Parameter parameter :
              { Parameter::EffectsCloudLightMarchSamples, Parameter::EffectsCloudShadowMapScalePercent,
                Parameter::EffectsCloudStopTransmittanceMilli } )
            quality.Values[static_cast<std::size_t>( parameter )] = table.ValueAt( parameter, level );
        return Graphic::CloudQualityOf( quality );
    }
} // namespace Desert::TestSupport
