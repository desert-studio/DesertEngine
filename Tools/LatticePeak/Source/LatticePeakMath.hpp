#pragma once

/**
 * @file
 * @brief The whole of LatticePeak's arithmetic, and nothing that reads a file or prints a line.
 *
 * SEPARATE FROM THE TOOL BECAUSE IT IS TESTABLE. An instrument nobody can break on purpose is an
 * instrument nobody can trust: if the autocorrelation had an off-by-one in its lag, every "the lattice is
 * gone" this task reports would be that off-by-one and nothing else. Desert/Tests/Engine/LatticeSpectrum
 * compiles this header and feeds it fields whose period it CHOSE, which is the only way to find out that
 * the peak comes back where it was put.
 *
 * WHAT IS MEASURED, IN ONE PARAGRAPH. A field that repeats with period P correlates with a copy of itself
 * shifted by P, so its autocorrelation has a local maximum at lag P and at every multiple of P. A field
 * placed without a lattice has an autocorrelation that decays over the size of one body and does not come
 * back. The quantity reported is therefore not the correlation — a broad body correlates strongly with
 * itself at small lags either way — but the PROMINENCE of the local maximum: its height above the higher
 * of the two troughs bracketing it. That number is exactly zero for any monotonically decaying curve,
 * whatever the curve's height, which is the property that makes it an answer rather than an impression.
 */

