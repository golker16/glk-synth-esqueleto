#include "PluginProcessor.h"

#include <cmath>
#include <vector>
#include <cstring>
#include <memory>

//==============================================================================
// Synth Sound (dummy)
struct SineSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override      { return true; }
    bool appliesToChannel (int) override   { return true; }
};

//==============================================================================
// Helpers (static, only inside this TU)
namespace
{
    // ------------------------------
    // Little-endian readers
    static inline bool canRead (const juce::uint8* ptr, size_t size, size_t offset, size_t bytes)
    {
        return (offset + bytes) <= size && ptr != nullptr;
    }

    static inline juce::uint16 readLEU16 (const juce::uint8* ptr, size_t size, size_t& off)
    {
        if (! canRead (ptr, size, off, 2))
            return 0;

        const auto lo = (juce::uint16) ptr[off + 0];
        const auto hi = (juce::uint16) ptr[off + 1];
        off += 2;
        return (juce::uint16) (lo | (hi << 8));
    }

    static inline juce::int16 readLEI16 (const juce::uint8* ptr, size_t size, size_t& off)
    {
        const auto u = readLEU16 (ptr, size, off);
        return (juce::int16) u;
    }

    // ------------------------------
    // JUCE var JSON navigation helpers
    static juce::String varToString (const juce::var& v)
    {
        if (v.isString())
            return v.toString();
        return {};
    }

    static juce::var getProp (const juce::var& objVar, const juce::Identifier& key)
    {
        if (auto* obj = objVar.getDynamicObject())
            return obj->getProperty (key);
        return {};
    }

    static const juce::Array<juce::var>* getArray (const juce::var& v)
    {
        return v.getArray();
    }

    static juce::var arrayAt (const juce::var& arrVar, int index)
    {
        if (auto* arr = getArray (arrVar))
            if (juce::isPositiveAndBelow (index, arr->size()))
                return arr->getReference (index);
        return {};
    }

    // ------------------------------
    // Band edges helper (must match exporter)
    static std::vector<int> linearBandEdges (int loBin, int hiBin, int bands)
    {
        bands = juce::jmax (1, bands);
        loBin = juce::jmax (0, loBin);
        hiBin = juce::jmax (loBin, hiBin);

        const int total = hiBin - loBin;
        std::vector<int> edges;
        edges.reserve ((size_t) bands + 1);

        edges.push_back (loBin);
        for (int i = 1; i < bands; ++i)
        {
            // floor(lo + i * total / bands)
            const double t = (double) i / (double) bands;
            const int edge = loBin + (int) std::floor (t * (double) total);
            edges.push_back (edge);
        }
        edges.push_back (hiBin);
        return edges;
    }

    // ------------------------------
    // Minimum-phase reconstruction from magnitude spectrum (real signal)
    // Implements the standard real-cepstrum method (same concept as the Python reference).
    static bool minimumPhaseFromMagRfft (const std::vector<float>& magRfft, int N, std::vector<float>& outTime)
    {
        if (N <= 0)
            return false;

        const int nBins = (N / 2) + 1;
        if ((int) magRfft.size() != nBins)
            return false;

        const float eps = 1.0e-12f;
        const int order = (int) std::round (std::log2 ((double) N));
        if ((1 << order) != N)
            return false;

        juce::dsp::FFT fft (order);
        using Complex = juce::dsp::Complex<float>;

        // Build full even log-magnitude spectrum (length N)
        std::vector<Complex> X ((size_t) N);
        for (int k = 0; k < N; ++k)
        {
            const int rk = (k <= N / 2) ? k : (N - k);
            const float m = juce::jmax (magRfft[(size_t) rk], eps);
            X[(size_t) k] = Complex (std::log (m), 0.0f);
        }

        // Real cepstrum: c = IFFT(log|X|)
        fft.perform (X.data(), X.data(), true);
        const float invN = 1.0f / (float) N;
        for (int n = 0; n < N; ++n)
            X[(size_t) n] *= invN;

        // Minimum-phase cepstrum shaping
        for (int n = 1; n < N; ++n)
        {
            if (n < N / 2)
                X[(size_t) n] *= 2.0f;
            else if (n > N / 2)
                X[(size_t) n] = Complex (0.0f, 0.0f);
        }

        // Back to frequency domain: L = FFT(c_min)
        fft.perform (X.data(), X.data(), false);

        // Exponentiate: H = exp(L)
        for (int k = 0; k < N; ++k)
        {
            const float a = X[(size_t) k].real();
            const float b = X[(size_t) k].imag();
            const float ea = std::exp (a);
            X[(size_t) k] = Complex (ea * std::cos (b), ea * std::sin (b));
        }

        // IFFT to get time-domain minimum-phase frame
        fft.perform (X.data(), X.data(), true);
        for (int n = 0; n < N; ++n)
            X[(size_t) n] *= invN;

        outTime.resize ((size_t) N);
        for (int n = 0; n < N; ++n)
            outTime[(size_t) n] = X[(size_t) n].real();

        return true;
    }

