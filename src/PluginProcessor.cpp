#include "PluginProcessor.h"

//==============================================================================
// Synth Sound: siempre válido
struct SineSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override      { return true; }
    bool appliesToChannel (int) override   { return true; }
};

//==============================================================================
// Synth Voice: oscilador seno simple + ADSR
struct SineVoice : public juce::SynthesiserVoice
{
    void setParameters (juce::AudioProcessorValueTreeState& apvtsRef)
    {
        apvts = &apvtsRef;
        gainParam   = apvts->getRawParameterValue ("gain");
        attackParam = apvts->getRawParameterValue ("attack");
        decayParam  = apvts->getRawParameterValue ("decay");
        sustainParam= apvts->getRawParameterValue ("sustain");
        releaseParam= apvts->getRawParameterValue ("release");
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<SineSound*> (s) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int) override
    {
        currentAngle = 0.0;
        level = juce::jlimit (0.0f, 1.0f, velocity);

        auto freq = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        angleDelta = (freq * juce::MathConstants<double>::twoPi) / getSampleRate();

        updateADSR();
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            adsr.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
    {
        if (angleDelta == 0.0 || apvts == nullptr)
            return;

        updateADSR();

        auto* g = gainParam;
        const float masterGain = (g != nullptr ? g->load() : 0.8f);

        while (numSamples-- > 0)
        {
            const float env = adsr.getNextSample();

            // seno
            float sample = (float) std::sin (currentAngle);
            currentAngle += angleDelta;

            sample *= (level * env * masterGain);

            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addSample (ch, startSample, sample);

            ++startSample;

            if (! adsr.isActive())
            {
                clearCurrentNote();
                angleDelta = 0.0;
                break;
            }
        }
    }

private:
    void updateADSR()
    {
        if (apvts == nullptr) return;

        juce::ADSR::Parameters p;
        p.attack  = (attackParam  ? attackParam->load()  : 0.01f);
        p.decay   = (decayParam   ? decayParam->load()   : 0.10f);
        p.sustain = (sustainParam ? sustainParam->load() : 0.80f);
        p.release = (releaseParam ? releaseParam->load() : 0.20f);
        adsr.setParameters (p);
    }

    juce::AudioProcessorValueTreeState* apvts = nullptr;

    std::atomic<float>* gainParam    = nullptr;
    std::atomic<float>* attackParam  = nullptr;
    std::atomic<float>* decayParam   = nullptr;
    std::atomic<float>* sustainParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;

    juce::ADSR adsr;
    double currentAngle = 0.0;
    double angleDelta   = 0.0;
    float level         = 0.0f;
};

//==============================================================================

juce::AudioProcessorValueTreeState::ParameterLayout
BasicInstrumentAudioProcessor::createParameterLayout()
{
    using P = juce::AudioParameterFloat;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<P>(
        "gain", "Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        0.8f
    ));

    params.push_back (std::make_unique<P>(
        "attack", "Attack",
        juce::NormalisableRange<float> (0.001f, 5.0f, 0.001f, 0.5f),
        0.01f
    ));

    params.push_back (std::make_unique<P>(
        "decay", "Decay",
        juce::NormalisableRange<float> (0.001f, 5.0f, 0.001f, 0.5f),
        0.10f
    ));

    params.push_back (std::make_unique<P>(
        "sustain", "Sustain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        0.80f
    ));

    params.push_back (std::make_unique<P>(
        "release", "Release",
        juce::NormalisableRange<float> (0.001f, 10.0f, 0.001f, 0.5f),
        0.20f
    ));

    return { params.begin(), params.end() };
}

//==============================================================================

BasicInstrumentAudioProcessor::BasicInstrumentAudioProcessor()
: AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
, apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    // Voices
    constexpr int numVoices = 8;
    for (int i = 0; i < numVoices; ++i)
    {
        auto* v = new SineVoice();
        v->setParameters (apvts);
        synth.addVoice (v);
    }

    synth.addSound (new SineSound());
}

const juce::String BasicInstrumentAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool BasicInstrumentAudioProcessor::acceptsMidi() const   { return true; }
bool BasicInstrumentAudioProcessor::producesMidi() const  { return false; }
bool BasicInstrumentAudioProcessor::isMidiEffect() const  { return false; }
double BasicInstrumentAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int BasicInstrumentAudioProcessor::getNumPrograms() { return 1; }
int BasicInstrumentAudioProcessor::getCurrentProgram() { return 0; }
void BasicInstrumentAudioProcessor::setCurrentProgram (int) {}
const juce::String BasicInstrumentAudioProcessor::getProgramName (int) { return {}; }
void BasicInstrumentAudioProcessor::changeProgramName (int, const juce::String&) {}

bool BasicInstrumentAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* BasicInstrumentAudioProcessor::createEditor()
{
    // Editor genérico: te crea sliders automáticamente para APVTS
    return new juce::GenericAudioProcessorEditor (*this);
}

void BasicInstrumentAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void BasicInstrumentAudioProcessor::releaseResources() {}

bool BasicInstrumentAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Solo stereo o mono (pero salida preferida stereo)
    const auto& out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void BasicInstrumentAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear();
    synth.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());
}

void BasicInstrumentAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BasicInstrumentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr)
        if (xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// This creates new instances of the plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BasicInstrumentAudioProcessor();
}
