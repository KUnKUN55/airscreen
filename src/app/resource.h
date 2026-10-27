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
#define IDM_SCENE_BASE      40040  // + scene index (0 Intro, 1 Screen, 2 Break, 3 Outro)
#define IDM_SCENE_EDIT      40050
#define IDM_SCENE_FRAMED    40051
#define IDM_CANVAS_BASE     40055  // + 0 1080p, 1 1440p, 2 2160p
#define IDM_BG_COLORS       40060
#define IDM_BG_BLUR         40061
#define IDM_BG_IMAGE        40062  // use the picture already chosen
#define IDM_BG_PICK         40063  // choose a picture…

#define IDD_SCENES          102
#define IDC_SCENE_TEXT0     1010   // .. IDC_SCENE_TEXT0 + 5
#define IDM_MIC_DEV_BASE    40100  // + index into the device list (up to 99)