    // ------------------------------
    // Parse a wtgen-1 JSON and return a constructed wavetable
    static bool buildWavetableFromWtgenJson (const juce::String& jsonText,
                                             const juce::String& nameHint,
                                             BasicInstrumentAudioProcessor::Wavetable::Ptr& outWt,
                                             juce::String& err)
    {
        outWt = nullptr;

        juce::var root;
        const auto parseRes = juce::JSON::parse (jsonText, root);
        if (parseRes.failed())
        {
            err = "JSON parse failed: " + parseRes.getErrorMessage();
            return false;
        }

        const auto schema = varToString (getProp (root, "schema"));
        if (schema != "wtgen-1")
        {
            err = "Invalid schema (expected wtgen-1)";
            return false;
        }

        const auto program = getProp (root, "program");
        const auto nodes = getProp (program, "nodes");
        const auto node0 = arrayAt (nodes, 0);

        const auto op = varToString (getProp (node0, "op"));
        if (op != "spectralData")
        {
            err = "Unsupported program.nodes[0].op (expected spectralData)";
            return false;
        }

        const auto p = getProp (node0, "p");
        const auto codec = varToString (getProp (p, "codec"));
        if (codec != "harm-noise-framepack-v1")
        {
            err = "Unsupported codec (expected harm-noise-framepack-v1)";
            return false;
        }

        const auto dataB64 = varToString (getProp (p, "data"));
        if (dataB64.isEmpty())
        {
            err = "Missing program.nodes[0].p.data";
            return false;
        }

        // Optional banding info (needed to spread noise bands)
        int loBin = 0;
        int hiBin = 0;
        {
            const auto noise = getProp (p, "noise");
            const auto banding = getProp (noise, "banding");
            loBin = (int) getProp (banding, "loBin");
            hiBin = (int) getProp (banding, "hiBin");
        }

        // Base64 decode into MemoryBlock
        juce::MemoryOutputStream mo;
        if (! juce::Base64::convertFromBase64 (mo, dataB64))
        {
            err = "Base64 decode failed";
            return false;
        }

        const auto mb = mo.getMemoryBlock();
        const auto* bytes = (const juce::uint8*) mb.getData();
        const auto size = (size_t) mb.getSize();

        // Decode header
        size_t off = 0;
        static constexpr juce::uint8 magic[7] = { 'H','N','F','P','v','1','\0' };
        if (! canRead (bytes, size, off, sizeof (magic)))
        {
            err = "Corrupt data (too small)";
            return false;
        }

        if (std::memcmp (bytes + off, magic, sizeof (magic)) != 0)
        {
            err = "Invalid magic (expected HNFPv1\\0)";
            return false;
        }
        off += sizeof (magic);

        const int tableSize = (int) readLEU16 (bytes, size, off);
        const int F = (int) readLEU16 (bytes, size, off);
        const int H = (int) readLEU16 (bytes, size, off);
        const int B = (int) readLEU16 (bytes, size, off);

        if (tableSize <= 0 || F <= 0)
        {
            err = "Invalid header (tableSize/frames)";
            return false;
        }
        if (H < 0 || B < 0)
        {
            err = "Invalid header (H/B)";
            return false;
        }
        if (tableSize != (1 << (int) std::round (std::log2 ((double) tableSize))))
        {
            err = "tableSize must be power-of-two (for FFT)";
            return false;
        }

        const int N = tableSize;
        const int nBins = (N / 2) + 1;

        if (hiBin <= 0)
            hiBin = nBins - 1;
        if (loBin <= 0)
            loBin = juce::jlimit (0, nBins - 1, H + 1);

        const auto edges = linearBandEdges (loBin, hiBin, juce::jmax (1, B));

        // Validate size expectation (best-effort)
        const size_t perFrameBytes = (size_t) H * 2 + (size_t) B * 2 + 3 * 2;
        const size_t expectedMin = sizeof (magic) + 4 * 2 + (size_t) F * perFrameBytes;
        if (size < expectedMin)
        {
            err = "Corrupt data (truncated framepack)";
            return false;
        }

        auto wt = BasicInstrumentAudioProcessor::Wavetable::Ptr (new BasicInstrumentAudioProcessor::Wavetable());
        wt->tableSize = tableSize;
        wt->frames = F;
        wt->name = nameHint.isNotEmpty() ? nameHint : "Wavetable";
        wt->table.setSize (F, tableSize);
        wt->table.clear();

        std::vector<float> mag ((size_t) nBins, 0.0f);
        std::vector<float> time;

        for (int f = 0; f < F; ++f)
        {
            std::fill (mag.begin(), mag.end(), 0.0f);

            // Harmonics (u16)
            for (int h = 0; h < H; ++h)
            {
                const auto q = (float) readLEU16 (bytes, size, off);
                const float harmAmpScaled = q / 4096.0f;                 // (mag * (2/N))
                const float binMag = harmAmpScaled * ((float) N * 0.5f); // back to rfft magnitude
                const int bin = 1 + h;
                if (bin >= 0 && bin < nBins)
                    mag[(size_t) bin] = binMag;
            }

            // Noise bands (i16 dB*2)
            for (int b = 0; b < B; ++b)
            {
                const auto qdb = (float) readLEI16 (bytes, size, off);
                const float db = qdb * 0.5f;
                const float rmsScaled = std::pow (10.0f, db / 20.0f);
                const float binMag = rmsScaled * ((float) N * 0.5f);

                const int a = edges[(size_t) b];
                const int c = edges[(size_t) b + 1];
                for (int k = a; k < c && k < nBins; ++k)
                    mag[(size_t) k] = binMag;
            }

            // 3*u16 tilt params (ignored for now)
            (void) readLEU16 (bytes, size, off);
            (void) readLEU16 (bytes, size, off);
            (void) readLEU16 (bytes, size, off);

            // Safety: DC and Nyquist to zero
            if (! mag.empty()) mag[0] = 0.0f;
            if (nBins > 1) mag[(size_t) (nBins - 1)] = 0.0f;

            if (! minimumPhaseFromMagRfft (mag, N, time))
            {
                err = "Minimum-phase reconstruction failed";
                return false;
            }

            auto* dst = wt->table.getWritePointer (f);
            for (int i = 0; i < N; ++i)
                dst[i] = time[(size_t) i];
        }

        // DC remove per frame
        for (int f = 0; f < F; ++f)
        {
            auto* dst = wt->table.getWritePointer (f);
            double sum = 0.0;
            for (int i = 0; i < N; ++i)
                sum += dst[i];
            const float mean = (float) (sum / (double) N);
            for (int i = 0; i < N; ++i)
                dst[i] -= mean;
        }

        // Normalize global peak
        float peak = 0.0f;
        for (int f = 0; f < F; ++f)
            peak = juce::jmax (peak, wt->table.getMagnitude (f, 0, N));

        if (peak > 0.0f)
            wt->table.applyGain (0.999f / peak);

        outWt = wt;
        return true;
    }
}

