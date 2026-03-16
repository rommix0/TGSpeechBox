/*
 * DictionaryEditorView -- Dictionary editor with type and language selection.
 *
 * Supports three dictionary sub-types (pronounce, stress, compound)
 * loaded from the C++ data query API. Each type has type-specific
 * add/edit dialogs. Paginated loading (100 entries at a time).
 *
 * License: GPL-3.0
 */

import SwiftUI
import UniformTypeIdentifiers

/// Human-readable label for a dict type.
private func dictTypeLabel(_ type: String) -> String {
    switch type {
    case "pronounce": return "Pronunciation"
    case "compound":  return "Compound"
    case "stress":    return "Stress"
    case "character": return "Characters"
    default:          return type.prefix(1).uppercased() + type.dropFirst()
    }
}

// MARK: - Dictionary Editor

struct DictionaryEditorView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool

    @State private var selectedType: String = ""
    @State private var langFilter: String = ""
    @State private var searchQuery: String = ""
    @State private var activeSearch: String = ""
    @State private var loadedCount: Int = 0
    @State private var showAddSheet = false
    @State private var editingEntry: TgsbEngine.DictEntry? = nil
    @State private var showRemoveConfirm = false
    @State private var showExcludeSheet = false
    @State private var statusMessage: String?

    // Export state — single fileExporter to avoid SwiftUI dual-modifier conflict
    @State private var showExportPicker = false
    @State private var exportUserOnly = false
    @State private var showImportPicker = false

    private var typePickerLabel: String {
        if selectedType.isEmpty { return "Select type" }
        return "\(dictTypeLabel(selectedType)) (\(engine.dictionaryTotalCount))"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Type dropdown + Language dropdown row
            HStack(spacing: 8) {
                // Type picker
                Menu {
                    ForEach(engine.dictTypes) { dt in
                        let label = dictTypeLabel(dt.type) + " (\(dt.count))"
                        Button(label) { selectedType = dt.type }
                    }
                } label: {
                    Text(typePickerLabel)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Color.secondary.opacity(0.15))
                        .cornerRadius(8)
                }
                .accessibilityLabel({
                    var desc = "Type: "
                    if selectedType.isEmpty {
                        desc += "Select type"
                    } else {
                        desc += dictTypeLabel(selectedType)
                        desc += ", \(engine.dictionaryTotalCount) entries"
                    }
                    return desc
                }())

                // Language picker
                Menu {
                    ForEach(engine.editorLanguages, id: \.self) { lang in
                        Button(lang) { langFilter = lang }
                    }
                } label: {
                    Text(langFilter.isEmpty ? "Select language" : langFilter)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Color.secondary.opacity(0.15))
                        .cornerRadius(8)
                }
                .accessibilityLabel("Language: \(langFilter.isEmpty ? "Select language" : langFilter)")

                Spacer()
            }
            .padding(.horizontal)
            .padding(.top, 8)

            // Search field
            TextField("Search", text: $searchQuery)
                .textFieldStyle(.roundedBorder)
                .autocorrectionDisabled()
#if os(iOS)
                .textInputAutocapitalization(.never)
