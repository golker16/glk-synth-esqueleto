#include "PluginProcessor.h"

//==============================================================================
// Synth Sound
struct SineSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override      { return true; }
    bool appliesToChannel (int) override   { return true; }
};

//==============================================================================
// Synth Voice
struct SineVoice : public juce::SynthesiserVoice
{
    void setParameters (juce::AudioProcessorValueTreeState& apvtsRef)
    {
        apvts = &apvtsRef;
        gainParam    = apvts->getRawParameterValue ("gain");
        attackParam  = apvts->getRawParameterValue ("attack");
        decayParam   = apvts->getRawParameterValue ("decay");
        sustainParam = apvts->getRawParameterValue ("sustain");
        releaseParam = apvts->getRawParameterValue ("release");
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
        if (allowTailOff) adsr.noteOff();
        else
        {
            adsr.reset();
            clearCurrentNote();
            angleDelta = 0.0;
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int numSamples) override
    {
        if (angleDelta == 0.0 || apvts == nullptr)
            return;

        updateADSR();

        const float masterGain = (gainParam ? gainParam->load() : 0.8f);

        while (numSamples-- > 0)
        {
            const float env = adsr.getNextSample();

            float s = (float) std::sin (currentAngle);
            currentAngle += angleDelta;

            s *= (level * env * masterGain);

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, startSample, s);

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
// Parameters
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
// Processor
BasicInstrumentAudioProcessor::BasicInstrumentAudioProcessor()
: juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
, apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    constexpr int numVoices = 8;
    for (int i = 0; i < numVoices; ++i)
    {
        auto* v = new SineVoice();
        v->setParameters (apvts);
        synth.addVoice (v);
    }
    synth.addSound (new SineSound());
}

const juce::String BasicInstrumentAudioProcessor::getName() const { return JucePlugin_Name; }
bool BasicInstrumentAudioProcessor::acceptsMidi() const   { return true; }
bool BasicInstrumentAudioProcessor::producesMidi() const  { return false; }
bool BasicInstrumentAudioProcessor::isMidiEffect() const  { return false; }
double BasicInstrumentAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int BasicInstrumentAudioProcessor::getNumPrograms() { return 1; }
int BasicInstrumentAudioProcessor::getCurrentProgram() { return 0; }
void BasicInstrumentAudioProcessor::setCurrentProgram (int) {}
const juce::String BasicInstrumentAudioProcessor::getProgramName (int) { return {}; }
void BasicInstrumentAudioProcessor::changeProgramName (int, const juce::String&) {}

void BasicInstrumentAudioProcessor::prepareToPlay (double sampleRate, int)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void BasicInstrumentAudioProcessor::releaseResources() {}

bool BasicInstrumentAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void BasicInstrumentAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());
}

//==============================================================================
// State
void BasicInstrumentAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BasicInstrumentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// UI: LookAndFeel + fuente global + knobs básicos
namespace ui
{
    struct BasicLNF : public juce::LookAndFeel_V4
    {
        BasicLNF()
        {
            // Colores base (sin “complicarse”, sobrio)
            setColour (juce::Slider::rotarySliderFillColourId,   juce::Colours::white.withAlpha (0.85f));
            setColour (juce::Slider::rotarySliderOutlineColourId,juce::Colours::white.withAlpha (0.20f));
            setColour (juce::Slider::thumbColourId,              juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::textColourId,                juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::outlineColourId,             juce::Colours::transparentBlack);

            // Cargar typeface desde BinaryData (embebido por CMake)
            // OJO: el nombre en BinaryData usa el nombre del archivo con '_' por caracteres raros.
            // Con "mi_fuente.ttf" normalmente queda "mi_fuente_ttf".
            typeface = juce::Typeface::createSystemTypefaceFor (BinaryData::mi_fuente_ttf,
                                                               BinaryData::mi_fuente_ttfSize);
        }

        // Helper para forzar el uso de la fuente embebida incluso en texto dibujado "a mano"
        // (por ejemplo, el valor dentro del knob), donde un juce::Font creado directamente
        // puede saltarse el override de getTypefaceForFont.
        juce::Font font (float height, int styleFlags = juce::Font::plain) const
        {
            juce::Font f;
            if (typeface != nullptr)
                f = juce::Font (typeface);

            f.setHeight (height);
            f.setStyleFlags (styleFlags);
            return f;
        }

        juce::Typeface::Ptr getTypefaceForFont (const juce::Font& /*font*/) override
        {
            // “Todo lo escrito” usa esta fuente
            if (typeface != nullptr)
                return typeface;

            return juce::LookAndFeel_V4::getTypefaceForFont (juce::Font());
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                               float sliderPosProportional,
                               float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider& slider) override
        {
            // Un poco menos de padding para que el knob se sienta más "preciso" y compacto.
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
            auto r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            auto cx = bounds.getCentreX();
            auto cy = bounds.getCentreY();

            const float lineW = juce::jmax (2.0f, r * 0.12f);
            const float arcR  = r - lineW * 0.5f;

            auto ang = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

            // Fondo knob (circulo)
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (bounds);

            // Borde
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawEllipse (bounds, 1.0f);

            // Arco “track”
            juce::Path bgArc, fgArc;
            bgArc.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
            fgArc.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, ang, true);