//==============================================================================
// Synth Voice: 4-osc wavetable
struct WavetableVoice : public juce::SynthesiserVoice
{
    void setParameters (juce::AudioProcessorValueTreeState& apvtsRef,
                        BasicInstrumentAudioProcessor& procRef)
    {
        apvts = &apvtsRef;
        proc = &procRef;

        gainParam    = apvts->getRawParameterValue ("gain");
        attackParam  = apvts->getRawParameterValue ("attack");
        decayParam   = apvts->getRawParameterValue ("decay");
        sustainParam = apvts->getRawParameterValue ("sustain");
        releaseParam = apvts->getRawParameterValue ("release");

        morphParam = apvts->getRawParameterValue ("wt_morph");
        oscLevelParam[0] = apvts->getRawParameterValue ("osc1_level");
        oscLevelParam[1] = apvts->getRawParameterValue ("osc2_level");
        oscLevelParam[2] = apvts->getRawParameterValue ("osc3_level");
        oscLevelParam[3] = apvts->getRawParameterValue ("osc4_level");
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<SineSound*> (s) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int) override
    {
        level = juce::jlimit (0.0f, 1.0f, velocity);

        const auto freq = (float) juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        const float sr = (float) getSampleRate();
        const float delta = (sr > 0.0f ? (freq / sr) : 0.0f); // cycles/sample

        for (int i = 0; i < 4; ++i)
        {
            phase[i] = 0.0f;
            phaseDelta[i] = delta;
        }

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
            for (int i = 0; i < 4; ++i)
                phaseDelta[i] = 0.0f;
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int numSamples) override
    {
        if (apvts == nullptr || proc == nullptr)
            return;

        if (phaseDelta[0] == 0.0f)
            return;

        updateADSR();

        const float masterGain = (gainParam ? gainParam->load() : 0.8f);
        const float morph = (morphParam ? juce::jlimit (0.0f, 1.0f, morphParam->load()) : 0.0f);

        float oscLevels[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; ++i)
            if (oscLevelParam[i] != nullptr)
                oscLevels[i] = juce::jlimit (0.0f, 1.0f, oscLevelParam[i]->load());

        // Copy wavetables ONCE per block (no per-sample locks)
        std::array<BasicInstrumentAudioProcessor::Wavetable::Ptr, 4> wts;
        proc->getWtSlotsSnapshot (wts);

        while (numSamples-- > 0)
        {
            const float env = adsr.getNextSample();

            float mix = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                const float lvl = oscLevels[k];
                if (lvl <= 0.0001f)
                {
                    phase[k] = phaseWrap (phase[k] + phaseDelta[k]);
                    continue;
                }

                float s = 0.0f;
                if (wts[(size_t) k] != nullptr && wts[(size_t) k]->tableSize > 0 && wts[(size_t) k]->frames > 0)
                    s = sampleWavetable (*wts[(size_t) k], phase[k], morph);
                else
                    s = std::sin (phase[k] * juce::MathConstants<float>::twoPi);

                mix += s * lvl;
                phase[k] = phaseWrap (phase[k] + phaseDelta[k]);
            }

            const float s = mix * (level * env * masterGain);

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, startSample, s);

