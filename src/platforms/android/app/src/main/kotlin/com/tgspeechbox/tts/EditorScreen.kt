/*
 * EditorScreen — Pack settings and phoneme editor.
 *
 * Tabbed interface: Packs (language pack settings) and Phonemes
 * (acoustic parameters with live preview).
 * Changes are stored in SharedPreferences and re-applied after setLanguage.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

@Composable
fun EditorScreen(viewModel: TgsbViewModel) {
    var selectedTab by remember { mutableIntStateOf(0) }
    val tabTitles = listOf("Packs", "Phonemes")

    Column(modifier = Modifier.fillMaxSize()) {
        TabRow(selectedTabIndex = selectedTab) {
            tabTitles.forEachIndexed { index, title ->
                Tab(
                    selected = selectedTab == index,
                    onClick = { selectedTab = index },
                    text = { Text(title) }
                )
            }
        }

        when (selectedTab) {
            0 -> PacksTab(viewModel)
            1 -> PhonemesTab(viewModel)
        }
    }
}

@Composable
private fun PacksTab(viewModel: TgsbViewModel) {
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
private fun PhonemesTab(viewModel: TgsbViewModel) {
    var selectedPhoneme by remember { mutableStateOf<String?>(null) }
    var langFilter by remember { mutableStateOf("") }

    if (selectedPhoneme != null) {
        PhonemeDetailScreen(
            viewModel = viewModel,
            phonemeKey = selectedPhoneme!!,
            onBack = { selectedPhoneme = null }
        )
    } else {
        PhonemeListScreen(
            viewModel = viewModel,
            langFilter = langFilter,
            onLangFilterChanged = { langFilter = it },
            onPhonemeSelected = { selectedPhoneme = it }
        )
    }
}

@Composable
private fun PhonemeListScreen(
    viewModel: TgsbViewModel,
    langFilter: String,
    onLangFilterChanged: (String) -> Unit,
    onPhonemeSelected: (String) -> Unit
) {
    val phonemes by viewModel.phonemeList.collectAsState()
    val langs by viewModel.editorLanguages.collectAsState()

    LaunchedEffect(langFilter) {
        viewModel.loadEditorLanguages()
        viewModel.loadPhonemeList(langFilter)
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp)
    ) {
        // Language filter dropdown
        var expanded by remember { mutableStateOf(false) }
        val filterLabel = if (langFilter.isEmpty()) "All phonemes" else langFilter
        Box {
            OutlinedButton(
                onClick = { expanded = true },
                modifier = Modifier.semantics {
                    contentDescription = "$filterLabel, dropdown menu"
                }
            ) {
                Text(filterLabel, modifier = Modifier.clearAndSetSemantics {})
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                DropdownMenuItem(
                    text = { Text("All phonemes") },
                    onClick = {
                        onLangFilterChanged("")
                        expanded = false
                    }
                )
                for (lang in langs) {
                    DropdownMenuItem(
                        text = { Text(lang) },
                        onClick = {
                            onLangFilterChanged(lang)
                            expanded = false
                        }
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(8.dp))

        Text(
            text = "${phonemes.size} phonemes",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Spacer(modifier = Modifier.height(8.dp))

        LazyColumn {
            items(phonemes, key = { it.key }) { entry ->
                val desc = buildString {
                    append(entry.key)
                    append(", ")
                    append(entry.phonemeClass)
                    if (entry.mappingFrom.isNotEmpty()) {
                        append(", mapped from ")
                        append(entry.mappingFrom)
                    }
                }
                Surface(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onPhonemeSelected(entry.key) }
                        .semantics { contentDescription = desc },
                    tonalElevation = 1.dp,
                    shape = MaterialTheme.shapes.small
                ) {
                    Row(
                        modifier = Modifier.padding(16.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = entry.key,
                            style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.clearAndSetSemantics {}
                        )
                        Spacer(modifier = Modifier.width(12.dp))
                        Column(modifier = Modifier
                            .weight(1f)
                            .clearAndSetSemantics {}
                        ) {
                            Text(
                                text = entry.phonemeClass,
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                            if (entry.mappingFrom.isNotEmpty()) {
                                Text(
                                    text = "from: ${entry.mappingFrom}",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.primary
                                )
                            }
                        }
                    }
                }
                Spacer(modifier = Modifier.height(4.dp))
            }
        }
    }
}

/** Slider range for a phoneme field based on its name and current value. */
private fun phonemeFieldRange(fieldName: String, value: Float): ClosedFloatingPointRange<Float> {
    return when {
        // Formant frequencies (cf1-6, pf1-6)
        fieldName.matches(Regex("^[cp]f[1-6]$")) -> 0f..8000f
        fieldName in listOf("cfNP", "cfN", "cfTP") -> 0f..5000f
        // End-of-diphthong formant frequencies
        fieldName.matches(Regex("^end[CP]f[1-6]$")) -> 0f..8000f
        // Bandwidths (cb1-6, pb1-6)
        fieldName.matches(Regex("^[cp]b[1-6]$")) -> 0f..1000f
        fieldName.matches(Regex("^end[CP]b[1-6]$")) -> 0f..1000f
        // Parallel amplitudes (pa1-6)
        fieldName.matches(Regex("^pa[1-6]$")) -> 0f..1.5f
        // Named amplitudes
        fieldName.contains("Amplitude") || fieldName.contains("amplitude") -> 0f..1.5f
        // Pitch
        fieldName.contains("Pitch") || fieldName.contains("pitch") -> 40f..500f
        // Gains and ratios (0-1 range)
        fieldName in listOf("preFormantGain", "parallelBypass",
            "glottalOpenQuotient", "breathiness", "creakiness",
            "jitter", "shimmer", "sharpness") -> 0f..1f
        // Fallback: relative range around current value
        value in 0f..1f -> 0f..1.5f
        value > 1f -> 0f..maxOf(value * 2.5f, 100f)
        else -> 0f..maxOf(value * 2.5f, 100f)
    }
}

