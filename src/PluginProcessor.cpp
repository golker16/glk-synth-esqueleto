#include "PluginProcessor.h"

#include <array>
#include <vector>
#include <cmath>
#include <cstdint>

//==============================================================================
// Helpers (WTGEN loader + DSP)
namespace wtgen
{
    // -------- small utilities
    static inline bool isPowerOfTwo (int x) { return x > 0 && (x & (x - 1)) == 0; }

    struct Reader
    {
        const uint8_t* p = nullptr;
        const uint8_t* end = nullptr;

        Reader (const void* data, size_t size)
        : p ((const uint8_t*) data), end ((const uint8_t*) data + size) {}

        bool canRead (size_t n) const { return p + n <= end; }

        bool readBytes (void* dst, size_t n)
        {
            if (! canRead (n)) return false;
            std::memcpy (dst, p, n);
            p += n;
            return true;
        }

        bool readU16LE (uint16_t& out)
        {
            uint8_t b[2];
            if (! readBytes (b, 2)) return false;
            out = (uint16_t) (b[0] | (uint16_t) (b[1] << 8));
            return true;
        }

        bool readI16LE (int16_t& out)
        {
            uint16_t u;
            if (! readU16LE (u)) return false;
            out = (int16_t) u;
            return true;
        }
    };

    static bool getObject (const juce::var& v, juce::DynamicObject*& obj)
    {
        obj = v.getDynamicObject();
        return obj != nullptr;
    }

    static bool getString (juce::DynamicObject* obj, const juce::Identifier& key, juce::String& out)
    {
        if (obj == nullptr) return false;
        auto vv = obj->getProperty (key);
        if (! vv.isString()) return false;
        out = vv.toString();
        return true;
    }

    static bool getInt (juce::DynamicObject* obj, const juce::Identifier& key, int& out)
    {
        if (obj == nullptr) return false;
        auto vv = obj->getProperty (key);
        if (! vv.isInt() && ! vv.isDouble()) return false;
        out = (int) vv;
        return true;
    }

    static bool getFloat (juce::DynamicObject* obj, const juce::Identifier& key, float& out)
    {
        if (obj == nullptr) return false;
        auto vv = obj->getProperty (key);
        if (! vv.isInt() && ! vv.isDouble()) return false;
        out = (float) (double) vv;
        return true;
    }

    // -------- minimum-phase reconstruction from magnitude (single-cycle wavetable)
    // Uses complex FFT (cepstrum trick):
    // 1) log|X| -> IFFT -> cepstrum
    // 2) make cepstrum causal (double positive quefrency, zero negative)
    // 3) FFT -> log spectrum, exp -> min-phase spectrum
    // 4) IFFT -> time signal
    static bool minimumPhaseFromMagnitude (const std::vector<float>& magHalf, std::vector<float>& outTime)
    {
        const int N = (int) outTime.size();
        if (! isPowerOfTwo (N)) return false;
        if ((int) magHalf.size() != (N / 2 + 1)) return false;

        const int order = (int) std::round (std::log2 ((double) N));
        juce::dsp::FFT fft (order);

        using C = juce::dsp::Complex<float>;
        std::vector<C> spec (N);
        std::vector<C> tmp  (N);

        // build symmetric real spectrum with log magnitude (imag=0)
        constexpr float eps = 1.0e-12f;
        for (int k = 0; k <= N / 2; ++k)
        {
            const float m = juce::jmax (magHalf[(size_t) k], eps);
            spec[(size_t) k] = C (std::log (m), 0.0f);
        }
        for (int k = N / 2 + 1; k < N; ++k)
            spec[(size_t) k] = std::conj (spec[(size_t) (N - k)]);

        // IFFT to cepstrum
        fft.perform (spec.data(), tmp.data(), /*inverse=*/ true);
        for (int n = 0; n < N; ++n)
            tmp[(size_t) n] /= (float) N;

        // causal cepstrum: c[0] keep, c[1..N/2-1]*=2, c[N/2] keep if even, rest=0
        for (int n = 1; n < N; ++n)
        {
            if (n < N / 2)
                tmp[(size_t) n] *= 2.0f;
            else if (n > N / 2)
                tmp[(size_t) n] = C (0.0f, 0.0f);
        }

        // FFT back to get minimum-phase log spectrum
        fft.perform (tmp.data(), spec.data(), /*inverse=*/ false);

        // exp(log spectrum) -> complex spectrum
        for (int k = 0; k < N; ++k)
        {
            const float a = spec[(size_t) k].real();
            const float b = spec[(size_t) k].imag();
            const float ea = std::exp (a);
            spec[(size_t) k] = C (ea * std::cos (b), ea * std::sin (b));
        }

        // enforce exact Hermitian symmetry for real IFFT output
        spec[0] = C (spec[0].real(), 0.0f);
        spec[(size_t) (N / 2)] = C (spec[(size_t) (N / 2)].real(), 0.0f);
        for (int k = N / 2 + 1; k < N; ++k)
            spec[(size_t) k] = std::conj (spec[(size_t) (N - k)]);

        // IFFT -> time
        fft.perform (spec.data(), tmp.data(), /*inverse=*/ true);
        for (int n = 0; n < N; ++n)
            outTime[(size_t) n] = tmp[(size_t) n].real() / (float) N;

        return true;
    }