            ++startSample;

            if (! adsr.isActive())
            {
                clearCurrentNote();
                for (int i = 0; i < 4; ++i)
                    phaseDelta[i] = 0.0f;
                break;
            }
        }
    }

private:
    static inline float phaseWrap (float x)
    {
        x -= std::floor (x);
        return x;
    }

    static float sampleWavetable (const BasicInstrumentAudioProcessor::Wavetable& wt,
                                  float phase01,
                                  float morph)
    {
        const int N = wt.tableSize;
        const int F = wt.frames;
        if (N <= 1 || F <= 0)
            return 0.0f;

        const float framePos = morph * (float) (F - 1);
        const int a = (int) std::floor (framePos);
        const int b = juce::jmin (a + 1, F - 1);
        const float tf = framePos - (float) a;

        const float idx = phase01 * (float) N;
        const int i0 = (int) std::floor (idx) % N;
        const int i1 = (i0 + 1) % N;
        const float sf = idx - (float) std::floor (idx);

        const auto* pa = wt.table.getReadPointer (a);
        const auto* pb = wt.table.getReadPointer (b);

        const float sa = juce::jmap (sf, pa[i0], pa[i1]);
        const float sb = juce::jmap (sf, pb[i0], pb[i1]);
        return juce::jmap (tf, sa, sb);
    }

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

    BasicInstrumentAudioProcessor* proc = nullptr;
    juce::AudioProcessorValueTreeState* apvts = nullptr;

    std::atomic<float>* gainParam    = nullptr;
    std::atomic<float>* attackParam  = nullptr;
    std::atomic<float>* decayParam   = nullptr;
    std::atomic<float>* sustainParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;

    std::atomic<float>* morphParam = nullptr;
    std::atomic<float>* oscLevelParam[4] = { nullptr, nullptr, nullptr, nullptr };

    juce::ADSR adsr;

    float phase[4]      = { 0, 0, 0, 0 };
    float phaseDelta[4] = { 0, 0, 0, 0 };
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

    // New wavetable controls
    params.push_back (std::make_unique<P>(
        "wt_morph", "WT Morph",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));

    params.push_back (std::make_unique<P>(
        "osc1_level", "Osc1 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        1.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc2_level", "Osc2 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc3_level", "Osc3 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc4_level", "Osc4 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
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
        auto* v = new WavetableVoice();
        v->setParameters (apvts, *this);
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
// Wavetable slots API
bool BasicInstrumentAudioProcessor::loadWtgenSlot (int slot, const juce::File& file, juce::String& err)
{
    err.clear();

    if (! juce::isPositiveAndBelow (slot, 4))
    {
        err = "Invalid slot";
        return false;
    }

    if (! file.existsAsFile())
    {
        err = "File does not exist";
        return false;
    }

    const auto jsonText = file.loadFileAsString();
    if (jsonText.isEmpty())
    {
        err = "Failed to read file";
        return false;
    }

    Wavetable::Ptr wt;
    if (! buildWavetableFromWtgenJson (jsonText, file.getFileNameWithoutExtension(), wt, err))
        return false;

    {
        const juce::SpinLock::ScopedLockType sl (wtLock);
        wtSlots[(size_t) slot]    = wt;
        wtSlotJson[(size_t) slot] = jsonText;
        wtSlotName[(size_t) slot] = file.getFileName();
    }

    return true;
}

BasicInstrumentAudioProcessor::Wavetable::Ptr BasicInstrumentAudioProcessor::getWtSlot (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, 4))
        return nullptr;

    const juce::SpinLock::ScopedLockType sl (wtLock);
    return wtSlots[(size_t) slot];
}

