/*
 * PackEditorView — Pack settings editor tab.
 *
 * Lets users view and override language pack settings.
 * Changes stored in App Group UserDefaults, re-applied after setLanguage.
 *
 * License: GPL-3.0
 */

import SwiftUI

struct PackEditorView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool
    @State private var selectedLang: String?

    var body: some View {
        if let lang = selectedLang {
            PackSettingsListView(
                engine: engine,
                langTag: lang,
                onBack: { selectedLang = nil }
            )
        } else {
            LanguageListView(
                engine: engine,
                engineStarted: $engineStarted,
                onLanguageSelected: { selectedLang = $0 }
            )
        }
    }
}

// MARK: - Language List

private struct LanguageListView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool
    var onLanguageSelected: (String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Packs")
                .font(.title2)
                .accessibilityAddTraits(.isHeader)

            Text("Select a language to edit")
                .font(.subheadline)
                .foregroundColor(.secondary)

            if engine.editorLanguages.isEmpty {
                Text("Start the engine first by pressing Speak.")
                    .foregroundColor(.secondary)
                    .padding(.top, 20)
            } else {
                List(engine.editorLanguages, id: \.self) { lang in
                    Button(action: { onLanguageSelected(lang) }) {
                        Text(lang)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .listStyle(.plain)
            }
        }
        .padding()
        .onAppear {
            if !engineStarted {
                if engine.start() {
                    engineStarted = true
                }
            }
            if engineStarted {
                engine.loadEditorLanguages()
            }
        }
    }
}

// MARK: - Settings List

private struct PackSettingsListView: View {
    @ObservedObject var engine: TgsbEngine
    let langTag: String
    var onBack: () -> Void

    @State private var editingKey: String?
    @State private var editingValue: String = ""
    @State private var showResetAll = false
    @AccessibilityFocusState private var headerFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Button(action: onBack) {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                        Text("Back")
                    }
                }
                .accessibilityFocused($headerFocused)
                Spacer()
                Text(langTag)
                    .font(.headline)
                    .accessibilityAddTraits(.isHeader)
                Spacer()
                Button("Reset All") { showResetAll = true }
            }
            .padding()

            if engine.editorSettings.isEmpty {
                Text("No settings found")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else {
                List(engine.editorSettings) { setting in
                    SettingRowView(
                        setting: setting,
                        onToggle: { newVal in
                            engine.setEditorOverride(langTag: langTag,
                                                     key: setting.key,
                                                     value: newVal)
                        },
                        onEdit: {
                            editingKey = setting.key
                            editingValue = setting.value
                        },
                        onReset: {
                            engine.removeEditorOverride(langTag: langTag,
                                                        key: setting.key)
                        }
                    )
                }
                .listStyle(.plain)
            }
        }
        .onAppear {
            engine.loadEditorSettings(langTag: langTag)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                headerFocused = true
            }
        }
        .alert("Edit Value", isPresented: Binding(
            get: { editingKey != nil },
            set: { if !$0 { editingKey = nil } }
        )) {
            TextField("Value", text: $editingValue)
            Button("OK") {
                if let key = editingKey {
                    engine.setEditorOverride(langTag: langTag,
                                             key: key, value: editingValue)
                }
                editingKey = nil
            }
            Button("Cancel", role: .cancel) { editingKey = nil }
        } message: {
            Text(editingKey ?? "")
        }
        .alert("Reset All Overrides", isPresented: $showResetAll) {
            Button("Reset", role: .destructive) {
                engine.resetAllEditorOverrides(langTag: langTag)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Remove all custom overrides for this language?")
        }
    }
}

// MARK: - Setting Row

private struct SettingRowView: View {
    let setting: TgsbEngine.PackSetting
    var onToggle: (String) -> Void
    var onEdit: () -> Void
    var onReset: () -> Void

    private var hasOptions: Bool { setting.options != nil && !setting.options!.isEmpty }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(setting.displayName)
                        .font(.body)
                    if setting.isOverridden {
                        Text("Customized")
                            .font(.caption)
                            .foregroundColor(.accentColor)
                    }
                }

                Spacer()

                switch setting.type {
                case .bool_:
                    Toggle("", isOn: Binding(
                        get: { setting.value == "true" },
                        set: { onToggle($0 ? "true" : "false") }
                    ))
                    .labelsHidden()
                default:
                    if hasOptions {
                        Picker(setting.displayName, selection: Binding(
                            get: { setting.value },
                            set: { onToggle($0) }
                        )) {
                            ForEach(setting.options!, id: \.self) { option in
                                Text(option).tag(option)
                            }
                        }
                        .pickerStyle(.menu)
                        .labelsHidden()
                    } else {
                        Text(setting.value)
                            .foregroundColor(.secondary)
                            .font(.body)
                    }
                }
            }
            .contentShape(Rectangle())
            .onTapGesture {
                if setting.type != .bool_ && !hasOptions {
                    onEdit()
                }
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("\(setting.displayName), \(setting.value)\(setting.isOverridden ? ", customized" : "")")
            .accessibilityAddTraits(setting.type != .bool_ && !hasOptions ? .isButton : [])
            .accessibilityHint(setting.type != .bool_ && !hasOptions ? "Double tap to edit" : "")

            if setting.isOverridden {
                Button("Reset \(setting.displayName)") {
                    onReset()
                }
                .font(.caption)
            }
        }
        .padding(.vertical, 4)
    }
}