    // -------- decode HNFPv1 framepack to frames x tableSize time-domain
    struct FramepackParams
    {
        int tableSize = 0;
        int frames = 0;
        int harmonics = 0;
        int noiseBands = 0;

        // from JSON
        float ampScale = 1.0f;

        int   noiseBandsJson = 0;
        float noiseDbRange = 60.0f;
        float noiseQuantDb = 120.0f;
    };

    static void removeDCAndNormalize (juce::AudioBuffer<float>& buf)
    {
        const int ch = buf.getNumChannels();
        const int n  = buf.getNumSamples();
        if (ch <= 0 || n <= 0) return;

        // DC remove per channel
        for (int c = 0; c < ch; ++c)
        {
            auto* x = buf.getWritePointer (c);
            double mean = 0.0;
            for (int i = 0; i < n; ++i) mean += x[i];
            mean /= (double) n;
            for (int i = 0; i < n; ++i) x[i] = (float) (x[i] - mean);
        }

        // global peak
        float peak = 0.0f;
        for (int c = 0; c < ch; ++c)
            peak = juce::jmax (peak, buf.getMagnitude (c, 0, n));

        if (peak > 1.0e-6f)
            buf.applyGain (1.0f / peak);
    }

    // Very lightweight noise model:
    // - interpret noise band values as dB (approx) and spread across frequency bins (excluding DC)
    // This is an approximation (good enough to "sound" and match your format flow).
    static void addNoiseBandsToMagnitude (std::vector<float>& magHalf,
                                         const std::vector<float>& bandAmp)
    {
        const int half = (int) magHalf.size() - 1; // N/2
        const int B = (int) bandAmp.size();
        if (half <= 0 || B <= 0) return;

        const int bins = half;
        for (int b = 0; b < B; ++b)
        {
            const int start = 1 + (b * bins) / B;
            const int end   = 1 + ((b + 1) * bins) / B;

            const float a = bandAmp[(size_t) b];
            for (int k = start; k < juce::jmin (end, half + 1); ++k)
                magHalf[(size_t) k] += a;
        }
    }