/** Format a slider value for display — drop trailing zeros. */
private fun fmtVal(v: Float): String {
    return if (v == v.toLong().toFloat()) v.toLong().toString()
    else "%.2f".format(v).trimEnd('0').trimEnd('.')
}

@Composable
private fun PhonemeDetailScreen(
    viewModel: TgsbViewModel,
    phonemeKey: String,
    onBack: () -> Unit
) {
    val fields by viewModel.phonemeFields.collectAsState()
    var editingKey by remember { mutableStateOf<String?>(null) }
    var editingValue by remember { mutableStateOf("") }
    var showResetAll by remember { mutableStateOf(false) }

    LaunchedEffect(phonemeKey) {
        viewModel.loadPhonemeFields(phonemeKey)
    }

    Column(modifier = Modifier.fillMaxSize()) {
        // Top bar
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
                text = phonemeKey,
                style = MaterialTheme.typography.titleLarge,
                modifier = Modifier.weight(1f)
            )
            // Preview button
            IconButton(
                onClick = { viewModel.previewPhoneme(phonemeKey) },
                modifier = Modifier.semantics {
                    contentDescription = "Preview $phonemeKey"
                }
            ) {
                Icon(Icons.Default.PlayArrow, contentDescription = null)
            }
            // Reset all overrides for this phoneme
            TextButton(onClick = { showResetAll = true }) {
                Text("Reset all")
            }
        }

        if (fields.isEmpty()) {
            Text(
                text = "No fields",
                modifier = Modifier.padding(16.dp)
            )
        } else {
            LazyColumn(
                modifier = Modifier.padding(horizontal = 16.dp)
            ) {
                items(fields, key = { it.key }) { field ->
                    PhonemeFieldRow(
                        field = field,
                        onValueChanged = { newVal ->
                            viewModel.setPhonemeOverride(field.key, newVal)
                        },
                        onEdit = {
                            editingKey = field.key
                            editingValue = field.value
                        },
                        onToggle = { newVal ->
                            viewModel.setPhonemeOverride(field.key, newVal)
                            viewModel.loadPhonemeFields(phonemeKey)
                        },
                        onReset = {
                            viewModel.removePhonemeOverride(field.key)
                            viewModel.loadPhonemeFields(phonemeKey)
                        },
                        onPreview = { viewModel.previewPhoneme(phonemeKey) }
                    )
                    HorizontalDivider()
                }
            }
        }
    }

    // Edit value dialog (tap field name for precise input)
    if (editingKey != null) {
        AlertDialog(
            onDismissRequest = { editingKey = null },
            title = { Text(editingKey!!.substringAfter(".")) },
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
                    viewModel.setPhonemeOverride(editingKey!!, editingValue)
                    viewModel.previewPhoneme(phonemeKey)
                    viewModel.loadPhonemeFields(phonemeKey)
                    editingKey = null
                }) { Text("OK") }
            },
            dismissButton = {
                TextButton(onClick = { editingKey = null }) {
                    Text("Cancel")
                }
            }
        )
    }

    // Reset all confirmation
    if (showResetAll) {
        AlertDialog(
            onDismissRequest = { showResetAll = false },
            title = { Text("Reset $phonemeKey") },
            text = { Text("Reset all overrides on this phoneme back to the original values?") },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.resetPhonemeOverrides(phonemeKey)
                    viewModel.loadPhonemeFields(phonemeKey)
                    showResetAll = false
                }) { Text("Reset") }
            },
            dismissButton = {
                TextButton(onClick = { showResetAll = false }) {
                    Text("Cancel")
                }
            }
        )
    }
}

