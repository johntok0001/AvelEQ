#pragma once

// Fits a set of EQ bands to a target magnitude curve (dB on a log-frequency grid).
// Used by Match EQ and EQ Sketch.
//
// 1. optional shelves for broad tilts at the extremes
// 2. greedy: put a bell where the weighted residual is largest, with a Q estimated from
//    the width of that bump, then re-optimise everything
// 3. final joint Levenberg-Marquardt over (log2 f, gain, log2 Q) of all bands

#include "Filters.h"
#include <vector>
#include <atomic>
#include <functional>

namespace avel
{
    struct FitOptions
    {
        int maxBands = 8;
        double maxGain = 15.0;
        double minQ = 0.3, maxQ = 5.0;
        bool allowShelves = true;
        double tolerance = 0.3;   // dB, stop adding bands below this residual
        double minBandGain = 0.2; // remove bands smaller than this
    };

    struct FitResult
    {
        std::vector<BandSettings> bands;
        double rmsError = 0.0; // weighted, dB
    };

    class CurveFitter
    {
    public:
        // freqs ascending (Hz), target (dB), weight (>= 0) all the same size
        static FitResult fit (const std::vector<double>& freqs, const std::vector<double>& target,
                              const std::vector<double>& weight, const FitOptions& opt,
                              const std::atomic<bool>* cancel = nullptr)
        {
            CurveFitter f (freqs, target, weight, opt, cancel);
            return f.run();
        }

        static std::vector<double> response (const std::vector<double>& freqs, const std::vector<BandSettings>& bands)
        {
            std::vector<double> r (freqs.size(), 0.0);
            for (const auto& b : bands)
                for (size_t i = 0; i < freqs.size(); ++i)
                    r[i] += analogBandMagDb (b, freqs[i], 1.0e9);
            return r;
        }

    private:
        struct Band
        {
            FilterType type;
            double lf, g, lq; // log2 freq, gain dB, log2 Q
            std::vector<double> curve;
        };

        CurveFitter (const std::vector<double>& f, const std::vector<double>& t, const std::vector<double>& w,
                     const FitOptions& o, const std::atomic<bool>* c)
            : freqs (f), target (t), weight (w), opt (o), cancel (c), K (f.size())
        {
            lfMin = std::log2 (std::max (15.0, freqs.front()));
            lfMax = std::log2 (std::min (22000.0, freqs.back()));
        }

        bool cancelled() const noexcept { return cancel != nullptr && cancel->load(); }

        BandSettings settingsOf (const Band& b) const
        {
            BandSettings s;
            s.type = b.type;
            s.freq = std::pow (2.0, b.lf);
            s.gainDb = b.g;
            s.q = std::pow (2.0, b.lq);
            return s;
        }

        void evalCurve (const Band& b, std::vector<double>& out) const
        {
            out.resize (K);
            const auto s = settingsOf (b);
            for (size_t i = 0; i < K; ++i)
                out[i] = analogBandMagDb (s, freqs[i], 1.0e9);
        }

        void clampBand (Band& b) const
        {
            b.lf = std::clamp (b.lf, lfMin, lfMax);
            b.g = std::clamp (b.g, -opt.maxGain, opt.maxGain);
            const bool shelf = b.type != FilterType::bell;
            b.lq = std::clamp (b.lq, std::log2 (shelf ? 0.4 : opt.minQ), std::log2 (shelf ? 1.2 : opt.maxQ));
        }

        double cost (const std::vector<Band>& bands) const
        {
            double c = 0.0;
            for (size_t i = 0; i < K; ++i)
            {
                double m = 0.0;
                for (const auto& b : bands) m += b.curve[i];
                const double e = target[i] - m;
                c += weight[i] * e * e;
            }
            return c;
        }

