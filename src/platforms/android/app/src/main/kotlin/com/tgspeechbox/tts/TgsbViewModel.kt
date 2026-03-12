/*
 * TgsbViewModel — Shared state for the TGSpeechBox consumer UI.
 *
 * Drives the Speak & Basics tab and the Advanced tab.  Uses
 * TgsbSpeakEngine (direct JNI) for speech — NOT the TextToSpeech
 * client API, which conflicts with TalkBack using the same service.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.app.Application
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File
import java.util.Locale
import kotlin.math.roundToInt

/** One entry in the language dropdown (deduplicated). */
data class LanguageItem(
    val displayName: String,
    val langDef: TgsbTtsService.Companion.LangDef
)

class TgsbViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "TgsbUI"
        private const val PREF_PREFIX = "adv_"
        val SAMPLE_RATES = listOf(11025, 16000, 22050, 44100)
    }

    private val prefs: SharedPreferences =
        application.getSharedPreferences(TgsbTtsService.PREFS_NAME, 0)

    private val engine = TgsbSpeakEngine(application)

    // ── UI state (Speak tab) ────────────────────────────────────────

    val textToSpeak = MutableStateFlow("Hello world. This is TGSpeechBox.")
    val selectedLanguageIndex = MutableStateFlow(0)
    val selectedVoiceIndex = MutableStateFlow(0)
    val speedRate = MutableStateFlow(1.0f)        // 0.3 – 3.0
    val pitchHz = MutableStateFlow(110f)           // 40 – 300 Hz
    val isSpeaking = MutableStateFlow(false)
    val engineReady = MutableStateFlow(false)
    val errorMessage = MutableStateFlow<String?>(null)

    // ── VoicingTone sliders (0–100, mapped to real ranges) ──────────

    val voiceTilt = MutableStateFlow(loadSlider("voiceTilt", 50f))
    val noiseGlottalMod = MutableStateFlow(loadSlider("noiseGlottalMod", 0f))
    val pitchSyncF1 = MutableStateFlow(loadSlider("pitchSyncF1", 50f))
    val pitchSyncB1 = MutableStateFlow(loadSlider("pitchSyncB1", 50f))
    val speedQuotient = MutableStateFlow(loadSlider("speedQuotient", 50f))
    val aspirationTilt = MutableStateFlow(loadSlider("aspirationTilt", 50f))
    val cascadeBwScale = MutableStateFlow(loadSlider("cascadeBwScale", 50f))
    val voiceTremor = MutableStateFlow(loadSlider("voiceTremor", 0f))
    val headSize = MutableStateFlow(loadSlider("headSize", if (currentVoiceId == "david") 100f else 50f))

    // ── Pitch settings ──────────────────────────────────────────────

    val pitchMode = MutableStateFlow(loadString("pitchMode", "espeak_style"))
    val inflectionScale = MutableStateFlow(loadSlider("inflectionScale", 58f))
    val inflection = MutableStateFlow(loadSlider("inflection", 50f))

    // ── System rate override (global, NOT per-voice) ────────────────

    val overrideSystemRate = MutableStateFlow(loadGlobalBool("overrideSystemRate", false))
    val globalRate = MutableStateFlow(loadGlobalFloat("globalRate", 1.0f))

    // ── Output (global, NOT per-voice) ───────────────────────────────

    val pauseMode = MutableStateFlow(loadGlobalInt("pauseMode", 1))  // 0=off, 1=short, 2=long
    val systemVolume = MutableStateFlow(loadGlobalFloat("systemVolume", 1.0f))
    val sampleRateIndex = MutableStateFlow(loadGlobalInt("sampleRate", 22050).let { rate ->
        SAMPLE_RATES.indexOfFirst { it == rate }.coerceAtLeast(0).toFloat()
    })

    // ── FrameEx sliders (0–100) ─────────────────────────────────────

    val creakiness = MutableStateFlow(loadSlider("creakiness", 0f))
    val breathiness = MutableStateFlow(loadSlider("breathiness", 0f))
    val jitter = MutableStateFlow(loadSlider("jitter", 0f))
    val shimmer = MutableStateFlow(loadSlider("shimmer", 0f))
    val glottalSharpness = MutableStateFlow(loadSlider("glottalSharpness", 50f))

    // ── Data lists ────────────────────────────────────────────────────

    val languages: List<LanguageItem> = buildLanguageList()
    val voices: List<TgsbTtsService.Companion.VoiceDef> = TgsbTtsService.VOICES

    // ── Language filter state ────────────────────────────────────────

    private val _enabledLocaleKeys = MutableStateFlow(loadEnabledKeys())
    val enabledLocaleKeys: StateFlow<Set<String>> = _enabledLocaleKeys
    val allLocaleEntries: List<Pair<String, String>> = buildLocaleEntries()

    // ── Init ────────────────────────────────────────────────────────

    init {
        val savedPreset = prefs.getString(
            TgsbTtsService.PREF_VOICE_PRESET,
            TgsbTtsService.DEFAULT_PRESET
        ) ?: TgsbTtsService.DEFAULT_PRESET
        val idx = voices.indexOfFirst { it.id == savedPreset }
        if (idx >= 0) selectedVoiceIndex.value = idx

        val deviceLocale = Locale.getDefault()
        val langMatch = languages.indexOfFirst { item ->
            val ld = item.langDef.displayLocale
            ld.language == deviceLocale.language && ld.country == deviceLocale.country
        }
        val langFallback = if (langMatch < 0) {
            languages.indexOfFirst { it.langDef.displayLocale.language == deviceLocale.language }
        } else langMatch
        if (langFallback >= 0) {
            selectedLanguageIndex.value = langFallback
            Log.i(TAG, "Default language: ${languages[langFallback].displayName}")
        }

        if (engine.start()) {
            engineReady.value = true
            errorMessage.value = null

            val ld = languages[selectedLanguageIndex.value].langDef
            engine.setLanguage(ld.espeakLang, ld.tgsbLang)
            applyStoredOverrides(ld.tgsbLang)
            engine.setVoice(voices[selectedVoiceIndex.value].id)
            applyVoicingTone()
            applyFrameExDefaults()
            applyPitchSettings()
            engine.setVolume(systemVolume.value)
            val savedRate = SAMPLE_RATES[sampleRateIndex.value.roundToInt().coerceIn(0, SAMPLE_RATES.size - 1)]
            engine.setSampleRate(savedRate)
            engine.setPauseMode(pauseMode.value)

            engine.onSpeakingChanged = { speaking ->
                isSpeaking.value = speaking
            }

            Log.i(TAG, "Standalone engine ready")
        } else {
            errorMessage.value = "Failed to start speech engine."
            Log.e(TAG, "Engine start failed")
        }
    }

    override fun onCleared() {
        engine.shutdown()
        super.onCleared()
    }

    // ── Speak / Stop ────────────────────────────────────────────────

    fun speak() {
        val text = textToSpeak.value
        if (text.isBlank()) return

        val ld = languages[selectedLanguageIndex.value].langDef
        engine.setLanguage(ld.espeakLang, ld.tgsbLang)
        applyStoredOverrides(ld.tgsbLang)
        engine.setVoice(voices[selectedVoiceIndex.value].id)
        applyVoicingTone()
        applyFrameExDefaults()
        applyPitchSettings()
        engine.setPauseMode(pauseMode.value)

        errorMessage.value = null
        engine.speak(text, speedRate.value.toDouble(), pitchHz.value.toDouble())
        Log.i(TAG, "speak: lang=${ld.espeakLang} speed=${speedRate.value} pitch=${pitchHz.value}")
    }

    fun stop() {
        engine.stop()
    }

    // ── Selection handlers ──────────────────────────────────────────

    fun onLanguageSelected(index: Int) {
        selectedLanguageIndex.value = index
        val ld = languages[index].langDef
        engine.setLanguage(ld.espeakLang, ld.tgsbLang)
        applyStoredOverrides(ld.tgsbLang)
        Log.i(TAG, "Language selected: ${ld.espeakLang}")
    }

    fun onVoiceSelected(index: Int) {
        selectedVoiceIndex.value = index
        val voiceId = voices[index].id
        engine.setVoice(voiceId)
        prefs.edit().putString(TgsbTtsService.PREF_VOICE_PRESET, voiceId).apply()
        loadSettingsForVoice()
        applyVoicingTone()
        applyFrameExDefaults()
        applyPitchSettings()
    }

    /** Switch which voice's settings are shown in Engine Settings,
     *  WITHOUT changing TalkBack's active voice or saving the preset. */
    fun onEditingVoiceSelected(index: Int) {
        selectedVoiceIndex.value = index
        loadSettingsForVoice()
    }

    /**
     * Reload all per-voice slider/setting StateFlows from SharedPreferences.
     * Called when the user switches voices so the UI reflects that voice's
     * saved values (matching iOS loadSettingsForVoice behaviour).
     */
    private fun loadSettingsForVoice() {
        // VoicingTone sliders
        voiceTilt.value          = loadSlider("voiceTilt", 50f)
        noiseGlottalMod.value    = loadSlider("noiseGlottalMod", 0f)
        pitchSyncF1.value        = loadSlider("pitchSyncF1", 50f)
        pitchSyncB1.value        = loadSlider("pitchSyncB1", 50f)
        speedQuotient.value      = loadSlider("speedQuotient", 50f)
        aspirationTilt.value     = loadSlider("aspirationTilt", 50f)
        cascadeBwScale.value     = loadSlider("cascadeBwScale", 50f)
        voiceTremor.value        = loadSlider("voiceTremor", 0f)
        headSize.value           = loadSlider("headSize", if (currentVoiceId == "david") 100f else 50f)

        // FrameEx sliders
        creakiness.value         = loadSlider("creakiness", 0f)
        breathiness.value        = loadSlider("breathiness", 0f)
        jitter.value             = loadSlider("jitter", 0f)
        shimmer.value            = loadSlider("shimmer", 0f)
        glottalSharpness.value   = loadSlider("glottalSharpness", 50f)

        // Pitch settings
        pitchMode.value          = loadString("pitchMode", "espeak_style")
        inflectionScale.value    = loadSlider("inflectionScale", 58f)
        inflection.value         = loadSlider("inflection", 50f)
    }

    // ── Voice quality: slider change handlers ───────────────────────

    fun onVoiceTiltChanged(v: Float)       { voiceTilt.value = v;       saveSlider("voiceTilt", v);       applyVoicingTone() }
    fun onNoiseGlottalModChanged(v: Float) { noiseGlottalMod.value = v; saveSlider("noiseGlottalMod", v); applyVoicingTone() }
    fun onPitchSyncF1Changed(v: Float)     { pitchSyncF1.value = v;     saveSlider("pitchSyncF1", v);     applyVoicingTone() }
    fun onPitchSyncB1Changed(v: Float)     { pitchSyncB1.value = v;     saveSlider("pitchSyncB1", v);     applyVoicingTone() }
    fun onSpeedQuotientChanged(v: Float)   { speedQuotient.value = v;   saveSlider("speedQuotient", v);   applyVoicingTone() }
    fun onAspirationTiltChanged(v: Float)  { aspirationTilt.value = v;  saveSlider("aspirationTilt", v);  applyVoicingTone() }
    fun onCascadeBwScaleChanged(v: Float)  { cascadeBwScale.value = v;  saveSlider("cascadeBwScale", v);  applyVoicingTone() }
    fun onVoiceTremorChanged(v: Float)     { voiceTremor.value = v;     saveSlider("voiceTremor", v);     applyVoicingTone() }
    fun onHeadSizeChanged(v: Float)       { headSize.value = v;       saveSlider("headSize", v);       applyVoicingTone() }

    fun onCreakinessChanged(v: Float)      { creakiness.value = v;      saveSlider("creakiness", v);      applyFrameExDefaults() }
    fun onBreathinessChanged(v: Float)     { breathiness.value = v;     saveSlider("breathiness", v);     applyFrameExDefaults() }
    fun onJitterChanged(v: Float)          { jitter.value = v;          saveSlider("jitter", v);          applyFrameExDefaults() }
    fun onShimmerChanged(v: Float)         { shimmer.value = v;         saveSlider("shimmer", v);         applyFrameExDefaults() }
    fun onGlottalSharpnessChanged(v: Float){ glottalSharpness.value = v; saveSlider("glottalSharpness", v); applyFrameExDefaults() }

    fun onPitchModeChanged(mode: String)       { pitchMode.value = mode;       saveString("pitchMode", mode);       applyPitchSettings() }
    fun onInflectionScaleChanged(v: Float)     { inflectionScale.value = v;    saveSlider("inflectionScale", v);    applyPitchSettings() }
    fun onInflectionChanged(v: Float)          { inflection.value = v;         saveSlider("inflection", v);         applyPitchSettings() }
    fun onOverrideSystemRateChanged(v: Boolean){ overrideSystemRate.value = v;  saveGlobalBool("overrideSystemRate", v) }
    fun onGlobalRateChanged(v: Float)          { globalRate.value = v;          saveGlobalFloat("globalRate", v) }
    fun onSystemVolumeChanged(v: Float)        { systemVolume.value = v;        saveGlobalFloat("systemVolume", v);  engine.setVolume(v) }
    fun onSampleRateChanged(index: Float) {
        sampleRateIndex.value = index
        val rate = SAMPLE_RATES[index.roundToInt().coerceIn(0, SAMPLE_RATES.size - 1)]
        saveGlobalInt("sampleRate", rate)
        engine.setSampleRate(rate)
    }
    fun onPauseModeChanged(mode: Int) {
        pauseMode.value = mode
        saveGlobalInt("pauseMode", mode)
        engine.setPauseMode(mode)
    }

    // ── Reset to defaults ──────────────────────────────────────────

    fun resetToDefaults(allVoices: Boolean = false) {
        if (allVoices) {
            // Reset per-voice prefs for ALL voices directly in SharedPreferences
            val ed = prefs.edit()
            for (voice in voices) {
                val v = voice.id
                ed.putFloat("${PREF_PREFIX}voiceTilt.$v", 50f)
                ed.putFloat("${PREF_PREFIX}speedQuotient.$v", 50f)
                ed.putFloat("${PREF_PREFIX}aspirationTilt.$v", 50f)
                ed.putFloat("${PREF_PREFIX}cascadeBwScale.$v", 50f)
                ed.putFloat("${PREF_PREFIX}noiseGlottalMod.$v", 0f)
                ed.putFloat("${PREF_PREFIX}pitchSyncF1.$v", 50f)
                ed.putFloat("${PREF_PREFIX}pitchSyncB1.$v", 50f)
                ed.putFloat("${PREF_PREFIX}voiceTremor.$v", 0f)
                ed.putFloat("${PREF_PREFIX}headSize.$v", 50f)
                ed.putFloat("${PREF_PREFIX}creakiness.$v", 0f)
                ed.putFloat("${PREF_PREFIX}breathiness.$v", 0f)
                ed.putFloat("${PREF_PREFIX}jitter.$v", 0f)
                ed.putFloat("${PREF_PREFIX}shimmer.$v", 0f)
                ed.putFloat("${PREF_PREFIX}glottalSharpness.$v", 50f)
                ed.putString("${PREF_PREFIX}pitchMode.$v", "espeak_style")
                ed.putFloat("${PREF_PREFIX}inflectionScale.$v", 58f)
                ed.putFloat("${PREF_PREFIX}inflection.$v", 50f)
            }
            ed.apply()
            // Reload current voice's UI state from the just-written defaults
            loadSettingsForVoice()
            applyVoicingTone()
            applyFrameExDefaults()
            applyPitchSettings()
        } else {
            // Per-voice: VoicingTone sliders (saved under current voice)
            onVoiceTiltChanged(50f)
            onSpeedQuotientChanged(50f)
            onAspirationTiltChanged(50f)
            onCascadeBwScaleChanged(50f)
            onNoiseGlottalModChanged(0f)
            onPitchSyncF1Changed(50f)
            onPitchSyncB1Changed(50f)
            onVoiceTremorChanged(0f)
            onHeadSizeChanged(50f)

            // Per-voice: FrameEx sliders
            onCreakinessChanged(0f)
            onBreathinessChanged(0f)
            onJitterChanged(0f)
            onShimmerChanged(0f)
            onGlottalSharpnessChanged(50f)

            // Per-voice: Pitch
            onPitchModeChanged("espeak_style")
            onInflectionScaleChanged(58f)
            onInflectionChanged(50f)
        }

        // Global: System rate
        onOverrideSystemRateChanged(false)
        globalRate.value = 1.0f;     onGlobalRateChanged(1.0f)

        // Global: Output
        onPauseModeChanged(1)  // short
        onSampleRateChanged(2f)  // 22050 Hz
        onSystemVolumeChanged(1.0f)

        // Global: Reset language filter — all checked
        val allKeys = allLocaleEntries.map { it.first }.toSet()
        _enabledLocaleKeys.value = allKeys
        prefs.edit().remove(TgsbTtsService.PREF_SUPPORTED_LANGUAGES).apply()
    }

    // ── Slider → engine value mapping (matches NVDA driver math) ────

    private fun applyVoicingTone() {
        val tilt = (voiceTilt.value - 50f) * (24f / 50f)          // -24..+24 dB/oct
        val noiseMod = noiseGlottalMod.value / 100f               // 0..1
        val psF1 = (pitchSyncF1.value - 50f) * 1.2f               // -60..+60 Hz
        val psB1 = (pitchSyncB1.value - 50f) * 1.0f               // -50..+50 Hz

        val sqSlider = speedQuotient.value
        val sq = if (sqSlider <= 50f)
            0.5 + (sqSlider / 50.0) * 1.5                         // 0.5..2.0
        else
            2.0 + ((sqSlider - 50.0) / 50.0) * 2.0                // 2.0..4.0

        val aspTilt = (aspirationTilt.value - 50f) * 0.24f         // -12..+12 dB/oct

        val bwSlider = cascadeBwScale.value
        val bw = if (bwSlider <= 50f)
            2.0 - (bwSlider / 50.0) * 1.0                         // 2.0..1.0
        else
            1.0 - ((bwSlider - 50.0) / 50.0) * 0.7                // 1.0..0.3

        val tremor = (voiceTremor.value / 100f) * 0.4f             // 0..0.4

        val hsSlider = headSize.value.toDouble()
        val hs = if (hsSlider <= 50.0)
            1.25 - (hsSlider / 50.0) * 0.25                        // 1.25..1.0
        else
            1.0 - ((hsSlider - 50.0) / 50.0) * 0.15                // 1.0..0.85

        engine.setVoicingTone(
            tilt.toDouble(), noiseMod.toDouble(),
            psF1.toDouble(), psB1.toDouble(),
            sq, aspTilt.toDouble(), bw, tremor.toDouble(), hs
        )
    }

    private fun applyFrameExDefaults() {
        engine.setFrameExDefaults(
            (creakiness.value / 100f).toDouble(),
            (breathiness.value / 100f).toDouble(),
            (jitter.value / 100f).toDouble(),
            (shimmer.value / 100f).toDouble(),
            (glottalSharpness.value / 50f).toDouble()   // 0..2.0, 50=1.0
        )
    }

    private fun applyPitchSettings() {
        engine.setPitchMode(pitchMode.value)
        engine.setInflectionScale((inflectionScale.value / 100f).toDouble())
        engine.setInflection((inflection.value / 100f).toDouble())
    }

    // ── Language filter ─────────────────────────────────────────────

    fun toggleLocaleKey(localeKey: String, enabled: Boolean): Boolean {
        val current = _enabledLocaleKeys.value.toMutableSet()
        if (enabled) {
            current.add(localeKey)
        } else {
            if (current.size <= 1) return false
            current.remove(localeKey)
        }
        _enabledLocaleKeys.value = current.toSet()
        saveEnabledKeys(current)
        return true
    }

    // ── Helpers ─────────────────────────────────────────────────────

    /** Current voice ID for per-voice keying (e.g. "adam", "benjamin"). */
    private val currentVoiceId: String
        get() = TgsbTtsService.VOICES.getOrNull(selectedVoiceIndex.value)?.id ?: "adam"

    /**
     * Load a per-voice slider value.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadSlider(key: String, default: Float): Float {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getFloat(voiceKey, default)
        // Fallback: old global key (pre-per-voice migration)
        return prefs.getFloat("${PREF_PREFIX}$key", default)
    }

    private fun saveSlider(key: String, value: Float) {
        prefs.edit().putFloat("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice string value.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadString(key: String, default: String): String {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getString(voiceKey, default) ?: default
        return prefs.getString("${PREF_PREFIX}$key", default) ?: default
    }

    private fun saveString(key: String, value: String) {
        prefs.edit().putString("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice boolean.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadBool(key: String, default: Boolean): Boolean {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getBoolean(voiceKey, default)
        return prefs.getBoolean("${PREF_PREFIX}$key", default)
    }

    private fun saveBool(key: String, value: Boolean) {
        prefs.edit().putBoolean("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice int.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadInt(key: String, default: Int): Int {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getInt(voiceKey, default)
        return prefs.getInt("${PREF_PREFIX}$key", default)
    }

    private fun saveInt(key: String, value: Int) {
        prefs.edit().putInt("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    // ── Global (NOT per-voice) preference helpers ─────────────────────

    private fun loadGlobalFloat(key: String, default: Float): Float =
        prefs.getFloat("${PREF_PREFIX}$key", default)

    private fun saveGlobalFloat(key: String, value: Float) {
        prefs.edit().putFloat("${PREF_PREFIX}$key", value).apply()
    }

    private fun loadGlobalBool(key: String, default: Boolean): Boolean =
        prefs.getBoolean("${PREF_PREFIX}$key", default)

    private fun saveGlobalBool(key: String, value: Boolean) {
        prefs.edit().putBoolean("${PREF_PREFIX}$key", value).apply()
    }

    private fun loadGlobalInt(key: String, default: Int): Int =
        prefs.getInt("${PREF_PREFIX}$key", default)

    private fun saveGlobalInt(key: String, value: Int) {
        prefs.edit().putInt("${PREF_PREFIX}$key", value).apply()
    }

    private fun buildLanguageList(): List<LanguageItem> {
        val seen = mutableSetOf<String>()
        val items = mutableListOf<LanguageItem>()
        for (ld in TgsbTtsService.LANGUAGES) {
            val key = ld.displayLocale.toString()
            if (key in seen) continue
            seen.add(key)
            items.add(LanguageItem(ld.displayLocale.getDisplayName(), ld))
        }
        items.sortBy { it.displayName }
        return items
    }

    private fun buildLocaleEntries(): List<Pair<String, String>> {
        val seen = mutableSetOf<String>()
        val entries = mutableListOf<Pair<String, String>>()
        for (ld in TgsbTtsService.LANGUAGES) {
            val key = ld.displayLocale.toString()
            if (key in seen) continue
            seen.add(key)
            entries.add(key to ld.displayLocale.getDisplayName())
        }
        entries.sortBy { it.second }
        return entries
    }

    private fun loadEnabledKeys(): Set<String> {
        val saved = TgsbTtsService.getEnabledLocaleKeys(prefs)
        if (saved != null) return saved
        return buildLocaleEntries().map { it.first }.toSet()
    }

    private fun saveEnabledKeys(keys: Set<String>) {
        if (keys.size >= allLocaleEntries.size) {
            prefs.edit().remove(TgsbTtsService.PREF_SUPPORTED_LANGUAGES).apply()
        } else {
            prefs.edit()
                .putStringSet(TgsbTtsService.PREF_SUPPORTED_LANGUAGES, keys)
                .apply()
        }
    }

    // ── Pack settings editor ─────────────────────────────────────────

    enum class SettingType { Bool, Number, Text }

    data class PackSetting(
        val key: String,
        val displayName: String,
        val value: String,
        val isOverridden: Boolean,
        val type: SettingType,
        val options: List<String>? = null  // dropdown options for enum-like strings
    )

    val editorLanguages = MutableStateFlow<List<String>>(emptyList())
    val editorSettings = MutableStateFlow<List<PackSetting>>(emptyList())
    private var editorLangTag: String = ""

    fun loadEditorLanguages() {
        editorLanguages.value = engine.getAvailableLanguages()
    }

    fun loadEditorSettings(langTag: String) {
        editorLangTag = langTag

        // Query settings directly by langTag — no temp language switch needed.
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_SETTINGS, langTag) ?: return
        val overrides = loadOverrides(langTag)

        // Settings managed by Engine Settings sliders — hide from editor.
        val hiddenKeys = setOf("legacyPitchMode", "legacyPitchInflectionScale")

        val settings = mutableListOf<PackSetting>()
        val arr = org.json.JSONArray(jsonStr)
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            val key = obj.getString("key")
            if (key in hiddenKeys) continue
            val baseValue = obj.get("value").toString()
            val effectiveValue = overrides[key] ?: baseValue
            val jsonType = obj.getString("type")
            val type = when (jsonType) {
                "bool" -> SettingType.Bool
                "float" -> SettingType.Number
                else -> SettingType.Text
            }
            val options = if (obj.has("options")) {
                val optArr = obj.getJSONArray("options")
                (0 until optArr.length()).map { optArr.getString(it) }
            } else null
            settings.add(PackSetting(
                key = key,
                displayName = camelToDisplay(key),
                value = effectiveValue,
                isOverridden = overrides.containsKey(key),
                type = type,
                options = options
            ))
        }
        editorSettings.value = settings

        // Apply overrides to the active language if it matches.
        if (overrides.isNotEmpty()) {
            for ((k, v) in overrides) {
                engine.setData(TgsbSpeakEngine.DATA_SETTINGS, langTag, k, v)
            }
        }
    }

    fun setEditorOverride(langTag: String, key: String, value: String) {
        // If the new value matches the pack base, remove the override instead.
        val baseValues = getBaseValues(langTag)
        val overrides = loadOverrides(langTag).toMutableMap()
        if (baseValues[key] == value) {
            overrides.remove(key)
        } else {
            overrides[key] = value
        }
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    /** Read base pack values (no overrides) for comparison. */
    private fun getBaseValues(langTag: String): Map<String, String> {
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_SETTINGS, langTag) ?: return emptyMap()
        val map = mutableMapOf<String, String>()
        val arr = org.json.JSONArray(jsonStr)
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            map[obj.getString("key")] = obj.get("value").toString()
        }
        return map
    }

    fun removeEditorOverride(langTag: String, key: String) {
        val overrides = loadOverrides(langTag).toMutableMap()
        overrides.remove(key)
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    fun resetAllEditorOverrides(langTag: String) {
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        prefs.edit()
            .remove("pack_overrides_$langTag")
            .putInt("pack_overrides_version", ver)
            .apply()
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    /** Reload the current language from disk so removed overrides take effect. */
    private fun reloadCurrentLanguage() {
        val curLang = languages.getOrNull(selectedLanguageIndex.value) ?: return
        engine.setLanguage(curLang.langDef.espeakLang, curLang.langDef.tgsbLang)
        applyStoredOverrides(curLang.langDef.tgsbLang)
    }

    /** Apply stored overrides after setLanguage via per-key setData. */
    fun applyStoredOverrides(tgsbLang: String) {
        val overrides = loadOverrides(tgsbLang)
        if (overrides.isEmpty()) return
        for ((k, v) in overrides) {
            engine.setData(TgsbSpeakEngine.DATA_SETTINGS, tgsbLang, k, v)
        }
    }

    private fun loadOverrides(langTag: String): Map<String, String> {
        val json = prefs.getString("pack_overrides_$langTag", null) ?: return emptyMap()
        return try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { emptyMap() }
    }

    private fun saveOverrides(langTag: String, overrides: Map<String, String>) {
        val e = prefs.edit()
        if (overrides.isEmpty()) {
            e.remove("pack_overrides_$langTag")
        } else {
            val obj = org.json.JSONObject()
            for ((k, v) in overrides) obj.put(k, v)
            e.putString("pack_overrides_$langTag", obj.toString())
        }
        // Bump version so the TTS Service knows to reload the pack.
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        e.putInt("pack_overrides_version", ver)
        e.apply()
    }

    private fun detectType(value: String): SettingType = when {
        value == "true" || value == "false" -> SettingType.Bool
        value.toDoubleOrNull() != null -> SettingType.Number
        else -> SettingType.Text
    }

    private fun camelToDisplay(key: String): String {
        // "boundarySmoothing.enabled" → "Boundary Smoothing: Enabled"
        // "yearSplittingEnabled" → "Year Splitting Enabled"
        return key.replace(".", ": ").fold(StringBuilder()) { sb, c ->
            if (c.isUpperCase() && sb.isNotEmpty() && sb.last().isLowerCase()) {
                sb.append(' ')
            }
            sb.append(c)
        }.toString().replaceFirstChar { it.uppercase() }
    }

    // ── Pack import / export ───────────────────────────────────────────

    private val _importExportStatus = MutableStateFlow<String?>(null)
    val importExportStatus: StateFlow<String?> = _importExportStatus

    fun clearImportExportStatus() { _importExportStatus.value = null }

    private fun packFileForLang(context: Context, langTag: String): File =
        File(context.filesDir, "tgsb/packs/lang/$langTag.yaml")

    fun exportPackYaml(context: Context, langTag: String, destUri: Uri) {
        val packFile = packFileForLang(context, langTag)
        if (!packFile.exists()) {
            _importExportStatus.value = "Pack file not found for $langTag"
            return
        }
        try {
            context.contentResolver.openOutputStream(destUri)?.use { out ->
                packFile.inputStream().use { it.copyTo(out) }
            }
            _importExportStatus.value = "Exported $langTag.yaml"
        } catch (e: Exception) {
            _importExportStatus.value = "Export failed: ${e.message}"
        }
    }

    fun sharePackYaml(context: Context, langTag: String) {
        val packFile = packFileForLang(context, langTag)
        if (!packFile.exists()) {
            _importExportStatus.value = "Pack file not found for $langTag"
            return
        }
        val content = try { packFile.readText() } catch (e: Exception) {
            _importExportStatus.value = "Could not read pack: ${e.message}"
            return
        }
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "$langTag.yaml")
            putExtra(Intent.EXTRA_TEXT, content)
        }
        context.startActivity(Intent.createChooser(intent, "Share $langTag pack"))
    }

    fun importPackYaml(context: Context, langTag: String, sourceUri: Uri): Boolean {
        val content = try {
            context.contentResolver.openInputStream(sourceUri)?.use {
                it.bufferedReader().readText()
            }
        } catch (e: Exception) {
            _importExportStatus.value = "Could not read file: ${e.message}"
            return false
        }
        if (content.isNullOrBlank()) {
            _importExportStatus.value = "File is empty"
            return false
        }

        // Reject phonemes files — they have phoneme-specific keys.
        if (content.contains("_isVowel:") && content.contains("_isNasal:")) {
            _importExportStatus.value = context.getString(R.string.editor_import_not_lang_pack)
            return false
        }

        try {
            val destFile = packFileForLang(context, langTag)
            destFile.writeText(content)
        } catch (e: Exception) {
            _importExportStatus.value = "Import failed: ${e.message}"
            return false
        }

        // Clear overrides — imported file is the new base.
        prefs.edit().remove("pack_overrides_$langTag").apply()
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
        _importExportStatus.value = "Imported into $langTag"
        return true
    }
}