void BasicInstrumentAudioProcessor::getWtSlotsSnapshot (std::array<Wavetable::Ptr, 4>& outSlots) const
{
    const juce::SpinLock::ScopedLockType sl (wtLock);
    outSlots = wtSlots;
}

juce::String BasicInstrumentAudioProcessor::getWtSlotName (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, 4))
        return {};

    const juce::SpinLock::ScopedLockType sl (wtLock);
    return wtSlotName[(size_t) slot];
}

juce::String BasicInstrumentAudioProcessor::getWtSlotJson (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, 4))
        return {};

    const juce::SpinLock::ScopedLockType sl (wtLock);
    return wtSlotJson[(size_t) slot];
}

//==============================================================================
// State
void BasicInstrumentAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Store WT JSON per slot for portability
    for (int i = 0; i < 4; ++i)
    {
        const auto key = juce::String ("wt_slot") + juce::String (i + 1) + "_json";
        state.setProperty (key, getWtSlotJson (i), nullptr);

        const auto keyName = juce::String ("wt_slot") + juce::String (i + 1) + "_name";
        state.setProperty (keyName, getWtSlotName (i), nullptr);
    }

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BasicInstrumentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    if (! xmlState->hasTagName (apvts.state.getType()))
        return;

    auto vt = juce::ValueTree::fromXml (*xmlState);
    apvts.replaceState (vt);

    // Restore wavetable slots from embedded JSON (best-effort)
    for (int i = 0; i < 4; ++i)
    {
        const auto key = juce::String ("wt_slot") + juce::String (i + 1) + "_json";
        const auto json = vt.getProperty (key).toString();

        if (json.isNotEmpty())
        {
            juce::String err;
            Wavetable::Ptr wt;

            const auto keyName = juce::String ("wt_slot") + juce::String (i + 1) + "_name";
            const auto nameHint = vt.getProperty (keyName).toString();

            if (buildWavetableFromWtgenJson (json, nameHint, wt, err))
            {
                const juce::SpinLock::ScopedLockType sl (wtLock);
                wtSlots[(size_t) i]    = wt;
                wtSlotJson[(size_t) i] = json;
                wtSlotName[(size_t) i] = nameHint;
            }
        }
    }
}

