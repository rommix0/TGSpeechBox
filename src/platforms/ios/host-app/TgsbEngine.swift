/*
 * TgsbEngine.swift — Swift wrapper around the C bridge.
 *
 * Manages the TGSpeechBox pipeline lifecycle and audio playback
 * via AVAudioPlayer (output-only, no microphone permission needed).
 */

import AVFoundation

/// Supported languages with their eSpeak and TGSB language tags.
struct TgsbLanguage: Identifiable, Hashable {
    let id: String          // display key, e.g. "en-us"
    let displayName: String
    let espeakTag: String
    let tgsbTag: String
}

let kLanguages: [TgsbLanguage] = [
    TgsbLanguage(id: "en-us", displayName: "English (US)",      espeakTag: "en-us", tgsbTag: "en-us"),
    TgsbLanguage(id: "en-gb", displayName: "English (GB)",      espeakTag: "en-gb", tgsbTag: "en-gb"),
    TgsbLanguage(id: "en-au", displayName: "English (AU)",      espeakTag: "en",    tgsbTag: "en-au"),
    TgsbLanguage(id: "en-ca", displayName: "English (CA)",      espeakTag: "en-us", tgsbTag: "en-ca"),
    TgsbLanguage(id: "fr",    displayName: "French",            espeakTag: "fr",    tgsbTag: "fr"),
    TgsbLanguage(id: "es",    displayName: "Spanish",           espeakTag: "es",    tgsbTag: "es"),
    TgsbLanguage(id: "es-mx", displayName: "Spanish (Mexico)",  espeakTag: "es-419",tgsbTag: "es-mx"),
    TgsbLanguage(id: "it",    displayName: "Italian",           espeakTag: "it",    tgsbTag: "it"),
    TgsbLanguage(id: "pt-br", displayName: "Portuguese (BR)",   espeakTag: "pt-br", tgsbTag: "pt-br"),
    TgsbLanguage(id: "pt",    displayName: "Portuguese (PT)",   espeakTag: "pt",    tgsbTag: "pt"),
    TgsbLanguage(id: "ro",    displayName: "Romanian",          espeakTag: "ro",    tgsbTag: "ro"),
    TgsbLanguage(id: "de",    displayName: "German",            espeakTag: "de",    tgsbTag: "de"),
    TgsbLanguage(id: "nl",    displayName: "Dutch",             espeakTag: "nl",    tgsbTag: "nl"),
    TgsbLanguage(id: "da",    displayName: "Danish",            espeakTag: "da",    tgsbTag: "da"),
    TgsbLanguage(id: "sv",    displayName: "Swedish",           espeakTag: "sv",    tgsbTag: "sv"),
    TgsbLanguage(id: "pl",    displayName: "Polish",            espeakTag: "pl",    tgsbTag: "pl"),
    TgsbLanguage(id: "cs",    displayName: "Czech",             espeakTag: "cs",    tgsbTag: "cs"),
    TgsbLanguage(id: "sk",    displayName: "Slovak",            espeakTag: "sk",    tgsbTag: "sk"),
    TgsbLanguage(id: "bg",    displayName: "Bulgarian",         espeakTag: "bg",    tgsbTag: "bg"),
    TgsbLanguage(id: "hr",    displayName: "Croatian",          espeakTag: "hr",    tgsbTag: "hr"),
    TgsbLanguage(id: "ru",    displayName: "Russian",           espeakTag: "ru",    tgsbTag: "ru"),
    TgsbLanguage(id: "uk",    displayName: "Ukrainian",         espeakTag: "uk",    tgsbTag: "uk"),
    TgsbLanguage(id: "hu",    displayName: "Hungarian",         espeakTag: "hu",    tgsbTag: "hu"),
    TgsbLanguage(id: "fi",    displayName: "Finnish",           espeakTag: "fi",    tgsbTag: "fi"),
    TgsbLanguage(id: "tr",    displayName: "Turkish",           espeakTag: "tr",    tgsbTag: "tr"),
    TgsbLanguage(id: "zh",    displayName: "Chinese (Mandarin)",espeakTag: "cmn",   tgsbTag: "zh"),
]

