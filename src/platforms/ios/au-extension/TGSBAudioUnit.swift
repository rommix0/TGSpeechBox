/*
 * TGSBAudioUnit.swift — AVSpeechSynthesisProviderAudioUnit subclass.
 *
 * Registers TGSpeechBox as a system-wide speech synthesizer,
 * usable by VoiceOver, Spoken Content, and any AVSpeechSynthesizer client.
 *
 * Both macOS and iOS run the full pipeline (eSpeak + frontend + DSP)
 * in-process. Apple hosts AU extensions in a separate process from
 * VoiceOver on both platforms, so crash isolation is built in.
 *
 * License: GPL-3.0 (links eSpeak-ng)
 */

import AVFoundation
import Accelerate
import CoreMedia

public class TGSBAudioUnit: AVSpeechSynthesisProviderAudioUnit {

    private var engine: OpaquePointer?    // TgsbEngine* (full pipeline)

    // Audio buffer — written by synthesizeSpeechRequest (synchronous),
    // read by render block. Protected by outputMutex.
    private var output: [Float32] = []
    private var outputOffset: Int = 0
    private var volume: Float32 = 1.0
    private var outputMutex = DispatchSemaphore(value: 1)

    // ASBD output rate — always 22050.
    // Lower rates (11025/16000) alias on the iPhone DAC; 44100 clicks.
    // 22050 is the sweet spot. DSP rate is resampled to match.
    private let sampleRate: Double = 22050.0

    // DSP rate — the rate speechPlayer actually runs at.
    // Can differ from sampleRate; output is resampled to match ASBD.
    private var dspRate: Int

    // Cached state to avoid redundant bridge calls & UserDefaults reads
    private var cachedVoice: String = ""
    private var cachedEspeakLang: String = ""
    private var cachedTgsbLang: String = ""
    private var cachedSettingsVersion: Int = -1
    private var requestCount: Int = 0

    // Reusable synthesis buffers to avoid per-utterance allocation
    private var pullChunk = [Int16](repeating: 0, count: 4096)

    // Audio Unit output bus.
    private let outputBus: AUAudioUnitBus
    private var _outputBusses: AUAudioUnitBusArray!
    private let outputFormat: AVAudioFormat

    // Language mapping: BCP-47 tag -> (espeakTag, tgsbTag)
    private static let languageMap: [(bcp47: String, espeak: String, tgsb: String)] = [
        ("en-US", "en-us", "en-us"),
        ("en-GB", "en-gb", "en-gb"),
        ("en-CA", "en-us", "en-ca"),
        ("en-AU", "en",    "en-au"),
        ("fr-FR", "fr",    "fr"),
        ("fr-CA", "fr",    "fr"),
        ("es-ES", "es",    "es"),
        ("es-MX", "es-419","es-mx"),
        ("it-IT", "it",    "it"),
        ("pt-BR", "pt-br", "pt-br"),
        ("pt-PT", "pt",    "pt"),
        ("ro-RO", "ro",    "ro"),
        ("de-DE", "de",    "de"),
        ("nl-NL", "nl",    "nl"),
        ("da-DK", "da",    "da"),
        ("sv-SE", "sv",    "sv"),
        ("pl-PL", "pl",    "pl"),
        ("cs-CZ", "cs",    "cs"),
        ("sk-SK", "sk",    "sk"),
        ("bg-BG", "bg",    "bg"),
        ("hr-HR", "hr",    "hr"),
        ("ru-RU", "ru",    "ru"),
        ("uk-UA", "uk",    "uk"),
        ("hu-HU", "hu",    "hu"),
        ("fi-FI", "fi",    "fi"),
        ("tr-TR", "tr",    "tr"),
        ("zh-CN", "cmn",   "zh"),
    ]

    // MARK: - Initialization