    static bool decodeFramepackToWavetable (const void* bytes, size_t size,
                                           const FramepackParams& fp,
                                           juce::AudioBuffer<float>& outFrames,
                                           juce::String& err)
    {
        Reader r (bytes, size);

        // magic "HNFPv1\0" (7 bytes)
        char magic[7] {};
        if (! r.readBytes (magic, 7))
        {
            err = "Framepack: archivo truncado (sin magic).";
            return false;
        }
        if (std::memcmp (magic, "HNFPv1\0", 7) != 0)
        {
            err = "Framepack: magic invalido (no es HNFPv1).";
            return false;
        }

        uint16_t tableSizeU = 0, framesU = 0, HU = 0, BU = 0;
        if (! r.readU16LE (tableSizeU) || ! r.readU16LE (framesU) || ! r.readU16LE (HU) || ! r.readU16LE (BU))
        {
            err = "Framepack: header truncado.";
            return false;
        }

        const int N = (int) tableSizeU;
        const int F = (int) framesU;
        const int H = (int) HU;
        const int B = (int) BU;

        if (N <= 0 || F <= 0)
        {
            err = "Framepack: dimensiones invalidas.";
            return false;
        }
        if (! isPowerOfTwo (N))
        {
            err = "Framepack: tableSize debe ser potencia de 2.";
            return false;
        }

        // Validate against JSON-declared values (if provided)
        if (fp.tableSize > 0 && fp.tableSize != N) { err = "Framepack: tableSize no coincide con JSON."; return false; }
        if (fp.frames   > 0 && fp.frames   != F) { err = "Framepack: frames no coincide con JSON."; return false; }

        // Prepare output [frames][tableSize]
        outFrames.setSize (1, F * N, false, false, true);
        outFrames.clear();

        std::vector<float> magHalf ((size_t) (N / 2 + 1), 0.0f);
        std::vector<float> time    ((size_t) N, 0.0f);

        // per-frame decode
        for (int f = 0; f < F; ++f)
        {
            // harmonics: H * u16
            std::fill (magHalf.begin(), magHalf.end(), 0.0f);

            for (int h = 0; h < H; ++h)
            {
                uint16_t ampQ = 0;
                if (! r.readU16LE (ampQ))
                {
                    err = "Framepack: truncado leyendo harmonics.";
                    return false;
                }

                // reconstruct amplitude
                const float amp = ((float) ampQ / 65535.0f) * fp.ampScale;

                // map harmonic # -> FFT bin (1..)
                const int bin = 1 + h;
                if (bin <= N / 2)
                    magHalf[(size_t) bin] = amp;
            }

            // noise: B * i16
            std::vector<float> bandAmp;
            if (B > 0)
                bandAmp.resize ((size_t) B, 0.0f);

            for (int b = 0; b < B; ++b)
            {
                int16_t q = 0;
                if (! r.readI16LE (q))
                {
                    err = "Framepack: truncado leyendo noise.";
                    return false;
                }

                // approximate decode: map int16 -> [0..1]
                const float norm = ((float) q + 32768.0f) / 65535.0f;

                // interpret as dB in [-range..0]
                const float db = -fp.noiseDbRange + norm * fp.noiseDbRange;
                const float a  = std::pow (10.0f, db / 20.0f);

                bandAmp[(size_t) b] = a;
            }

            // phase lock: 3*u16 (ignored, but consumed)
            uint16_t ph0 = 0, ph1 = 0, ph2 = 0;
            if (! r.readU16LE (ph0) || ! r.readU16LE (ph1) || ! r.readU16LE (ph2))
            {
                err = "Framepack: truncado leyendo phase-lock.";
                return false;
            }

            // add noise into mag
            if (B > 0)
                addNoiseBandsToMagnitude (magHalf, bandAmp);

            // min-phase -> time
            std::fill (time.begin(), time.end(), 0.0f);
            if (! minimumPhaseFromMagnitude (magHalf, time))
            {
                err = "Framepack: fallo reconstruyendo minimum-phase.";
                return false;
            }

            // copy to output
            auto* dst = outFrames.getWritePointer (0) + f * N;
            for (int i = 0; i < N; ++i)
                dst[i] = time[(size_t) i];
        }

        // Convert to [frames][tableSize] layout in a 2D sense:
        // We'll store as a single channel with contiguous frames, but later we copy into Wavetable::table as 2D.
        return true;
    }