/// Voice preset names (from C bridge + YAML voice profiles).
struct TgsbVoice: Identifiable, Hashable {
    let id: String
    let displayName: String
    let isProfile: Bool  // true = YAML voice profile (Beth, Bobby)

    init(id: String, displayName: String, isProfile: Bool = false) {
        self.id = id
        self.displayName = displayName
        self.isProfile = isProfile
    }
}

@MainActor
class TgsbEngine: ObservableObject {
    @Published var isSpeaking = false
    @Published var selectedLanguage: TgsbLanguage
    @Published var selectedVoice: TgsbVoice
    @Published var speed: Double = 1.0
    @Published var pitch: Double = 110.0
    @Published var inflectionValue: Double = 50.0  // 0–100 slider

    @Published var voices: [TgsbVoice]

    private var engine: OpaquePointer?
    private var sampleRate: Int

    private static func loadSampleRate() -> Int {
        let d = UserDefaults(suiteName: kAppGroupId)
        let valid = [11025, 16000, 22050, 44100]
        if let d = d, d.object(forKey: "adv_sampleRate") != nil {
            let saved = d.integer(forKey: "adv_sampleRate")
            if valid.contains(saved) { return saved }
        }
        return 22050
    }

    // Audio playback (AVAudioPlayer — output only, no mic permission)
    private var audioPlayer: AVAudioPlayer?
    private var synthQueue = DispatchQueue(label: "com.tgspeechbox.synth",
                                           qos: .userInitiated)

    init() {
        self.sampleRate = Self.loadSampleRate()

        // Gather voice names: DSP presets from C bridge + YAML profiles
        let numVoices = tgsb_get_num_voices()
        var v: [TgsbVoice] = []
        for i in 0..<numVoices {
            if let namePtr = tgsb_get_voice_name(Int32(i)) {
                let name = String(cString: namePtr)
                v.append(TgsbVoice(id: name,
                                   displayName: name.capitalized))
            }
        }
        // Add known YAML voice profiles as fallback (replaced by
        // dynamic discovery in start() if engine finds more).
        v.append(TgsbVoice(id: "beth", displayName: "Beth", isProfile: true))
        v.append(TgsbVoice(id: "bobby", displayName: "Bobby", isProfile: true))
        self.voices = v
        self.selectedVoice = v.first ?? TgsbVoice(id: "adam",
                                                   displayName: "Adam")
        self.selectedLanguage = kLanguages[0] // en-us
    }

    func start() -> Bool {
        guard engine == nil else { return true }

        guard let espeakDataDir = Self.resourcePath(for: "espeak-ng-data"),
              let packDir = Self.resourcePath(for: "packs") else {
            print("[TgsbEngine] Runtime data not found in bundle")
            return false
        }

        // espeak_Initialize accepts the espeak-ng-data directory directly
        print("[TgsbEngine] espeakData: \(espeakDataDir)")
        print("[TgsbEngine] packDir: \(packDir)")

        engine = tgsb_create(espeakDataDir, packDir, Int32(sampleRate))
        guard engine != nil else {
            print("[TgsbEngine] tgsb_create failed")
            return false
        }

        // Set initial language and voice
        tgsb_set_language(engine,
                          selectedLanguage.espeakTag,
                          selectedLanguage.tgsbTag)
        // Discover YAML voice profiles now that engine is available
        if let namesPtr = tgsb_get_voice_profile_names(engine!) {
            let names = String(cString: namesPtr)
            free(namesPtr)
            var updatedVoices = self.voices
            for name in names.split(separator: "\n") where !name.isEmpty {
                let n = String(name)
                if !updatedVoices.contains(where: { $0.id == n.lowercased() }) {
                    updatedVoices.append(TgsbVoice(id: n.lowercased(),
                                                    displayName: n,
                                                    isProfile: true))
                }
            }
            self.voices = updatedVoices
        }

        applySelectedVoice(engine!)

        print("[TgsbEngine] Engine ready")
        return true
    }