        // Levenberg-Marquardt over all band parameters
        void optimise (std::vector<Band>& bands, int iterations) const
        {
            const size_t P = bands.size() * 3;
            if (P == 0) return;

            std::vector<double> model (K), resid (K), J (K * P), JtJ (P * P), Jtr (P), delta (P);
            std::vector<double> tmp;
            double lambda = 1.0e-2;
            double current = cost (bands);

            for (int it = 0; it < iterations && ! cancelled(); ++it)
            {
                std::fill (model.begin(), model.end(), 0.0);
                for (const auto& b : bands)
                    for (size_t i = 0; i < K; ++i) model[i] += b.curve[i];
                for (size_t i = 0; i < K; ++i) resid[i] = target[i] - model[i];

                // numeric Jacobian, one band at a time
                for (size_t bi = 0; bi < bands.size(); ++bi)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        Band pb = bands[bi];
                        const double h = k == 0 ? 0.004 : k == 1 ? 0.02 : 0.004;
                        (k == 0 ? pb.lf : k == 1 ? pb.g : pb.lq) += h;
                        evalCurve (pb, tmp);
                        const size_t col = bi * 3 + (size_t) k;
                        for (size_t i = 0; i < K; ++i)
                            J[i * P + col] = (tmp[i] - bands[bi].curve[i]) / h;
                    }
                }

                std::fill (JtJ.begin(), JtJ.end(), 0.0);
                std::fill (Jtr.begin(), Jtr.end(), 0.0);
                for (size_t i = 0; i < K; ++i)
                {
                    const double w = weight[i];
                    if (w <= 0.0) continue;
                    const double* row = &J[i * P];
                    for (size_t a = 0; a < P; ++a)
                    {
                        const double wa = w * row[a];
                        if (wa == 0.0) continue;
                        Jtr[a] += wa * resid[i];
                        for (size_t b = a; b < P; ++b) JtJ[a * P + b] += wa * row[b];
                    }
                }
                for (size_t a = 0; a < P; ++a)
                    for (size_t b = 0; b < a; ++b) JtJ[a * P + b] = JtJ[b * P + a];

