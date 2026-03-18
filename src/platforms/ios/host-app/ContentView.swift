/*
 * ContentView.swift — Main UI for TGSpeechBox.
 *
 * Two-tab layout: Speak (text input + controls) and
 * Engine Settings (voice quality sliders + volume).
 */

import SwiftUI

let kAppGroupId = "group.com.tgspeechbox.app"

struct ContentView: View {
    @StateObject private var engine = TgsbEngine()
    @State private var text = "Hello world. This is TGSpeechBox running on Apple."
    @State private var engineStarted = false
    @State private var errorMessage: String?
    @State private var selectedTab = 0

    var body: some View {
        TabView(selection: $selectedTab) {
            speakTab
                .tag(0)
                .tabItem {
                    Label("Speak", systemImage: "play.circle")
                }

            EngineSettingsView(engine: engine, engineStarted: $engineStarted)
                .tag(1)
                .tabItem {
                    Label("Engine", systemImage: "slider.horizontal.3")
                }

            PackEditorView(engine: engine, engineStarted: $engineStarted)
                .tag(2)
                .tabItem {
                    Label("Editor", systemImage: "pencil")
                }
        }
        #if os(macOS)
        .frame(minWidth: 500, minHeight: 450)
        #endif
        .onDisappear {
            engine.shutdown()
        }
    }

    // MARK: - Speak tab

    private var speakTab: some View {
        VStack(spacing: 16) {
            Text("TGSpeechBox")
                .font(.largeTitle)
                .accessibilityAddTraits(.isHeader)
            Text("Preview and test the voice")
                .font(.subheadline)
                .foregroundColor(.secondary)

            if let error = errorMessage {
                Text(error)
                    .foregroundColor(.red)
                    .accessibilityLabel("Error: \(error)")
            }

            // Text input
            Group {
                #if os(macOS)
                TextEditor(text: $text)
                    .frame(minHeight: 100)
                    .border(Color.gray.opacity(0.3))
                    .accessibilityLabel("Text to speak")
                #else
                TextEditor(text: $text)
                    .frame(minHeight: 100)
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(Color.gray.opacity(0.3)))
                    .accessibilityLabel("Text to speak")
                #endif
            }

            // Controls
            HStack(spacing: 12) {
                // Language picker
                Picker("Language", selection: $engine.selectedLanguage) {
                    ForEach(kLanguages) { lang in
                        Text(lang.displayName).tag(lang)
                    }
                }
                .accessibilityLabel("Language")
                .accessibilityValue(engine.selectedLanguage.displayName)
                #if os(macOS)
                .frame(maxWidth: 200)
                #endif

                // Voice picker
                Picker("Voice", selection: $engine.selectedVoice) {
                    ForEach(engine.voices) { voice in
                        Text(voice.displayName).tag(voice)
                    }
                }
                .accessibilityLabel("Voice")
                .accessibilityValue(engine.selectedVoice.displayName)
                #if os(macOS)
                .frame(maxWidth: 150)
                #endif
            }

            // Speed and pitch sliders
            VStack(spacing: 8) {
                HStack {
                    Text("Speed: \(engine.speed, specifier: "%.1f")x")
                        .frame(width: 100, alignment: .leading)
                        .accessibilityHidden(true)
                    Slider(value: $engine.speed, in: 0.3...4.0, step: 0.1)
                        .accessibilityLabel("Speed")
                        .accessibilityValue("\(engine.speed, specifier: "%.1f") times")
                }
                HStack {
                    Text("Pitch: \(Int(engine.pitch)) Hz")
                        .frame(width: 100, alignment: .leading)
                        .accessibilityHidden(true)
                    Slider(value: $engine.pitch, in: 40...300, step: 5)
                        .accessibilityLabel("Pitch")
                        .accessibilityValue("\(Int(engine.pitch)) hertz")
                }
                HStack {
                    Text("Inflection: \(Int(engine.inflectionValue))")
                        .frame(width: 100, alignment: .leading)
                        .accessibilityHidden(true)
                    Slider(value: $engine.inflectionValue, in: 0...100, step: 1)
                        .accessibilityLabel("Inflection")
                        .accessibilityValue("\(Int(engine.inflectionValue))")
                }
            }

            // Speak / Stop buttons
            HStack(spacing: 16) {
                Button(action: {
                    if !engineStarted {
                        if engine.start() {
                            engineStarted = true
                            errorMessage = nil
                        } else {
                            errorMessage = "Failed to initialize engine"
                            return
                        }
                    }
                    engine.speak(text)
                }) {
                    Label("Speak", systemImage: "play.fill")
                        .frame(minWidth: 80)
                }
                .disabled(engine.isSpeaking || text.isEmpty)
                .keyboardShortcut(.return, modifiers: .command)
                .accessibilityLabel("Speak")
                .accessibilityHint("Press to speak the entered text")

                Button(action: {
                    engine.stopSpeaking()
                }) {
                    Label("Stop", systemImage: "stop.fill")
                        .frame(minWidth: 80)
                }
                .disabled(!engine.isSpeaking)
                .keyboardShortcut(.escape, modifiers: [])
                .accessibilityLabel("Stop")
                .accessibilityHint("Press to stop speaking")
            }
            .buttonStyle(.borderedProminent)
        }
        .padding(20)
    }
}