    // Parse WTGEN JSON and produce Wavetable (frames x tableSize)
    static bool loadWtgenJsonToWavetable (const juce::String& jsonText,
                                         BasicInstrumentAudioProcessor::Wavetable::Ptr& outWT,
                                         juce::String& err)
    {
        juce::var root = juce::JSON::parse (jsonText, err);
        if (root.isVoid() || root.isUndefined())
        {
            if (err.isEmpty()) err = "JSON invalido.";
            return false;
        }

        juce::DynamicObject* robj = nullptr;
        if (! getObject (root, robj))
        {
            err = "JSON: root no es objeto.";
            return false;
        }

        juce::String schema;
        if (! getString (robj, "schema", schema) || schema != "wtgen-1")
        {
            err = "JSON: schema no soportado (se esperaba wtgen-1).";
            return false;
        }

        // program.nodes[0]
        juce::var programV = robj->getProperty ("program");
        juce::DynamicObject* pobj = nullptr;
        if (! getObject (programV, pobj))
        {
            err = "JSON: falta program.";
            return false;
        }

        juce::var nodesV = pobj->getProperty ("nodes");
        if (! nodesV.isArray() || nodesV.getArray()->isEmpty())
        {
            err = "JSON: program.nodes vacio.";
            return false;
        }

        juce::var node0 = nodesV.getArray()->getReference (0);
        juce::DynamicObject* n0 = nullptr;
        if (! getObject (node0, n0))
        {
            err = "JSON: nodes[0] invalido.";
            return false;
        }

        juce::String op;
        if (! getString (n0, "op", op) || op != "spectralData")
        {
            err = "JSON: op no soportado (se esperaba spectralData).";
            return false;
        }

        juce::var pV = n0->getProperty ("p");
        juce::DynamicObject* pObj = nullptr;
        if (! getObject (pV, pObj))
        {
            err = "JSON: falta nodes[0].p.";
            return false;
        }

        juce::String codec;
        if (! getString (pObj, "codec", codec) || codec != "harm-noise-framepack-v1")
        {
            err = "JSON: codec no soportado (se esperaba harm-noise-framepack-v1).";
            return false;
        }

        FramepackParams fp;
        getInt (pObj, "tableSize", fp.tableSize);
        getInt (pObj, "frames", fp.frames);

        // harmonics object
        if (auto hv = pObj->getProperty ("harmonics"); hv.isObject())
        {
            juce::DynamicObject* hObj = hv.getDynamicObject();
            int hc = 0;
            float ampScale = 1.0f;
            getInt (hObj, "count", hc);
            getFloat (hObj, "ampScale", ampScale);
            fp.harmonics = hc;
            fp.ampScale  = ampScale;
        }

        // noise object
        if (auto nv = pObj->getProperty ("noise"); nv.isObject())
        {
            juce::DynamicObject* nObj = nv.getDynamicObject();
            int bands = 0;
            float dbRange = 60.0f;
            float quantDb = 120.0f;
            getInt (nObj, "bands", bands);
            getFloat (nObj, "dbRange", dbRange);
            getFloat (nObj, "quantDb", quantDb);
            fp.noiseBandsJson = bands;
            fp.noiseDbRange   = dbRange;
            fp.noiseQuantDb   = quantDb;
        }

        juce::String b64;
        if (! getString (pObj, "data", b64) || b64.isEmpty())
        {
            err = "JSON: falta nodes[0].p.data (base64).";
            return false;
        }

        // Base64 decode
        juce::MemoryBlock bin;
        if (! juce::Base64::convertFromBase64 (bin, b64))
        {
            err = "JSON: base64 invalido.";
            return false;
        }

        // Decode framepack -> single channel contiguous frames
        juce::AudioBuffer<float> contiguous;
        if (! decodeFramepackToWavetable (bin.getData(), bin.getSize(), fp, contiguous, err))
            return false;

        // Build Wavetable object
        auto wt = new BasicInstrumentAudioProcessor::Wavetable();
        wt->tableSize = fp.tableSize;
        wt->frames    = fp.frames;
        wt->table.setSize (fp.frames, fp.tableSize, false, false, true);

        // Copy into [frames][tableSize]
        const float* src = contiguous.getReadPointer (0);
        for (int f = 0; f < fp.frames; ++f)
        {
            float* dst = wt->table.getWritePointer (f);
            std::memcpy (dst, src + f * fp.tableSize, sizeof (float) * (size_t) fp.tableSize);
        }

        removeDCAndNormalize (wt->table);

        outWT = wt;
        return true;
    }
}

