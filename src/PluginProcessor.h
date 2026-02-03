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

    // Carga un WTGen / wavetable en un slot (0..3). Devuelve false y llena 'err' si falla.
    bool loadWtgenSlot (int slot, const juce::File& file, juce::String& err);

    // Obtiene el wavetable del slot (0..3) o nullptr si está vacío/índice inválido.
    Wavetable::Ptr getWtSlot (int slot) const;

    //==============================================================================
    // Métodos que tu PluginProcessor.cpp ya está llamando (según tus errores)
    std::array<Wavetable::Ptr, 4> getWtSlotsSnapshot() const;
    juce::String getWtSlotName (int index) const;
    juce::String getWtSlotJson (int index) const;

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
    // Wavetable slots storage (estos miembros son los que te faltaban)
    mutable juce::SpinLock wtLock;
    std::array<Wavetable::Ptr, 4> wtSlots {};
    std::array<juce::String, 4> wtSlotName {};
    std::array<juce::String, 4> wtSlotJson {};

    //==============================================================================
    juce::Synthesiser synth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicInstrumentAudioProcessor)
};