#include <Common/Core/Math/Rounding.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace LatticePeak
{
    /// One bump of an autocorrelation curve: where it is, how high it is, and how far it stands above its
    /// own surroundings. `Prominence` is the number a claim is made with; `Value` alone would call the
    /// broad central lobe of any blobby field a lattice.
    struct Peak
    {
        int    Lag        = 0;
        double Value      = 0.0;
        double Prominence = 0.0;
        bool   Found      = false;
    };

    /// The topographic prominence of the local maximum at @p k: its height above the higher of the two
    /// troughs that bracket it, where a trough ends at the first sample that rises above the maximum
    /// itself. Independent of whatever overall decay the curve is sitting on, which is the point.
    inline double ProminenceAt( const std::vector<double>& r, int k, int firstLag )
    {
        const int n = static_cast<int>( r.size() );
        if ( k <= 0 || k + 1 >= n )
            return 0.0;

        double left = r[k];
        for ( int i = k - 1; i >= firstLag; --i )
        {
            if ( r[i] > r[k] )
                break;
            left = std::min( left, r[i] );
        }

        double right = r[k];
        for ( int i = k + 1; i < n; ++i )
        {
            if ( r[i] > r[k] )
                break;
            right = std::min( right, r[i] );
        }

        return r[k] - std::max( left, right );
    }

    inline bool IsLocalMaximum( const std::vector<double>& r, int k )
    {
        const int n = static_cast<int>( r.size() );
        return k > 0 && k + 1 < n && r[k] >= r[k - 1] && r[k] > r[k + 1];
    }

    /**
     * @brief Where the search for a lattice may begin: past the central lobe.
     *
     * Every coverage field correlates with itself over the width of one CLOUD, and that lobe is the size
     * of a body rather than a statement about how bodies are arranged. The first turn upwards is where the
     * body's own lobe ends and where a period could first show, so that is where the search starts. A
     * curve that never turns back up has no local maximum at all, which is the answer a field placed
     * without a lattice is supposed to give.
     */
    inline int FirstLagAfterCentralLobe( const std::vector<double>& r )
    {
        const int n = static_cast<int>( r.size() );
        int       k = 1;
        while ( k + 1 < n && r[k + 1] < r[k] )
            ++k;
        return k;
    }

    /// The most prominent bump of @p r, searched past the central lobe.
    inline Peak StrongestPeak( const std::vector<double>& r )
    {
        const int first = FirstLagAfterCentralLobe( r );
        const int n     = static_cast<int>( r.size() );

        Peak best;
        for ( int k = first + 1; k + 1 < n; ++k )
        {
            if ( !IsLocalMaximum( r, k ) )
                continue;

            const double prominence = ProminenceAt( r, k, first );
            if ( !best.Found || prominence > best.Prominence )
                best = Peak{ k, r[k], prominence, true };
        }
        return best;
    }

    /**
     * @brief The most prominent bump within @p tolerance of @p lag — how a KNOWN period is asked about.
     *
     * The field mode predicts the lattice's period from the generator's own cell extent and then asks this
     * question at that lag. It is a different question from "where is the strongest bump", and both are
     * reported: a strongest bump somewhere else with nothing at the prediction means either the prediction
     * or the instrument is wrong, and that is a finding rather than a nuisance.
     */
    inline Peak PeakNear( const std::vector<double>& r, int lag, int tolerance )
    {
        const int n     = static_cast<int>( r.size() );
        const int first = FirstLagAfterCentralLobe( r );

        Peak best;
        for ( int k = std::max( 1, lag - tolerance ); k <= std::min( n - 2, lag + tolerance ); ++k )
        {
            if ( !IsLocalMaximum( r, k ) )
                continue;

            const double prominence = ProminenceAt( r, k, 1 );
            if ( !best.Found || prominence > best.Prominence )
                best = Peak{ k, r[k], prominence, true };
        }

        (void)first;
        return best;
    }

    /**
     * @brief How big a bump this curve produces out of nothing — the estimator's own noise, by jackknife.
     *
     * A PROMINENCE HAS NO MEANING WITHOUT THIS NUMBER, and leaving it out is how an instrument lies. A
     * finite field is a finite sample: an autocorrelation estimated from a few hundred clusters wobbles
     * whatever is underneath it, and every wobble is a local maximum with a prominence of its own. The
     * first run of this tool reported a "peak" of prominence 0.085 at 9.5 km, and there is no lattice at
     * 9.5 km — that number was this noise and nothing else.
     *
     * IT IS ESTIMATED FROM THE REALISATIONS AND NOT FROM THE CURVE, and the difference is the whole reason
     * this function exists in its second form. Taking the median prominence of the curve's own local
     * maxima — which is what it did first — measures the LATTICE when there is one, because on a strongly
     * periodic curve nearly every local maximum sits on a multiple of the period. Splitting the
     * realisations into two halves and taking half their difference cancels everything the two halves
     * agree about — the signal — and leaves exactly what they disagree about, which is the noise.
     *
     * @param curves one autocorrelation per independent realisation; fewer than two means no estimate is
     *        possible and zero is returned rather than a number that would look like one.
     * @param firstLag where the search for peaks begins, so the central lobe (which is signal, and huge)
     *        is not counted as disagreement.
     */
    inline double JackknifeNoise( const std::vector<std::vector<double>>& curves, int firstLag )
    {
        if ( curves.size() < 2 )
            return 0.0;

        const size_t length = curves.front().size();

        std::vector<double> evenSum( length, 0.0 );
        std::vector<double> oddSum( length, 0.0 );
        double              evenCount = 0.0;
        double              oddCount  = 0.0;

        for ( size_t c = 0; c < curves.size(); ++c )
        {
            std::vector<double>& into  = ( c % 2 == 0 ) ? evenSum : oddSum;
            double&              count = ( c % 2 == 0 ) ? evenCount : oddCount;
            for ( size_t k = 0; k < length && k < curves[c].size(); ++k )
                into[k] += curves[c][k];
            count += 1.0;
        }

        if ( evenCount < 1.0 || oddCount < 1.0 )
            return 0.0;

        double sum   = 0.0;
        double taken = 0.0;
        for ( size_t k = static_cast<size_t>( std::max( firstLag, 0 ) ); k < length; ++k )
        {
            const double half = 0.5 * ( evenSum[k] / evenCount - oddSum[k] / oddCount );
            sum += half * half;
            taken += 1.0;
        }

        return ( taken > 0.0 ) ? std::sqrt( sum / taken ) : 0.0;
    }

    /**
     * @brief THE NUMBER THIS TASK IS MEASURED BY: the strongest bump standing on a multiple of a KNOWN
     *        period, and which multiple it stands on.
     *
     * A lattice does not announce itself at its period alone — it repeats, so it puts a bump at P, at 2P,
     * at 3P and so on. And the FIRST of those is the one most likely to be missing, because a cluster
     * nearly as wide as its own cell is still inside its own correlation lobe one period away: measured on
     * the field this task was handed, the bump at 1P was 0.015 while the ones at 2P, 3P and 4P were 0.050,
     * 0.047 and 0.055. An instrument that asked only about P would have reported that sky as clean.
     *
     * @param r          the averaged autocorrelation.
     * @param periodLag  the period predicted by the generator, in lags.
     * @param multiples  how many multiples of it to ask about.
     * @return the most prominent of them; `Lag` is where it stands, which a caller divides by @p periodLag
     *         to say WHICH multiple.
     */
    inline Peak LatticeScore( const std::vector<double>& r, int periodLag, int multiples )
    {
        Peak best;
        if ( periodLag <= 0 )
            return best;

        for ( int m = 1; m <= multiples; ++m )
        {
            // A tolerance of an eighth of the period, because the measurement is quantised to a voxel and
            // a cell is not a whole number of them. Wider than a quarter would let two multiples' windows
            // touch, which would report one bump twice.
            const Peak at = PeakNear( r, periodLag * m, std::max( 1, periodLag / 8 ) );
            if ( at.Found && ( !best.Found || at.Prominence > best.Prominence ) )
                best = at;
        }

        return best;
    }

    /**
     * @brief The CIRCULAR autocorrelation of a periodic map along one axis, lags 0..maxLag.
     *
     * Circular and not windowed because the procedural volume IS exactly periodic — the bake wraps every
     * lump across the region's faces on purpose — so wrapping the correlation introduces no edge and every
     * lag is estimated from the same number of samples. A windowed estimate on a periodic map would decay
     * merely because the overlap shrinks, and the peak finder would then have to be defended against a
     * slope that is an artefact of the estimator.
     */
    inline std::vector<double> CircularAutocorrelation( const std::vector<float>& map, int width, int height,
                                                        bool alongX, int maxLag )
    {
        std::vector<double> r( static_cast<size_t>( std::max( maxLag, 0 ) ) + 1, 0.0 );
        if ( map.empty() || width <= 0 || height <= 0 )
            return r;

        double mean = 0.0;
        for ( float v : map )
            mean += v;
        mean /= static_cast<double>( map.size() );

        double variance = 0.0;
        for ( float v : map )
            variance += ( v - mean ) * ( v - mean );
        variance /= static_cast<double>( map.size() );

        if ( variance <= 1e-12 )
            return r;

        for ( int lag = 0; lag <= maxLag; ++lag )
        {
            double sum = 0.0;
            for ( int y = 0; y < height; ++y )
            {
                for ( int x = 0; x < width; ++x )
                {
                    const int sx = alongX ? ( x + lag ) % width : x;
                    const int sy = alongX ? y : ( y + lag ) % height;
                    sum += ( map[static_cast<size_t>( y ) * width + x] - mean ) *
                           ( map[static_cast<size_t>( sy ) * width + sx] - mean );
                }
            }
            r[static_cast<size_t>( lag )] = sum / ( static_cast<double>( map.size() ) * variance );
        }

        return r;
    }

    /**
     * @brief The autocorrelation of a NON-periodic image along one axis, lags 0..maxLag.
     *
     * A rendered frame is not periodic, so each lag is a Pearson coefficient of the OVERLAP alone — its
     * own two means and its own two spreads. Normalising against the whole image's mean instead would make
     * the curve fall as the overlap slides off a bright horizon, which is a slope with no lattice in it.
     */
    inline std::vector<double> WindowedAutocorrelation( const std::vector<float>& map, int width, int height,
                                                        bool alongX, int maxLag )
    {
        std::vector<double> r( static_cast<size_t>( std::max( maxLag, 0 ) ) + 1, 0.0 );

        for ( int lag = 0; lag <= maxLag; ++lag )
        {
            const int spanX = alongX ? width - lag : width;
            const int spanY = alongX ? height : height - lag;
            if ( spanX <= 1 || spanY <= 1 )
                break;

            double sumA  = 0.0;
            double sumB  = 0.0;
            double sumAA = 0.0;
            double sumBB = 0.0;
            double sumAB = 0.0;

            for ( int y = 0; y < spanY; ++y )
            {
                for ( int x = 0; x < spanX; ++x )
                {
                    const double a = map[static_cast<size_t>( y ) * width + x];
                    const double b = alongX ? map[static_cast<size_t>( y ) * width + x + lag]
                                            : map[static_cast<size_t>( y + lag ) * width + x];
                    sumA += a;
                    sumB += b;
                    sumAA += a * a;
                    sumBB += b * b;
                    sumAB += a * b;
                }
            }

            const double n     = static_cast<double>( spanX ) * static_cast<double>( spanY );
            const double covAB = sumAB / n - ( sumA / n ) * ( sumB / n );
            const double varA  = sumAA / n - ( sumA / n ) * ( sumA / n );
            const double varB  = sumBB / n - ( sumB / n ) * ( sumB / n );

            r[static_cast<size_t>( lag )] =
                 ( varA > 1e-12 && varB > 1e-12 ) ? covAB / std::sqrt( varA * varB ) : 0.0;
        }

        return r;
    }

    /// Averages several autocorrelation curves of the same length — one per independent realisation of the
    /// field. The estimator's own wobble falls as one over the square root of the count while a real
    /// lattice peak does not move at all, which is what separates them.
    inline std::vector<double> Average( const std::vector<std::vector<double>>& curves )
    {
        if ( curves.empty() )
            return {};

        std::vector<double> out( curves.front().size(), 0.0 );
        for ( const std::vector<double>& curve : curves )
            for ( size_t k = 0; k < out.size() && k < curve.size(); ++k )
                out[k] += curve[k];

        for ( double& v : out )
            v /= static_cast<double>( curves.size() );
        return out;
    }

    /**
     * @brief Otsu's threshold over a luminance histogram — the split minimising the within-class variance.
     *
     * PARAMETER-FREE ON PURPOSE. A hand-set cloud/sky threshold is a second knob to argue about, and a
     * before/after pair measured at two different thresholds is not a comparison at all. Otsu's number is
     * a property of the image, so whoever runs the tool cannot move it.
     */
    inline double OtsuThreshold( const std::vector<float>& lum )
    {
        int histogram[256] = { 0 };
        for ( float v : lum )
            histogram[Common::Math::QuantiseUnitToByte( v )]++;

        const double total = static_cast<double>( lum.size() );
        if ( total <= 0.0 )
            return 0.0;

        double sum = 0.0;
        for ( int i = 0; i < 256; ++i )
            sum += i * static_cast<double>( histogram[i] );

        double sumB    = 0.0;
        double weightB = 0.0;
        double best    = -1.0;
        int    bestAt  = 0;

        for ( int i = 0; i < 256; ++i )
        {
            weightB += static_cast<double>( histogram[i] );
            if ( weightB <= 0.0 )
                continue;

            const double weightF = total - weightB;
            if ( weightF <= 0.0 )
                break;

            sumB += i * static_cast<double>( histogram[i] );

            const double meanB   = sumB / weightB;
            const double meanF   = ( sum - sumB ) / weightF;
            const double between = weightB * weightF * ( meanB - meanF ) * ( meanB - meanF );

            if ( between > best )
            {
                best   = between;
                bestAt = i;
            }
        }

        return bestAt / 255.0;
    }

    /// How long a body is along one axis, gathered over many scan lines of the same field.
    ///
    /// WHY A CHORD AND NOT A BOUNDING BOX. The autocorrelation above answers "where are the bodies", and it
    /// is blind to what each body IS: a sky of flat lenses and a sky of towers of the same footprint have
    /// the same placement and the same peak. The chord is the complementary question — how far a ray
    /// travelling along one axis stays inside cloud — and it is the quantity the march integrates, so a
    /// ratio of two chords is a ratio of two things the picture actually shows rather than of two
    /// parameters that happen to have the same units.
    struct ChordCensus
    {
        double VoxelsInRuns = 0.0;
        size_t Runs         = 0;

        double MeanVoxels() const
        {
            return ( Runs > 0 ) ? VoxelsInRuns / static_cast<double>( Runs ) : 0.0;
        }
    };

    /// Adds every unbroken run of cloud in @p line to @p into. A sample counts as cloud when it is above
    /// zero, which is the same convention the field mode's cover uses — the byte is already the quantised
    /// density, so any other threshold would be the operator's opinion rather than the field's.
    ///
    /// @p wrap JOINS THE TWO ENDS, and it is not cosmetic: the baked volume is exactly periodic across the
    /// region's faces, so a body straddling a face is ONE body seen twice. Counted without the wrap it
    /// arrives as two short chords and the mean falls — on the shipped field by about a twentieth, which is
    /// the size of the differences this instrument is used to argue about. The vertical axis is the one
    /// axis that must NOT wrap: the layer has a floor and a ceiling, and a body clipped by them is honestly
    /// as tall as the march can see it.
    inline void AccumulateChords( const std::vector<float>& line, bool wrap, ChordCensus& into )
    {
        const size_t n = line.size();
        if ( n == 0 )
            return;

        size_t first = 0;
        while ( first < n && line[first] > 0.0f )
            ++first;

        // Cloud everywhere: one run that circles the line, and it is one run whether or not the ends are
        // joined. Returning two here is the failure the wrap exists to prevent, taken to its extreme.
        if ( first == n )
        {
            into.VoxelsInRuns += static_cast<double>( n );
            into.Runs += 1;
            return;
        }

        // Walking from the first CLEAR sample makes the wrap free: every run then begins and ends inside
        // the walk, so the head and the tail of the original line are met as one.
        const size_t start = wrap ? first : 0;

        size_t run = 0;
        for ( size_t step = 0; step < n; ++step )
        {
            const size_t at = wrap ? ( start + step ) % n : step;
            if ( line[at] > 0.0f )
            {
                ++run;
                continue;
            }

            if ( run > 0 )
            {
                into.VoxelsInRuns += static_cast<double>( run );
                into.Runs += 1;
                run = 0;
            }
        }

        if ( run > 0 )
        {
            into.VoxelsInRuns += static_cast<double>( run );
            into.Runs += 1;
        }
    }

    /// The distance from the first cloud sample of @p line to its last, inclusive; zero if there is none.
    ///
    /// WHY THIS IS NOT THE CHORD, AND THE DIFFERENCE IS THE WHOLE QUESTION. A chord stops at the first gap,
    /// so a tower of lobes with air between them counts as several short chords and reads as flat. A SPAN
    /// does not: it reaches from the body's first sample to its last over whatever air is between them.
    /// A body whose chord is a fifth of its span is a tower one can see through; a body whose chord IS its
    /// span is a solid plate. Reporting only one of the two would have made those two skies the same
    /// number, and they are not the same picture.
    ///
    /// IT IS A SPAN ALONG ONE LINE AND NOT A SILHOUETTE, and the distinction is worth the sentence: taken
    /// down a column it says how much of the layer has cloud in it ABOVE THIS PATCH OF GROUND, which is
    /// less than how tall the body would look from the side, because a cluster's lobes are spread over a
    /// disc and the top of it does not stand over the bottom.
    /// HOW SCALLOPED THE CLOUD/SKY BOUNDARY IS: the length of that boundary divided by the square root of
    /// the area it encloses, over a binary mask.
    ///
    /// WHY THE NORMALISATION IS `sqrt(area)` AND NOT ANYTHING ELSE. A perimeter on its own is a size, and a
    /// frame with more cloud in it has more of one whatever its edge looks like. Dividing by the square
    /// root of the area makes the number answer "how ragged" rather than "how much".
    ///
    /// IT IS IN PIXELS AND IT IS SCALE-FREE, and a test is what settled that against the alternative. §DS
    /// published its numbers as a few thousandths, which is `(P/N) / sqrt(A/N)` — perimeter and area as
    /// FRACTIONS of the frame — and that form carries a `1/sqrt(N)`: the same picture rendered at twice the
    /// resolution measures half as much. The test written to assert scale-invariance is what caught it. So
    /// the raw ratio is what is computed, and §DS's table converts by multiplying by `sqrt(N)` — 839.8 on
    /// its 1280 x 551 crop, which puts its `0.0038` at **3.19** here. That conversion is arithmetic a
    /// reader can check rather than a claim.
    ///
    /// WHY IT IS HERE. `Docs/Clouds/CALIBRATION.md` §DS chose the erosion's Detail Strength on exactly this
    /// quantity and reported it to four decimals — and the instrument that produced those numbers was never
    /// committed, so the erosion §DS bought could not be re-measured by anybody who came after. §SIL had to
    /// prove that a change to the PLACEMENT had not spent it, found nothing in the tree to prove it with,
    /// and this is that gap closed rather than worked around.
    ///
    /// THE BOUNDARY IS COUNTED IN 4-NEIGHBOUR STEPS and the frame's edge does NOT count as boundary: a
    /// cloud running out of the picture is not an edge of the cloud, and counting it would make the number
    /// depend on where the crop was taken.
    inline double SilhouetteRaggedness( const std::vector<float>& mask, int width, int height )
    {
        if ( mask.empty() || width <= 1 || height <= 1 )
            return 0.0;

        double area      = 0.0;
        double perimeter = 0.0;

        for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
            {
                const bool here = mask[static_cast<size_t>( y ) * width + x] > 0.5f;
                if ( !here )
                    continue;

                area += 1.0;

                if ( x + 1 < width && mask[static_cast<size_t>( y ) * width + x + 1] <= 0.5f )
                    perimeter += 1.0;
                if ( x > 0 && mask[static_cast<size_t>( y ) * width + x - 1] <= 0.5f )
                    perimeter += 1.0;
                if ( y + 1 < height && mask[static_cast<size_t>( y + 1 ) * width + x] <= 0.5f )
                    perimeter += 1.0;
                if ( y > 0 && mask[static_cast<size_t>( y - 1 ) * width + x] <= 0.5f )
                    perimeter += 1.0;
            }

        return ( area > 0.0 ) ? perimeter / std::sqrt( area ) : 0.0;
    }

    /// HOW MUCH TEXTURE THERE IS INSIDE THE BODY: the mean absolute Laplacian of @p value at radius
    /// @p radius, gathered over the pixels @p mask says are cloud and over those only.
    ///
    /// IT IS THE OTHER HALF OF THE EROSION'S BARGAIN. Raggedness reads the outline; this reads the
    /// billows. A cut that only nibbled the silhouette would raise one and leave the other, and §DS's own
    /// table reports both for that reason.
    ///
    /// THE RADIUS IS AN ARGUMENT because a Laplacian at one pixel measures the renderer's noise and one at
    /// a billow's own radius measures billows. §DS used 4 px on a 1280-wide frame and this file keeps that
    /// as the reported default rather than re-choosing it.
    inline double InteriorLaplacian( const std::vector<float>& value, const std::vector<float>& mask, int width,
                                     int height, int radius )
    {
        if ( value.empty() || width <= 2 * radius || height <= 2 * radius || radius < 1 )
            return 0.0;

        double sum   = 0.0;
        double count = 0.0;

        for ( int y = radius; y + radius < height; ++y )
            for ( int x = radius; x + radius < width; ++x )
            {
                const size_t at = static_cast<size_t>( y ) * width + x;

                // THE WHOLE STENCIL HAS TO BE CLOUD, not only its centre, and a test is what insisted. A
                // Laplacian reads four neighbours a radius away, so a cloud pixel within one radius of the
                // silhouette samples SKY — and on a frame where the sky is darker than the body that is a
                // large number arriving from the boundary rather than from any billow. Fed a flat cloud
                // beside a checkered sky the centre-only form reported 0.0714 of texture that is not in the
                // cloud at all. This differs from §DS's own reading of the same quantity by that boundary
                // ring, so the two files' numbers are the same measurement only away from an edge.
                if ( mask[at] <= 0.5f || mask[at - radius] <= 0.5f || mask[at + radius] <= 0.5f ||
                     mask[at - static_cast<size_t>( radius ) * width] <= 0.5f ||
                     mask[at + static_cast<size_t>( radius ) * width] <= 0.5f )
                    continue;

                const double centre = value[at];
                const double lap    = value[at - radius] + value[at + radius] +
                                   value[at - static_cast<size_t>( radius ) * width] +
                                   value[at + static_cast<size_t>( radius ) * width] - 4.0 * centre;

                sum += std::fabs( lap );
                count += 1.0;
            }

        return ( count > 0.0 ) ? sum / count : 0.0;
    }

    inline size_t OccupiedSpan( const std::vector<float>& line )
    {
        size_t first = line.size();
        size_t last  = 0;

        for ( size_t i = 0; i < line.size(); ++i )
        {
            if ( line[i] <= 0.0f )
                continue;
            if ( first == line.size() )
                first = i;
            last = i;
        }

        return ( first == line.size() ) ? 0u : ( last - first + 1u );
    }
} // namespace LatticePeak