    /// Apply the selected voice — either a DSP preset or a YAML profile.
    private func applySelectedVoice(_ eng: OpaquePointer) {
        if selectedVoice.isProfile {
            tgsb_set_voice(eng, "adam")
            tgsb_set_voice_profile(eng, selectedVoice.displayName)
        } else {
            tgsb_set_voice(eng, selectedVoice.id)
        }
    }

    func shutdown() {
        stopSpeaking()
        if let e = engine {
            tgsb_destroy(e)
            engine = nil
        }
    }

    /// Change DSP sample rate without tearing down the whole engine.
    func changeSampleRate(_ rate: Int) {
        sampleRate = rate
        if let eng = engine {
            tgsb_set_sample_rate(eng, Int32(rate))
        }
    }

    // MARK: - Engine settings (voice quality sliders)

    /// Apply VoicingTone from 0–100 slider values.
    func applyVoicingToneFromSliders(
        voiceTilt: Double, speedQuotient sq: Double,
        aspirationTilt: Double, cascadeBwScale bw: Double,
        noiseGlottalMod: Double, pitchSyncF1: Double,
        pitchSyncB1: Double, voiceTremor: Double,
        headSize: Double
    ) {
        guard let eng = engine else { return }

        let tilt     = (voiceTilt - 50.0) * (24.0 / 50.0)
        let noiseMod = noiseGlottalMod / 100.0
        let psF1     = (pitchSyncF1 - 50.0) * 1.2
        let psB1     = (pitchSyncB1 - 50.0) * 1.0
        let sqVal    = sq <= 50.0
            ? 0.5 + (sq / 50.0) * 1.5
            : 2.0 + ((sq - 50.0) / 50.0) * 2.0
        let aspTilt  = (aspirationTilt - 50.0) * 0.24
        let bwVal    = bw <= 50.0
            ? 2.0 - (bw / 50.0) * 1.0
            : 1.0 - ((bw - 50.0) / 50.0) * 0.7
        let tremor   = (voiceTremor / 100.0) * 0.4
        let hs       = headSize <= 50.0
            ? 1.25 - (headSize / 50.0) * 0.25
            : 1.0 - ((headSize - 50.0) / 50.0) * 0.15

        tgsb_set_voicing_tone(eng, tilt, noiseMod, psF1, psB1,
                              sqVal, aspTilt, bwVal, tremor,
                              1.0, hs, 1.0)
    }

    /// Apply FrameEx defaults from 0–100 slider values.
    func applyFrameExFromSliders(
        creakiness: Double, breathiness: Double,
        jitter: Double, shimmer: Double,
        glottalSharpness: Double
    ) {
        guard let eng = engine else { return }
        tgsb_set_frame_ex_defaults(eng,
            creakiness / 100.0,
            breathiness / 100.0,
            jitter / 100.0,
            shimmer / 100.0,
            glottalSharpness / 50.0)
    }

    /// Set pitch intonation model.
    func setPitchMode(_ mode: String) {
        guard let eng = engine else { return }
        tgsb_set_pitch_mode(eng, mode)
    }

    /// Set legacy pitch inflection scale (0.0–2.0).
    func setInflectionScale(_ scale: Double) {
        guard let eng = engine else { return }
        tgsb_set_legacy_pitch_inflection_scale(eng, scale)
    }

    /// Set voice inflection / pitch range (0.0–1.0).
    func setInflection(_ value: Double) {
        guard let eng = engine else { return }
        tgsb_set_inflection(eng, value)
    }

    /// Set pause mode (0=off, 1=short, 2=long).
    func setPauseMode(_ mode: Int) {
        guard let eng = engine else { return }
        tgsb_set_pause_mode(eng, Int32(mode))
    }

