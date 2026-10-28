#include <Engine/Graphic/API/NeuralComputeCaps.hpp>

#include <Common/Json/Template.hpp>

#include <format>
#include <optional>

namespace Desert::Engine
{
    namespace
    {
        const char* YesNo( bool value )
        {
            return value ? "yes" : "no";
        }

        bool IsFp16Shape( const CooperativeMatrixShape& shape )
        {
            const bool accumulates =
                 ( shape.C == NeuralScalar::Float32 && shape.Result == NeuralScalar::Float32 ) ||
                 ( shape.C == NeuralScalar::Float16 && shape.Result == NeuralScalar::Float16 );
            return shape.SubgroupScope && shape.A == NeuralScalar::Float16 && shape.B == NeuralScalar::Float16 &&
                   accumulates && !shape.SaturatingAccumulation;
        }

        // fp32 accumulation first (a network's sums lose precision in fp16), then the largest tile, then the
        // smallest K (fewer padding columns for a narrow layer).
        bool Better( const CooperativeMatrixShape& candidate, const CooperativeMatrixShape& best )
        {
            const bool candidateFp32 = candidate.C == NeuralScalar::Float32;
            const bool bestFp32      = best.C == NeuralScalar::Float32;
            if ( candidateFp32 != bestFp32 )
                return candidateFp32;
            const uint64_t candidateSize = uint64_t( candidate.M ) * candidate.N * candidate.K;
            const uint64_t bestSize      = uint64_t( best.M ) * best.N * best.K;
            if ( candidateSize != bestSize )
                return candidateSize > bestSize;
            return candidate.K < best.K;
        }

    } // namespace

    const char* NeuralKernelPathName( NeuralKernelPath path )
    {
        switch ( path )
        {
            case NeuralKernelPath::Fp32:
                return "fp32";
            case NeuralKernelPath::Fp16:
                return "fp16";
            case NeuralKernelPath::CoopMatFp16:
                return "coopmat_fp16";
        }
        return "?";
    }

    const char* NeuralScalarName( NeuralScalar scalar )
    {
        switch ( scalar )
        {
            case NeuralScalar::Float16:
                return "f16";
            case NeuralScalar::Float32:
                return "f32";
            case NeuralScalar::Float64:
                return "f64";
            case NeuralScalar::BFloat16:
                return "bf16";
            case NeuralScalar::Sint8:
                return "s8";
            case NeuralScalar::Uint8:
                return "u8";
            case NeuralScalar::Sint32:
                return "s32";
            case NeuralScalar::Uint32:
                return "u32";
            case NeuralScalar::Other:
                return "other";
        }
        return "?";
    }

    std::string FormatCooperativeMatrixShape( const CooperativeMatrixShape& shape )
    {
        return std::format( "{}x{}x{} {}*{}+{}->{}{}{}", shape.M, shape.N, shape.K, NeuralScalarName( shape.A ),
                            NeuralScalarName( shape.B ), NeuralScalarName( shape.C ),
                            NeuralScalarName( shape.Result ), shape.SaturatingAccumulation ? " sat" : "",
                            shape.SubgroupScope ? "" : " non-subgroup" );
    }

    void SelectNeuralKernelPath( NeuralComputeCaps& caps )
    {
        std::vector<std::string> fp16Missing;
        if ( !caps.ShaderFloat16 )
            fp16Missing.emplace_back( "shaderFloat16" );
        if ( !caps.StorageBuffer16BitAccess )
            fp16Missing.emplace_back( "storageBuffer16BitAccess" );

        std::vector<std::string>              coopMissing = fp16Missing;
        std::optional<CooperativeMatrixShape> tile;
        if ( !caps.CooperativeMatrix )
            coopMissing.emplace_back( "cooperativeMatrix" );
        else
        {
            if ( !caps.CooperativeMatrixInCompute )
                coopMissing.emplace_back( "cooperative matrix in the compute stage" );
            for ( const CooperativeMatrixShape& shape : caps.CooperativeMatrixShapes )
                if ( IsFp16Shape( shape ) && ( !tile || Better( shape, *tile ) ) )
                    tile = shape;
            if ( !tile )
                coopMissing.emplace_back( "a subgroup f16*f16 cooperative-matrix shape" );
        }

        caps.Tile = {};
        if ( coopMissing.empty() )
        {
            caps.Path   = NeuralKernelPath::CoopMatFp16;
            caps.Tile   = *tile;
            caps.Reason = {};
        }
        else if ( fp16Missing.empty() )
        {
            caps.Path   = NeuralKernelPath::Fp16;
            caps.Reason = std::format( "coopmat_fp16 lacks {}", Common::Text::Join( coopMissing, ", " ) );
        }
        else
        {
            caps.Path = NeuralKernelPath::Fp32;
            caps.Reason =
                 std::format( "coopmat_fp16 lacks {}; fp16 lacks {}", Common::Text::Join( coopMissing, ", " ),
                              Common::Text::Join( fp16Missing, ", " ) );
        }
    }

    std::string FormatNeuralComputeCaps( const NeuralComputeCaps& caps )
    {
        std::string line = std::format( "[nn_caps] path={}", NeuralKernelPathName( caps.Path ) );
        if ( caps.Path == NeuralKernelPath::CoopMatFp16 )
            line += std::format( " tile={}", FormatCooperativeMatrixShape( caps.Tile ) );
        if ( !caps.Reason.empty() )
            line += std::format( " ({})", caps.Reason );
        line += std::format(
             " | shaderFloat16={} shaderInt8={} storageBuffer16BitAccess={} storageBuffer8BitAccess={} "
             "subgroupSizeControl={} cooperativeMatrix={} cooperativeMatrixCompute={} | subgroupSize={} "
             "minSubgroupSize={} maxSubgroupSize={} computeRequiredSubgroupSize={} "
             "maxPushConstantsSize={} maxComputeSharedMemorySize={} | coopmat shapes {}:",
             YesNo( caps.ShaderFloat16 ), YesNo( caps.ShaderInt8 ), YesNo( caps.StorageBuffer16BitAccess ),
             YesNo( caps.StorageBuffer8BitAccess ), YesNo( caps.SubgroupSizeControl ),
             YesNo( caps.CooperativeMatrix ), YesNo( caps.CooperativeMatrixInCompute ), caps.SubgroupSize,
             caps.MinSubgroupSize, caps.MaxSubgroupSize, YesNo( caps.ComputeSubgroupSizeRequirable ),
             caps.MaxPushConstantsSize, caps.MaxComputeSharedMemorySize, caps.CooperativeMatrixShapes.size() );
        if ( caps.CooperativeMatrixShapes.empty() )
            line += " none";
        for ( const CooperativeMatrixShape& shape : caps.CooperativeMatrixShapes )
            line += std::format( " [{}]", FormatCooperativeMatrixShape( shape ) );
        return line;
    }
} // namespace Desert::Engine