@Composable
private fun PhonemeFieldRow(
    field: TgsbViewModel.PhonemeField,
    onValueChanged: (String) -> Unit,
    onEdit: () -> Unit,
    onToggle: (String) -> Unit,
    onReset: () -> Unit,
    onPreview: () -> Unit
) {
    val isBool = field.type == TgsbViewModel.SettingType.Bool
    val isNumber = field.type == TgsbViewModel.SettingType.Number

    if (isBool) {
        // Boolean toggle row
        val checked = field.value == "true"
        val overriddenSuffix = if (field.isOverridden) ", overridden" else ""
        val rowDesc = "${field.displayName}, ${if (checked) "on" else "off"}$overriddenSuffix"

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .toggleable(
                    value = checked,
                    onValueChange = { onToggle(if (it) "true" else "false") },
                    role = androidx.compose.ui.semantics.Role.Switch
                )
                .semantics { contentDescription = rowDesc }
                .padding(vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = field.displayName,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f).clearAndSetSemantics {}
            )
            Switch(
                checked = checked,
                onCheckedChange = null,
                modifier = Modifier.clearAndSetSemantics {}
            )
        }
        if (field.isOverridden) {
            TextButton(onClick = onReset) {
                Text("Reset", style = MaterialTheme.typography.labelSmall)
            }
        }
    } else if (isNumber) {
        // Numeric slider row with live preview on release
        val baseValue = field.value.toFloatOrNull() ?: 0f
        var sliderValue by remember(field.key, field.value) {
            mutableStateOf(baseValue)
        }
        val range = phonemeFieldRange(field.fieldName, baseValue)
        val overriddenSuffix = if (field.isOverridden) ", overridden" else ""

        Column(modifier = Modifier.padding(vertical = 8.dp)) {
            // Label row — tap to open text input for precise values
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onEdit)
                    .semantics {
                        contentDescription =
                            "${field.displayName}, ${fmtVal(sliderValue)}$overriddenSuffix, double tap for exact input"
                    },
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = field.displayName,
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f).clearAndSetSemantics {}
                )
                Text(
                    text = fmtVal(sliderValue),
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (field.isOverridden)
                        MaterialTheme.colorScheme.primary
                    else
                        MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.clearAndSetSemantics {}
                )
            }
            // Slider — drag to adjust, preview plays on release
            Slider(
                value = sliderValue.coerceIn(range),
                onValueChange = { newVal ->
                    sliderValue = newVal
                    onValueChanged(fmtVal(newVal))
                },
                onValueChangeFinished = {
                    onPreview()
                },
                valueRange = range,
                modifier = Modifier
                    .fillMaxWidth()
                    .semantics {
                        contentDescription =
                            "${field.displayName} slider, ${fmtVal(sliderValue)}"
                    }
            )
            if (field.isOverridden) {
                TextButton(onClick = onReset) {
                    Text("Reset", style = MaterialTheme.typography.labelSmall)
                }
            }
        }
    } else {
        // Text field row (non-numeric, non-bool)
        val overriddenSuffix = if (field.isOverridden) ", overridden" else ""
        val rowDesc = "${field.displayName}, ${field.value}$overriddenSuffix"

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable(onClick = onEdit)
                .semantics { contentDescription = rowDesc }
                .padding(vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = field.displayName,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f).clearAndSetSemantics {}
            )
            Text(
                text = field.value,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.clearAndSetSemantics {}
            )
        }
        if (field.isOverridden) {
            TextButton(onClick = onReset) {
                Text("Reset", style = MaterialTheme.typography.labelSmall)
            }
        }
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
            .verticalScroll(rememberScrollState())
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