    private static func loadDspRate() -> Int {
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        // Force cross-process sync so AU extension sees host app's writes.
        // Required for App Group containers after VoiceOver restart.
        d?.synchronize()
        let valid: Set<Int> = [11025, 16000, 22050, 44100]
        if let d = d, d.object(forKey: "adv_sampleRate") != nil {
            let saved = d.integer(forKey: "adv_sampleRate")
            if valid.contains(saved) { return saved }
        }
        return 22050
    }

    @objc
    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {

        dspRate = Self.loadDspRate()

        let asbd = AudioStreamBasicDescription(
            mSampleRate: 22050,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked
                        | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 32,
            mReserved: 0)

        outputFormat = AVAudioFormat(
            cmAudioFormatDescription: try CMAudioFormatDescription(
                audioStreamBasicDescription: asbd))
        outputBus = try AUAudioUnitBus(format: outputFormat)

        try super.init(componentDescription: componentDescription,
                       options: options)

        _outputBusses = AUAudioUnitBusArray(
            audioUnit: self,
            busType: .output,
            busses: [outputBus])

        initializeBackend()
    }

    private func initializeBackend() {
        guard let bundle = Bundle(for: TGSBAudioUnit.self).resourcePath else { return }
        let fm = FileManager.default

        let espeakDataPath = bundle + "/espeak-ng-data"
        let packDir = bundle + "/packs"
        guard fm.fileExists(atPath: espeakDataPath),
              fm.fileExists(atPath: packDir) else { return }
        engine = tgsb_create(espeakDataPath, packDir, Int32(dspRate))
    }

    deinit {
        if let e = engine { tgsb_destroy(e) }
    }

    // MARK: - Voices

    /// Discover all available voices: DSP presets + YAML voice profiles.
    /// Profile names are queried dynamically from the engine so that
    /// user-defined profiles in phonemes.yaml appear automatically.
    private func discoverVoices() -> [(name: String, isProfile: Bool)] {
        var result: [(String, Bool)] = []

        // DSP presets (compiled-in)
        let numPresets = tgsb_get_num_voices()
        for i in 0..<numPresets {
            if let p = tgsb_get_voice_name(Int32(i)) {
                result.append((String(cString: p).capitalized, false))
            }
        }

        // YAML voice profiles (from phonemes.yaml)
        if let eng = engine,
           let namesPtr = tgsb_get_voice_profile_names(eng) {
            let names = String(cString: namesPtr)
            free(namesPtr)
            for name in names.split(separator: "\n") where !name.isEmpty {
                let n = String(name)
                // Skip if already in presets (shouldn't happen, but defensive)
                if !result.contains(where: { $0.0.lowercased() == n.lowercased() }) {
                    result.append((n, true))
                }
            }
        }

        return result
    }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            let voiceDefs = discoverVoices()
            var voices: [AVSpeechSynthesisProviderVoice] = []

            for vd in voiceDefs {
                for lang in Self.languageMap {
                    let voice = AVSpeechSynthesisProviderVoice(
                        name: "\(vd.0) (\(lang.bcp47))",
                        identifier: "com.tgspeechbox.\(vd.0.lowercased()).\(lang.bcp47.lowercased())",
                        primaryLanguages: [lang.bcp47],
                        supportedLanguages: [lang.bcp47]
                    )
                    // Infer gender from profile (could add metadata later)
                    voice.gender = .male
                    voices.append(voice)
                }
            }