    /// Read all adv_* settings from AppGroup UserDefaults and apply.
    func applyEngineSettings() {
        guard engine != nil else { return }
        let d = UserDefaults(suiteName: kAppGroupId)

        func load(_ key: String, _ dflt: Double) -> Double {
            guard let d = d, d.object(forKey: "adv_\(key)") != nil
            else { return dflt }
            return d.double(forKey: "adv_\(key)")
        }

        applyVoicingToneFromSliders(
            voiceTilt: load("voiceTilt", 50),
            speedQuotient: load("speedQuotient", 50),
            aspirationTilt: load("aspirationTilt", 50),
            cascadeBwScale: load("cascadeBwScale", 50),
            noiseGlottalMod: load("noiseGlottalMod", 0),
            pitchSyncF1: load("pitchSyncF1", 50),
            pitchSyncB1: load("pitchSyncB1", 50),
            voiceTremor: load("voiceTremor", 0),
            headSize: load("headSize", 50))

        applyFrameExFromSliders(
            creakiness: load("creakiness", 0),
            breathiness: load("breathiness", 0),
            jitter: load("jitter", 0),
            shimmer: load("shimmer", 0),
            glottalSharpness: load("glottalSharpness", 50))

        let mode = d?.string(forKey: "adv_pitchMode") ?? "espeak_style"
        setPitchMode(mode)
        setInflectionScale(load("inflectionScale", 58) / 100.0)
        setInflection(load("inflection", 50) / 100.0)

        let pauseMode = d?.object(forKey: "adv_pauseMode") != nil
            ? d!.integer(forKey: "adv_pauseMode") : 1  // default: short
        setPauseMode(pauseMode)
    }

    // MARK: - Pack settings editor

    /// Settings managed by Engine Settings sliders — hidden from editor.
    private static let hiddenKeys: Set<String> = [
        "legacyPitchMode", "legacyPitchInflectionScale"
    ]

    enum SettingType { case bool_, number, text }

    struct PackSetting: Identifiable {
        let id: String  // key
        let key: String
        let displayName: String
        let value: String
        let isOverridden: Bool
        let type: SettingType
        let options: [String]?  // dropdown options for enum-like strings
    }

    @Published var editorLanguages: [String] = []
    @Published var editorSettings: [PackSetting] = []
    private var editorLangTag: String = ""

    func loadEditorLanguages() {
        guard let eng = engine else { return }
        guard let ptr = tgsb_get_available_languages(eng) else { return }
        let raw = String(cString: ptr)
        tgsb_free_string(ptr)
        editorLanguages = raw.split(separator: "\n").map(String.init).sorted()
    }

