#pragma once
#include <JuceHeader.h>

#include <array>

class BasicInstrumentAudioProcessor : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Wavetable slots API
    struct Wavetable : public juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<Wavetable>;
        int tableSize = 0;
        int frames = 0;
        juce::AudioBuffer<float> table; // [frames][tableSize]
        juce::String name;
    };

    bool loadWtgenSlot (int slot, const juce::File& file, juce::String& err);
    Wavetable::Ptr getWtSlot (int slot) const;

    //==============================================================================
    BasicInstrumentAudioProcessor();
    ~BasicInstrumentAudioProcessor() override = default;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    //==============================================================================
    // Wavetable slots storage
    mutable juce::SpinLock wtLock;
    std::array<Wavetable::Ptr, 4> wtSlots {};
    std::array<juce::String, 4> wtSlotJson {};

    juce::Synthesiser synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicInstrumentAudioProcessor)
};

