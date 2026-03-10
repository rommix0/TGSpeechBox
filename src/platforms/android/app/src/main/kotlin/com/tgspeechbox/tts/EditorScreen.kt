/*
 * EditorScreen — Pack settings editor tab.
 *
 * Lets users view and override language pack settings.
 * Changes are stored in SharedPreferences and re-applied after setLanguage.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

@Composable
fun EditorScreen(viewModel: TgsbViewModel) {
    var selectedLang by remember { mutableStateOf<String?>(null) }

    if (selectedLang != null) {
        PackSettingsScreen(
            viewModel = viewModel,
            langTag = selectedLang!!,
            onBack = { selectedLang = null }
        )
    } else {
        LanguageListScreen(
            viewModel = viewModel,
            onLanguageSelected = { selectedLang = it }
        )
    }
}

@Composable
private fun LanguageListScreen(
    viewModel: TgsbViewModel,
    onLanguageSelected: (String) -> Unit
) {
    val langs by viewModel.editorLanguages.collectAsState()

    LaunchedEffect(Unit) {
        viewModel.loadEditorLanguages()
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        Text(
            text = stringResource(R.string.editor_packs_section),
            style = MaterialTheme.typography.headlineSmall,
            modifier = Modifier
                .padding(bottom = 8.dp)
                .semantics { heading() }
        )

        Text(
            text = stringResource(R.string.editor_select_language),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 16.dp)
        )

        Column(
            modifier = Modifier.verticalScroll(rememberScrollState())
        ) {
            for (lang in langs) {
                Surface(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onLanguageSelected(lang) },
                    tonalElevation = 1.dp,
                    shape = MaterialTheme.shapes.small
                ) {
                    Text(
                        text = lang,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.padding(16.dp)
                    )
                }
                Spacer(modifier = Modifier.height(4.dp))
            }
        }
    }
}

@Composable
private fun PackSettingsScreen(
    viewModel: TgsbViewModel,
    langTag: String,
    onBack: () -> Unit
) {
    val settings by viewModel.editorSettings.collectAsState()
    var editingKey by remember { mutableStateOf<String?>(null) }
    var editingValue by remember { mutableStateOf("") }
    var selectingKey by remember { mutableStateOf<String?>(null) }
    var selectingOptions by remember { mutableStateOf<List<String>>(emptyList()) }
    var showResetAll by remember { mutableStateOf(false) }

    LaunchedEffect(langTag) {
        viewModel.loadEditorSettings(langTag)
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Top bar with back button
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onBack) {
                Icon(
                    Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "Back"
                )
            }
            Text(
                text = langTag,
                style = MaterialTheme.typography.titleLarge,
                modifier = Modifier.weight(1f)
            )
            TextButton(onClick = { showResetAll = true }) {
                Text(stringResource(R.string.editor_reset_all))
            }
        }

        if (settings.isEmpty()) {
            Text(
                text = stringResource(R.string.editor_no_settings),
                modifier = Modifier.padding(16.dp)
            )
        } else {
            Column(
                modifier = Modifier
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp)
            ) {
                for (setting in settings) {
                    SettingRow(
                        setting = setting,
                        onToggle = { newVal ->
                            viewModel.setEditorOverride(langTag, setting.key, newVal)
                        },
                        onEdit = {
                            if (setting.options != null) {
                                selectingKey = setting.key
                                selectingOptions = setting.options
                            } else {
                                editingKey = setting.key
                                editingValue = setting.value
                            }
                        },
                        onReset = {
                            viewModel.removeEditorOverride(langTag, setting.key)
                        }
                    )
                    HorizontalDivider()
                }
                Spacer(modifier = Modifier.height(32.dp))
            }
        }
    }

    // Edit value dialog
    if (editingKey != null) {
        AlertDialog(
            onDismissRequest = { editingKey = null },
            title = { Text(editingKey!!) },
            text = {
                OutlinedTextField(
                    value = editingValue,
                    onValueChange = { editingValue = it },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.setEditorOverride(langTag, editingKey!!, editingValue)
                    editingKey = null
                }) { Text("OK") }
            },
            dismissButton = {
                TextButton(onClick = { editingKey = null }) {
                    Text(stringResource(R.string.cancel_button))
                }
            }
        )
    }

    // Option selection dialog (for enum-like string settings)
    if (selectingKey != null) {
        AlertDialog(
            onDismissRequest = { selectingKey = null },
            title = { Text(selectingKey!!) },
            text = {
                Column {
                    val current = settings.find { it.key == selectingKey }?.value
                    for (option in selectingOptions) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = option == current,
                                    onClick = {
                                        viewModel.setEditorOverride(langTag, selectingKey!!, option)
                                        selectingKey = null
                                    },
                                    role = androidx.compose.ui.semantics.Role.RadioButton
                                )
                                .padding(vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = option == current,
                                onClick = null,
                                modifier = Modifier.clearAndSetSemantics {}
                            )
                            Spacer(modifier = Modifier.width(8.dp))
                            Text(option, style = MaterialTheme.typography.bodyLarge)
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { selectingKey = null }) {
                    Text(stringResource(R.string.cancel_button))
                }
            }
        )
    }

    // Reset all confirmation
    if (showResetAll) {
        AlertDialog(
            onDismissRequest = { showResetAll = false },
            title = { Text(stringResource(R.string.editor_reset_all)) },
            text = { Text(stringResource(R.string.editor_reset_all_confirm)) },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.resetAllEditorOverrides(langTag)
                    showResetAll = false
                }) { Text(stringResource(R.string.reset_button)) }
            },
            dismissButton = {
                TextButton(onClick = { showResetAll = false }) {
                    Text(stringResource(R.string.cancel_button))
                }
            }
        )
    }
}

@Composable
private fun SettingRow(
    setting: TgsbViewModel.PackSetting,
    onToggle: (String) -> Unit,
    onEdit: () -> Unit,
    onReset: () -> Unit
) {
    val isBool = setting.type == TgsbViewModel.SettingType.Bool
    val checked = setting.value == "true"
    val stateDesc = if (isBool) {
        if (checked) "on" else "off"
    } else {
        setting.value
    }
    val overriddenSuffix = if (setting.isOverridden) ", ${stringResource(R.string.editor_overridden)}" else ""
    val rowDesc = "${setting.displayName}, $stateDesc$overriddenSuffix"

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .let { mod ->
                if (isBool) {
                    mod.toggleable(
                        value = checked,
                        onValueChange = { onToggle(if (it) "true" else "false") },
                        role = androidx.compose.ui.semantics.Role.Switch
                    )
                } else {
                    mod.clickable(onClick = onEdit)
                }
            }
            .semantics { contentDescription = rowDesc }
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier
            .weight(1f)
            .clearAndSetSemantics {}
        ) {
            Text(
                text = setting.displayName,
                style = MaterialTheme.typography.bodyMedium
            )
            if (setting.isOverridden) {
                Text(
                    text = stringResource(R.string.editor_overridden),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary
                )
            }
        }

        when (setting.type) {
            TgsbViewModel.SettingType.Bool -> {
                Switch(
                    checked = checked,
                    onCheckedChange = null,
                    modifier = Modifier.clearAndSetSemantics {}
                )
            }
            else -> {
                Text(
                    text = setting.value,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.clearAndSetSemantics {}
                )
            }
        }
    }

    if (setting.isOverridden) {
        val resetLabel = "${stringResource(R.string.editor_reset_setting)} ${setting.displayName}"
        TextButton(
            onClick = onReset,
            modifier = Modifier.semantics {
                contentDescription = resetLabel
            }
        ) {
            Text(
                stringResource(R.string.editor_reset_setting),
                style = MaterialTheme.typography.labelSmall
            )
        }
    }
}
