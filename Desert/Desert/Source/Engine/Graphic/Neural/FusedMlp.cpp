#include <Engine/Graphic/Neural/FusedMlp.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace Desert::Engine::Neural
{
    namespace
    {
        constexpr uint32_t W = kFusedMlpWidth;

        // splitmix64: a fixed, platform-independent stream (std::uniform_real_distribution is not).
        struct SeededStream
        {
            uint64_t State;

            uint64_t Next()
            {
                uint64_t z = ( State += 0x9E3779B97F4A7C15ull );
                z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
                z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
                return z ^ ( z >> 31 );
            }

            // Uniform in [-1, 1): the top 24 bits, so the value is exact in fp32.
            float Signed()
            {
                return static_cast<float>( Next() >> 40 ) / 8388608.0f - 1.0f;
            }
        };
    } // namespace

    std::string ValidateFusedMlp( const FusedMlpShape& shape, uint32_t batch )
    {
        if ( shape.InputDims == 0 || shape.InputDims > W )
            return std::format( "input dims {} outside 1..{}", shape.InputDims, W );
        if ( shape.OutputDims == 0 || shape.OutputDims > W )
            return std::format( "output dims {} outside 1..{}", shape.OutputDims, W );
        if ( shape.HiddenLayers == 0 )
            return "no hidden layer";
        if ( batch == 0 || batch % kFusedMlpBatchGranularity != 0 )
            return std::format( "batch {} is not a positive multiple of {}", batch, kFusedMlpBatchGranularity );
        return {};
    }

    FusedMlpProblem MakeSeededFusedMlpProblem( const FusedMlpShape& shape, uint32_t batch, uint64_t seed )
    {
        FusedMlpProblem problem;
        problem.Shape = shape;
        problem.Batch = batch;
        problem.Weights.assign( size_t( shape.MatrixCount() ) * W * W, 0.0f );
        problem.Input.assign( size_t( batch ) * W, 0.0f );
        problem.OutputGrad.assign( size_t( batch ) * W, 0.0f );

        SeededStream   stream{ seed };
        const uint32_t last = shape.MatrixCount() - 1;
        for ( uint32_t l = 0; l <= last; ++l )
        {
            const uint32_t rows  = l == last ? shape.OutputDims : W;
            const uint32_t cols  = l == 0 ? shape.InputDims : W;
            const float    bound = std::sqrt( 6.0f / float( cols ) );
            for ( uint32_t j = 0; j < rows; ++j )
                for ( uint32_t k = 0; k < cols; ++k )
                    problem.Weights[( size_t( l ) * W + j ) * W + k] = stream.Signed() * bound;
        }
        for ( uint32_t b = 0; b < batch; ++b )
            for ( uint32_t k = 0; k < shape.InputDims; ++k )
                problem.Input[size_t( b ) * W + k] = stream.Signed();
        for ( uint32_t b = 0; b < batch; ++b )
            for ( uint32_t j = 0; j < shape.OutputDims; ++j )
                problem.OutputGrad[size_t( b ) * W + j] = stream.Signed();
        return problem;
    }

    namespace
    {
        // The reference's forward and backward. The backward (its ReLU masks AND the h_l of every g_l (x) h_l)
        // runs on `backwardActivations` when given (the kernel's stored h_l), else on the reference's own forward.
        FusedMlpResult RunReference( const FusedMlpProblem&    problem,
                                     const std::vector<float>* backwardActivations )
        {
            const uint32_t L     = problem.Shape.MatrixCount();
            const uint32_t batch = problem.Batch;

            // h[l] = the input of matrix l, for every batch row.
            std::vector<std::vector<float>> h( L, std::vector<float>( size_t( batch ) * W, 0.0f ) );
            h[0] = problem.Input;

            FusedMlpResult result;
            result.Output.assign( size_t( batch ) * W, 0.0f );
            result.WeightGrad.assign( size_t( L ) * W * W, 0.0f );
            result.WeightGradMagnitude.assign( size_t( L ) * W * W, 0.0f );

            for ( uint32_t l = 0; l < L; ++l )
            {
                const float* weights = &problem.Weights[size_t( l ) * W * W];
                float*       out     = l + 1 < L ? h[l + 1].data() : result.Output.data();
                for ( uint32_t b = 0; b < batch; ++b )
                    for ( uint32_t j = 0; j < W; ++j )
                    {
                        double sum = 0.0;
                        for ( uint32_t k = 0; k < W; ++k )
                            sum += double( weights[j * W + k] ) * double( h[l][size_t( b ) * W + k] );
                        const float z            = float( sum );
                        out[size_t( b ) * W + j] = l + 1 < L ? std::max( z, 0.0f ) : z;
                    }
            }

            // h_l as the backward sees it: the given tensor, else the reference's own forward.
            const auto backwardH = [&]( const uint32_t l, const size_t row ) -> float
            { return backwardActivations ? ( *backwardActivations )[size_t( l ) * batch * W + row] : h[l][row]; };

            // g is the gradient, m its propagated magnitude: m_last = |dL/dy|, m_{l-1} = [h_l > 0] |W_l|^T m_l.
            // Every rounding a kernel makes in g_{l-1} (products, sums, the fp16 store) is a fraction of m_{l-1},
            // and the error carried in from g_l reaches g_{l-1} through |W_l|^T, so |g_gpu - g| <= c * m.
            std::vector<float> g = problem.OutputGrad;
            std::vector<float> m( g.size() );
            std::transform( g.begin(), g.end(), m.begin(), []( const float value ) { return std::abs( value ); } );
            std::vector<float> previous( size_t( batch ) * W, 0.0f );
            std::vector<float> previousMagnitude( size_t( batch ) * W, 0.0f );
            for ( uint32_t l = L; l-- > 0; )
            {
                const float* weights = &problem.Weights[size_t( l ) * W * W];
                float*       grad    = &result.WeightGrad[size_t( l ) * W * W];
                float*       mag     = &result.WeightGradMagnitude[size_t( l ) * W * W];
                for ( uint32_t j = 0; j < W; ++j )
                    for ( uint32_t k = 0; k < W; ++k )
                    {
                        double sum = 0.0, magnitude = 0.0;
                        for ( uint32_t b = 0; b < batch; ++b )
                        {
                            const double hk = double( backwardH( l, size_t( b ) * W + k ) );
                            sum += double( g[size_t( b ) * W + j] ) * hk;
                            magnitude += double( m[size_t( b ) * W + j] ) * std::abs( hk );
                        }
                        grad[j * W + k] = float( sum );
                        mag[j * W + k]  = float( magnitude );
                    }
                if ( l == 0 )
                    break;
                for ( uint32_t b = 0; b < batch; ++b )
                    for ( uint32_t k = 0; k < W; ++k )
                    {
                        double sum = 0.0, magnitude = 0.0;
                        for ( uint32_t j = 0; j < W; ++j )
                        {
                            sum += double( weights[j * W + k] ) * double( g[size_t( b ) * W + j] );
                            magnitude +=
                                 std::abs( double( weights[j * W + k] ) ) * double( m[size_t( b ) * W + j] );
                        }
                        const size_t row       = size_t( b ) * W + k;
                        const bool   live      = backwardH( l, row ) > 0.0f;
                        previous[row]          = live ? float( sum ) : 0.0f;
                        previousMagnitude[row] = live ? float( magnitude ) : 0.0f;
                    }
                std::swap( g, previous );
                std::swap( m, previousMagnitude );
            }

            result.Activations.reserve( size_t( L ) * batch * W );
            for ( const std::vector<float>& layer : h )
                result.Activations.insert( result.Activations.end(), layer.begin(), layer.end() );
            return result;
        }
    } // namespace

    FusedMlpResult RunFusedMlpReference( const FusedMlpProblem& problem )
    {
        return RunReference( problem, nullptr );
    }

    std::optional<FusedMlpResult> RunFusedMlpReferenceOnActivations( const FusedMlpProblem&    problem,
                                                                     const std::vector<float>& kernelActivations )
    {
        if ( kernelActivations.size() != size_t( problem.Shape.MatrixCount() ) * problem.Batch * W )
            return std::nullopt;
        return RunReference( problem, &kernelActivations );
    }

    namespace
    {
        // Round to the nearest fp16 value (ties to even), kept as fp32. fp16: 10 mantissa bits, normal
        // exponent >= -14, subnormal spacing 2^-24; values past the largest fp16 become infinite.
        float RoundToFp16( const float value )
        {
            if ( !std::isfinite( value ) || value == 0.0f )
                return value;
            int exponent = 0;
            std::frexp( value, &exponent ); // value = m * 2^exponent, 0.5 <= |m| < 1
            const float quantum = std::ldexp( 1.0f, std::max( exponent, -13 ) - 11 );
            const float rounded = std::nearbyint( value / quantum ) * quantum;
            return std::abs( rounded ) > 65504.0f ? std::copysign( INFINITY, rounded ) : rounded;
        }

        std::vector<float> RoundedToFp16( const std::vector<float>& values )
        {
            std::vector<float> rounded( values.size() );
            std::transform( values.begin(), values.end(), rounded.begin(), RoundToFp16 );
            return rounded;
        }
    } // namespace

    FusedMlpProblem QuantizedToFp16( const FusedMlpProblem& problem )
    {
        FusedMlpProblem quantized = problem;
        quantized.Weights         = RoundedToFp16( problem.Weights );
        quantized.Input           = RoundedToFp16( problem.Input );
        quantized.OutputGrad      = RoundedToFp16( problem.OutputGrad );
        return quantized;
    }

    uint64_t CountReluMaskFlips( const std::vector<float>& gpuActivations,
                                 const std::vector<float>& referenceActivations, const FusedMlpShape& shape,
                                 const uint32_t batch )
    {
        const size_t layerElements = size_t( batch ) * W;
        const size_t end           = std::min( { gpuActivations.size(), referenceActivations.size(),
                                                 size_t( shape.MatrixCount() ) * layerElements } );
        uint64_t     flips         = 0;
        for ( size_t i = layerElements; i < end; ++i ) // layer 0 is the input, not a ReLU
            flips += ( gpuActivations[i] > 0.0f ) != ( referenceActivations[i] > 0.0f ) ? 1u : 0u;
        return flips;
    }

    FusedMlpError CompareFusedMlp( const std::vector<float>& gpu, const std::vector<float>& reference )
    {
        FusedMlpError error;
        const size_t  count = std::min( gpu.size(), reference.size() );
        for ( size_t i = 0; i < count; ++i )
        {
            error.MaxAbs = std::max( error.MaxAbs, std::abs( double( gpu[i] ) - double( reference[i] ) ) );
            error.MaxRef = std::max( error.MaxRef, std::abs( double( reference[i] ) ) );
        }
        if ( gpu.size() != reference.size() )
            error.MaxAbs = INFINITY;
        error.Rel = error.MaxRef > 0.0 ? error.MaxAbs / error.MaxRef : error.MaxAbs;
        return error;
    }

    FusedMlpError CompareFusedMlpSum( const std::vector<float>& gpu, const std::vector<float>& reference,
                                      const std::vector<float>& magnitude )
    {
        FusedMlpError error = CompareFusedMlp( gpu, reference );
        if ( magnitude.size() != reference.size() || std::isinf( error.MaxAbs ) )
        {
            error.Rel = INFINITY;
            return error;
        }
        error.Rel = 0.0;
        for ( size_t i = 0; i < reference.size(); ++i )
        {
            const double difference = std::abs( double( gpu[i] ) - double( reference[i] ) );
            // A zero magnitude means every term was zero: the sum must be exactly zero too.
            const double rel = magnitude[i] > 0.0f ? difference / double( magnitude[i] )
                                                   : ( difference > 0.0 ? INFINITY : 0.0 );
            if ( i == 0 || rel > error.Rel )
            {
                error.Rel                 = rel;
                error.Worst.Matrix    = uint32_t( i / ( size_t( W ) * W ) );
                error.Worst.Row       = uint32_t( i / W % W );
                error.Worst.Column    = uint32_t( i % W );
                error.Worst.Gpu       = gpu[i];
                error.Worst.Reference = reference[i];
                error.Worst.Magnitude = magnitude[i];
                error.Worst.Valid     = true;
            }
        }
        return error;
    }

    double FusedMlpTolerance( NeuralKernelPath path )
    {
        return path == NeuralKernelPath::Fp32 ? 1e-5 : 1e-2;
    }

    double FusedMlpGradientTolerance( const NeuralKernelPath path, const FusedMlpShape& shape, const uint32_t batch )
    {
        // One ulp of the format each operation rounds to: Vulkan leaves a conversion's and an fp16 product's
        // rounding mode to the implementation (round-to-nearest OR toward zero), so a rounding is bounded by one
        // ulp, not half. Products and stores round to nn_t (fp16 on the fp16 paths); sums to the accumulator.
        const double product    = std::ldexp( 1.0, path == NeuralKernelPath::Fp32 ? -23 : -10 );
        const double accumulate = std::ldexp( 1.0, -23 ); // every accumulator is fp32 (IsFp16Shape)
        // Each backward layer adds, relative to the propagated magnitude m (RunReference): the product, a
        // W-long sum and the store of g. The deepest weight gradient (matrix 0) sits HiddenLayers backward layers
        // below dL/dy (stored exactly: the fp16 reference holds the fp16-rounded dL/dy); its own step adds the
        // product g h and a batch-long sum. First-order bound; the second-order terms are below 1e-6 here.
        const double perBackwardLayer = 2.0 * product + double( W ) * accumulate;
        return double( shape.HiddenLayers ) * perBackwardLayer + product + double( batch ) * accumulate;
    }
} // namespace Desert::Engine::Neural