//==============================================================================
// UI: LookAndFeel + fuente global + knobs básicos
namespace ui
{
    struct BasicLNF : public juce::LookAndFeel_V4
    {
        BasicLNF()
        {
            setColour (juce::Slider::rotarySliderFillColourId,    juce::Colours::white.withAlpha (0.85f));
            setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::white.withAlpha (0.20f));
            setColour (juce::Slider::thumbColourId,               juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::textColourId,                 juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::outlineColourId,              juce::Colours::transparentBlack);

            typeface = juce::Typeface::createSystemTypefaceFor (BinaryData::mi_fuente_ttf,
                                                               BinaryData::mi_fuente_ttfSize);
        }

        juce::Font font (float height, int styleFlags = juce::Font::plain) const
        {
            juce::Font f (juce::FontOptions (height));
            if (typeface != nullptr)
                f = juce::Font (typeface).withHeight (height);

            f.setStyleFlags (styleFlags);
            return f;
        }

        juce::Typeface::Ptr getTypefaceForFont (const juce::Font& /*font*/) override
        {
            if (typeface != nullptr)
                return typeface;

            return juce::LookAndFeel_V4::getTypefaceForFont (juce::Font (juce::FontOptions (12.0f)));
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                               float sliderPosProportional,
                               float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider& slider) override
        {
            auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
            auto r = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            auto cx = bounds.getCentreX();
            auto cy = bounds.getCentreY();

            const float lineW = juce::jmax (2.0f, r * 0.12f);
            const float arcR  = r - lineW * 0.5f;

            auto ang = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (bounds);

            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawEllipse (bounds, 1.0f);

            juce::Path bgArc, fgArc;
            bgArc.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
            fgArc.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, ang, true);