                bool improved = false;
                for (int attempt = 0; attempt < 6 && ! improved; ++attempt)
                {
                    std::vector<double> A (JtJ);
                    for (size_t a = 0; a < P; ++a) A[a * P + a] += lambda * (JtJ[a * P + a] + 1.0e-6);
                    if (! solve (A, Jtr, delta, P)) { lambda *= 10.0; continue; }

                    std::vector<Band> trial (bands);
                    for (size_t bi = 0; bi < trial.size(); ++bi)
                    {
                        trial[bi].lf += std::clamp (delta[bi * 3 + 0], -0.5, 0.5);
                        trial[bi].g  += std::clamp (delta[bi * 3 + 1], -6.0, 6.0);
                        trial[bi].lq += std::clamp (delta[bi * 3 + 2], -0.5, 0.5);
                        clampBand (trial[bi]);
                        evalCurve (trial[bi], trial[bi].curve);
                    }
                    const double c = cost (trial);
                    if (c < current)
                    {
                        bands.swap (trial);
                        improved = (current - c) > 1.0e-9 * (1.0 + current);
                        current = c;
                        lambda = std::max (1.0e-7, lambda * 0.3);
                        if (! improved) return;
                    }
                    else
                        lambda *= 4.0;
                }
                if (! improved) return;
            }
        }

        static bool solve (std::vector<double>& A, const std::vector<double>& b, std::vector<double>& x, size_t n)
        {
            std::vector<double> r (b);
            for (size_t c = 0; c < n; ++c)
            {
                size_t piv = c;
                for (size_t i = c + 1; i < n; ++i) if (std::abs (A[i * n + c]) > std::abs (A[piv * n + c])) piv = i;
                if (std::abs (A[piv * n + c]) < 1.0e-14) return false;
                if (piv != c)
                {
                    for (size_t k = 0; k < n; ++k) std::swap (A[c * n + k], A[piv * n + k]);
                    std::swap (r[c], r[piv]);
                }
                for (size_t i = c + 1; i < n; ++i)
                {
                    const double f = A[i * n + c] / A[c * n + c];
                    if (f == 0.0) continue;
                    for (size_t k = c; k < n; ++k) A[i * n + k] -= f * A[c * n + k];
                    r[i] -= f * r[c];
                }
            }
            x.assign (n, 0.0);
            for (size_t c = n; c-- > 0;)
            {
                double s = r[c];
                for (size_t k = c + 1; k < n; ++k) s -= A[c * n + k] * x[k];
                x[c] = s / A[c * n + c];
            }
            return true;
        }

        std::vector<double> residual (const std::vector<Band>& bands) const
        {
            std::vector<double> r (target);
            for (const auto& b : bands)
                for (size_t i = 0; i < K; ++i) r[i] -= b.curve[i];
            return r;
        }

        double weightedMean (const std::vector<double>& v, double fLo, double fHi) const
        {
            double s = 0, w = 0;
            for (size_t i = 0; i < K; ++i)
                if (freqs[i] >= fLo && freqs[i] <= fHi) { s += weight[i] * v[i]; w += weight[i]; }
            return w > 0 ? s / w : 0.0;
        }

        FitResult run()
        {
            std::vector<Band> bands;
            auto addBand = [&] (FilterType t, double f, double g, double q)
            {
                Band b { t, std::log2 (f), g, std::log2 (q), {} };
                clampBand (b);
                evalCurve (b, b.curve);
                bands.push_back (std::move (b));
            };

            if (opt.allowShelves && opt.maxBands >= 3)
            {
                const double lo = weightedMean (target, 20.0, 70.0);
                const double mid = weightedMean (target, 150.0, 5000.0);
                const double hi = weightedMean (target, 9000.0, 20000.0);
                if (std::abs (lo - mid) > 1.0) addBand (FilterType::lowShelf, 120.0, lo - mid, 0.7);
                if (std::abs (hi - mid) > 1.0) addBand (FilterType::highShelf, 6000.0, hi - mid, 0.7);
                optimise (bands, 20);
            }

            while ((int) bands.size() < opt.maxBands && ! cancelled())
            {
                const auto r = residual (bands);
                size_t peak = 0;
                double best = 0.0;
                for (size_t i = 0; i < K; ++i)
                {
                    const double v = std::sqrt (weight[i]) * std::abs (r[i]);
                    if (v > best) { best = v; peak = i; }
                }
                if (std::abs (r[peak]) < opt.tolerance || best <= 0.0) break;

                // width of the bump at half height
                const double h = r[peak];
                size_t lo = peak, hi = peak;
                while (lo > 0 && r[lo - 1] * h > 0 && std::abs (r[lo - 1]) > 0.5 * std::abs (h)) --lo;
                while (hi + 1 < K && r[hi + 1] * h > 0 && std::abs (r[hi + 1]) > 0.5 * std::abs (h)) ++hi;
                const double bw = std::max (0.12, std::log2 (freqs[hi] / freqs[lo]));
                const double p2 = std::pow (2.0, bw);
                const double q = std::sqrt (p2) / (p2 - 1.0);

                addBand (FilterType::bell, freqs[peak], h, q);
                optimise (bands, 12);
            }

            optimise (bands, 80);

            FitResult res;
            for (const auto& b : bands)
                if (std::abs (b.g) >= opt.minBandGain)
                    res.bands.push_back (settingsOf (b));

            double c = 0, w = 0;
            const auto r = residual (bands);
            for (size_t i = 0; i < K; ++i) { c += weight[i] * r[i] * r[i]; w += weight[i]; }
            res.rmsError = w > 0 ? std::sqrt (c / w) : 0.0;
            return res;
        }

        const std::vector<double>& freqs;
        const std::vector<double>& target;
        const std::vector<double>& weight;
        FitOptions opt;
        const std::atomic<bool>* cancel;
        size_t K;
        double lfMin, lfMax;
    };

    // Log-spaced analysis grid shared by Match / Sketch
    inline std::vector<double> makeLogGrid (int points = 240, double lo = 20.0, double hi = 20000.0)
    {
        std::vector<double> f ((size_t) points);
        for (int i = 0; i < points; ++i)
            f[(size_t) i] = lo * std::pow (hi / lo, (double) i / (points - 1));
        return f;
    }
}