            g.setColour (findColour (juce::Slider::rotarySliderOutlineColourId));
            g.strokePath (bgArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (fgArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // Indicador (línea)
            juce::Point<float> p1 (cx, cy);
            juce::Point<float> p2 (cx + std::cos (ang) * (arcR * 0.85f),
                                   cy + std::sin (ang) * (arcR * 0.85f));

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawLine ({ p1, p2 }, juce::jmax (2.0f, lineW * 0.45f));

            // Texto valor (más cerca del indicador: dentro del knob, en la mitad inferior)
            if (slider.isEnabled())
            {
                g.setColour (juce::Colours::white.withAlpha (0.80f));
                auto valueText = slider.getTextFromValue (slider.getValue());

                auto valueArea = bounds.toNearestInt();
                valueArea = valueArea.withTrimmedTop (valueArea.getHeight() / 2 - 4)
                                     .reduced (10, 6);

                g.setFont (font (12.0f));
                g.drawFittedText (valueText, valueArea, juce::Justification::centred, 1);
            }
        }

        juce::Typeface::Ptr typeface;
    };

    struct KnobWithLabel : public juce::Component
    {
        KnobWithLabel (juce::AudioProcessorValueTreeState& apvts,
                       const juce::String& paramId,
                       const juce::String& labelText)
        : attachment (apvts, paramId, slider)
        {
            // Movilidad: el modo "velocity" suele sentirse raro en knobs.
            // Preferimos drag vertical (más predecible) + sensibilidad controlada.
            slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setPopupDisplayEnabled (true, true, this);
            slider.setVelocityBasedMode (false);
            slider.setMouseDragSensitivity (160);
            slider.setScrollWheelEnabled (true);

            // Arco típico de knob (de ~7:30 a ~4:30). StopAtEnd = true evita “saltos”.
            slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                       juce::MathConstants<float>::pi * 2.75f,
                                       true);

            label.setText (labelText, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setInterceptsMouseClicks (false, false);

            addAndMakeVisible (slider);
            addAndMakeVisible (label);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            label.setBounds (r.removeFromBottom (18));
            slider.setBounds (r.reduced (2));
        }

        juce::Slider slider;
        juce::Label  label;
        juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    };
}

//==============================================================================
// Editor (dentro del mismo .cpp para no crear más archivos)
class BasicInstrumentAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit BasicInstrumentAudioProcessorEditor (BasicInstrumentAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p),
      knobGain    (p.apvts, "gain",    "GAIN"),
      knobAttack  (p.apvts, "attack",  "ATTACK"),
      knobDecay   (p.apvts, "decay",   "DECAY"),
      knobSustain (p.apvts, "sustain", "SUSTAIN"),
      knobRelease (p.apvts, "release", "RELEASE")
    {
        setLookAndFeel (&lnf);

        // Asegurar tipografía en TODO el texto (títulos/labels)
        title.setFont (lnf.font (18.0f, juce::Font::bold));

        title.setText ("BASIC INSTRUMENT", juce::dontSendNotification);
        title.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (title);

        auto labelFont = lnf.font (12.0f, juce::Font::bold);
        knobGain.label.setFont    (labelFont);
        knobAttack.label.setFont  (labelFont);
        knobDecay.label.setFont   (labelFont);
        knobSustain.label.setFont (labelFont);
        knobRelease.label.setFont (labelFont);

        addAndMakeVisible (knobGain);
        addAndMakeVisible (knobAttack);
        addAndMakeVisible (knobDecay);
        addAndMakeVisible (knobSustain);
        addAndMakeVisible (knobRelease);

        // Tamaño ajustado (los knobs antes quedaban "2x" grandes)
        setSize (400, 200);
    }

    ~BasicInstrumentAudioProcessorEditor() override
    {
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);

        // panel básico
        auto b = getLocalBounds().toFloat().reduced (12.0f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (b, 14.0f);

        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawRoundedRectangle (b, 14.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);

        title.setBounds (r.removeFromTop (28));

        r.removeFromTop (6);

        auto knobsArea = r;
        const int knobW = 64;
        const int knobH = 112;

        // layout simple en fila
        auto row = knobsArea.removeFromTop (knobH);
        auto place = [&](juce::Component& c)
        {
            c.setBounds (row.removeFromLeft (knobW).reduced (3, 0));
        };

        place (knobGain);
        place (knobAttack);
        place (knobDecay);
        place (knobSustain);
        place (knobRelease);
    }

private:
    BasicInstrumentAudioProcessor& proc;

    ui::BasicLNF lnf;

    juce::Label title;

    ui::KnobWithLabel knobGain;
    ui::KnobWithLabel knobAttack;
    ui::KnobWithLabel knobDecay;
    ui::KnobWithLabel knobSustain;
    ui::KnobWithLabel knobRelease;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicInstrumentAudioProcessorEditor)
};

//==============================================================================
// Editor hooks
bool BasicInstrumentAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* BasicInstrumentAudioProcessor::createEditor()
{
    return new BasicInstrumentAudioProcessorEditor (*this);
}

//==============================================================================
// Factory
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BasicInstrumentAudioProcessor();
}

