#pragma once
#include <JuceHeader.h>
#include <array>

class BasicInstrumentAudioProcessor : public juce::AudioProcessor
{
public:
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

    //=========================
    // APVTS
    //=========================
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //=========================
    // Wavetable slots API (4 wavetables)
    //=========================
    struct Wavetable : public juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<Wavetable>;

        int tableSize = 0;
        int frames = 0;

        // layout: [frames][tableSize]
        juce::AudioBuffer<float> table;

        juce::String name;
    };

    // Carga un archivo .wtgen.json en el slot [0..3]
    // Devuelve true si ok; si falla, err contiene el motivo.
    bool loadWtgenSlot (int slot, const juce::File& file, juce::String& err);

    // Obtiene el wavetable del slot [0..3] (o nullptr si vacío)
    Wavetable::Ptr getWtSlot (int slot) const;

private:
    juce::Synthesiser synth;

    //=========================
    // Storage + lock
    //=========================
    juce::SpinLock wtLock;
    std::array<Wavetable::Ptr, 4> wtSlots { nullptr, nullptr, nullptr, nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicInstrumentAudioProcessor)
};