#endif
                .padding(.horizontal)
                .padding(.vertical, 4)

            // Entry count + Add button + More options row
            HStack {
                Text("Showing \(engine.dictionaryEntries.count) of \(engine.dictionaryTotalCount) entries")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Spacer()

                Button(action: { showAddSheet = true }) {
                    Image(systemName: "plus")
                        .font(.body)
                }
                .accessibilityLabel("Add entry")
                .disabled(langFilter.isEmpty || selectedType.isEmpty)

                // More options menu
                Menu {
                    // Export all (pronunciation + character)
                    if selectedType == "pronounce" || selectedType == "character" {
                        Button(action: {
                            exportUserOnly = false
                            showExportPicker = true
                        }) {
                            Label("Export all", systemImage: "square.and.arrow.up")
                        }
                    }

                    // Export changed (all types)
                    Button(action: {
                        exportUserOnly = true
                        showExportPicker = true
                    }) {
                        Label("Export changed", systemImage: "square.and.arrow.up")
                    }

                    // Share all (pronunciation + character)
                    if selectedType == "pronounce" || selectedType == "character" {
                        if let url = exportDictToTempFile(userOnly: false) {
                            ShareLink("Share all", item: url)
                        }
                    }

                    // Share changed (all types)
                    if let url = exportDictToTempFile(userOnly: true) {
                        ShareLink("Share changed", item: url)
                    }

                    // Import (pronunciation + character)
                    Button(action: { showImportPicker = true }) {
                        Label("Import", systemImage: "square.and.arrow.down")
                    }
                    .disabled(selectedType != "pronounce" && selectedType != "character")

                    Divider()

                    // Remove duplicates
                    Button(action: {
                        let mainKeys = Set(engine.dictionaryEntries.filter { $0.source == "main" }.map { $0.fromText.lowercased() })
                        let duplicates = engine.dictionaryEntries.filter { $0.source == "user" && mainKeys.contains($0.fromText.lowercased()) }
                        if duplicates.count > 500 {
                            statusMessage = "Too many duplicates (\(duplicates.count)), remove manually"
                        } else if duplicates.isEmpty {
                            statusMessage = "No duplicates found"
                        } else {
                            for d in duplicates { engine.deleteDictEntry(fromText: d.fromText) }
                            statusMessage = "Removed \(duplicates.count) duplicates"
                        }
                    }) {
                        Label("Remove duplicates", systemImage: "doc.on.doc")
                    }

                    // Exclude dictionaries
                    Button(action: { showExcludeSheet = true }) {
                        Label("Exclude dictionaries", systemImage: "eye.slash")
                    }

                    // Remove changed entries
                    Button(role: .destructive, action: { showRemoveConfirm = true }) {
                        Label("Remove changed entries", systemImage: "trash")
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                        .font(.body)
                }
                .accessibilityLabel("More options")
            }
            .padding(.horizontal)
            .padding(.vertical, 4)

            // Entry list
            if langFilter.isEmpty || selectedType.isEmpty {
                Spacer()
                Text("Select a type and language to view dictionary entries.")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else if engine.dictionaryEntries.isEmpty {
                Spacer()
                VStack(spacing: 8) {
                    if !activeSearch.isEmpty {
                        Text("No matches for \"\(activeSearch)\"")
                            .foregroundColor(.secondary)
                    } else {
                        Text("No \(dictTypeLabel(selectedType).lowercased()) entries yet")
                            .font(.headline)
                            .foregroundColor(.secondary)
                        Text("Want to be the first to add one? Tap the + (add) button above.")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .multilineTextAlignment(.center)
                    }
                }
                .padding()
                Spacer()
            } else {
                List {
                    ForEach(engine.dictionaryEntries) { entry in
                        DictEntryRow(
                            entry: entry,
                            onEdit: { editingEntry = entry },
                            onMask: {
                                engine.maskDictEntry(fromText: entry.fromText,
                                                     masked: !entry.masked)
                            },
                            onDelete: {
                                engine.deleteDictEntry(fromText: entry.fromText)
                            }
                        )
                    }

                    // Load more button
                    if engine.dictionaryEntries.count < engine.dictionaryTotalCount
                        && activeSearch.isEmpty {
                        Button(action: {
                            engine.loadDictionary(
                                langTag: langFilter,
                                subType: selectedType,
                                offset: loadedCount,
                                limit: 100,
                                append: true
                            )
                            loadedCount += 100
                        }) {
                            HStack {
                                Spacer()
                                Text("Load more")
                                Spacer()
                            }
                        }
                        .accessibilityLabel("Load more entries, showing \(engine.dictionaryEntries.count) of \(engine.dictionaryTotalCount)")
                    }
                }
                .listStyle(.plain)
            }
        }
        .onAppear {
            if !engineStarted {
                if engine.start() { engineStarted = true }
            }
            if engineStarted {
                engine.loadEditorLanguages()
                engine.loadDictTypes()
            }
            // Default language from system locale if not yet set.
            if langFilter.isEmpty && !engine.editorLanguages.isEmpty {
                let engineLang = engine.currentEngineLangTag()
                if engine.editorLanguages.contains(engineLang) {
                    langFilter = engineLang
                } else {
                    // Try system locale (e.g. "es" from "es-ES").
                    let sysLang = Locale.current.language.languageCode?.identifier ?? ""
                    let match = engine.editorLanguages.first(where: { $0.hasPrefix(sysLang) })
                    langFilter = match ?? engine.editorLanguages.first ?? ""
                }
            }
        }
        .onChange(of: engine.editorLanguages) { langs in
            // Default language to engine's current language
            if langFilter.isEmpty && !langs.isEmpty {
                let engineLang = engine.currentEngineLangTag()
                langFilter = langs.contains(engineLang) ? engineLang : (langs.first ?? "")
            }
        }
        .onChange(of: engine.dictTypes) { types in
            // Default type to "pronounce" if available
            if selectedType.isEmpty && !types.isEmpty {
                let pronounce = types.first(where: { $0.type == "pronounce" })
                selectedType = pronounce?.type ?? (types.first?.type ?? "")
            }
        }
        .onChange(of: selectedType) { _ in reloadEntries() }
        .onChange(of: langFilter) { _ in reloadEntries() }
        .onChange(of: activeSearch) { _ in reloadEntries() }
        .task(id: searchQuery) {
            if searchQuery.isEmpty {
                activeSearch = ""
                return
            }
            try? await Task.sleep(nanoseconds: 300_000_000) // 300ms debounce
            if !Task.isCancelled {
                activeSearch = searchQuery
            }
        }
        .sheet(isPresented: $showAddSheet) {
            DictEntrySheet(
                title: "Add Entry",
                dictType: selectedType
            ) { from, to, cat in
                engine.addDictEntry(fromText: from, toText: to, category: cat)
            }
        }
        .sheet(item: $editingEntry) { entry in
            DictEntrySheet(
                title: "Edit Entry",
                dictType: selectedType,
                initialFrom: entry.fromText,
                initialTo: entry.toText,
                initialCategory: entry.category,
                isEdit: true
            ) { from, to, cat in
                if from != entry.fromText {
                    engine.deleteDictEntry(fromText: entry.fromText)
                }
                engine.addDictEntry(fromText: from, toText: to, category: cat)
            }
        }
        .alert("Remove changed entries", isPresented: $showRemoveConfirm) {
            Button("Remove", role: .destructive) {
                let userEntries = engine.dictionaryEntries.filter { $0.source == "user" }
                for e in userEntries {
                    engine.deleteDictEntry(fromText: e.fromText)
                }
                let maskedEntries = engine.dictionaryEntries.filter { $0.source == "main" && $0.masked }
                for e in maskedEntries {
                    engine.maskDictEntry(fromText: e.fromText, masked: false)
                }
                statusMessage = "Removed \(userEntries.count + maskedEntries.count) user changes"
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This will remove all user overrides for \(dictTypeLabel(selectedType)) in \(langFilter). This cannot be undone.")
        }
        .sheet(isPresented: $showExcludeSheet) {
            ExcludeDictionariesSheet(engine: engine, langTag: langFilter)
        }
        .fileExporter(
            isPresented: $showExportPicker,
            document: DictTsvDocument(entries: engine.dictionaryEntries, userOnly: exportUserOnly),
            contentType: .tabSeparatedText,
            defaultFilename: exportUserOnly
                ? "\(selectedType)_\(langFilter)_changed.tsv"
                : "\(selectedType)_\(langFilter).tsv"
        ) { result in
            if case .success = result {
                statusMessage = exportUserOnly ? "Exported changed entries" : "Exported all entries"
            } else if case .failure(let error) = result {
                statusMessage = "Export failed: \(error.localizedDescription)"
            }
        }
        .fileImporter(
            isPresented: $showImportPicker,
            allowedContentTypes: [.tabSeparatedText, .plainText, .data],
            allowsMultipleSelection: false
        ) { result in
            if case .success(let urls) = result, let url = urls.first {
                importDictTsv(from: url)
            }
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

    private func reloadEntries() {
        guard !langFilter.isEmpty, !selectedType.isEmpty else { return }
        // Re-apply user overrides so they show up in the query results.
        engine.reapplyDictOverrides(langFilter)
        engine.loadDictTypes(langTag: langFilter)
        engine.loadDictionary(
            langTag: langFilter,
            subType: selectedType,
            offset: 0,
            limit: 100,
            search: activeSearch
        )
        loadedCount = 100
    }

    private func exportDictToTempFile(userOnly: Bool) -> URL? {
        let entries = userOnly
            ? engine.dictionaryEntries.filter { $0.source == "user" }
            : engine.dictionaryEntries.filter { !$0.masked }
        if entries.isEmpty { return nil }
        var tsv = ""
        for e in entries {
            tsv += "\(e.fromText)\t\(e.toText)\n"
        }
        let filename = userOnly
            ? "\(selectedType)_\(langFilter)_changed.tsv"
            : "\(selectedType)_\(langFilter).tsv"
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent(filename)
        guard let _ = try? tsv.write(to: url, atomically: true, encoding: .utf8) else {
            return nil
        }
        return url
    }

    private func importDictTsv(from url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        guard let content = try? String(contentsOf: url, encoding: .utf8) else {
            statusMessage = "Could not read file"
            return
        }
        if content.isEmpty {
            statusMessage = "File is empty"
            return
        }
        var count = 0
        for line in content.components(separatedBy: .newlines) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") { continue }
            let parts = trimmed.components(separatedBy: "\t")
            if parts.count >= 2 {
                let word = parts[0].trimmingCharacters(in: .whitespaces)
                let value = parts[1].trimmingCharacters(in: .whitespaces)
                if !word.isEmpty && !value.isEmpty {
                    engine.addDictEntry(fromText: word, toText: value)
                    count += 1
                }
            }
        }
        statusMessage = "Imported \(count) entries"
    }
}

// MARK: - Entry Row

private struct DictEntryRow: View {
    let entry: TgsbEngine.DictEntry
    let onEdit: () -> Void
    let onMask: () -> Void
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("\(entry.fromText) \u{2192} \(entry.toText)")
                .foregroundColor(entry.masked ? .secondary : .primary)

            HStack(spacing: 4) {
                Text(entry.source)
                    .font(.caption2)
                    .foregroundColor(.secondary)
                if !entry.category.isEmpty {
                    Text("\u{00B7} \(entry.category)")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                }
                if entry.masked {
                    Text("\u{00B7} excluded")
                        .font(.caption2)
                        .foregroundColor(.red)
                }
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { onEdit() }
        .swipeActions(edge: .trailing) {
            if entry.source == "user" {
                Button("Delete", role: .destructive) { onDelete() }
            }
            if entry.source == "main" {
                Button(entry.masked ? "Include" : "Exclude") { onMask() }
                    .tint(entry.masked ? .green : .orange)
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(
            "\(entry.fromText) maps to \(entry.toText), " +
            "\(entry.source) dictionary" +
            (entry.masked ? ", excluded" : "") +
            (entry.category.isEmpty ? "" : ", \(entry.category)")
        )
        .accessibilityAction(named: "Edit") { onEdit() }
        .accessibilityAddTraits(.isButton)
        .accessibilityHint("Double tap to edit")
    }
}

// MARK: - Add / Edit Sheet

private struct DictEntrySheet: View {
    let title: String
    let dictType: String
    @State var fromText: String = ""
    @State var toText: String = ""
    @State var category: String = ""
    @State var caseSensitive: Bool = false
    var isEdit: Bool = false
    let onSave: (String, String, String) -> Void
    @Environment(\.dismiss) private var dismiss

    init(title: String, dictType: String, initialFrom: String = "",
         initialTo: String = "", initialCategory: String = "",
         isEdit: Bool = false,
         onSave: @escaping (String, String, String) -> Void) {
        self.title = title
        self.dictType = dictType
        self._fromText = State(initialValue: initialFrom)
        self._toText = State(initialValue: initialTo)
        self._category = State(initialValue: initialCategory)
        self._caseSensitive = State(initialValue: initialFrom != initialFrom.lowercased())
        self.isEdit = isEdit
        self.onSave = onSave
    }

    private var toLabel: String {
        switch dictType {
        case "stress":    return "Stress pattern"
        case "compound":  return "Split as"
        case "character": return "Description"
        default:          return "Pronounce as"
        }
    }

    private var toHelperText: String? {
        switch dictType {
        case "stress":    return "Space-separated digits: 1 = primary, 2 = secondary, 0 = none"
        case "compound":  return "Space-separated parts, e.g. \"lock box\""
        case "character": return "How this character is spoken, e.g. \"a acentuada\""
        default:          return nil
        }
    }

    private var fromLabel: String {
        dictType == "character" ? "Symbol" : "Word"
    }

    private var showCategory: Bool {
        return dictType != "stress" && dictType != "compound" && dictType != "character"
    }

    var body: some View {
        NavigationView {
            Form {
                Section(fromLabel) {
                    TextField(fromLabel, text: $fromText)
                        .autocorrectionDisabled()
                        .accessibilityLabel(fromLabel)
                        .onChange(of: fromText) { newValue in
                            if dictType == "character" {
                                let stripped = newValue.replacingOccurrences(of: " ", with: "")
                                if stripped != newValue { fromText = stripped }
                            }
                        }
                }
                Section(toLabel) {
                    TextField(toLabel, text: $toText)
                        .autocorrectionDisabled()
                    if let helper = toHelperText {
                        Text(helper)
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                if showCategory {
                    Section("Category (optional)") {
                        TextField("Category", text: $category)
                            .autocorrectionDisabled()
                    }
                }
                if dictType == "pronounce" {
                    Section {
                        Toggle("Match capitalization", isOn: $caseSensitive)
                    }
                }
            }
            .navigationTitle(title)
#if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
#endif
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        var word = fromText.trimmingCharacters(in: .whitespaces)
                        if !caseSensitive && dictType == "pronounce" {
                            word = word.lowercased()
                        }
                        onSave(
                            word,
                            toText.trimmingCharacters(in: .whitespaces),
                            category.trimmingCharacters(in: .whitespaces)
                        )
                        dismiss()
                    }
                    .disabled(
                        fromText.trimmingCharacters(in: .whitespaces).isEmpty ||
                        toText.trimmingCharacters(in: .whitespaces).isEmpty
                    )
                }
            }
        }
#if os(iOS)
        .presentationDetents([.medium])
#endif
    }
}

// MARK: - Exclude Dictionaries Sheet

private struct ExcludeDictionariesSheet: View {
    @ObservedObject var engine: TgsbEngine
    let langTag: String
    @Environment(\.dismiss) private var dismiss
    @State private var config: [(type: String, enabled: Bool)] = []

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Text("Uncheck a dictionary type to exclude it for \(langTag).")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Section {
                    ForEach(config.indices, id: \.self) { idx in
                        Toggle(dictTypeLabel(config[idx].type), isOn: Binding(
                            get: { config[idx].enabled },
                            set: { newVal in
                                config[idx].enabled = newVal
                                engine.setDictTypeEnabled(
                                    langTag: langTag,
                                    type: config[idx].type,
                                    enabled: newVal
                                )
                            }
                        ))
                    }
                }
            }
            .navigationTitle("Exclude dictionaries")
#if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
#endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .onAppear {
            config = engine.loadDictConfig(langTag: langTag)
                .sorted(by: { $0.type < $1.type })
        }
#if os(iOS)
        .presentationDetents([.medium])
#endif
    }
}

// MARK: - TSV File Document for Export

struct DictTsvDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.tabSeparatedText, .plainText] }

    let content: String

    init(entries: [TgsbEngine.DictEntry], userOnly: Bool) {
        let filtered = userOnly
            ? entries.filter { $0.source == "user" }
            : entries.filter { !$0.masked }
        var tsv = ""
        for e in filtered {
            tsv += "\(e.fromText)\t\(e.toText)\n"
        }
        content = tsv
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
