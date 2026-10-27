#pragma once

// Filter design for Avel EQ.
//
// Every section is designed with the "matched" method (M. Vicanek, "Matched Second Order
// Digital Filters", 2016): poles come from the impulse-invariant mapping of the analog poles,
// the numerator is solved so that the digital magnitude equals the analog prototype at DC,
// at Nyquist and at the centre frequency. Result: no "cramping" of bells and shelves near
// Nyquist, the response looks and sounds like the analog prototype, with zero latency.

#include <array>
#include <cmath>
#include <complex>
#include <algorithm>

namespace avel
{
    constexpr double pi = 3.14159265358979323846;

    enum class FilterType { bell, lowShelf, highShelf, lowCut, highCut, notch, bandPass, tiltShelf, numTypes };

    inline bool typeHasGain (FilterType t) noexcept
    {
        return t == FilterType::bell || t == FilterType::lowShelf || t == FilterType::highShelf || t == FilterType::tiltShelf;
    }

    inline bool typeHasSlope (FilterType t) noexcept
    {
        return t == FilterType::lowCut || t == FilterType::highCut;
    }

    // Slopes offered for cut filters, dB/octave
    constexpr std::array<int, 9> slopeChoices { 6, 12, 18, 24, 30, 36, 48, 72, 96 };

    //==============================================================================
    struct Coeffs
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

