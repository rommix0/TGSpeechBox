/*
TGSpeechBox — Dictionary editor implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#define UNICODE
#define _UNICODE

#include "DictionaryEditor.h"
#include "WinUtils.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace tgsb_editor {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DictEditorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DictEntryDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

static void setupListColumns(HWND hList, const std::string& dictType);
static void loadEntries(HWND hDlg, DictionaryEditorState* st);
static void applyFilter(HWND hDlg, DictionaryEditorState* st);
static void updateStatusText(HWND hDlg, DictionaryEditorState* st);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Case-insensitive ASCII tolower for prefix matching.
static char asciiLower(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
  return c;
}

// Case-insensitive prefix match.
static bool prefixMatchCI(const std::string& text, const std::string& prefix) {
  if (prefix.size() > text.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (asciiLower(text[i]) != asciiLower(prefix[i])) return false;
  }
  return true;
}

// Display name for dict type combo.
static const char* typeDisplayNames[] = {
  "Pronunciation", "Stress", "Compound", "Character"
};
static const char* typeInternalNames[] = {
  "pronounce", "stress", "compound", "character"
};
static constexpr int kNumTypes = 4;

// Get wstring from edit control.
static std::wstring getDlgItemText(HWND hDlg, int id) {
  int len = GetWindowTextLengthW(GetDlgItem(hDlg, id));
  if (len <= 0) return {};
  std::wstring buf(static_cast<size_t>(len + 1), L'\0');
  GetDlgItemTextW(hDlg, id, &buf[0], len + 1);
  buf.resize(static_cast<size_t>(len));
  return buf;
}

// Split a line on tabs.
static std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> cols;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      cols.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  cols.push_back(cur);
  return cols;
}

// ---------------------------------------------------------------------------
// Path construction
// ---------------------------------------------------------------------------
std::wstring dictTsvPath(const std::wstring& packDir, const std::string& langTag,
                         const std::string& dictType) {
  fs::path base = fs::path(packDir) / "dict";
  std::string tag = langTag;
  // Normalize tag: en-us → en-us (keep hyphens).
  if (dictType == "pronounce") {
    return (base / (tag + "-dict.tsv")).wstring();
  } else if (dictType == "stress") {
    return (base / (tag + "-stress.tsv")).wstring();
  } else if (dictType == "compound") {
    return (base / (tag + "-compounds.tsv")).wstring();
  } else if (dictType == "character") {
    return (base / (tag + "-letters.tsv")).wstring();
  }
  return (base / (tag + "-dict.tsv")).wstring();
}

// ---------------------------------------------------------------------------
// TSV I/O
// ---------------------------------------------------------------------------
bool loadDictTsv(const std::wstring& path, const std::string& dictType,
                 std::vector<DictEntryData>& out, std::string& outError) {
  out.clear();

  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) {
    outError = "Could not open file: " + wideToUtf8(path);
    return false;
  }

  // Read entire file.
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string data(static_cast<size_t>(sz), '\0');
  size_t rd = fread(&data[0], 1, static_cast<size_t>(sz), f);
  fclose(f);
  data.resize(rd);

  // Strip UTF-8 BOM if present.
  if (data.size() >= 3 &&
      static_cast<unsigned char>(data[0]) == 0xEF &&
      static_cast<unsigned char>(data[1]) == 0xBB &&
      static_cast<unsigned char>(data[2]) == 0xBF) {
    data = data.substr(3);
  }

  // Split into lines, handle \r\n and \n.
  std::vector<std::string> lines;
  {
    std::string line;
    for (size_t i = 0; i < data.size(); ++i) {
      if (data[i] == '\r') {
        lines.push_back(line);
        line.clear();
        if (i + 1 < data.size() && data[i + 1] == '\n') ++i;
      } else if (data[i] == '\n') {
        lines.push_back(line);
        line.clear();
      } else {
        line += data[i];
      }
    }
    if (!line.empty()) lines.push_back(line);
  }

  for (const auto& ln : lines) {
    // Skip comments and blank lines.
    if (ln.empty()) continue;
    if (ln[0] == '#') continue;

    auto cols = splitTabs(ln);
    if (cols.empty()) continue;

    DictEntryData e;
    if (dictType == "pronounce") {
      e.fromText = cols.size() > 0 ? cols[0] : "";
      e.toText   = cols.size() > 1 ? cols[1] : "";
      e.fromIpa  = cols.size() > 2 ? cols[2] : "";
      e.toIpa    = cols.size() > 3 ? cols[3] : "";
      e.category = cols.size() > 4 ? cols[4] : "";
    } else if (dictType == "stress" || dictType == "compound") {
      e.fromText = cols[0];
      // Join remaining columns with tab (stress patterns may have tabs).
      for (size_t c = 1; c < cols.size(); ++c) {
        if (c > 1) e.toText += '\t';
        e.toText += cols[c];
      }
    } else if (dictType == "character") {
      e.fromText = cols.size() > 0 ? cols[0] : "";
      e.toText   = cols.size() > 1 ? cols[1] : "";
    } else {
      e.fromText = cols.size() > 0 ? cols[0] : "";
      e.toText   = cols.size() > 1 ? cols[1] : "";
    }

    if (!e.fromText.empty()) {
      out.push_back(std::move(e));
    }
  }

  return true;
}

bool saveDictTsv(const std::wstring& path, const std::string& dictType,
                 const std::vector<DictEntryData>& entries, std::string& outError) {
  // Ensure parent directory exists.
  {
    fs::path p(path);
    fs::path parent = p.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
      std::error_code ec;
      fs::create_directories(parent, ec);
      if (ec) {
        outError = "Could not create directory: " + wideToUtf8(parent.wstring());
        return false;
      }
    }
  }

  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) {
    outError = "Could not open file for writing: " + wideToUtf8(path);
    return false;
  }

  fprintf(f, "# TGSpeechBox dictionary\n");

  for (const auto& e : entries) {
    if (dictType == "pronounce") {
      // Write all columns, trim trailing empty ones.
      if (!e.category.empty()) {
        fprintf(f, "%s\t%s\t%s\t%s\t%s\n",
                e.fromText.c_str(), e.toText.c_str(),
                e.fromIpa.c_str(), e.toIpa.c_str(),
                e.category.c_str());
      } else if (!e.toIpa.empty()) {
        fprintf(f, "%s\t%s\t%s\t%s\n",
                e.fromText.c_str(), e.toText.c_str(),
                e.fromIpa.c_str(), e.toIpa.c_str());
      } else if (!e.fromIpa.empty()) {
        fprintf(f, "%s\t%s\t%s\n",
                e.fromText.c_str(), e.toText.c_str(),
                e.fromIpa.c_str());
      } else {
        fprintf(f, "%s\t%s\n", e.fromText.c_str(), e.toText.c_str());
      }
    } else {
      // stress, compound, character: fromText\ttoText
      fprintf(f, "%s\t%s\n", e.fromText.c_str(), e.toText.c_str());
    }
  }

  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// List column setup
// ---------------------------------------------------------------------------
static void setupListColumns(HWND hList, const std::string& dictType) {
  // Remove all existing columns.
  while (true) {
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    if (!ListView_GetColumn(hList, 0, &col)) break;
    ListView_DeleteColumn(hList, 0);
  }

  auto addCol = [&](const wchar_t* text, int width, int idx) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    col.cx = width;
    col.pszText = const_cast<wchar_t*>(text);
    ListView_InsertColumn(hList, idx, &col);
  };

  if (dictType == "pronounce") {
    addCol(L"From",     120, 0);
    addCol(L"To",       120, 1);
    addCol(L"From IPA", 100, 2);
    addCol(L"To IPA",   100, 3);
    addCol(L"Category",  80, 4);
  } else if (dictType == "stress") {
    addCol(L"Word",    150, 0);
    addCol(L"Pattern", 300, 1);
  } else if (dictType == "compound") {
    addCol(L"Word",  150, 0);
    addCol(L"Parts", 300, 1);
  } else if (dictType == "character") {
    addCol(L"Symbol",      100, 0);
    addCol(L"Description", 350, 1);
  }
}

// ---------------------------------------------------------------------------
// Load entries from TSV
// ---------------------------------------------------------------------------
static void loadEntries(HWND hDlg, DictionaryEditorState* st) {
  st->entries.clear();
  st->filteredIndices.clear();
  st->modified = false;

  std::wstring path = dictTsvPath(st->packDir, st->currentLang, st->currentType);

  if (fs::exists(path)) {
    std::string err;
    if (!loadDictTsv(path, st->currentType, st->entries, err)) {
      msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
    }
  }

  // Setup list columns for current type.
  HWND hList = GetDlgItem(hDlg, IDC_DICT_LIST);
  setupListColumns(hList, st->currentType);

  applyFilter(hDlg, st);
}

// ---------------------------------------------------------------------------
// Apply filter and populate listview
// ---------------------------------------------------------------------------
static void applyFilter(HWND hDlg, DictionaryEditorState* st) {
  HWND hList = GetDlgItem(hDlg, IDC_DICT_LIST);
  std::wstring searchW = getDlgItemText(hDlg, IDC_DICT_SEARCH);
  std::string search = wideToUtf8(searchW);

  st->filteredIndices.clear();

  static constexpr size_t kMaxDisplay = 5000;

  if (search.empty()) {
    // Show all, but cap at kMaxDisplay when no search is active.
    size_t limit = (st->entries.size() > kMaxDisplay) ? kMaxDisplay : st->entries.size();
    for (size_t i = 0; i < limit; ++i) {
      st->filteredIndices.push_back(i);
    }
  } else {
    // Case-insensitive prefix match on fromText.
    for (size_t i = 0; i < st->entries.size(); ++i) {
      if (prefixMatchCI(st->entries[i].fromText, search)) {
        st->filteredIndices.push_back(i);
      }
    }
  }

  // Populate listview.
  SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(hList);

  for (size_t fi = 0; fi < st->filteredIndices.size(); ++fi) {
    const auto& e = st->entries[st->filteredIndices[fi]];

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = static_cast<int>(fi);
    lvi.iSubItem = 0;
    std::wstring col0 = utf8ToWide(e.fromText);
    lvi.pszText = const_cast<wchar_t*>(col0.c_str());
    ListView_InsertItem(hList, &lvi);

    // Set sub-items based on dict type.
    if (st->currentType == "pronounce") {
      std::wstring s1 = utf8ToWide(e.toText);
      ListView_SetItemText(hList, static_cast<int>(fi), 1, const_cast<wchar_t*>(s1.c_str()));
      std::wstring s2 = utf8ToWide(e.fromIpa);
      ListView_SetItemText(hList, static_cast<int>(fi), 2, const_cast<wchar_t*>(s2.c_str()));
      std::wstring s3 = utf8ToWide(e.toIpa);
      ListView_SetItemText(hList, static_cast<int>(fi), 3, const_cast<wchar_t*>(s3.c_str()));
      std::wstring s4 = utf8ToWide(e.category);
      ListView_SetItemText(hList, static_cast<int>(fi), 4, const_cast<wchar_t*>(s4.c_str()));
    } else {
      // stress, compound, character: 2 columns
      std::wstring s1 = utf8ToWide(e.toText);
      ListView_SetItemText(hList, static_cast<int>(fi), 1, const_cast<wchar_t*>(s1.c_str()));
    }
  }

  SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(hList, nullptr, TRUE);

  updateStatusText(hDlg, st);
}

// ---------------------------------------------------------------------------
// Status text
// ---------------------------------------------------------------------------
static void updateStatusText(HWND hDlg, DictionaryEditorState* st) {
  // Build status string in the dialog title.
  std::wstring title = L"Dictionary Editor";
  if (!st->currentLang.empty()) {
    title += L" - " + utf8ToWide(st->currentLang);
  }

  size_t total = st->entries.size();
  size_t shown = st->filteredIndices.size();

  if (shown < total) {
    title += L" (showing " + std::to_wstring(shown) + L" of " + std::to_wstring(total) + L")";
  } else {
    title += L" (" + std::to_wstring(total) + L" entries)";
  }

  if (st->modified) {
    title += L" *";
  }

  SetWindowTextW(hDlg, title.c_str());
}

// ---------------------------------------------------------------------------
// Prompt to save if modified
// ---------------------------------------------------------------------------
static bool promptSaveIfModified(HWND hDlg, DictionaryEditorState* st) {
  if (!st->modified) return true;

  int ret = MessageBoxW(hDlg, L"Save changes to current dictionary?",
                        L"Dictionary Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (ret == IDCANCEL) return false;
  if (ret == IDYES) {
    std::wstring path = dictTsvPath(st->packDir, st->currentLang, st->currentType);
    std::string err;
    if (!saveDictTsv(path, st->currentType, st->entries, err)) {
      msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
      return false;
    }
    st->modified = false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Get selected listview index
// ---------------------------------------------------------------------------
static int getSelectedListIndex(HWND hList) {
  return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// Open/Save file dialog helpers
// ---------------------------------------------------------------------------
static bool openTsvFileDialog(HWND hDlg, std::wstring& outPath) {
  wchar_t buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hDlg;
  ofn.lpstrFilter = L"TSV Files (*.tsv)\0*.tsv\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  ofn.lpstrTitle = L"Import Dictionary TSV";
  if (GetOpenFileNameW(&ofn)) {
    outPath = buf;
    return true;
  }
  return false;
}

static bool saveTsvFileDialog(HWND hDlg, std::wstring& outPath) {
  wchar_t buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hDlg;
  ofn.lpstrFilter = L"TSV Files (*.tsv)\0*.tsv\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"tsv";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrTitle = L"Export Dictionary TSV";
  if (GetSaveFileNameW(&ofn)) {
    outPath = buf;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Dict entry sub-dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DictEntryDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<DictEntryDialogState*>(GetWindowLongPtrW(hDlg, DWLP_USER));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<DictEntryDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));

      // Set caption.
      SetWindowTextW(hDlg, st->isEdit ? L"Edit Dictionary Entry" : L"Add Dictionary Entry");

      // Adjust visibility and labels based on dict type.
      bool isPronounce = (st->dictType == "pronounce");

      // Hide IPA buttons and fields for non-pronounce types.
      auto showCtrl = [&](int id, bool show) {
        ShowWindow(GetDlgItem(hDlg, id), show ? SW_SHOW : SW_HIDE);
        EnableWindow(GetDlgItem(hDlg, id), show ? TRUE : FALSE);
      };

      showCtrl(IDC_DENTRY_FROM_IPA_BTN, isPronounce);
      showCtrl(IDC_DENTRY_FROM_IPA, isPronounce);
      showCtrl(IDC_DENTRY_LBL_FROM_IPA, isPronounce);
      showCtrl(IDC_DENTRY_TO_IPA_BTN, isPronounce);
      showCtrl(IDC_DENTRY_TO_IPA, isPronounce);
      showCtrl(IDC_DENTRY_LBL_TO_IPA, isPronounce);
      showCtrl(IDC_DENTRY_CATEGORY, isPronounce);
      showCtrl(IDC_DENTRY_LBL_CATEGORY, isPronounce);

      // Disable IPA buttons if convertToIpa callback is null.
      if (!st->convertToIpa) {
        EnableWindow(GetDlgItem(hDlg, IDC_DENTRY_FROM_IPA_BTN), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_DENTRY_TO_IPA_BTN), FALSE);
      }

      // Relabel for specific types.
      if (st->dictType == "stress") {
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_FROM, L"Word:");
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_TO, L"Pattern:");
      } else if (st->dictType == "compound") {
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_FROM, L"Word:");
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_TO, L"Parts:");
      } else if (st->dictType == "character") {
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_FROM, L"Symbol:");
        SetDlgItemTextW(hDlg, IDC_DENTRY_LBL_TO, L"Description:");
      }

      // Pre-fill fields if editing.
      if (st->isEdit) {
        SetDlgItemTextW(hDlg, IDC_DENTRY_FROM, utf8ToWide(st->entry.fromText).c_str());
        SetDlgItemTextW(hDlg, IDC_DENTRY_TO, utf8ToWide(st->entry.toText).c_str());
        if (isPronounce) {
          SetDlgItemTextW(hDlg, IDC_DENTRY_FROM_IPA, utf8ToWide(st->entry.fromIpa).c_str());
          SetDlgItemTextW(hDlg, IDC_DENTRY_TO_IPA, utf8ToWide(st->entry.toIpa).c_str());
          SetDlgItemTextW(hDlg, IDC_DENTRY_CATEGORY, utf8ToWide(st->entry.category).c_str());
        }
      }

      // Set focus to From field.
      SetFocus(GetDlgItem(hDlg, IDC_DENTRY_FROM));
      return FALSE;  // We set focus manually.
    }

    case WM_COMMAND: {
      int id = LOWORD(wParam);

      if (id == IDC_DENTRY_FROM_IPA_BTN) {
        if (st->convertToIpa) {
          std::wstring text = getDlgItemText(hDlg, IDC_DENTRY_FROM);
          std::string ipa, err;
          if (st->convertToIpa(text, ipa, err)) {
            st->entry.fromIpa = ipa;
            SetDlgItemTextW(hDlg, IDC_DENTRY_FROM_IPA, utf8ToWide(ipa).c_str());
          } else {
            msgBox(hDlg, L"IPA conversion failed:\n" + utf8ToWide(err),
                   L"Dictionary Editor", MB_ICONERROR);
          }
        }
        return TRUE;
      }

      if (id == IDC_DENTRY_TO_IPA_BTN) {
        if (st->convertToIpa) {
          std::wstring text = getDlgItemText(hDlg, IDC_DENTRY_TO);
          std::string ipa, err;
          if (st->convertToIpa(text, ipa, err)) {
            st->entry.toIpa = ipa;
            SetDlgItemTextW(hDlg, IDC_DENTRY_TO_IPA, utf8ToWide(ipa).c_str());
          } else {
            msgBox(hDlg, L"IPA conversion failed:\n" + utf8ToWide(err),
                   L"Dictionary Editor", MB_ICONERROR);
          }
        }
        return TRUE;
      }

      if (id == IDOK) {
        // Read all fields.
        st->entry.fromText = wideToUtf8(getDlgItemText(hDlg, IDC_DENTRY_FROM));
        st->entry.toText = wideToUtf8(getDlgItemText(hDlg, IDC_DENTRY_TO));

        if (st->dictType == "pronounce") {
          st->entry.fromIpa = wideToUtf8(getDlgItemText(hDlg, IDC_DENTRY_FROM_IPA));
          st->entry.toIpa = wideToUtf8(getDlgItemText(hDlg, IDC_DENTRY_TO_IPA));
          st->entry.category = wideToUtf8(getDlgItemText(hDlg, IDC_DENTRY_CATEGORY));
        }

        // Validate.
        if (st->entry.fromText.empty()) {
          msgBox(hDlg, L"The 'From' field cannot be empty.", L"Dictionary Editor", MB_ICONWARNING);
          SetFocus(GetDlgItem(hDlg, IDC_DENTRY_FROM));
          return TRUE;
        }
        if (st->entry.toText.empty()) {
          msgBox(hDlg, L"The 'To' field cannot be empty.", L"Dictionary Editor", MB_ICONWARNING);
          SetFocus(GetDlgItem(hDlg, IDC_DENTRY_TO));
          return TRUE;
        }

        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }

      if (id == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }

    case WM_CLOSE:
      EndDialog(hDlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

// ---------------------------------------------------------------------------
// Main dictionary editor dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DictEditorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<DictionaryEditorState*>(GetWindowLongPtrW(hDlg, DWLP_USER));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<DictionaryEditorState*>(lParam);
      SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));

      // Populate language combo.
      HWND hLang = GetDlgItem(hDlg, IDC_DICT_LANG);
      int selIdx = 0;
      for (size_t i = 0; i < st->availableLangs.size(); ++i) {
        std::wstring w = utf8ToWide(st->availableLangs[i]);
        SendMessageW(hLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
        if (st->availableLangs[i] == st->initialLang) {
          selIdx = static_cast<int>(i);
        }
      }
      SendMessageW(hLang, CB_SETCURSEL, static_cast<WPARAM>(selIdx), 0);

      // Populate type combo.
      HWND hType = GetDlgItem(hDlg, IDC_DICT_TYPE);
      for (int i = 0; i < kNumTypes; ++i) {
        std::wstring w = utf8ToWide(typeDisplayNames[i]);
        SendMessageW(hType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
      }
      SendMessageW(hType, CB_SETCURSEL, 0, 0);

      // Set initial state.
      if (!st->availableLangs.empty()) {
        st->currentLang = st->availableLangs[static_cast<size_t>(selIdx)];
      }
      st->currentType = "pronounce";

      // Init listview extended style.
      HWND hList = GetDlgItem(hDlg, IDC_DICT_LIST);
      ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

      // Load entries.
      loadEntries(hDlg, st);

      return TRUE;
    }

    case WM_COMMAND: {
      int id = LOWORD(wParam);
      int code = HIWORD(wParam);

      // Language combo changed.
      if (id == IDC_DICT_LANG && code == CBN_SELCHANGE) {
        int sel = static_cast<int>(SendMessageW(GetDlgItem(hDlg, IDC_DICT_LANG), CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->availableLangs.size())) {
          if (!promptSaveIfModified(hDlg, st)) {
            // Revert combo selection.
            for (size_t i = 0; i < st->availableLangs.size(); ++i) {
              if (st->availableLangs[i] == st->currentLang) {
                SendMessageW(GetDlgItem(hDlg, IDC_DICT_LANG), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                break;
              }
            }
            return TRUE;
          }
          st->currentLang = st->availableLangs[static_cast<size_t>(sel)];
          loadEntries(hDlg, st);
        }
        return TRUE;
      }

      // Type combo changed.
      if (id == IDC_DICT_TYPE && code == CBN_SELCHANGE) {
        int sel = static_cast<int>(SendMessageW(GetDlgItem(hDlg, IDC_DICT_TYPE), CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < kNumTypes) {
          if (!promptSaveIfModified(hDlg, st)) {
            // Revert combo selection.
            for (int i = 0; i < kNumTypes; ++i) {
              if (std::string(typeInternalNames[i]) == st->currentType) {
                SendMessageW(GetDlgItem(hDlg, IDC_DICT_TYPE), CB_SETCURSEL, static_cast<WPARAM>(i), 0);
                break;
              }
            }
            return TRUE;
          }
          st->currentType = typeInternalNames[sel];
          loadEntries(hDlg, st);
        }
        return TRUE;
      }

      // Search text changed.
      if (id == IDC_DICT_SEARCH && code == EN_CHANGE) {
        applyFilter(hDlg, st);
        return TRUE;
      }

      // Add button.
      if (id == IDC_DICT_ADD) {
        DictEntryDialogState des;
        des.dictType = st->currentType;
        des.convertToIpa = st->convertToIpa;
        des.isEdit = false;

        DialogBoxParamW(st->hInst, MAKEINTRESOURCEW(IDD_DICT_ENTRY), hDlg,
                        DictEntryDlgProc, reinterpret_cast<LPARAM>(&des));

        if (des.ok) {
          st->entries.push_back(des.entry);
          st->modified = true;
          applyFilter(hDlg, st);
        }
        return TRUE;
      }

      // Edit button.
      if (id == IDC_DICT_EDIT) {
        HWND hList = GetDlgItem(hDlg, IDC_DICT_LIST);
        int sel = getSelectedListIndex(hList);
        if (sel < 0 || sel >= static_cast<int>(st->filteredIndices.size())) {
          msgBox(hDlg, L"Select an entry to edit.", L"Dictionary Editor", MB_ICONINFORMATION);
          return TRUE;
        }

        size_t realIdx = st->filteredIndices[static_cast<size_t>(sel)];
        DictEntryDialogState des;
        des.dictType = st->currentType;
        des.convertToIpa = st->convertToIpa;
        des.isEdit = true;
        des.entry = st->entries[realIdx];

        DialogBoxParamW(st->hInst, MAKEINTRESOURCEW(IDD_DICT_ENTRY), hDlg,
                        DictEntryDlgProc, reinterpret_cast<LPARAM>(&des));

        if (des.ok) {
          st->entries[realIdx] = des.entry;
          st->modified = true;
          applyFilter(hDlg, st);
        }
        return TRUE;
      }

      // Delete button.
      if (id == IDC_DICT_DELETE) {
        HWND hList = GetDlgItem(hDlg, IDC_DICT_LIST);
        int sel = getSelectedListIndex(hList);
        if (sel < 0 || sel >= static_cast<int>(st->filteredIndices.size())) {
          msgBox(hDlg, L"Select an entry to delete.", L"Dictionary Editor", MB_ICONINFORMATION);
          return TRUE;
        }

        size_t realIdx = st->filteredIndices[static_cast<size_t>(sel)];
        std::wstring fromW = utf8ToWide(st->entries[realIdx].fromText);

        int ret = MessageBoxW(hDlg,
          (L"Delete entry \"" + fromW + L"\"?").c_str(),
          L"Dictionary Editor", MB_YESNO | MB_ICONQUESTION);

        if (ret == IDYES) {
          st->entries.erase(st->entries.begin() + static_cast<ptrdiff_t>(realIdx));
          st->modified = true;
          applyFilter(hDlg, st);
        }
        return TRUE;
      }

      // Save button.
      if (id == IDC_DICT_SAVE) {
        std::wstring path = dictTsvPath(st->packDir, st->currentLang, st->currentType);
        std::string err;
        if (saveDictTsv(path, st->currentType, st->entries, err)) {
          st->modified = false;
          updateStatusText(hDlg, st);
          msgBox(hDlg, L"Dictionary saved.", L"Dictionary Editor", MB_ICONINFORMATION);
        } else {
          msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
        }
        return TRUE;
      }

      // Import button.
      if (id == IDC_DICT_IMPORT) {
        std::wstring importPath;
        if (openTsvFileDialog(hDlg, importPath)) {
          std::vector<DictEntryData> imported;
          std::string err;
          if (!loadDictTsv(importPath, st->currentType, imported, err)) {
            msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
            return TRUE;
          }

          // Merge: append entries whose fromText is not already present.
          std::unordered_set<std::string> existing;
          for (const auto& e : st->entries) {
            existing.insert(e.fromText);
          }

          int added = 0;
          for (auto& e : imported) {
            if (existing.find(e.fromText) == existing.end()) {
              existing.insert(e.fromText);
              st->entries.push_back(std::move(e));
              ++added;
            }
          }

          if (added > 0) {
            st->modified = true;
            applyFilter(hDlg, st);
          }

          msgBox(hDlg,
            L"Imported " + std::to_wstring(added) + L" new entries (" +
            std::to_wstring(imported.size() - static_cast<size_t>(added)) + L" duplicates skipped).",
            L"Dictionary Editor", MB_ICONINFORMATION);
        }
        return TRUE;
      }

      // Export button.
      if (id == IDC_DICT_EXPORT) {
        std::wstring exportPath;
        if (saveTsvFileDialog(hDlg, exportPath)) {
          std::string err;
          if (saveDictTsv(exportPath, st->currentType, st->entries, err)) {
            msgBox(hDlg, L"Exported " + std::to_wstring(st->entries.size()) + L" entries.",
                   L"Dictionary Editor", MB_ICONINFORMATION);
          } else {
            msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
          }
        }
        return TRUE;
      }

      // Close / Cancel.
      if (id == IDCANCEL) {
        if (st->modified) {
          int ret = MessageBoxW(hDlg, L"Save changes before closing?",
                                L"Dictionary Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
          if (ret == IDCANCEL) return TRUE;
          if (ret == IDYES) {
            std::wstring path = dictTsvPath(st->packDir, st->currentLang, st->currentType);
            std::string err;
            if (!saveDictTsv(path, st->currentType, st->entries, err)) {
              msgBox(hDlg, utf8ToWide(err), L"Dictionary Editor", MB_ICONERROR);
              return TRUE;
            }
            st->modified = false;
          }
        }
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }

      break;
    }

    case WM_NOTIFY: {
      NMHDR* nmh = reinterpret_cast<NMHDR*>(lParam);
      if (nmh->idFrom == IDC_DICT_LIST && nmh->code == NM_DBLCLK) {
        // Double-click on listview: same as Edit.
        SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_DICT_EDIT, BN_CLICKED), 0);
        return TRUE;
      }
      break;
    }

    case WM_CLOSE:
      // Treat as cancel.
      SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
      return TRUE;
  }

  return FALSE;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool ShowDictionaryEditorDialog(HINSTANCE hInst, HWND parent, DictionaryEditorState& st) {
  st.hInst = hInst;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_DICT_EDITOR), parent,
                  DictEditorDlgProc, reinterpret_cast<LPARAM>(&st));
  return st.modified;
}

} // namespace tgsb_editor