            return voices
        }
        set { }
    }

    // MARK: - Audio Unit Bus

    public override var outputBusses: AUAudioUnitBusArray {
        return _outputBusses
    }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
    }

    // MARK: - Synthesis

    public override func synthesizeSpeechRequest(
        _ speechRequest: AVSpeechSynthesisProviderRequest
    ) {
        // Extract voice name and language from identifier
        let parts = speechRequest.voice.identifier.split(separator: ".")
        let voiceName = parts.count >= 3 ? String(parts[2]) : "adam"
        let bcp47 = parts.count >= 4 ? String(parts[3]) : "en-us"

        requestCount += 1
        var plainText = extractPlainText(from: speechRequest.ssmlRepresentation)
        if plainText.isEmpty {
            if requestCount == 1 {
                // First request with empty text — likely a voice preview
                // from VoiceOver Settings. Speak a demo so the user can
                // hear the voice. Subsequent empty requests (hints, spacers)
                // get a silent frame instead.
                plainText = "Hello, this is \(voiceName.capitalized)."
            } else {
                // Empty text during normal use — provide a single silent
                // frame so the render block can signal completion and
                // VoiceOver proceeds to the next utterance (e.g. hint).
                outputMutex.wait()
                output = [0]
                outputOffset = 0
                outputMutex.signal()
                return
            }
        }

        // Force cross-process sync so AU extension sees host app's latest writes.
        // Without this, VoiceOver restart can leave the extension with stale/empty
        // UserDefaults, causing all settings to revert to factory defaults.
        UserDefaults(suiteName: "group.com.tgspeechbox.app")?.synchronize()

        let curVersion = UserDefaults(suiteName: "group.com.tgspeechbox.app")?
            .integer(forKey: "adv_settingsVersion") ?? 0
        let voiceChanged = voiceName != cachedVoice

        guard let eng = engine else {
            // No engine — still need to signal render block to complete
            // so VoiceOver doesn't hang waiting for audio.
            outputMutex.wait()
            output = [0]   // single silent frame
            outputOffset = 0
            outputMutex.signal()
            return
        }

        let langEntry = Self.languageMap.first {
            $0.bcp47.lowercased() == bcp47.lowercased()
        }
        let espeakLang = langEntry?.espeak ?? "en-us"
        let tgsbLang = langEntry?.tgsb ?? "en-us"

        let ssml = speechRequest.ssmlRepresentation
        let speed = extractRate(from: ssml)
        let pitch = extractPitch(from: ssml)

        // Volume: SSML prosody if present, multiplied by shared app setting
        var vol = Float32(extractVolume(from: ssml))
        let savedVol = UserDefaults(suiteName: "group.com.tgspeechbox.app")?.double(forKey: "systemVolume") ?? 0.0
        if savedVol > 0.0 {
            vol *= Float32(savedVol)
        }

        // Set voice and language identity FIRST — tgsb_set_voice resets
        // voicing tone and tgsb_set_language reloads the pack (resetting
        // pitch mode). Engine settings must be applied AFTER these.
        if voiceName != cachedVoice {
            // Check if this voice name is a YAML profile by querying
            // available profile names from the engine.
            var isProfile = false
            if let namesPtr = tgsb_get_voice_profile_names(eng) {
                let names = String(cString: namesPtr)
                free(namesPtr)
                let profileNames = names.split(separator: "\n").map {
                    String($0).lowercased()
                }
                isProfile = profileNames.contains(voiceName.lowercased())
            }

            if isProfile {
                tgsb_set_voice(eng, "adam")
                tgsb_set_voice_profile(eng, voiceName.capitalized(with: nil))
            } else {
                tgsb_set_voice(eng, voiceName)
            }
            cachedVoice = voiceName
        }
        let languageChanged = espeakLang != cachedEspeakLang || tgsbLang != cachedTgsbLang
        if languageChanged {
            tgsb_set_language(eng, espeakLang, tgsbLang)
            cachedEspeakLang = espeakLang
            cachedTgsbLang = tgsbLang
        }

        // Re-apply engine settings after voice/language identity is set.
        // Language change reloads the pack which resets pitch mode, so
        // settings must also be re-applied after a language change.
        if curVersion != cachedSettingsVersion || voiceChanged || languageChanged {
            let newRate = Self.loadDspRate()
            if newRate != dspRate {
                tgsb_set_sample_rate(eng, Int32(newRate))
                dspRate = newRate
            }
            applyEngineSettings(eng, voice: voiceName)
            cachedSettingsVersion = curVersion
        }
        applyStoredOverrides(tgsbLang)

        tgsb_queue_text(eng, plainText, speed, pitch)

        // Pull PCM and convert Int16 → Float32 using vDSP
        let curDspRate = dspRate
        var samples: [Float32] = []
        samples.reserveCapacity(curDspRate * 2)

        while true {
            let n = Int(tgsb_pull_audio(eng, &pullChunk, Int32(pullChunk.count)))
            if n <= 0 { break }
            let startIdx = samples.count
            samples.append(contentsOf: repeatElement(Float32(0), count: n))
            pullChunk.withUnsafeBufferPointer { srcBuf in
                samples.withUnsafeMutableBufferPointer { dstBuf in
                    vDSP_vflt16(
                        srcBuf.baseAddress!, 1,
                        dstBuf.baseAddress! + startIdx, 1,
                        vDSP_Length(n))
                    var scale = Float32(1.0 / 32768.0)
                    vDSP_vsmul(
                        dstBuf.baseAddress! + startIdx, 1,
                        &scale,
                        dstBuf.baseAddress! + startIdx, 1,
                        vDSP_Length(n))
                }
            }
        }

        // Resample from DSP rate to ASBD rate (22050) if needed
        let asbdRate = Int(sampleRate)
        if curDspRate != asbdRate && !samples.isEmpty {
            samples = resample(samples, from: curDspRate, to: asbdRate)
        }

        // Hand complete buffer to the render block
        outputMutex.wait()
        output = samples
        outputOffset = 0
        volume = vol
        outputMutex.signal()
    }

    public override func cancelSpeechRequest() {
        if let e = engine { tgsb_stop(e) }

        outputMutex.wait()
        output.removeAll()
        outputOffset = 0
        outputMutex.signal()
    }

    // MARK: - Audio Render

    public override var internalRenderBlock: AUInternalRenderBlock {
        // Matches eSpeak-NG-mobile's render pattern:
        // blocking semaphore, mDataByteSize = actual sample count,
        // Complete when buffer drained.
        return {
            actionFlags, timestamp, frameCount, outputBusNumber,
            outputAudioBufferList, _, _ in

            let outFrames = UnsafeMutableAudioBufferListPointer(
                outputAudioBufferList)[0].mData!
                .assumingMemoryBound(to: Float32.self)
            let frames = Int(frameCount)

            outFrames.assign(repeating: 0, count: frames)

            self.outputMutex.wait()

            let count = min(self.output.count - self.outputOffset, frames)
            if count > 0 {
                var vol = self.volume
                self.output.withUnsafeBufferPointer { buf in
                    vDSP_vsmul(buf.baseAddress! + self.outputOffset, 1,
                               &vol,
                               outFrames, 1,
                               vDSP_Length(count))
                }
                self.outputOffset += count
            }

            outputAudioBufferList.pointee.mBuffers.mDataByteSize =
                UInt32(count * MemoryLayout<Float32>.size)

            if self.outputOffset >= self.output.count {
                actionFlags.pointee = .offlineUnitRenderAction_Complete
                self.output.removeAll()
                self.outputOffset = 0
            }

            self.outputMutex.signal()
            return noErr
        }
    }

    // MARK: - Engine Settings from AppGroup

    private func applyEngineSettings(_ eng: OpaquePointer, voice: String) {
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")

        // Per-voice key with fallback to old global key for migration
        func load(_ key: String, _ dflt: Double) -> Double {
            guard let d = d else { return dflt }
            let voiceKey = "adv_\(key).\(voice)"
            if d.object(forKey: voiceKey) != nil {
                return d.double(forKey: voiceKey)
            }
            let globalKey = "adv_\(key)"
            if d.object(forKey: globalKey) != nil {
                return d.double(forKey: globalKey)
            }
            return dflt
        }

        // VoicingTone: convert 0–100 sliders to engine parameters
        let voiceTilt      = load("voiceTilt", 50)
        let speedQuotient  = load("speedQuotient", 50)
        let aspirationTilt = load("aspirationTilt", 50)
        let cascadeBwScale = load("cascadeBwScale", 50)
        let noiseGlottalMod = load("noiseGlottalMod", 0)
        let pitchSyncF1    = load("pitchSyncF1", 50)
        let pitchSyncB1    = load("pitchSyncB1", 50)
        let voiceTremor    = load("voiceTremor", 0)
        let headSizeSlider = load("headSize", voice == "david" ? 100 : 50)

        let tilt     = (voiceTilt - 50.0) * (24.0 / 50.0)
        let noiseMod = noiseGlottalMod / 100.0
        let psF1     = (pitchSyncF1 - 50.0) * 1.2
        let psB1     = (pitchSyncB1 - 50.0) * 1.0
        let sq       = speedQuotient <= 50.0
            ? 0.5 + (speedQuotient / 50.0) * 1.5
            : 2.0 + ((speedQuotient - 50.0) / 50.0) * 2.0
        let aspTilt  = (aspirationTilt - 50.0) * 0.24
        let bw       = cascadeBwScale <= 50.0
            ? 2.0 - (cascadeBwScale / 50.0) * 1.0
            : 1.0 - ((cascadeBwScale - 50.0) / 50.0) * 0.7
        let tremor   = (voiceTremor / 100.0) * 0.4
        let hs       = headSizeSlider <= 50.0
            ? 1.25 - (headSizeSlider / 50.0) * 0.25
            : 1.0 - ((headSizeSlider - 50.0) / 50.0) * 0.15

        tgsb_set_voicing_tone(eng, tilt, noiseMod, psF1, psB1,
                              sq, aspTilt, bw, tremor,
                              1.0, hs, 1.0)

        // FrameEx: convert 0–100 sliders to engine parameters
        let creak    = load("creakiness", 0) / 100.0
        let breath   = load("breathiness", 0) / 100.0
        let jit      = load("jitter", 0) / 100.0
        let shim     = load("shimmer", 0) / 100.0
        let sharp    = load("glottalSharpness", 50) / 50.0

        tgsb_set_frame_ex_defaults(eng, creak, breath, jit, shim, sharp)

        // Pitch mode (per-voice with global fallback)
        let mode = d?.string(forKey: "adv_pitchMode.\(voice)")
            ?? d?.string(forKey: "adv_pitchMode")
            ?? "espeak_style"
        tgsb_set_pitch_mode(eng, mode)

        let inflScale = load("inflectionScale", 58) / 100.0
        tgsb_set_legacy_pitch_inflection_scale(eng, inflScale)

        let infl = load("inflection", 50) / 100.0
        tgsb_set_inflection(eng, infl)

        // Pause mode stays global (not voice-specific)
        let pauseMode = d?.object(forKey: "adv_pauseMode") != nil
            ? d!.integer(forKey: "adv_pauseMode") : 1  // default: short
        tgsb_set_pause_mode(eng, Int32(pauseMode))
    }

    // MARK: - Pack setting overrides from AppGroup

    private func applyStoredOverrides(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        guard let json = d?.string(forKey: "pack_overrides_\(tgsbLang)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String],
              !obj.isEmpty
        else { return }
        let yaml = obj.map { "\($0.key): \($0.value)" }.joined(separator: "\n")
        _ = tgsb_apply_setting_overrides(eng, yaml)
    }

    // MARK: - Resampling

    /// Linear-interpolation resample from one rate to another.
    private func resample(_ input: [Float32], from srcRate: Int, to dstRate: Int) -> [Float32] {
        guard let srcFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                         sampleRate: Double(srcRate),
                                         channels: 1, interleaved: false),
              let dstFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                         sampleRate: Double(dstRate),
                                         channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: srcFmt, to: dstFmt)
        else { return input }

        let frameCount = AVAudioFrameCount(input.count)
        guard let srcBuf = AVAudioPCMBuffer(pcmFormat: srcFmt,
                                             frameCapacity: frameCount)
        else { return input }
        srcBuf.frameLength = frameCount
        memcpy(srcBuf.floatChannelData![0], input,
               input.count * MemoryLayout<Float32>.size)

        let ratio = Double(dstRate) / Double(srcRate)
        // Extra capacity for sinc filter tail flushed on endOfStream
        let outFrames = AVAudioFrameCount(ceil(Double(input.count) * ratio)) + 256
        guard let dstBuf = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                             frameCapacity: outFrames)
        else { return input }

        var error: NSError?
        var consumed = false
        converter.convert(to: dstBuf, error: &error) { _, outStatus in
            if consumed {
                outStatus.pointee = .endOfStream
                return nil
            }
            consumed = true
            outStatus.pointee = .haveData
            return srcBuf
        }
        if error != nil { return input }

        let count = Int(dstBuf.frameLength)
        return Array(UnsafeBufferPointer(start: dstBuf.floatChannelData![0],
                                         count: count))
    }

    // MARK: - Helpers

    private func extractPlainText(from ssml: String) -> String {
        var text = ssml.replacingOccurrences(of: "<[^>]+>", with: " ",
                                              options: .regularExpression)
        text = text.replacingOccurrences(of: "&apos;", with: "'")
        text = text.replacingOccurrences(of: "&quot;", with: "\"")
        text = text.replacingOccurrences(of: "&amp;",  with: "&")
        text = text.replacingOccurrences(of: "&lt;",   with: "<")
        text = text.replacingOccurrences(of: "&gt;",   with: ">")
        text = text.replacingOccurrences(of: "&#39;",  with: "'")
        text = text.replacingOccurrences(of: "&#34;",  with: "\"")
        text = text.replacingOccurrences(of: "\\s+", with: " ",
                                          options: .regularExpression)
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func extractRate(from ssml: String) -> Double {
        guard let match = ssml.range(
            of: #"<prosody[^>]*\brate="([^"]+)""#,
            options: .regularExpression
        ) else { return 1.0 }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"rate="([^"]+)""#, options: .regularExpression
        ) else { return 1.0 }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "rate=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "x-slow":  return 0.3
        case "slow":    return 0.6
        case "medium":  return 1.0
        case "fast":    return 2.0
        case "x-fast":  return 3.5
        default: break
        }

        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) { return max(0.1, pct / 100.0) }
        }
        if let num = Double(val) { return max(0.1, num) }
        return 1.0
    }

    private func extractPitch(from ssml: String) -> Double {
        let defaultPitch = 110.0
        guard let match = ssml.range(
            of: #"<prosody[^>]*\bpitch="([^"]+)""#,
            options: .regularExpression
        ) else { return defaultPitch }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"pitch="([^"]+)""#, options: .regularExpression
        ) else { return defaultPitch }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "pitch=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "x-low":  return 70.0
        case "low":    return 90.0
        case "medium": return 120.0
        case "high":   return 160.0
        case "x-high": return 200.0
        default: break
        }

        if val.lowercased().hasSuffix("hz") {
            val = String(val.dropLast(2))
            if let hz = Double(val) { return max(40.0, min(hz, 500.0)) }
        }
        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) {
                return max(40.0, min(defaultPitch * (1.0 + pct / 100.0), 500.0))
            }
        }
        if let num = Double(val) { return max(40.0, min(num, 500.0)) }
        return defaultPitch
    }

    private func extractVolume(from ssml: String) -> Double {
        guard let match = ssml.range(
            of: #"<prosody[^>]*\bvolume="([^"]+)""#,
            options: .regularExpression
        ) else { return 1.0 }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"volume="([^"]+)""#, options: .regularExpression
        ) else { return 1.0 }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "volume=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "silent": return 0.0
        case "x-soft": return 0.25
        case "soft":   return 0.5
        case "medium": return 1.0
        case "loud":   return 1.5
        case "x-loud": return 2.0
        default: break
        }

        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) { return max(0.0, min(pct / 100.0, 2.0)) }
        }
        if let num = Double(val) { return max(0.0, min(num, 2.0)) }
        return 1.0
    }
}
