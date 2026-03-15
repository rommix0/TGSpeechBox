/*
 * DictionaryEditorView -- Word pronunciation dictionary editor.
 *
 * Lists dictionary entries with language filter, supports add/edit/delete
 * for user entries and mask/unmask for built-in entries.
 * Changes stored in App Group UserDefaults, re-applied after setLanguage.
 *
 * License: GPL-3.0
 */

import SwiftUI

// MARK: - Dictionary List

struct DictionaryEditorView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool
    @State private var langFilter: String = ""
    @State private var showAddSheet = false
    @State private var editingEntry: TgsbEngine.DictEntry? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            // Language filter + count + add button
            HStack {
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
                .accessibilityLabel("Filter by language, \(langFilter.isEmpty ? "none selected" : langFilter)")

                Spacer()

                Text("\(engine.dictionaryTotalCount) entries")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Button(action: { showAddSheet = true }) {
                    Image(systemName: "plus")
                        .font(.body)
                }
                .accessibilityLabel("Add dictionary entry")
                .disabled(langFilter.isEmpty)
            }
            .padding(.horizontal)
            .padding(.top, 8)

            if langFilter.isEmpty {
                Spacer()
                Text("Select a language to view dictionary entries.")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else if engine.dictionaryEntries.isEmpty {
                Spacer()
                Text("No dictionary entries for \(langFilter)")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else {
                List {
                    ForEach(engine.dictionaryEntries) { entry in
                        DictEntryRow(entry: entry, onEdit: {
                            if entry.source == "user" { editingEntry = entry }
                        }, onMask: {
                            engine.maskDictEntry(fromText: entry.fromText,
                                                 masked: !entry.masked)
                        }, onDelete: {
                            engine.deleteDictEntry(fromText: entry.fromText)
                        })
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
                if !langFilter.isEmpty {
                    engine.loadDictionary(langTag: langFilter)
                }
            }
        }
        .onChange(of: langFilter) { newLang in
            if !newLang.isEmpty {
                engine.loadDictionary(langTag: newLang)
            }
        }
        .sheet(isPresented: $showAddSheet) {
            DictEntrySheet(title: "Add Entry") { from, to, cat in
                engine.addDictEntry(fromText: from, toText: to, category: cat)
            }
        }
        .sheet(item: $editingEntry) { entry in
            DictEntrySheet(
                title: "Edit Entry",
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
                    Text("\u{00B7} masked")
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
                Button(entry.masked ? "Unmask" : "Mask") { onMask() }
                    .tint(entry.masked ? .green : .orange)
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(
            "\(entry.fromText) pronounced as \(entry.toText), " +
            "\(entry.source) dictionary" +
            (entry.masked ? ", masked" : "") +
            (entry.category.isEmpty ? "" : ", \(entry.category)")
        )
        .accessibilityAddTraits(entry.source == "user" ? .isButton : [])
        .accessibilityHint(entry.source == "user" ? "Double tap to edit" : "Swipe left for actions")
    }
}

// MARK: - Add / Edit Sheet

private struct DictEntrySheet: View {
    let title: String
    @State var fromText: String = ""
    @State var toText: String = ""
    @State var category: String = ""
    var isEdit: Bool = false
    let onSave: (String, String, String) -> Void
    @Environment(\.dismiss) private var dismiss

    init(title: String, initialFrom: String = "", initialTo: String = "",
         initialCategory: String = "", isEdit: Bool = false,
         onSave: @escaping (String, String, String) -> Void) {
        self.title = title
        self._fromText = State(initialValue: initialFrom)
        self._toText = State(initialValue: initialTo)
        self._category = State(initialValue: initialCategory)
        self.isEdit = isEdit
        self.onSave = onSave
    }

    var body: some View {
        NavigationView {
            Form {
                Section("Word") {
                    TextField("Word", text: $fromText)
                        .disabled(isEdit)
                        .autocorrectionDisabled()
                        .accessibilityLabel(isEdit ? "Word, \(fromText)" : "Word")
                }
                Section("Pronunciation") {
                    TextField("Pronounce as", text: $toText)
                        .autocorrectionDisabled()
                }
                Section("Category (optional)") {
                    TextField("Category", text: $category)
                        .autocorrectionDisabled()
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
                        onSave(
                            fromText.trimmingCharacters(in: .whitespaces),
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
