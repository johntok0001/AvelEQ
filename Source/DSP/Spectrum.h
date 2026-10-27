#pragma once

// Long-term average spectrum (for Match EQ) and log-grid helpers.

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

namespace avel
{
    class SpectrumAccumulator
    {
    public:
        static constexpr int order = 13, size = 1 << order; // 8192

        SpectrumAccumulator()
            : fft (order), window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false),
              power ((size_t) size / 2 + 1, 0.0), buf ((size_t) size * 2, 0.0f) {}

        void reset (double sr) { sampleRate = sr; std::fill (power.begin(), power.end(), 0.0); frames = 0; seconds = 0.0; }

        // mono frame of exactly `size` samples; quiet frames are ignored
        void addFrame (const float* x, double newSeconds)
        {
            double e = 0;
            for (int i = 0; i < size; ++i) e += (double) x[i] * x[i];
            if (e / size < 1.0e-8) return; // below about -80 dBFS

            std::copy (x, x + size, buf.begin());
            std::fill (buf.begin() + size, buf.end(), 0.0f);
            window.multiplyWithWindowingTable (buf.data(), (size_t) size);
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            for (int k = 0; k <= size / 2; ++k)
                power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
            ++frames;
            seconds += newSeconds;
        }

        // whole mono/stereo buffer, hop = size / 2
        void addBuffer (const juce::AudioBuffer<float>& b, const std::atomic<bool>* cancel = nullptr)
        {
            std::vector<float> mono ((size_t) size);
            const int n = b.getNumSamples();
            for (int pos = 0; pos + size <= n; pos += size / 2)
            {
                if (cancel != nullptr && cancel->load()) return;
                for (int i = 0; i < size; ++i)
                {
                    float v = 0;
                    for (int ch = 0; ch < b.getNumChannels(); ++ch) v += b.getSample (ch, pos + i);
                    mono[(size_t) i] = v / (float) juce::jmax (1, b.getNumChannels());
                }
                addFrame (mono.data(), 0.0);
            }
            seconds = n / sampleRate;
        }

        bool hasData() const noexcept { return frames > 0; }
        double getSeconds() const noexcept { return seconds; }
        double getSampleRate() const noexcept { return sampleRate; }

        // dB on the given grid, smoothed over `smoothOct` octaves (power average)
        std::vector<double> toGrid (const std::vector<double>& freqs, double smoothOct) const
        {
            std::vector<double> out (freqs.size(), -120.0);
            if (frames == 0) return out;

            const int half = size / 2;
            std::vector<double> prefix ((size_t) half + 2, 0.0);
            for (int k = 0; k <= half; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + power[(size_t) k] / frames;

            const double binHz = sampleRate / size;
            const double r = std::pow (2.0, 0.5 * std::max (1.0 / 48.0, smoothOct));
            auto cum = [&] (double bin)
            {
                bin = juce::jlimit (0.0, (double) half, bin + 0.5);
                const int i0 = (int) bin;
                return prefix[(size_t) i0] + (prefix[(size_t) juce::jmin (i0 + 1, half + 1)] - prefix[(size_t) i0]) * (bin - i0);
            };

            for (size_t i = 0; i < freqs.size(); ++i)
            {
                double lo = freqs[i] / r / binHz, hi = freqs[i] * r / binHz;
                if (hi - lo < 1.0) { const double c = 0.5 * (lo + hi); lo = c - 0.5; hi = c + 0.5; }
                const double p = (cum (hi) - cum (lo)) / (hi - lo);
                out[i] = 10.0 * std::log10 (std::max (1.0e-20, p));
            }
            return out;
        }

    private:
        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
        std::vector<double> power;
        std::vector<float> buf;
        int frames = 0;
        double sampleRate = 48000.0, seconds = 0.0;
    };

    // Weight used when matching: full weight 40 Hz - 14 kHz, fading towards the edges
    inline std::vector<double> matchWeights (const std::vector<double>& freqs)
    {
        std::vector<double> w (freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const double f = freqs[i];
            double v = 1.0;
            if (f < 40.0) v = std::pow (f / 40.0, 2.0);
            if (f > 14000.0) v = std::pow (14000.0 / f, 4.0);
            w[i] = v;
        }
        return w;
    }
}
