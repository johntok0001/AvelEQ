#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "DSP/Filters.h"
#include "DSP/LinearPhase.h"
#include "DSP/ScopeRing.h"

class AvelProcessor : public juce::AudioProcessor,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater
{
public:
    AvelProcessor();
    ~AvelProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Avel EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    juce::UndoManager undo;
    juce::AudioProcessorValueTreeState apvts;
    std::array<avel::BandParams, avel::numBands> band;

    // Band management (message thread). Each is one undo step.
    int addBand (avel::FilterType type, float freq, float gainDb, float q = 1.0f);
    void removeBand (int b);
    void resetBand (int b);
    void setParam (const juce::String& id, float plainValue);
    juce::RangedAudioParameter* param (const juce::String& id) const { return apvts.getParameter (id); }

    // A/B
    int getABSlot() const { return abSlot; }
    void switchAB (int slot);
    void copyABToOther();

    // Presets
    static juce::StringArray getFactoryPresetNames();
    void loadFactoryPreset (int index);
    static juce::File getUserPresetFolder();
    bool saveUserPreset (const juce::File&);
    bool loadUserPreset (const juce::File&);
    juce::String currentPresetName { "Default" };

    // UI feedback (any thread)
    float getDynamicGain (int b) const noexcept { return dynGainNow[(size_t) b].load(); }
    bool isDynamicActive (int b) const noexcept { return dynActive[(size_t) b].load(); }
    float getAutoGainDb() const noexcept { return autoGainNow.load(); }
    float takeOutputPeak (int ch) noexcept { return outPeak[(size_t) ch].exchange (0.0f); }
    void setSoloBand (int b) noexcept { soloBand.store (b); }
    int getSoloBand() const noexcept { return soloBand.load(); }
    // 0 zero latency, 1 natural phase, 2 linear phase
    int getPhaseMode() const noexcept { return juce::jlimit (0, 2, (int) apvts.getRawParameterValue ("phase")->load()); }
    bool isLinearPhase() const noexcept { return getPhaseMode() > 0; } // any FIR mode
    int latencyForCurrentMode() const noexcept;

    avel::ScopeRing preScope, postScope;

    // UI preferences stored with the session
    juce::ValueTree uiState { "UI" };

private:
    struct BandRuntime
    {
        avel::BandDesign design;
        std::array<std::array<avel::BiquadState, avel::BandDesign::maxSections>, 2> st {};
        avel::BandSettings cur;
        bool initialised = false;
        float mix = 0.0f;            // fades the band in/out to avoid clicks
        bool wasDynamic = false;
        avel::Placement placement = avel::Placement::stereo;

        avel::Coeffs detect[2];
        int detectSections = 0;
        avel::BiquadState detSt[2];
        double env = 0.0;

        void resetState() noexcept
        {
            for (auto& ch : st) for (auto& s : ch) s.reset();
            for (auto& s : detSt) s.reset();
            env = 0.0;
        }
    };

    void parameterChanged (const juce::String& id, float) override;
    void handleAsyncUpdate() override;

    void processZeroLatency (float* L, float* R, int n, bool stereo);
    void processBand (int b, BandRuntime& rt, float* L, float* R, int n, bool stereo, float targetMix);
    void updateAutoGain (int n);
    avel::EQSnapshot snapshot() const;
    uint64_t snapshotHash() const;
    void applySolo (float* L, float* R, int n, bool stereo);

    std::array<BandRuntime, avel::numBands> rt;
    double sr = 44100.0;
    bool anyRedesign = true;

    // auto gain / output
    float autoGainTarget = 0.0f;
    juce::SmoothedValue<float> outGainSmoothed;
    std::atomic<float> autoGainNow { 0.0f };
    std::array<std::atomic<float>, 2> outPeak {};

    // dynamics display
    std::array<std::atomic<float>, avel::numBands> dynGainNow {};
    std::array<std::atomic<bool>, avel::numBands> dynActive {};

    // solo
    std::atomic<int> soloBand { -1 };
    int lastSolo = -2;
    avel::BandDesign soloDesign;
    std::array<std::array<avel::BiquadState, avel::BandDesign::maxSections>, 2> soloSt {};

    // linear phase
    class KernelThread;
    std::unique_ptr<KernelThread> kernelThread;
    avel::LinearPhaseEngine linear;
    juce::CriticalSection engineLock;
    std::atomic<uint64_t> wantedHash { 0 };
    std::atomic<bool> linearMode { false };
    std::atomic<int> firMode { 0 };
    bool lastLinear = false;

    // A/B
    int abSlot = 0;
    juce::ValueTree abStates[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AvelProcessor)
};