    func loadEditorSettings(langTag: String) {
        guard let eng = engine else { return }
        editorLangTag = langTag

        // Query settings directly by langTag — no temp language switch needed.
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_SETTINGS, langTag, 0, 0) else { return }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)

        let overrides = loadOverrides(langTag)

        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return }

        var settings: [PackSetting] = []
        for obj in arr {
            guard let key = obj["key"] as? String else { continue }
            if Self.hiddenKeys.contains(key) { continue }
            let jsonType = obj["type"] as? String ?? "string"
            let baseValue: String
            if jsonType == "bool", let b = obj["value"] as? Bool {
                baseValue = b ? "true" : "false"
            } else {
                baseValue = "\(obj["value"] ?? "")"
            }
            let effectiveValue = overrides[key] ?? baseValue
            let type: SettingType = jsonType == "bool" ? .bool_ :
                                    jsonType == "float" ? .number : .text
            let options = (obj["options"] as? [String])
            settings.append(PackSetting(
                id: key, key: key,
                displayName: camelToDisplay(key),
                value: effectiveValue,
                isOverridden: overrides[key] != nil,
                type: type,
                options: options))
        }
        editorSettings = settings

        // Apply overrides to the active language if it matches.
        if !overrides.isEmpty {
            for (k, v) in overrides {
                tgsb_set_data(eng, TGSB_DATA_SETTINGS, langTag, k, v)
            }
        }
    }

    func setEditorOverride(langTag: String, key: String, value: String) {
        let baseValues = getBaseValues(langTag)
        var overrides = loadOverrides(langTag)
        if baseValues[key] == value {
            overrides.removeValue(forKey: key)
        } else {
            overrides[key] = value
        }
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    func removeEditorOverride(langTag: String, key: String) {
        var overrides = loadOverrides(langTag)
        overrides.removeValue(forKey: key)
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    func resetAllEditorOverrides(langTag: String) {
        let d = UserDefaults(suiteName: kAppGroupId)
        d?.removeObject(forKey: "pack_overrides_\(langTag)")
        d?.synchronize()
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    func applyStoredOverrides(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let overrides = loadOverrides(tgsbLang)
        if overrides.isEmpty { return }
        let yaml = overrides.map { "\($0.key): \($0.value)" }.joined(separator: "\n")
        tgsb_apply_setting_overrides(eng, yaml)
    }

    private func reloadCurrentLanguage() {
        guard let eng = engine else { return }
        tgsb_set_language(eng, selectedLanguage.espeakTag, selectedLanguage.tgsbTag)
        applyStoredOverrides(selectedLanguage.tgsbTag)
    }

    private func getBaseValues(_ langTag: String) -> [String: String] {
        guard let eng = engine else { return [:] }
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_SETTINGS, langTag, 0, 0) else { return [:] }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)
        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return [:] }
        var map: [String: String] = [:]
        for obj in arr {
            guard let key = obj["key"] as? String else { continue }
            let jsonType = obj["type"] as? String ?? "string"
            if jsonType == "bool", let b = obj["value"] as? Bool {
                map[key] = b ? "true" : "false"
            } else {
                map[key] = "\(obj["value"] ?? "")"
            }
        }
        return map
    }

    private func loadOverrides(_ langTag: String) -> [String: String] {
        let d = UserDefaults(suiteName: kAppGroupId)
        guard let json = d?.string(forKey: "pack_overrides_\(langTag)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return [:] }
        return obj
    }

    private func saveOverrides(_ langTag: String, _ overrides: [String: String]) {
        let d = UserDefaults(suiteName: kAppGroupId)
        if overrides.isEmpty {
            d?.removeObject(forKey: "pack_overrides_\(langTag)")
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "pack_overrides_\(langTag)")
        }
        d?.synchronize()
    }

    private func detectType(_ value: String) -> SettingType {
        if value == "true" || value == "false" { return .bool_ }
        if Double(value) != nil { return .number }
        return .text
    }

    private func camelToDisplay(_ key: String) -> String {
        let dotReplaced = key.replacingOccurrences(of: ".", with: ": ")
        var result = ""
        for c in dotReplaced {
            if c.isUppercase && !result.isEmpty && result.last?.isLowercase == true {
                result.append(" ")
            }
            result.append(c)
        }
        return result.prefix(1).uppercased() + result.dropFirst()
    }

    func speak(_ text: String) {
        guard let eng = engine else { return }
        stopSpeaking()

        // Apply current settings
        tgsb_set_language(eng,
                          selectedLanguage.espeakTag,
                          selectedLanguage.tgsbTag)
        applyStoredOverrides(selectedLanguage.tgsbTag)
        applySelectedVoice(eng)
        applyEngineSettings()
        tgsb_set_inflection(eng, inflectionValue / 100.0)

        isSpeaking = true

        let speed = self.speed
        let pitch = self.pitch
        let sr = self.sampleRate

        synthQueue.async { [weak self] in
            // Queue text (fast, <10ms)
            tgsb_queue_text(eng, text, speed, pitch)

            // Pull all PCM into a buffer
            let chunkSize = 4096
            var chunk = [Int16](repeating: 0, count: chunkSize)
            var allSamples = [Int16]()

            while true {
                let n = tgsb_pull_audio(eng, &chunk, Int32(chunkSize))
                if n <= 0 { break }
                allSamples.append(contentsOf: chunk.prefix(Int(n)))
            }

            guard !allSamples.isEmpty else {
                DispatchQueue.main.async { self?.isSpeaking = false }
                return
            }

            // Build WAV data in memory
            let wavData = Self.makeWAV(samples: allSamples,
                                        sampleRate: sr)

            DispatchQueue.main.async {
                self?.playWAV(wavData)
            }
        }
    }

    func stopSpeaking() {
        if let eng = engine {
            tgsb_stop(eng)
        }
        audioPlayer?.stop()
        audioPlayer = nil
        isSpeaking = false
    }

    // MARK: - WAV playback

    private func playWAV(_ data: Data) {
        do {
            let player = try AVAudioPlayer(data: data)
            player.delegate = audioDelegate
            let savedVol = UserDefaults(suiteName: kAppGroupId)?.double(forKey: "systemVolume") ?? 0.0
            if savedVol > 0.0 {
                player.volume = Float(savedVol)
            }
            player.play()
            self.audioPlayer = player
        } catch {
            print("[TgsbEngine] AVAudioPlayer error: \(error)")
            isSpeaking = false
        }
    }

    // Delegate to detect playback completion
    private lazy var audioDelegate = AudioDelegate { [weak self] in
        DispatchQueue.main.async {
            self?.isSpeaking = false
        }
    }

    // MARK: - WAV builder

    private static func makeWAV(samples: [Int16], sampleRate: Int) -> Data {
        let numSamples = samples.count
        let dataSize = numSamples * 2
        let fileSize = 36 + dataSize

        var data = Data(capacity: 44 + dataSize)

        func writeU16(_ v: UInt16) {
            var le = v.littleEndian
            data.append(Data(bytes: &le, count: 2))
        }
        func writeU32(_ v: UInt32) {
            var le = v.littleEndian
            data.append(Data(bytes: &le, count: 4))
        }
        func writeTag(_ s: String) {
            data.append(Data(s.utf8))
        }

        // RIFF header
        writeTag("RIFF")
        writeU32(UInt32(fileSize))
        writeTag("WAVE")

        // fmt  chunk
        writeTag("fmt ")
        writeU32(16)                             // chunk size
        writeU16(1)                              // PCM format
        writeU16(1)                              // mono
        writeU32(UInt32(sampleRate))
        writeU32(UInt32(sampleRate * 2))         // byte rate
        writeU16(2)                              // block align
        writeU16(16)                             // bits per sample

        // data chunk
        writeTag("data")
        writeU32(UInt32(dataSize))

        // PCM samples
        samples.withUnsafeBytes { raw in
            data.append(contentsOf: raw)
        }

        return data
    }

    // MARK: - Resource paths

    private static func resourcePath(for name: String) -> String? {
        // Check app bundle first
        if let path = Bundle.main.resourcePath {
            let full = (path as NSString).appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: full) {
                return full
            }
        }

        // Development fallback: look for data relative to the source tree.
        let fm = FileManager.default
        let sourceRoot = (#filePath as NSString)
            .deletingLastPathComponent          // TGSpeechBox/
            .appending("/..")                   // apple/
        let candidates: [String]
        switch name {
        case "espeak-ng-data":
            candidates = [
                (sourceRoot as NSString).appendingPathComponent("Resources/espeak-ng-data"),
                (sourceRoot as NSString).appendingPathComponent("../data/espeak-ng-data"),
            ]
        case "packs":
            candidates = [
                (sourceRoot as NSString).appendingPathComponent("Resources/packs"),
                (sourceRoot as NSString).appendingPathComponent("../../tgspeechbox/packs"),
            ]
        default:
            candidates = []
        }
        for path in candidates {
            let resolved = (path as NSString).standardizingPath
            if fm.fileExists(atPath: resolved) {
                return resolved
            }
        }
        return nil
    }
}

// MARK: - Helpers

private class AudioDelegate: NSObject, AVAudioPlayerDelegate {
    let onFinish: () -> Void
    init(onFinish: @escaping () -> Void) { self.onFinish = onFinish }
    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer,
                                     successfully flag: Bool) {
        onFinish()
    }
}

private extension UInt16 {
    var littleEndianBytes: [UInt8] {
        let le = self.littleEndian
        return [UInt8(le & 0xFF), UInt8(le >> 8)]
    }
}

private extension UInt32 {
    var littleEndianBytes: [UInt8] {
        let le = self.littleEndian
        return [UInt8(le & 0xFF), UInt8((le >> 8) & 0xFF),
                UInt8((le >> 16) & 0xFF), UInt8((le >> 24) & 0xFF)]
    }
}
