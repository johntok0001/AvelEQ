#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/Filters.h"

namespace avel
{
    constexpr int numBands = 24;
    constexpr double minFreq = 10.0, maxFreq = 30000.0;

    enum class Placement { stereo, left, right, mid, side };
    enum class PhaseMode { zeroLatency, linear };

    inline const juce::StringArray& typeNames()
    {
        static const juce::StringArray n { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt Shelf" };
        return n;
    }

    inline const juce::StringArray& placementNames()
    {
        static const juce::StringArray n { "Stereo", "Left", "Right", "Mid", "Side" };
        return n;
    }

    inline juce::StringArray slopeNames()
    {
        juce::StringArray s;
        for (int v : slopeChoices) s.add (juce::String (v) + " dB/oct");
        return s;
    }

    inline juce::String bandId (int band, const char* name)
    {
        return "b" + juce::String (band + 1) + "_" + name;
    }

    // Logarithmic ranges
    inline juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float s, float e, float v) { return s * std::pow (e / s, v); },
                 [] (float s, float e, float v) { return std::log (v / s) / std::log (e / s); },
                 [] (float s, float e, float v) { return juce::jlimit (s, e, v); } };
    }

    inline juce::String formatFreq (float hz, bool withUnit = true)
    {
        if (hz >= 10000.0f) return juce::String (hz / 1000.0f, 1) + (withUnit ? " kHz" : "k");
        if (hz >= 1000.0f)  return juce::String (hz / 1000.0f, 2) + (withUnit ? " kHz" : "k");
        if (hz >= 100.0f)   return juce::String (juce::roundToInt (hz)) + (withUnit ? " Hz" : "");
        return juce::String (hz, 1) + (withUnit ? " Hz" : "");
    }

    inline float parseFreq (const juce::String& t)
    {
        auto s = t.trim().toLowerCase();
        float mult = 1.0f;
        if (s.containsChar ('k')) mult = 1000.0f;
        return s.retainCharacters ("0123456789.-").getFloatValue() * mult;
    }

    inline juce::String formatDb (float db, int decimals = 1)
    {
        const auto v = std::abs (db) < 0.05f ? 0.0f : db;
        return (v > 0 ? "+" : "") + juce::String (v, decimals) + " dB";
    }

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        for (int b = 0; b < numBands; ++b)
        {
            auto group = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b + 1), "Band " + String (b + 1), " | ");
            const String n = "Band " + String (b + 1) + " ";

            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "used"), 1 }, n + "Used", StringArray { "Off", "On" }, 0));
            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "on"), 1 }, n + "Enabled", StringArray { "Off", "On" }, 1));
            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "type"), 1 }, n + "Shape", typeNames(), 0));

            group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, "freq"), 1 }, n + "Frequency",
                logRange ((float) minFreq, (float) maxFreq), 1000.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatFreq (v); })
                                               .withValueFromStringFunction ([] (const String& t) { return parseFreq (t); })));

            group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, "gain"), 1 }, n + "Gain",
                NormalisableRange<float> (-30.0f, 30.0f, 0.01f), 0.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatDb (v, 2); })
                                               .withLabel ("dB")));

            group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, "q"), 1 }, n + "Q",
                logRange (0.025f, 40.0f), 1.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1 ? 3 : 2); })));

            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "slope"), 1 }, n + "Slope", slopeNames(), 1));
            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "stereo"), 1 }, n + "Stereo Placement", placementNames(), 0));

            group->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "dyn"), 1 }, n + "Dynamic", StringArray { "Off", "On" }, 0));
            group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, "range"), 1 }, n + "Dynamic Range",
                NormalisableRange<float> (-30.0f, 30.0f, 0.01f), -6.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatDb (v, 1); })));
            group->addChild (std::make_unique<AudioParameterFloat> (ParameterID { bandId (b, "thr"), 1 }, n + "Threshold",
                NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -24.0f,
                AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));

            layout.add (std::move (group));
        }

        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "outGain", 1 }, "Output Gain",
            NormalisableRange<float> (-36.0f, 36.0f, 0.01f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatDb (v, 1); })));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "autoGain", 1 }, "Auto Gain", StringArray { "Off", "On" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "phase", 1 }, "Processing Mode",
                                                            StringArray { "Zero Latency", "Natural Phase", "Linear Phase" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "lpRes", 1 }, "Linear Phase Resolution",
                                                            StringArray { "Low", "Medium", "High" }, 1));
        return layout;
    }

    // Raw pointers to one band's parameter values
    struct BandParams
    {
        std::atomic<float>* used = nullptr;
        std::atomic<float>* on = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;
        std::atomic<float>* slope = nullptr;
        std::atomic<float>* stereo = nullptr;
        std::atomic<float>* dyn = nullptr;
        std::atomic<float>* range = nullptr;
        std::atomic<float>* thr = nullptr;

        void attach (juce::AudioProcessorValueTreeState& s, int b)
        {
            used = s.getRawParameterValue (bandId (b, "used"));
            on = s.getRawParameterValue (bandId (b, "on"));
            type = s.getRawParameterValue (bandId (b, "type"));
            freq = s.getRawParameterValue (bandId (b, "freq"));
            gain = s.getRawParameterValue (bandId (b, "gain"));
            q = s.getRawParameterValue (bandId (b, "q"));
            slope = s.getRawParameterValue (bandId (b, "slope"));
            stereo = s.getRawParameterValue (bandId (b, "stereo"));
            dyn = s.getRawParameterValue (bandId (b, "dyn"));
            range = s.getRawParameterValue (bandId (b, "range"));
            thr = s.getRawParameterValue (bandId (b, "thr"));
        }

        bool isActive() const noexcept { return used->load() > 0.5f && on->load() > 0.5f; }

        BandSettings settings (float gainOverride = std::numeric_limits<float>::quiet_NaN()) const noexcept
        {
            BandSettings s;
            s.type = (FilterType) juce::jlimit (0, (int) FilterType::numTypes - 1, (int) type->load());
            s.freq = freq->load();
            s.gainDb = std::isnan (gainOverride) ? gain->load() : gainOverride;
            s.q = q->load();
            s.slopeIndex = (int) slope->load();
            return s;
        }

        Placement placement() const noexcept { return (Placement) juce::jlimit (0, 4, (int) stereo->load()); }
        bool isDynamic() const noexcept { return dyn->load() > 0.5f && typeHasGain ((FilterType) (int) type->load()); }
    };
}
