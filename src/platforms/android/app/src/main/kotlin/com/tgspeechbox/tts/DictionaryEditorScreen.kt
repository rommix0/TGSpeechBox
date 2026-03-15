/*
 * DictionaryEditorScreen -- Dictionary word pronunciation editor.
 *
 * List screen with language filter, add/edit/delete/mask entries.
 * Entries from the main dictionary can be masked; user entries can
 * be edited or deleted.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun DictionaryListScreen(viewModel: TgsbViewModel) {
    val entries by viewModel.dictionaryEntries.collectAsState()
    val totalCount by viewModel.dictionaryTotalCount.collectAsState()
    val langs by viewModel.editorLanguages.collectAsState()
    var langFilter by rememberSaveable { mutableStateOf("") }
    var showAddDialog by remember { mutableStateOf(false) }
    var editingEntry by remember { mutableStateOf<TgsbViewModel.DictEntry?>(null) }
    var contextMenuEntry by remember { mutableStateOf<String?>(null) }

    // Load on first composition and when language changes
    LaunchedEffect(langFilter) {
        if (langFilter.isEmpty() && langs.isNotEmpty()) {
            langFilter = langs.firstOrNull() ?: ""
        }
        if (langFilter.isNotEmpty()) {
            viewModel.loadDictionary(langFilter)
        }
    }

    // Also load languages on first composition
    LaunchedEffect(Unit) {
        viewModel.loadEditorLanguages()
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Language filter + count
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Language dropdown
            var expanded by remember { mutableStateOf(false) }
            Box {
                OutlinedButton(
                    onClick = { expanded = true },
                    modifier = Modifier.semantics {
                        contentDescription = "${if (langFilter.isEmpty()) "Select language" else langFilter}, dropdown menu"
                    }
                ) {
                    Text(if (langFilter.isEmpty()) "Select language" else langFilter)
                }
                DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                    for (lang in langs) {
                        DropdownMenuItem(
                            text = { Text(lang) },
                            onClick = { langFilter = lang; expanded = false }
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.weight(1f))

            Text("$totalCount entries", style = MaterialTheme.typography.bodySmall)

            Spacer(modifier = Modifier.width(8.dp))

            // Add button
            IconButton(onClick = { showAddDialog = true }) {
                Icon(Icons.Default.Add, contentDescription = "Add dictionary entry")
            }
        }

        if (entries.isEmpty() && langFilter.isNotEmpty()) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    "No dictionary entries for $langFilter",
                    style = MaterialTheme.typography.bodyMedium
                )
            }
        } else {
            LazyColumn(modifier = Modifier.fillMaxSize()) {
                items(entries, key = { it.fromText }) { entry ->
                    Box {
                        Surface(
                            modifier = Modifier
                                .fillMaxWidth()
                                .combinedClickable(
                                    onClick = {
                                        if (entry.source == "user") editingEntry = entry
                                    },
                                    onLongClick = { contextMenuEntry = entry.fromText }
                                )
                                .semantics {
                                    contentDescription = buildString {
                                        append("${entry.fromText} pronounced as ${entry.toText}")
                                        append(", ${entry.source} dictionary")
                                        if (entry.masked) append(", masked")
                                        if (entry.category.isNotEmpty()) append(", ${entry.category}")
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

                        // Context menu
                        DropdownMenu(
                            expanded = contextMenuEntry == entry.fromText,
                            onDismissRequest = { contextMenuEntry = null }
                        ) {
                            if (entry.source == "user") {
                                DropdownMenuItem(
                                    text = { Text("Edit") },
                                    onClick = {
                                        editingEntry = entry
                                        contextMenuEntry = null
                                    }
                                )
                                DropdownMenuItem(
                                    text = { Text("Delete") },
                                    onClick = {
                                        viewModel.deleteDictEntry(entry.fromText)
                                        contextMenuEntry = null
                                    }
                                )
                            }
                            if (entry.source == "main") {
                                DropdownMenuItem(
                                    text = { Text(if (entry.masked) "Unmask" else "Mask") },
                                    onClick = {
                                        viewModel.maskDictEntry(entry.fromText, !entry.masked)
                                        contextMenuEntry = null
                                    }
                                )
                            }
                        }
                    }
                    HorizontalDivider()
                }
            }
        }
    }

    // Add dialog
    if (showAddDialog) {
        DictEntryDialog(
            title = "Add Dictionary Entry",
            initialFrom = "",
            initialTo = "",
            initialCategory = "",
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
        DictEntryDialog(
            title = "Edit Dictionary Entry",
            initialFrom = entry.fromText,
            initialTo = entry.toText,
            initialCategory = entry.category,
            isEdit = true,
            onDismiss = { editingEntry = null },
            onConfirm = { from, to, cat ->
                // Delete old if fromText changed
                if (from != entry.fromText) viewModel.deleteDictEntry(entry.fromText)
                viewModel.addDictEntry(from, to, cat)
                editingEntry = null
            }
        )
    }
}

@Composable
fun DictEntryDialog(
    title: String,
    initialFrom: String,
    initialTo: String,
    initialCategory: String,
    isEdit: Boolean = false,
    onDismiss: () -> Unit,
    onConfirm: (from: String, to: String, category: String) -> Unit
) {
    var fromText by rememberSaveable { mutableStateOf(initialFrom) }
    var toText by rememberSaveable { mutableStateOf(initialTo) }
    var category by rememberSaveable { mutableStateOf(initialCategory) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = fromText,
                    onValueChange = { fromText = it },
                    label = { Text("Word") },
                    singleLine = true,
                    enabled = !isEdit,
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = toText,
                    onValueChange = { toText = it },
                    label = { Text("Pronounce as") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
                OutlinedTextField(
                    value = category,
                    onValueChange = { category = it },
                    label = { Text("Category (optional)") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
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
