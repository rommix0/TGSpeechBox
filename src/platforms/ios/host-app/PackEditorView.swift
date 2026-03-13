/*
 * PackEditorView — Pack settings and phoneme editor.
 *
 * Tabbed interface: Packs (language pack settings) and Phonemes
 * (acoustic parameters with live preview).
 * Changes stored in App Group UserDefaults, re-applied after setLanguage.
 *
 * License: GPL-3.0
 */

import SwiftUI
import UniformTypeIdentifiers

/// Navigation wrapper types so Packs and Phonemes don't clash in a single NavigationStack.
struct PackLangNav: Hashable { let lang: String }
struct PhonemeNav: Hashable { let key: String }

struct PackEditorView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool
    @SceneStorage("editorSelectedTab") private var selectedTab = 0
    @State private var langFilter: String = ""

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Picker("", selection: $selectedTab) {
                    Text("Packs").tag(0)
                    Text("Phonemes").tag(1)
                }
                .pickerStyle(.segmented)
                .padding(.horizontal)
                .padding(.top, 8)

                switch selectedTab {
                case 0:
                    LanguageListView(
                        engine: engine,
                        engineStarted: $engineStarted
                    )
                default:
                    PhonemeListView(
                        engine: engine,
                        engineStarted: $engineStarted,
                        langFilter: $langFilter
                    )
                }
            }
            .navigationDestination(for: PackLangNav.self) { nav in
                PackSettingsListView(
                    engine: engine,
                    langTag: nav.lang
                )
#if os(iOS)
                .toolbar(.hidden, for: .tabBar)
#endif
            }
            .navigationDestination(for: PhonemeNav.self) { nav in
                PhonemeDetailView(
                    engine: engine,
                    phonemeKey: nav.key
                )
#if os(iOS)
                .toolbar(.hidden, for: .tabBar)
#endif
            }
        }
    }
}

// MARK: - Language List

private struct LanguageListView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool

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
                    NavigationLink(value: PackLangNav(lang: lang)) {
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
    @Environment(\.dismiss) private var dismiss

    @State private var editingKey: String?
    @State private var editingValue: String = ""
    @State private var showResetAll = false
    @State private var showImportPicker = false
    @State private var showExportPicker = false
    @State private var exportFileURL: URL?
    @State private var showImportConfirm = false
    @State private var pendingImportURL: URL?
    @State private var statusMessage: String?

    var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Spacer()
                Button("Reset All") { showResetAll = true }
            }
            .padding(.horizontal)
            .padding(.top, 8)

            // Import / Export actions
            HStack(spacing: 12) {
                Button("Import") { showImportPicker = true }
                    .buttonStyle(.bordered)
                Button("Export") { showExportPicker = true }
                    .buttonStyle(.bordered)
                if let url = exportFileURL {
                    ShareLink("Share", item: url)
                        .buttonStyle(.bordered)
                }
            }
            .padding(.horizontal)
            .padding(.bottom, 8)

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
            exportFileURL = engine.exportPackToTempFile(langTag: langTag)
        }
        .navigationTitle(langTag)
        .alert("Edit Value", isPresented: Binding(
            get: { editingKey != nil },
            set: { if !$0 { editingKey = nil } }
        )) {
            let isNumeric = engine.editorSettings.first(where: { $0.key == editingKey })?.type == .number
            TextField("Value", text: $editingValue)
#if os(iOS)
                .keyboardType(isNumeric ? .decimalPad : .default)
#endif
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
        .fileImporter(
            isPresented: $showImportPicker,
            allowedContentTypes: [.yaml, .plainText, .data],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                pendingImportURL = url
                showImportConfirm = true
            }
        }
        .fileExporter(
            isPresented: $showExportPicker,
            document: PackYamlDocument(engine: engine, langTag: langTag),
            contentType: .yaml,
            defaultFilename: "\(langTag).yaml"
        ) { result in
            if case .success = result {
                statusMessage = "Exported \(langTag).yaml"
            } else if case .failure(let error) = result {
                statusMessage = "Export failed: \(error.localizedDescription)"
            }
        }
        .alert("Import Pack", isPresented: $showImportConfirm) {
            Button("Import") {
                if let url = pendingImportURL {
                    statusMessage = engine.importPackYaml(langTag: langTag, from: url)
                }
                pendingImportURL = nil
            }
            Button("Cancel", role: .cancel) { pendingImportURL = nil }
        } message: {
            Text("Replace the \(langTag) language pack with the selected file? Any existing overrides will be cleared.")
        }
        .alert("Result", isPresented: Binding(
            get: { statusMessage != nil },
            set: { if !$0 { statusMessage = nil } }
        )) {
            Button("OK") { statusMessage = nil }
        } message: {
            Text(statusMessage ?? "")
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
            .accessibilityLabel({
                let valLabel = setting.type == .bool_ ? (setting.value == "true" ? "on" : "off") : setting.value
                return "\(setting.displayName), \(valLabel)\(setting.isOverridden ? ", customized" : "")"
            }())
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


// MARK: - File Document for Export

struct PackYamlDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.yaml, .plainText] }

    let content: String

    @MainActor init(engine: TgsbEngine, langTag: String) {
        content = engine.packYamlContent(langTag: langTag) ?? ""
    }

    init(configuration: ReadConfiguration) throws {
        if let data = configuration.file.regularFileContents {
            content = String(data: data, encoding: .utf8) ?? ""
        } else {
            content = ""
        }
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        let data = content.data(using: .utf8) ?? Data()
        return FileWrapper(regularFileWithContents: data)
    }
}