@Composable
private fun PackSettingsScreen(
    viewModel: TgsbViewModel,
    langTag: String,
    onBack: () -> Unit
) {
    val context = LocalContext.current
    val settings by viewModel.editorSettings.collectAsState()
    var editingKey by remember { mutableStateOf<String?>(null) }
    var editingValue by remember { mutableStateOf("") }
    var selectingKey by remember { mutableStateOf<String?>(null) }
    var selectingOptions by remember { mutableStateOf<List<String>>(emptyList()) }
    var showResetAll by remember { mutableStateOf(false) }
    var showImportConfirm by remember { mutableStateOf(false) }
    var pendingImportUri by remember { mutableStateOf<android.net.Uri?>(null) }
    val importExportStatus by viewModel.importExportStatus.collectAsState()

    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            pendingImportUri = uri
            showImportConfirm = true
        }
    }

    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri ->
        uri?.let { viewModel.exportPackYaml(context, langTag, it) }
    }

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

        // Import / Export actions
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            OutlinedButton(onClick = { importLauncher.launch(arrayOf("*/*")) }) {
                Text(stringResource(R.string.editor_import_pack))
            }
            OutlinedButton(onClick = { exportLauncher.launch("$langTag.yaml") }) {
                Text(stringResource(R.string.editor_export_pack))
            }
            OutlinedButton(onClick = { viewModel.sharePackYaml(context, langTag) }) {
                Text(stringResource(R.string.editor_share_pack))
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

    // Import confirmation
    if (showImportConfirm) {
        AlertDialog(
            onDismissRequest = { showImportConfirm = false; pendingImportUri = null },
            title = { Text(stringResource(R.string.editor_import_pack)) },
            text = { Text(stringResource(R.string.editor_import_confirm, langTag)) },
            confirmButton = {
                TextButton(onClick = {
                    pendingImportUri?.let { viewModel.importPackYaml(context, langTag, it) }
                    showImportConfirm = false
                    pendingImportUri = null
                }) { Text(stringResource(R.string.editor_import_pack)) }
            },
            dismissButton = {
                TextButton(onClick = { showImportConfirm = false; pendingImportUri = null }) {
                    Text(stringResource(R.string.cancel_button))
                }
            }
        )
    }

    // Import/export status
    if (importExportStatus != null) {
        AlertDialog(
            onDismissRequest = { viewModel.clearImportExportStatus() },
            text = { Text(importExportStatus!!) },
            confirmButton = {
                TextButton(onClick = { viewModel.clearImportExportStatus() }) {
                    Text("OK")
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