            g.setColour (findColour (juce::Slider::rotarySliderOutlineColourId));
            g.strokePath (bgArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour (findColour (juce::Slider::rotarySliderFillColourId));
            g.strokePath (fgArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            juce::Point<float> p1 (cx, cy);
            juce::Point<float> p2 (cx + std::cos (ang) * (arcR * 0.85f),
                                   cy + std::sin (ang) * (arcR * 0.85f));

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawLine ({ p1, p2 }, juce::jmax (2.0f, lineW * 0.45f));

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
            slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setPopupDisplayEnabled (true, true, this);
            slider.setVelocityBasedMode (false);
            slider.setMouseDragSensitivity (160);
            slider.setScrollWheelEnabled (true);

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
// Editor (dentro del mismo .cpp)
class BasicInstrumentAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit BasicInstrumentAudioProcessorEditor (BasicInstrumentAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p),
      knobGain    (p.apvts, "gain",    "GAIN"),
      knobAttack  (p.apvts, "attack",  "ATTACK"),
      knobDecay   (p.apvts, "decay",   "DECAY"),
      knobSustain (p.apvts, "sustain", "SUSTAIN"),
      knobRelease (p.apvts, "release", "RELEASE"),
      knobMorph   (p.apvts, "wt_morph", "MORPH"),
      knobOsc1    (p.apvts, "osc1_level", "OSC1"),
      knobOsc2    (p.apvts, "osc2_level", "OSC2"),
      knobOsc3    (p.apvts, "osc3_level", "OSC3"),
      knobOsc4    (p.apvts, "osc4_level", "OSC4")
    {
        setLookAndFeel (&lnf);

        title.setFont (lnf.font (18.0f, juce::Font::bold));
        title.setText ("BASIC INSTRUMENT", juce::dontSendNotification);
        title.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (title);

        auto labelFont = lnf.font (12.0f, juce::Font::bold);
        for (auto* k : { &knobGain, &knobAttack, &knobDecay, &knobSustain, &knobRelease, &knobMorph,
                         &knobOsc1, &knobOsc2, &knobOsc3, &knobOsc4 })
            k->label.setFont (labelFont);

        for (auto* k : { &knobGain, &knobAttack, &knobDecay, &knobSustain, &knobRelease, &knobMorph,
                         &knobOsc1, &knobOsc2, &knobOsc3, &knobOsc4 })
            addAndMakeVisible (*k);

        for (int i = 0; i < 4; ++i)
        {
            wtButtons[i].setButtonText ("Load WT" + juce::String (i + 1));
            wtButtons[i].onClick = [this, i] { chooseAndLoad (i); };
            addAndMakeVisible (wtButtons[i]);

            wtLabels[i].setFont (lnf.font (12.0f));
            wtLabels[i].setJustificationType (juce::Justification::centredLeft);
            wtLabels[i].setText ("(empty)", juce::dontSendNotification);
            addAndMakeVisible (wtLabels[i]);
        }

        refreshWtLabels();

        setSize (720, 300);
    }

    ~BasicInstrumentAudioProcessorEditor() override
    {
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);

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
        r.removeFromTop (8);

        // WT buttons + labels
        auto wtRow = r.removeFromTop (36);
        for (int i = 0; i < 4; ++i)
        {
            auto cell = wtRow.removeFromLeft (wtRow.getWidth() / (4 - i));
            auto btnArea = cell.removeFromTop (22);
            wtButtons[i].setBounds (btnArea.removeFromLeft (90));
            wtLabels[i].setBounds (btnArea);
        }

        r.removeFromTop (10);

        // Knobs
        const int knobW = 62;
        const int knobH = 108;

        auto row1 = r.removeFromTop (knobH);
        auto place = [&](juce::Component& c)
        {
            c.setBounds (row1.removeFromLeft (knobW).reduced (3, 0));
        };

        place (knobGain);
        place (knobAttack);
        place (knobDecay);
        place (knobSustain);
        place (knobRelease);
        place (knobMorph);
        place (knobOsc1);
        place (knobOsc2);
        place (knobOsc3);
        place (knobOsc4);
    }

private:
    void refreshWtLabels()
    {
        for (int i = 0; i < 4; ++i)
        {
            const auto name = proc.getWtSlotName (i);
            wtLabels[i].setText (name.isNotEmpty() ? name : "(empty)", juce::dontSendNotification);
        }
    }

    void chooseAndLoad (int slot)
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Load WTGEN (.wtgen.json)",
            juce::File(),
            "*.wtgen.json;*.json");

        const int chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        fileChooser->launchAsync (chooserFlags, [this, slot] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (! file.existsAsFile())
                return;

            juce::String err;
            if (! proc.loadWtgenSlot (slot, file, err))
            {
                juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                       "WT Load Error",
                                                       err);
            }
            refreshWtLabels();
        });
    }

    BasicInstrumentAudioProcessor& proc;
    ui::BasicLNF lnf;

    juce::Label title;

    ui::KnobWithLabel knobGain;
    ui::KnobWithLabel knobAttack;
    ui::KnobWithLabel knobDecay;
    ui::KnobWithLabel knobSustain;
    ui::KnobWithLabel knobRelease;
    ui::KnobWithLabel knobMorph;
    ui::KnobWithLabel knobOsc1;
    ui::KnobWithLabel knobOsc2;
    ui::KnobWithLabel knobOsc3;
    ui::KnobWithLabel knobOsc4;

    std::array<juce::TextButton, 4> wtButtons;
    std::array<juce::Label, 4> wtLabels;
    std::unique_ptr<juce::FileChooser> fileChooser;

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



