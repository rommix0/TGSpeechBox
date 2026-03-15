/*
 * DictionaryEditorScreen -- Dictionary editor with type selection.
 *
 * Supports three dictionary sub-types (pronounce, stress, compound)
 * loaded from the C++ data query API. Each type has a type-specific
 * add dialog. Paginated loading (100 entries at a time).
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.content.Context
import android.content.Intent
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import java.util.Locale

/** Human-readable label for a dict type. */
private fun dictTypeLabel(type: String): String = when (type) {
    "compound" -> "Compound"
    "pronounce" -> "Pronunciation"
    "stress" -> "Stress"
    "character" -> "Characters"
    else -> type.replaceFirstChar { if (it.isLowerCase()) it.titlecase(Locale.ROOT) else it.toString() }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun DictionaryListScreen(viewModel: TgsbViewModel) {
    val context = LocalContext.current

    val types by viewModel.dictTypes.collectAsState()
    val entries by viewModel.dictionaryEntries.collectAsState()
    val totalCount by viewModel.dictionaryTotalCount.collectAsState()
    val langs by viewModel.editorLanguages.collectAsState()

    var selectedType by rememberSaveable { mutableStateOf("") }
    var langFilter by rememberSaveable { mutableStateOf("") }
    var loadedCount by remember { mutableIntStateOf(0) }
    var showAddDialog by remember { mutableStateOf(false) }
    var editingEntry by remember { mutableStateOf<TgsbViewModel.DictEntry?>(null) }
    var contextMenuEntry by remember { mutableStateOf<String?>(null) }

    // Search state
    var searchQuery by rememberSaveable { mutableStateOf("") }
    var activeSearch by remember { mutableStateOf("") }

    // More options menu state
    var showMoreMenu by remember { mutableStateOf(false) }
    var showRemoveConfirm by remember { mutableStateOf(false) }
    var showExcludeDialog by remember { mutableStateOf(false) }
    var pendingExportUserOnly by remember { mutableStateOf(true) }

    // Export launcher (file picker → save TSV)
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("text/tab-separated-values")
    ) { uri ->
        if (uri != null) {
            try {
                val exportEntries = if (pendingExportUserOnly) {
                    entries.filter { it.source == "user" }
                } else {
                    entries.filter { !it.masked }
                }
                val tsv = buildString {
                    for (e in exportEntries) append("${e.fromText}\t${e.toText}\n")
                }
                context.contentResolver.openOutputStream(uri)?.use {
                    it.write(tsv.toByteArray())
                }
                Toast.makeText(context, "Exported ${exportEntries.size} entries", Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                Toast.makeText(context, "Export failed: ${e.message}", Toast.LENGTH_SHORT).show()
            }
        }
    }

    // Import launcher (pronunciation + character types)
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            try {
                val content = context.contentResolver.openInputStream(uri)?.use {
                    it.bufferedReader().readText()
                }
                if (content.isNullOrBlank()) {
                    Toast.makeText(context, "File is empty", Toast.LENGTH_SHORT).show()
                    return@rememberLauncherForActivityResult
                }
                var count = 0
                for (line in content.lines()) {
                    val trimmed = line.trim()
                    if (trimmed.isEmpty() || trimmed.startsWith("#")) continue
                    val parts = trimmed.split("\t", limit = 2)
                    if (parts.size == 2 && parts[0].isNotBlank() && parts[1].isNotBlank()) {
                        viewModel.addDictEntry(parts[0].trim(), parts[1].trim())
                        count++
                    }
                }
                Toast.makeText(context, "Imported $count entries", Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                Toast.makeText(context, "Import failed: ${e.message}", Toast.LENGTH_SHORT).show()
            }
        }
    }

    // Load types and languages on first composition
    LaunchedEffect(Unit) {
        viewModel.loadEditorLanguages()
        viewModel.loadDictTypes()
    }

    // Default language to engine's current language once languages are loaded
    LaunchedEffect(langs) {
        if (langFilter.isEmpty() && langs.isNotEmpty()) {
            val engineLang = viewModel.currentEngineLangTag()
            langFilter = if (engineLang in langs) engineLang
                         else {
                             // Fall back to system locale prefix match.
                             val sysLang = Locale.getDefault().language
                             langs.firstOrNull { it.startsWith(sysLang) } ?: langs.firstOrNull() ?: ""
                         }
        }
    }

    // Default type to first available once types are loaded
    LaunchedEffect(types) {
        if (selectedType.isEmpty() && types.isNotEmpty()) {
            // Prefer "pronounce" if available, else first
            val pronounce = types.find { it.type == "pronounce" }
            selectedType = pronounce?.type ?: types.first().type
        }
    }

    // Reload types and entries when language changes
    LaunchedEffect(langFilter) {
        if (langFilter.isNotEmpty()) viewModel.loadDictTypes(langFilter)
    }

    // Reload entries when type, language, or active search changes
    LaunchedEffect(selectedType, langFilter, activeSearch) {
        if (selectedType.isNotEmpty() && langFilter.isNotEmpty()) {
            viewModel.loadDictionary(langFilter, selectedType, offset = 0, limit = 100, search = activeSearch)
            loadedCount = 100
        }
    }

    // Debounce search input
    LaunchedEffect(searchQuery) {
        if (searchQuery.isEmpty()) {
            activeSearch = ""
            return@LaunchedEffect
        }
        delay(300)
        activeSearch = searchQuery
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Type dropdown + Language dropdown row
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            // Type dropdown
            var typeExpanded by remember { mutableStateOf(false) }
            Text("Type:", style = MaterialTheme.typography.labelMedium)
            Spacer(modifier = Modifier.width(4.dp))
            Box {
                OutlinedButton(
                    onClick = { typeExpanded = true },
                    modifier = Modifier.clearAndSetSemantics {
                        contentDescription = buildString {
                            append("Type: ")
                            append(if (selectedType.isEmpty()) "Select type" else dictTypeLabel(selectedType))
                            if (selectedType.isNotEmpty()) append(", $totalCount entries")
                            append(", dropdown button")
                        }
                    }
                ) {
                    val label = if (selectedType.isEmpty()) "Select type"
                    else "${dictTypeLabel(selectedType)} ($totalCount)"
                    Text(label)
                }
                DropdownMenu(expanded = typeExpanded, onDismissRequest = { typeExpanded = false }) {
                    for (dt in types) {
                        DropdownMenuItem(
                            text = { Text("${dictTypeLabel(dt.type)} (${dt.count})") },
                            onClick = {
                                selectedType = dt.type
                                typeExpanded = false
                            }
                        )
                    }
                }
            }

            // Language dropdown
            var langExpanded by remember { mutableStateOf(false) }
            Box {
                OutlinedButton(
                    onClick = { langExpanded = true },
                    modifier = Modifier.clearAndSetSemantics {
                        contentDescription = "Language: ${if (langFilter.isEmpty()) "Select language" else langFilter}, dropdown button"
                    }
                ) {
                    Text(if (langFilter.isEmpty()) "Select language" else langFilter)
                }
                DropdownMenu(expanded = langExpanded, onDismissRequest = { langExpanded = false }) {
                    for (lang in langs) {
                        DropdownMenuItem(
                            text = { Text(lang) },
                            onClick = { langFilter = lang; langExpanded = false }
                        )
                    }
                }
            }
        }

        // Search field
        OutlinedTextField(
            value = searchQuery,
            onValueChange = { searchQuery = it },
            label = { Text("Search") },
            singleLine = true,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 4.dp)
        )

        // Entry count + Add button + More options row
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            val showingCount = entries.size
            Text(
                "Showing $showingCount of $totalCount entries",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(modifier = Modifier.weight(1f))

            IconButton(
                onClick = { showAddDialog = true },
                modifier = Modifier.semantics {
                    contentDescription = "Add entry"
                }
            ) {
                Icon(Icons.Default.Add, contentDescription = null)
            }

            // More options menu
            Box {
                IconButton(
                    onClick = { showMoreMenu = true },
                    modifier = Modifier.semantics {
                        contentDescription = "More options"
                    }
                ) {
                    Icon(Icons.Default.MoreVert, contentDescription = null)
                }
                DropdownMenu(
                    expanded = showMoreMenu,
                    onDismissRequest = { showMoreMenu = false }
                ) {
                    // Export all (file picker — pronunciation + character)
                    if (selectedType == "pronounce" || selectedType == "character") {
                        DropdownMenuItem(
                            text = { Text("Export all") },
                            onClick = {
                                showMoreMenu = false
                                pendingExportUserOnly = false
                                exportLauncher.launch("${selectedType}_${langFilter}.tsv")
                            }
                        )
                    }
                    // Export changed (file picker)
                    DropdownMenuItem(
                        text = { Text("Export changed") },
                        onClick = {
                            showMoreMenu = false
                            pendingExportUserOnly = true
                            exportLauncher.launch("${selectedType}_${langFilter}_changed.tsv")
                        }
                    )
                    // Share all (share sheet — pronunciation + character)
                    if (selectedType == "pronounce" || selectedType == "character") {
                        DropdownMenuItem(
                            text = { Text("Share all") },
                            onClick = {
                                showMoreMenu = false
                                shareDictEntries(context, entries, selectedType, langFilter, userOnly = false)
                            }
                        )
                    }
                    // Share changed (share sheet)
                    DropdownMenuItem(
                        text = { Text("Share changed") },
                        onClick = {
                            showMoreMenu = false
                            shareDictEntries(context, entries, selectedType, langFilter, userOnly = true)
                        }
                    )
                    // Import (pronunciation + character only)
                    DropdownMenuItem(
                        text = { Text("Import") },
                        enabled = selectedType == "pronounce" || selectedType == "character",
                        onClick = {
                            showMoreMenu = false
                            importLauncher.launch(arrayOf("*/*"))
                        }
                    )
                    // Remove duplicates
                    DropdownMenuItem(
                        text = { Text("Remove duplicates") },
                        onClick = {
                            showMoreMenu = false
                            val mainKeys = entries.filter { it.source == "main" }.map { it.fromText.lowercase() }.toSet()
                            val duplicates = entries.filter { it.source == "user" && it.fromText.lowercase() in mainKeys }
                            if (duplicates.size > 500) {
                                Toast.makeText(context, "Too many duplicates (${duplicates.size}), remove manually", Toast.LENGTH_SHORT).show()
                            } else if (duplicates.isEmpty()) {
                                Toast.makeText(context, "No duplicates found", Toast.LENGTH_SHORT).show()
                            } else {
                                for (d in duplicates) viewModel.deleteDictEntry(d.fromText)
                                Toast.makeText(context, "Removed ${duplicates.size} duplicates", Toast.LENGTH_SHORT).show()
                            }
                        }
                    )
                    // Remove changed entries
                    DropdownMenuItem(
                        text = { Text("Remove changed entries") },
                        onClick = {
                            showMoreMenu = false
                            showRemoveConfirm = true
                        }
                    )
                    // Exclude dictionaries
                    DropdownMenuItem(
                        text = { Text("Exclude dictionaries") },
                        onClick = {
                            showMoreMenu = false
                            showExcludeDialog = true
                        }
                    )
                }
            }
        }

        if (entries.isEmpty() && langFilter.isNotEmpty() && selectedType.isNotEmpty()) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    if (activeSearch.isNotEmpty()) {
                        Text(
                            "No matches for \"$activeSearch\"",
                            style = MaterialTheme.typography.bodyMedium
                        )
                    } else {
                        Text(
                            "No ${dictTypeLabel(selectedType).lowercase()} entries yet",
                            style = MaterialTheme.typography.titleMedium
                        )
                        Spacer(modifier = Modifier.height(4.dp))
                        Text(
                            "Want to be the first to add one? Tap the + (add) button above.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        } else {
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                items(entries, key = { "${it.source}:${it.fromText}" }) { entry ->
                    Box {
                        Surface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .combinedClickable(
                                    onClick = { editingEntry = entry },
                                    onLongClick = { contextMenuEntry = entry.fromText }
                                )
                                .semantics {
                                    contentDescription = buildString {
                                        append("${entry.fromText} maps to ${entry.toText}")
                                        append(", ${entry.source} dictionary")
                                        if (entry.masked) append(", masked")
                                        if (entry.category.isNotEmpty()) append(", ${entry.category}")
                                    }
                                    customActions = buildList {
                                        add(CustomAccessibilityAction("Edit") {
                                            editingEntry = entry; true
                                        })
                                        if (entry.source == "main") {
                                            add(CustomAccessibilityAction(
                                                if (entry.masked) "Include" else "Exclude"
                                            ) {
                                                viewModel.maskDictEntry(entry.fromText, !entry.masked); true
                                            })
                                        }
                                        if (entry.source == "user") {
                                            add(CustomAccessibilityAction("Delete") {
                                                viewModel.deleteDictEntry(entry.fromText); true
                                            })
                                        }
                                    }
                                },
                            tonalElevation = if (entry.masked) 0.dp else 1.dp
                        ) {
                            Row(
                                modifier = Modifier
                                    .padding(16.dp)
                                    .fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(
                                        text = "${entry.fromText} \u2192 ${entry.toText}",
                                        style = MaterialTheme.typography.bodyLarge,
                                        color = if (entry.masked)
                                            MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                                        else
                                            MaterialTheme.colorScheme.onSurface
                                    )
                                    Row {
                                        Text(
                                            text = entry.source,
                                            style = MaterialTheme.typography.labelSmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                        if (entry.category.isNotEmpty()) {
                                            Text(
                                                text = " \u00B7 ${entry.category}",
                                                style = MaterialTheme.typography.labelSmall,
                                                color = MaterialTheme.colorScheme.onSurfaceVariant
                                            )
                                        }
                                        if (entry.masked) {
                                            Text(
                                                text = " \u00B7 masked",
                                                style = MaterialTheme.typography.labelSmall,
                                                color = MaterialTheme.colorScheme.error
                                            )
                                        }
                                    }
                                }
                            }
                        }

                        // Long-press context menu
                        DropdownMenu(
                            expanded = contextMenuEntry == entry.fromText,
                            onDismissRequest = { contextMenuEntry = null }
                        ) {
                            DropdownMenuItem(
                                text = { Text("Edit") },
                                onClick = {
                                    editingEntry = entry
                                    contextMenuEntry = null
                                }
                            )
                            if (entry.source == "main") {
                                DropdownMenuItem(
                                    text = { Text(if (entry.masked) "Include" else "Exclude") },
                                    onClick = {
                                        viewModel.maskDictEntry(entry.fromText, !entry.masked)
                                        contextMenuEntry = null
                                    }
                                )
                            }
                            if (entry.source == "user") {
                                DropdownMenuItem(
                                    text = { Text("Delete") },
                                    onClick = {
                                        viewModel.deleteDictEntry(entry.fromText)
                                        contextMenuEntry = null
                                    }
                                )
                            }
                        }
                    }
                    HorizontalDivider()
                }

                // "Load more" button when there are more entries and no active search
                if (entries.size < totalCount && activeSearch.isEmpty()) {
                    item {
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            OutlinedButton(
                                onClick = {
                                    viewModel.loadDictionary(
                                        langFilter, selectedType,
                                        offset = loadedCount, limit = 100,
                                        append = true
                                    )
                                    loadedCount += 100
                                },
                                modifier = Modifier.semantics {
                                    contentDescription = "Load more entries, showing ${entries.size} of $totalCount"
                                }
                            ) {
                                Text("Load more")
                            }
                        }
                    }
                }
            }
        }
    }

    // Add dialog (type-specific)
    if (showAddDialog) {
        DictAddDialog(
            dictType = selectedType,
            onDismiss = { showAddDialog = false },
            onConfirm = { from, to, cat ->
                viewModel.addDictEntry(from, to, cat)
                showAddDialog = false
            }
        )
    }

    // Edit dialog
    if (editingEntry != null) {
        val entry = editingEntry!!
        DictEditDialog(
            dictType = selectedType,
            entry = entry,
            onDismiss = { editingEntry = null },
            onConfirm = { from, to, cat ->
                if (from != entry.fromText) viewModel.deleteDictEntry(entry.fromText)
                viewModel.addDictEntry(from, to, cat)
                editingEntry = null
            }
        )
    }

    // Exclude dictionaries dialog
    if (showExcludeDialog) {
        ExcludeDictionariesDialog(
            langTag = langFilter,
            viewModel = viewModel,
            onDismiss = { showExcludeDialog = false }
        )
    }

    // Remove changed entries confirmation dialog
    if (showRemoveConfirm) {
        AlertDialog(
            onDismissRequest = { showRemoveConfirm = false },
            title = { Text("Remove changed entries") },
            text = {
                Text("This will remove all user overrides for ${dictTypeLabel(selectedType)} in $langFilter. This cannot be undone.")
            },
            confirmButton = {
                TextButton(onClick = {
                    showRemoveConfirm = false
                    // Delete all user entries
                    val userEntries = entries.filter { it.source == "user" }
                    for (e in userEntries) {
                        viewModel.deleteDictEntry(e.fromText)
                    }
                    // Also remove any mask overrides (re-include masked main entries)
                    val maskedEntries = entries.filter { it.source == "main" && it.masked }
                    for (e in maskedEntries) {
                        viewModel.maskDictEntry(e.fromText, false)
                    }
                    Toast.makeText(
                        context,
                        "Removed ${userEntries.size + maskedEntries.size} user changes",
                        Toast.LENGTH_SHORT
                    ).show()
                }) { Text("Remove") }
            },
            dismissButton = {
                TextButton(onClick = { showRemoveConfirm = false }) { Text("Cancel") }
            }
        )
    }
}

