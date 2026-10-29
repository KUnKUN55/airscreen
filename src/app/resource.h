#pragma once

#define IDD_RENAME          101
#define IDC_NAME            1001
#define IDI_APP             102
#define IDR_LOGO            103  // logo_hi.png, drawn on the home screen

#define IDM_TRAY_SHOW       40001
#define IDM_TRAY_RENAME     40002
#define IDM_TRAY_TOP        40003
#define IDM_TRAY_PIN        40004
#define IDM_TRAY_FULL       40005
#define IDM_TRAY_FIREWALL   40006
#define IDM_TRAY_LOG        40007
#define IDM_TRAY_EXIT       40008
#define IDM_TRAY_FILL       40009
#define IDC_SETTINGS        40010
#define IDM_TRAY_STARTUP    40011
#define IDM_REC_TOGGLE      40012
#define IDM_REC_FOLDER      40013
#define IDM_QUALITY         40014
#define IDM_SMOOTH          40015
#define IDM_MIC_OFF         40020
#define IDM_MIC_DEFAULT     40021
#define IDM_MIC_GAIN_BASE   40030  // + index into the gain steps
#define IDM_SCENE_BASE      40040  // + index into the profile's scenes (up to 10)
#define IDM_SCENE_EDIT      40050
#define IDM_CANVAS_BASE     40055  // + 0 1080p, 1 1440p, 2 2160p
#define IDM_MIC_DEV_BASE    40100  // + index into the device list (up to 99)

// Teaching studio dialogs
#define IDD_PROMPT          110
#define IDC_PROMPT_LABEL    1100
#define IDC_PROMPT_EDIT     1101
#define IDD_PICKER          111
#define IDC_PICK_LIST       1110
#define IDC_PICK_PREVIEW    1111
#define IDC_PICK_WARN       1112
#define IDC_PICK_CURSOR     1113
#define IDC_PICK_REFRESH    1114
#define IDD_LIVETEXT        112
#define IDC_LT_LAYER        1120
#define IDC_LT_TEXT         1121
#define IDC_LT_SNIP         1122
#define IDC_LT_USE          1123
#define IDC_LT_SHOW         1124
#define IDC_LT_KEEP         1125
#define IDD_PREFS           113
#define IDC_PREF_TABS       1130
#define IDD_PREF_PANEL      114
#define IDC_PP_SHARE        1140
#define IDC_PP_RECORD       1141
#define IDC_PP_STARTUP      1142
#define IDC_PP_TOP          1143
#define IDC_PP_COMPACT      1144
#define IDC_PP_HIDE         1145
#define IDC_PP_TRANSITION   1146
#define IDC_PP_APPLY        1147
#define IDC_PP_FPS          1148
#define IDC_PP_CURSOR       1149
#define IDD_PREF_QUICK      115
#define IDC_PQ_SLOT0        1150   // .. IDC_PQ_SLOT0 + 7
#define IDD_PREF_KEYS       116
#define IDC_PK_ENABLE       1160
#define IDC_PK_LIST         1161
#define IDC_PK_HOTKEY       1162
#define IDC_PK_SET          1163
#define IDC_PK_CLEAR        1164
#define IDC_PK_RESET        1165
#define IDC_PK_NOTE         1166
#define IDD_PREF_PROFILES   117
#define IDC_PF_LIST         1170
#define IDC_PF_USE          1171
#define IDC_PF_NEW          1172
#define IDC_PF_DUP          1173
#define IDC_PF_RENAME       1174
#define IDC_PF_DELETE       1175
#define IDM_PANEL           40070
#define IDM_EDITOR          40071
#define IDM_PREVIEW         40072
#define IDM_COMPUTER        40073
#define IDM_COMPUTER_STOP   40074
#define IDM_TEACH_PREFS     40075
#define IDM_REC_PAUSE       40076
