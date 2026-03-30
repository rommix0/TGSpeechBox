/*
TGSpeechBox — SAPI settings dialog resource identifiers.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#pragma once

#define IDD_MAIN_DIALOG 101

#define IDC_LANG_LIST 1001
#define IDC_ENABLE_LOGGING 1002

// Audio section
#define IDC_SAMPLE_RATE 1010
#define IDC_PAUSE_MODE  1011
#define IDC_PITCH_MODE  1012
#define IDC_SL_INFLECTION 1013

// Voicing Tone sliders (trackbar + static label pairs)
#define IDC_SL_VOICE_TILT      1100
#define IDC_SL_NOISE_MOD       1101
#define IDC_SL_PITCH_F1        1102
#define IDC_SL_PITCH_B1        1103
#define IDC_SL_SPEED_QUOT      1104
#define IDC_SL_ASP_TILT        1105
#define IDC_SL_CASCADE_BW      1106
#define IDC_SL_TREMOR          1107
#define IDC_SL_HEAD_SIZE       1108
#define IDC_SL_CHORUS_DEPTH    1109
#define IDC_SL_CHORUS_DETUNE   1115

// FrameEx sliders
#define IDC_SL_CREAKINESS      1110
#define IDC_SL_BREATHINESS     1111
#define IDC_SL_JITTER          1112
#define IDC_SL_SHIMMER         1113
#define IDC_SL_SHARPNESS       1114

// Rate controls
#define IDC_RATE_BOOST         1120
#define IDC_OVERRIDE_RATE      1121
#define IDC_SL_GLOBAL_RATE     1122

// Reset button
#define IDC_RESET_DEFAULTS     1200