//==============================================================================
// Synth Sound (dummy)
struct SineSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override      { return true; }
    bool appliesToChannel (int) override   { return true; }
};

//==============================================================================
// Synth Voice (4 wavetables, 4 oscs)
struct WavetableVoice : public juce::SynthesiserVoice
{
    void setParameters (juce::AudioProcessorValueTreeState& apvtsRef,
                        BasicInstrumentAudioProcessor& procRef)
    {
        apvts = &apvtsRef;
        proc  = &procRef;

        gainParam    = apvts->getRawParameterValue ("gain");
        attackParam  = apvts->getRawParameterValue ("attack");
        decayParam   = apvts->getRawParameterValue ("decay");
        sustainParam = apvts->getRawParameterValue ("sustain");
        releaseParam = apvts->getRawParameterValue ("release");

        morphParam   = apvts->getRawParameterValue ("wt_morph");
        oscLevel[0]  = apvts->getRawParameterValue ("osc1_level");
        oscLevel[1]  = apvts->getRawParameterValue ("osc2_level");
        oscLevel[2]  = apvts->getRawParameterValue ("osc3_level");
        oscLevel[3]  = apvts->getRawParameterValue ("osc4_level");
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<SineSound*> (s) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int) override
    {
        level = juce::jlimit (0.0f, 1.0f, velocity);

        const auto freq = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        const double sr = getSampleRate();
        const double inc = freq / sr;

        for (int i = 0; i < 4; ++i)
        {
            phase[i] = 0.0;
            phaseDelta[i] = inc; // no detune (por ahora)
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
            for (auto& d : phaseDelta) d = 0.0;
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int startSample, int numSamples) override
    {
        if (apvts == nullptr || proc == nullptr)
            return;

        // Copy slot pointers once per block (NO lock per sample)
        BasicInstrumentAudioProcessor::Wavetable::Ptr wt[4];
        for (int i = 0; i < 4; ++i)
            wt[i] = proc->getWtSlot (i);

        updateADSR();

        const float masterGain = (gainParam ? gainParam->load() : 0.8f);
        const float morph      = (morphParam ? morphParam->load() : 0.0f);

        float levels[4] {};
        for (int i = 0; i < 4; ++i)
            levels[i] = (oscLevel[i] ? oscLevel[i]->load() : (i == 0 ? 1.0f : 0.0f));

        auto sampleWT = [] (const BasicInstrumentAudioProcessor::Wavetable& w, float morph01, double ph) -> float
        {
            const int N = w.tableSize;
            const int F = w.frames;
            if (N <= 0 || F <= 0) return 0.0f;

            const float framePos = juce::jlimit (0.0f, 1.0f, morph01) * (float) (F - 1);
            const int f0 = (int) std::floor (framePos);
            const int f1 = juce::jmin (f0 + 1, F - 1);
            const float tf = framePos - (float) f0;

            const double p = ph - std::floor (ph); // wrap
            const double idx = p * (double) N;
            const int i0 = (int) idx;
            const int i1 = (i0 + 1) % N;
            const float t = (float) (idx - (double) i0);

            const float a0 = w.table.getSample (f0, i0);
            const float a1 = w.table.getSample (f0, i1);
            const float b0 = w.table.getSample (f1, i0);
            const float b1 = w.table.getSample (f1, i1);

            const float sa = a0 + (a1 - a0) * t;
            const float sb = b0 + (b1 - b0) * t;

            return sa + (sb - sa) * tf;
        };

        while (numSamples-- > 0)
        {
            const float env = adsr.getNextSample();

            float mix = 0.0f;

            for (int k = 0; k < 4; ++k)
            {
                float s = 0.0f;
                if (wt[k] != nullptr)
                    s = sampleWT (*wt[k], morph, phase[k]);
                else
                    s = 0.0f; // fallback (podrías poner seno si quieres)

                mix += s * levels[k];

                phase[k] += phaseDelta[k];
                if (phase[k] >= 1.0) phase[k] -= 1.0;
            }

            const float outSamp = mix * (level * env * masterGain);

            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, startSample, outSamp);

            ++startSample;

            if (! adsr.isActive())
            {
                clearCurrentNote();
                for (auto& d : phaseDelta) d = 0.0;
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
    BasicInstrumentAudioProcessor* proc = nullptr;

    std::atomic<float>* gainParam    = nullptr;
    std::atomic<float>* attackParam  = nullptr;
    std::atomic<float>* decayParam   = nullptr;
    std::atomic<float>* sustainParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;

    std::atomic<float>* morphParam   = nullptr;
    std::atomic<float>* oscLevel[4]  = { nullptr, nullptr, nullptr, nullptr };

    juce::ADSR adsr;

    std::array<double, 4> phase { 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 4> phaseDelta { 0.0, 0.0, 0.0, 0.0 };

    float level = 0.0f;
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

    // NEW: wavetable morph (frame scan)
    params.push_back (std::make_unique<P>(
        "wt_morph", "WT Morph",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));

    // NEW: 4 osc levels (mix)
    params.push_back (std::make_unique<P>(
        "osc1_level", "OSC1 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        1.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc2_level", "OSC2 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc3_level", "OSC3 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));
    params.push_back (std::make_unique<P>(
        "osc4_level", "OSC4 Level",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f),
        0.0f
    ));

    return { params.begin(), params.end() };
}

//==============================================================================
// WT slots API
bool BasicInstrumentAudioProcessor::loadWtgenSlot (int slot, const juce::File& file, juce::String& err)
{
    err.clear();

    if (slot < 0 || slot >= 4)
    {
        err = "Slot fuera de rango (0..3).";
        return false;
    }

    if (! file.existsAsFile())
    {
        err = "Archivo no existe.";
        return false;
    }

    auto jsonText = file.loadFileAsString();
    if (jsonText.isEmpty())
    {
        err = "Archivo vacío o no se pudo leer.";
        return false;
    }

    Wavetable::Ptr wt;
    if (! wtgen::loadWtgenJsonToWavetable (jsonText, wt, err))
        return false;

    // store a friendly name (file name)
    wt->name = file.getFileName();

    {
        const juce::SpinLock::ScopedLockType lock (wtLock);
        wtSlots[(size_t) slot] = wt;
    }

    return true;
}

BasicInstrumentAudioProcessor::Wavetable::Ptr
BasicInstrumentAudioProcessor::getWtSlot (int slot) const
{
    if (slot < 0 || slot >= 4) return nullptr;
    const juce::SpinLock::ScopedLockType lock (wtLock);
    return wtSlots[(size_t) slot];
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
// State
void BasicInstrumentAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Save APVTS + (minimal) wavetable identity
    // Recomendado: guardar JSON completo. Aquí guardamos el *nombre* (y si el usuario lo carga de nuevo, se recarga).
    // Si quieres portabilidad total, dime y te lo adapto para almacenar JSON completo en el state.
    auto state = apvts.copyState();

    // store slot "names" (no paths) as metadata
    for (int i = 0; i < 4; ++i)
    {
        juce::String nm;
        if (auto wt = getWtSlot (i); wt != nullptr)
            nm = wt->name;

        state.setProperty ("wt_slot" + juce::String (i + 1) + "_name", nm, nullptr);
    }

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BasicInstrumentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
        // Nota: aquí no recargamos slots automáticamente porque no guardamos JSON/path.
        // Si decides guardar JSON completo por slot, aquí se reconstruye y listo.
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
            setColour (juce::Slider::rotarySliderFillColourId,   juce::Colours::white.withAlpha (0.85f));
            setColour (juce::Slider::rotarySliderOutlineColourId,juce::Colours::white.withAlpha (0.20f));
            setColour (juce::Slider::thumbColourId,              juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::textColourId,                juce::Colours::white.withAlpha (0.90f));
            setColour (juce::Label::outlineColourId,             juce::Colours::transparentBlack);

            typeface = juce::Typeface::createSystemTypefaceFor (BinaryData::mi_fuente_ttf,
                                                               BinaryData::mi_fuente_ttfSize);
        }

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
            if (typeface != nullptr)
                return typeface;

            return juce::LookAndFeel_V4::getTypefaceForFont (juce::Font());
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
      knobMorph   (p.apvts, "wt_morph", "MORPH")
    {
        setLookAndFeel (&lnf);

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
        knobMorph.label.setFont   (labelFont);

        addAndMakeVisible (knobGain);
        addAndMakeVisible (knobAttack);
        addAndMakeVisible (knobDecay);
        addAndMakeVisible (knobSustain);
        addAndMakeVisible (knobRelease);
        addAndMakeVisible (knobMorph);

        // --- WT load UI (4 slots)
        for (int i = 0; i < 4; ++i)
        {
            loadBtn[i].setButtonText ("LOAD WT" + juce::String (i + 1));
            loadBtn[i].setClickingTogglesState (false);
            loadBtn[i].onClick = [this, i] { onLoadWtClicked (i); };
            addAndMakeVisible (loadBtn[i]);

            wtLabel[i].setFont (lnf.font (12.0f));
            wtLabel[i].setText ("(empty)", juce::dontSendNotification);
            wtLabel[i].setJustificationType (juce::Justification::left);
            addAndMakeVisible (wtLabel[i]);
        }

        // tamaño ajustado
        setSize (540, 260);
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
        r.removeFromTop (6);

        // WT load row
        {
            auto wtArea = r.removeFromTop (78);

            for (int i = 0; i < 4; ++i)
            {
                auto row = wtArea.removeFromTop (18);
                loadBtn[i].setBounds (row.removeFromLeft (90));
                row.removeFromLeft (8);
                wtLabel[i].setBounds (row);
                wtArea.removeFromTop (2);
            }

            r.removeFromTop (8);
        }

        // knobs row
        auto knobsArea = r;
        const int knobW = 64;
        const int knobH = 112;

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
        place (knobMorph);
    }

private:
    void onLoadWtClicked (int slot)
    {
        juce::FileChooser chooser ("Select .wtgen.json wavetable",
                                  juce::File{},
                                  "*.json;*.wtgen.json");

        if (! chooser.browseForFileToOpen())
            return;

        auto file = chooser.getResult();
        juce::String err;
        if (! proc.loadWtgenSlot (slot, file, err))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                   "Load wavetable failed",
                                                   err.isEmpty() ? "Unknown error." : err);
            return;
        }

        // update label
        if (auto wt = proc.getWtSlot (slot); wt != nullptr)
            wtLabel[slot].setText (wt->name, juce::dontSendNotification);
        else
            wtLabel[slot].setText ("(empty)", juce::dontSendNotification);
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

    juce::TextButton loadBtn[4];
    juce::Label wtLabel[4];

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