        // |H(e^jw)|^2
        double magSq (double w) const noexcept
        {
            const double s = std::sin (0.5 * w);
            const double p1 = s * s, p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
            const double n = (b0 + b1 + b2) * (b0 + b1 + b2) * p0 + (b0 - b1 + b2) * (b0 - b1 + b2) * p1 - 4.0 * b0 * b2 * p2;
            const double d = (1.0 + a1 + a2) * (1.0 + a1 + a2) * p0 + (1.0 - a1 + a2) * (1.0 - a1 + a2) * p1 - 4.0 * a2 * p2;
            return d > 1.0e-300 ? std::max (0.0, n) / d : 0.0;
        }
    };

    // Analog prototype normalised to w0:  H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0)
    // First order sections use n2 = d2 = 0.
    struct Analog
    {
        double n0, n1, n2, d0, d1, d2;

        double magSq (double x) const noexcept // x = w / w0
        {
            const std::complex<double> num (n0 - n2 * x * x, n1 * x);
            const std::complex<double> den (d0 - d2 * x * x, d1 * x);
            const double dd = std::norm (den);
            return dd > 1.0e-300 ? std::norm (num) / dd : 1.0e300;
        }

        bool isFirstOrder() const noexcept { return d2 == 0.0 && n2 == 0.0; }
    };

    // w0 is the centre frequency in radians/sample.
    inline Coeffs designMatched (const Analog& a, double w0) noexcept
    {
        Coeffs c;

        if (a.isFirstOrder())
        {
            const double wp = (a.d0 / a.d1) * w0;
            const double p = std::exp (-wp);
            const double t0 = std::sqrt (a.magSq (0.0));
            const double tN = std::sqrt (a.magSq (pi / w0));
            c.a1 = -p;
            c.a2 = 0.0;
            c.b0 = 0.5 * (t0 * (1.0 - p) + tN * (1.0 + p));
            c.b1 = 0.5 * (t0 * (1.0 - p) - tN * (1.0 + p));
            c.b2 = 0.0;
            return c;
        }

        // poles: s^2 + (d1/d2) s + d0/d2
        const double wpn = std::sqrt (a.d0 / a.d2);
        const double zeta = (a.d1 / a.d2) / (2.0 * wpn);
        const double wp = wpn * w0;

        if (zeta <= 1.0)
            c.a1 = -2.0 * std::exp (-zeta * wp) * std::cos (wp * std::sqrt (1.0 - zeta * zeta));
        else
            c.a1 = -2.0 * std::exp (-zeta * wp) * std::cosh (wp * std::sqrt (zeta * zeta - 1.0));
        c.a2 = std::exp (-2.0 * zeta * wp);

        const double A0 = (1.0 + c.a1 + c.a2) * (1.0 + c.a1 + c.a2);
        const double A1 = (1.0 - c.a1 + c.a2) * (1.0 - c.a1 + c.a2);
        const double A2 = -4.0 * c.a2;

        const double wm = std::clamp (w0, 1.0e-6, 0.9 * pi);
        const double s = std::sin (0.5 * wm);
        const double p1 = s * s, p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
        const double Dm = A0 * p0 + A1 * p1 + A2 * p2;
        const double target = a.magSq (wm / w0) * Dm; // |N(wm)|^2 we need

        const bool pureHighPass = a.n0 == 0.0 && a.n1 == 0.0;
        const bool pureLowPass  = a.n1 == 0.0 && a.n2 == 0.0;
        const bool pureNotch    = a.n1 == 0.0 && a.n0 == a.n2 && a.n0 != 0.0;

        if (pureHighPass)
        {
            // double zero at DC: N = k (1 - z^-1)^2, |N|^2 = 16 k^2 phi1^2
            c.b0 = std::sqrt (target) / (4.0 * p1);
            c.b1 = -2.0 * c.b0;
            c.b2 = c.b0;
            return c;
        }

        if (pureLowPass)
        {
            // b2 = 0, match DC and the centre frequency
            const double B0 = A0 * a.magSq (0.0);
            const double B1 = std::max (0.0, (target - B0 * p0) / p1);
            c.b0 = 0.5 * (std::sqrt (B0) + std::sqrt (B1));
            c.b1 = std::sqrt (B0) - c.b0;
            c.b2 = 0.0;
            return c;
        }

        if (pureNotch)
        {
            // zeros exactly on the unit circle at w0, unity gain at DC
            const double k = (1.0 + c.a1 + c.a2) / (2.0 - 2.0 * std::cos (w0));
            c.b0 = k;
            c.b1 = -2.0 * std::cos (w0) * k;
            c.b2 = k;
            return c;
        }

        const double B0 = a.magSq (0.0) * A0;
        const double B1 = a.magSq (pi / w0) * A1;
        double B2 = (target - B0 * p0 - B1 * p1) / p2;

        const double sB0 = std::sqrt (std::max (0.0, B0)), sB1 = std::sqrt (std::max (0.0, B1));
        const double W = 0.5 * (sB0 + sB1);
        if (W * W + B2 < 0.0)
            B2 = -W * W;

        c.b0 = 0.5 * (W + std::sqrt (W * W + B2));
        c.b1 = 0.5 * (sB0 - sB1);
        c.b2 = c.b0 > 1.0e-300 ? -B2 / (4.0 * c.b0) : 0.0;
        return c;
    }

    //==============================================================================
    // A band = cascade of up to 8 second-order sections (96 dB/oct = 16th order)
    struct BandSettings
    {
        FilterType type = FilterType::bell;
        double freq = 1000.0, gainDb = 0.0, q = 1.0;
        int slopeIndex = 1;
    };

    struct BandDesign
    {
        static constexpr int maxSections = 8;
        std::array<Coeffs, maxSections> sections;
        int numSections = 0;

        double magSq (double w) const noexcept
        {
            double m = 1.0;
            for (int i = 0; i < numSections; ++i)
                m *= sections[(size_t) i].magSq (w);
            return m;
        }

        double magDb (double w) const noexcept
        {
            return 10.0 * std::log10 (std::max (1.0e-30, magSq (w)));
        }
    };

    inline double clampFreq (double f, double sampleRate) noexcept
    {
        return std::clamp (f, 5.0, 0.47 * sampleRate);
    }

    // 1/H(z), renormalised. Valid because the matched numerator is minimum phase.
    inline Coeffs invert (const Coeffs& c) noexcept
    {
        Coeffs r;
        const double k = std::abs (c.b0) > 1.0e-300 ? 1.0 / c.b0 : 0.0;
        r.b0 = k; r.b1 = c.a1 * k; r.b2 = c.a2 * k;
        r.a1 = c.b1 * k; r.a2 = c.b2 * k;
        return r;
    }

    inline BandDesign designBand (const BandSettings& b, double sampleRate) noexcept
    {
        BandDesign d;
        const double w0 = 2.0 * pi * clampFreq (b.freq, sampleRate) / sampleRate;
        const double Q = std::clamp (b.q, 0.025, 40.0);
        auto add = [&] (const Analog& a) { d.sections[(size_t) d.numSections++] = designMatched (a, w0); };
        auto addInverted = [&] (const Analog& a) { d.sections[(size_t) d.numSections++] = invert (designMatched (a, w0)); };

        // Gain filters: always design the orientation whose poles are well behaved
        // (low enough and not over-damped), then invert it when the other one is needed.
        const double g = b.gainDb;
        const double Au = std::pow (10.0, std::abs (g) / 40.0); // >= 1
        const double Ad = 1.0 / Au;                              // <= 1

        auto bell  = [&] (double A) { return Analog { 1.0, A / Q, 1.0, 1.0, 1.0 / (A * Q), 1.0 }; };
        auto lowSh = [&] (double A) { const double sA = std::sqrt (A); return Analog { A * A, A * sA / Q, A, 1.0, sA / Q, A }; };
        auto highSh = [&] (double A) { const double sA = std::sqrt (A); return Analog { A, A * sA / Q, A * A, A, sA / Q, 1.0 }; };
        auto tilt  = [&] (double A) { const double sA = std::sqrt (A); return Analog { 1.0, sA / Q, A, A, sA / Q, 1.0 }; };

        switch (b.type)
        {
            case FilterType::bell:
                if (g >= 0) add (bell (Au)); else addInverted (bell (Au));
                break;

            case FilterType::lowShelf:
                if (g >= 0) add (lowSh (Au)); else addInverted (lowSh (Au));
                break;

            case FilterType::highShelf:
                if (g <= 0) add (highSh (Ad)); else addInverted (highSh (Ad));
                break;

            case FilterType::tiltShelf:
                if (g <= 0) add (tilt (Ad)); else addInverted (tilt (Ad));
                break;

            case FilterType::notch:
                add ({ 1.0, 0.0, 1.0, 1.0, 1.0 / Q, 1.0 });
                break;

            case FilterType::bandPass:
                add ({ 0.0, 1.0 / Q, 0.0, 1.0, 1.0 / Q, 1.0 });
                break;

            case FilterType::lowCut:
            case FilterType::highCut:
            {
                const bool hp = b.type == FilterType::lowCut;
                const int order = slopeChoices[(size_t) std::clamp (b.slopeIndex, 0, (int) slopeChoices.size() - 1)] / 6;
                const double resonance = Q / 0.7071067811865476;

                if (order % 2 == 1)
                    add (hp ? Analog { 0.0, 1.0, 0.0, 1.0, 1.0, 0.0 } : Analog { 1.0, 0.0, 0.0, 1.0, 1.0, 0.0 });

                const int pairs = order / 2;
                for (int k = 1; k <= pairs; ++k)
                {
                    const double theta = (order % 2 == 0) ? (2.0 * k - 1.0) * pi / (2.0 * order) : k * pi / order;
                    double qk = 1.0 / (2.0 * std::cos (theta));
                    if (k == pairs) qk *= resonance; // the sharpest pair carries the resonance
                    add (hp ? Analog { 0.0, 0.0, 1.0, 1.0, 1.0 / qk, 1.0 } : Analog { 1.0, 0.0, 0.0, 1.0, 1.0 / qk, 1.0 });
                }
                break;
            }

            case FilterType::numTypes:
                break;
        }
        return d;
    }

    // Calls fn (const Analog&) for every analog section of a band (normalised to its centre frequency).
    // This is the exact analog prototype the digital designs reproduce.
    template <typename Fn>
    inline void forEachAnalogSection (const BandSettings& b, Fn&& fn)
    {
        const double Q = std::clamp (b.q, 0.025, 40.0);
        const double A = std::pow (10.0, b.gainDb / 40.0);
        const double sA = std::sqrt (A);

        switch (b.type)
        {
            case FilterType::bell:      fn (Analog { 1.0, A / Q, 1.0, 1.0, 1.0 / (A * Q), 1.0 }); break;
            case FilterType::lowShelf:  fn (Analog { A * A, A * sA / Q, A, 1.0, sA / Q, A }); break;
            case FilterType::highShelf: fn (Analog { A, A * sA / Q, A * A, A, sA / Q, 1.0 }); break;
            case FilterType::tiltShelf: fn (Analog { 1.0, sA / Q, A, A, sA / Q, 1.0 }); break;
            case FilterType::notch:     fn (Analog { 1.0, 0.0, 1.0, 1.0, 1.0 / Q, 1.0 }); break;
            case FilterType::bandPass:  fn (Analog { 0.0, 1.0 / Q, 0.0, 1.0, 1.0 / Q, 1.0 }); break;
            case FilterType::lowCut:
            case FilterType::highCut:
            {
                const bool hp = b.type == FilterType::lowCut;
                const int order = slopeChoices[(size_t) std::clamp (b.slopeIndex, 0, (int) slopeChoices.size() - 1)] / 6;
                const double resonance = Q / 0.7071067811865476;
                if (order % 2 == 1)
                    fn (hp ? Analog { 0.0, 1.0, 0.0, 1.0, 1.0, 0.0 } : Analog { 1.0, 0.0, 0.0, 1.0, 1.0, 0.0 });
                const int pairs = order / 2;
                for (int k = 1; k <= pairs; ++k)
                {
                    const double theta = (order % 2 == 0) ? (2.0 * k - 1.0) * pi / (2.0 * order) : k * pi / order;
                    double qk = 1.0 / (2.0 * std::cos (theta));
                    if (k == pairs) qk *= resonance;
                    fn (hp ? Analog { 0.0, 0.0, 1.0, 1.0, 1.0 / qk, 1.0 } : Analog { 1.0, 0.0, 0.0, 1.0, 1.0 / qk, 1.0 });
                }
                break;
            }
            case FilterType::numTypes: break;
        }
    }

    inline double analogBandMagDb (const BandSettings& b, double freqHz, double sampleRate) noexcept
    {
        const double x = freqHz / clampFreq (b.freq, sampleRate);
        double m = 1.0;
        forEachAnalogSection (b, [&] (const Analog& a) { m *= a.magSq (x); });
        return 10.0 * std::log10 (std::max (1.0e-30, m));
    }

    // Complex analog response (magnitude and phase) at freqHz
    inline std::complex<double> analogBandResponse (const BandSettings& b, double freqHz, double sampleRate) noexcept
    {
        const double x = freqHz / clampFreq (b.freq, sampleRate);
        const std::complex<double> s (0.0, x);
        std::complex<double> h (1.0, 0.0);
        forEachAnalogSection (b, [&] (const Analog& a)
        {
            const auto num = a.n2 * s * s + a.n1 * s + a.n0;
            const auto den = a.d2 * s * s + a.d1 * s + a.d0;
            h *= std::abs (den) > 1.0e-300 ? num / den : std::complex<double> (0.0);
        });
        return h;
    }

    //==============================================================================
    // Transposed direct form II, double precision state
    struct BiquadState
    {
        double z1 = 0, z2 = 0;

        inline double process (const Coeffs& c, double x) noexcept
        {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }

        void reset() noexcept { z1 = z2 = 0; }
    };

    // Musical note name for a frequency, e.g. "A4 +3c"
    inline void noteForFrequency (double f, int& midiNote, int& cents) noexcept
    {
        const double n = 69.0 + 12.0 * std::log2 (std::max (1.0, f) / 440.0);
        midiNote = (int) std::lround (n);
        cents = (int) std::lround ((n - midiNote) * 100.0);
    }
}
