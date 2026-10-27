#pragma once

// Linear-phase engine. The whole EQ (all bands, including per-band L/R/M/S placement) is
// turned into a 2x2 matrix of zero-phase FIR filters:
//     outL = hLL * L + hLR * R
//     outR = hRL * L + hRR * R
// Magnitudes come straight from the analog prototypes, so the response is exact up to Nyquist.

#include <juce_dsp/juce_dsp.h>
#include "Filters.h"
#include "../Params.h"

namespace avel
{
    struct BandSnapshot
    {
        bool active = false;
        BandSettings s;
        Placement placement = Placement::stereo;
    };

    using EQSnapshot = std::array<BandSnapshot, numBands>;

    class LinearPhaseEngine
    {
    public:
        static int lengthForResolution (int res) noexcept { return res <= 0 ? 4096 : res == 1 ? 16384 : 32768; }

        void prepare (double sr, int blockSize)
        {
            sampleRate = sr;
            const juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize), 1 };
            for (auto& c : conv)
            {
                c = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { 512 }, queue);
                c->prepare (spec);
            }
            tmp.setSize (6, juce::jmax (1, blockSize));
            built = false;
        }

        void reset()
        {
            for (auto& c : conv)
                if (c != nullptr) c->reset();
        }

        int getLength() const noexcept { return length; }

        // Natural phase: analog magnitude AND analog phase, realised as a causal FIR with a short pre-delay
        static int naturalDelay (int length) noexcept { return juce::jmin (length / 8, 1024); }

        // Background thread: builds and installs new kernels
        void build (const EQSnapshot& snap, int newLength, bool natural)
        {
            length = newLength;
            const int L = newLength;
            const int order = juce::roundToInt (std::log2 ((double) L));
            const int D = naturalDelay (L);
            juce::dsp::FFT fft (order);

            using C = std::complex<double>;
            std::array<std::vector<float>, 4> spec;
            for (auto& s : spec) s.assign ((size_t) L * 2, 0.0f);

            for (int k = 0; k <= L / 2; ++k)
            {
                const double f = juce::jmax (1.0e-3, k * sampleRate / L);
                C m00 = 1, m01 = 0, m10 = 0, m11 = 1;

                for (const auto& b : snap)
                {
                    if (! b.active) continue;
                    const C g = natural ? analogBandResponse (b.s, f, sampleRate)
                                        : C (std::pow (10.0, analogBandMagDb (b.s, f, sampleRate) / 20.0), 0.0);
                    C t00 = 1, t01 = 0, t10 = 0, t11 = 1;
                    switch (b.placement)
                    {
                        case Placement::stereo: t00 = t11 = g; break;
                        case Placement::left:   t00 = g; break;
                        case Placement::right:  t11 = g; break;
                        case Placement::mid:    t00 = t11 = 0.5 * (g + 1.0); t01 = t10 = 0.5 * (g - 1.0); break;
                        case Placement::side:   t00 = t11 = 0.5 * (1.0 + g); t01 = t10 = 0.5 * (1.0 - g); break;
                    }
                    const C n00 = t00 * m00 + t01 * m10, n01 = t00 * m01 + t01 * m11;
                    const C n10 = t10 * m00 + t11 * m10, n11 = t10 * m01 + t11 * m11;
                    m00 = n00; m01 = n01; m10 = n10; m11 = n11;
                }

                // natural: delay by D samples; linear: zero phase, centred later
                const C rot = natural ? std::polar (1.0, -2.0 * pi * k * D / L) : C (1.0, 0.0);
                const C e[4] { m00 * rot, m01 * rot, m10 * rot, m11 * rot };
                for (int i = 0; i < 4; ++i)
                {
                    spec[(size_t) i][(size_t) (2 * k)] = (float) e[i].real();
                    spec[(size_t) i][(size_t) (2 * k + 1)] = (k == 0 || k == L / 2) ? 0.0f : (float) e[i].imag();
                }
            }

            const bool cross = hasCross (snap);

            for (int i = 0; i < 4; ++i)
            {
                fft.performRealOnlyInverseTransform (spec[(size_t) i].data());

                juce::AudioBuffer<float> ir (1, L);
                auto* d = ir.getWritePointer (0);
                for (int n = 0; n < L; ++n)
                {
                    if (natural)
                    {
                        // gentle fade-in of the pre-delay region, long fade-out of the tail
                        double w = 1.0;
                        if (n < D / 2) w = std::pow (std::sin (0.5 * pi * n / (D / 2.0)), 2.0);
                        const int tail = L / 4;
                        if (n > L - tail) w *= std::pow (std::cos (0.5 * pi * (n - (L - tail)) / tail), 2.0);
                        d[n] = (float) (spec[(size_t) i][(size_t) n] * w);
                    }
                    else
                    {
                        // rotate so the zero-phase response is centred, then window
                        const float v = spec[(size_t) i][(size_t) ((n + L / 2) % L)];
                        const double x = (double) n / L; // periodic window: symmetric about the centre tap
                        const double w = 0.42 - 0.5 * std::cos (2.0 * pi * x) + 0.08 * std::cos (4.0 * pi * x);
                        d[n] = (float) (v * w);
                    }
                }

                if ((i == 1 || i == 2) && ! cross)
                    ir.clear();

                conv[(size_t) i]->loadImpulseResponse (std::move (ir), sampleRate,
                                                       juce::dsp::Convolution::Stereo::no,
                                                       juce::dsp::Convolution::Trim::no,
                                                       juce::dsp::Convolution::Normalise::no);
            }

            crossActive.store (cross);
            built = true;
        }

        void process (float* L, float* R, int n) noexcept
        {
            if (tmp.getNumSamples() < n) return; // never happens after prepare()

            float* inL  = tmp.getWritePointer (0);
            float* inR  = tmp.getWritePointer (1);
            float* oLL  = tmp.getWritePointer (2);
            float* oLR  = tmp.getWritePointer (3);
            float* oRL  = tmp.getWritePointer (4);
            float* oRR  = tmp.getWritePointer (5);
            juce::FloatVectorOperations::copy (inL, L, n);
            juce::FloatVectorOperations::copy (inR, R, n);

            auto run = [n] (juce::dsp::Convolution& c, const float* in, float* out)
            {
                const float* inPtr[] = { in };
                float* outPtr[] = { out };
                juce::dsp::AudioBlock<const float> ib (inPtr, 1, (size_t) n);
                juce::dsp::AudioBlock<float> ob (outPtr, 1, (size_t) n);
                c.process (juce::dsp::ProcessContextNonReplacing<float> (ib, ob));
            };

            run (*conv[0], inL, oLL);
            run (*conv[3], inR, oRR);
            run (*conv[1], inR, oLR);
            run (*conv[2], inL, oRL);

            for (int i = 0; i < n; ++i)
            {
                L[i] = oLL[i] + oLR[i];
                R[i] = oRL[i] + oRR[i];
            }
        }

        static bool hasCross (const EQSnapshot& snap) noexcept
        {
            for (const auto& b : snap)
                if (b.active && (b.placement == Placement::mid || b.placement == Placement::side))
                    return true;
            return false;
        }

        bool isBuilt() const noexcept { return built; }

    private:
        double sampleRate = 44100.0;
        int length = 16384;
        juce::dsp::ConvolutionMessageQueue queue;
        std::array<std::unique_ptr<juce::dsp::Convolution>, 4> conv;
        juce::AudioBuffer<float> tmp;
        std::atomic<bool> crossActive { false };
        std::atomic<bool> built { false };
    };
}