/** Share dictionary entries as TSV via share sheet. */
private fun shareDictEntries(
    context: Context,
    entries: List<TgsbViewModel.DictEntry>,
    dictType: String,
    langFilter: String,
    userOnly: Boolean = true
) {
    val shareEntries = if (userOnly) {
        entries.filter { it.source == "user" }
    } else {
        entries.filter { !it.masked }
    }

    if (shareEntries.isEmpty()) {
        Toast.makeText(context, if (userOnly) "No changed entries to share" else "No entries to share", Toast.LENGTH_SHORT).show()
        return
    }

    val tsv = buildString {
        for (e in shareEntries) {
            append("${e.fromText}\t${e.toText}\n")
        }
    }

    val intent = Intent(Intent.ACTION_SEND).apply {
        type = "text/tab-separated-values"
        putExtra(Intent.EXTRA_SUBJECT, "${dictType}_${langFilter}.tsv")
        putExtra(Intent.EXTRA_TEXT, tsv)
    }
    context.startActivity(Intent.createChooser(intent, "Export ${dictTypeLabel(dictType)} entries"))
}

@Composable
private fun DictAddDialog(
    dictType: String,
    onDismiss: () -> Unit,
    onConfirm: (from: String, to: String, category: String) -> Unit
) {
    var fromText by rememberSaveable { mutableStateOf("") }
    var toText by rememberSaveable { mutableStateOf("") }
    var category by rememberSaveable { mutableStateOf("") }

    val title = "Add Entry"
    val toLabel: String
    val toHelper: String?
    val showCategory: Boolean

    when (dictType) {
        "stress" -> {
            toLabel = "Stress pattern"
            toHelper = "Space-separated digits: 1 = primary, 2 = secondary, 0 = none"
            showCategory = false
        }
        "compound" -> {
            toLabel = "Split as"
            toHelper = "Space-separated parts, e.g. \"lock box\""
            showCategory = false
        }
        "character" -> {
            toLabel = "Description"
            toHelper = "How this character is spoken, e.g. \"a acentuada\""
            showCategory = false
        }
        else -> { // "pronounce" or unknown
            toLabel = "Pronounce as"
            toHelper = null
            showCategory = true
        }
    }

    val fromLabel = if (dictType == "character") "Symbol" else "Word"

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = fromText,
                    onValueChange = { v ->
                        fromText = if (dictType == "character") v.replace(" ", "") else v
                    },
                    label = { Text(fromLabel) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = toText,
                    onValueChange = { toText = it },
                    label = { Text(toLabel) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                if (toHelper != null) {
                    Text(
                        text = toHelper,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                if (showCategory) {
                    OutlinedTextField(
                        value = category,
                        onValueChange = { category = it },
                        label = { Text("Category (optional)") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(fromText.trim(), toText.trim(), category.trim()) },
                enabled = fromText.isNotBlank() && toText.isNotBlank()
            ) { Text("Save") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun DictEditDialog(
    dictType: String,
    entry: TgsbViewModel.DictEntry,
    onDismiss: () -> Unit,
    onConfirm: (from: String, to: String, category: String) -> Unit
) {
    var fromText by rememberSaveable { mutableStateOf(entry.fromText) }
    var toText by rememberSaveable { mutableStateOf(entry.toText) }
    var category by rememberSaveable { mutableStateOf(entry.category) }

    val title = "Edit Entry"
    val toLabel: String
    val showCategory: Boolean

    when (dictType) {
        "stress" -> {
            toLabel = "Stress pattern"
            showCategory = false
        }
        "compound" -> {
            toLabel = "Split as"
            showCategory = false
        }
        "character" -> {
            toLabel = "Description"
            showCategory = false
        }
        else -> {
            toLabel = "Pronounce as"
            showCategory = true
        }
    }

    val fromLabel = if (dictType == "character") "Symbol" else "Word"

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = fromText,
                    onValueChange = { fromText = it },
                    label = { Text(fromLabel) },
                    singleLine = true,
                    enabled = false,
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = toText,
                    onValueChange = { toText = it },
                    label = { Text(toLabel) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                if (showCategory) {
                    OutlinedTextField(
                        value = category,
                        onValueChange = { category = it },
                        label = { Text("Category (optional)") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(fromText.trim(), toText.trim(), category.trim()) },
                enabled = fromText.isNotBlank() && toText.isNotBlank()
            ) { Text("Save") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun ExcludeDictionariesDialog(
    langTag: String,
    viewModel: TgsbViewModel,
    onDismiss: () -> Unit
) {
    // Load current config on show
    val config = remember(langTag) { mutableStateMapOf<String, Boolean>() }
    LaunchedEffect(langTag) {
        config.clear()
        config.putAll(viewModel.loadDictConfig(langTag))
    }

    // Stable order: character, compound, pronounce, stress
    val sortedTypes = config.keys.sorted()

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Exclude dictionaries") },
        text = {
            Column {
                Text(
                    "Uncheck a dictionary type to exclude it for $langTag.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                for (type in sortedTypes) {
                    val enabled = config[type] ?: true
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 2.dp)
                    ) {
                        Checkbox(
                            checked = enabled,
                            onCheckedChange = { checked ->
                                config[type] = checked
                                viewModel.setDictTypeEnabled(langTag, type, checked)
                            }
                        )
                        Text(
                            text = dictTypeLabel(type),
                            style = MaterialTheme.typography.bodyLarge,
                            modifier = Modifier.padding(start = 8.dp)
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Done") }
        }
    )
}
