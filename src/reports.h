#ifndef _REPORTS_H_
#define _REPORTS_H_

#include <stddef.h>
#include <stdint.h>

// G29 HID report
typedef struct __attribute__((packed)) {
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint32_t dpad : 4;
    uint32_t square : 1;
    uint32_t cross : 1;
    uint32_t circle : 1;
    uint32_t triangle : 1;
    uint32_t L1 : 1;
    uint32_t R1 : 1;
    uint32_t L2 : 1;
    uint32_t R2 : 1;
    uint32_t select : 1;
    uint32_t start : 1;
    uint32_t L3 : 1;
    uint32_t R3 : 1;
    uint32_t PS : 1;
    uint32_t touchpad : 1;
    uint32_t counter : 6;
    uint8_t whatever[35];
    uint16_t wheel;
    uint16_t throttle;
    uint16_t brake;
    uint16_t clutch;
    uint8_t gears;  // bits 0-5 = gears 1-6, bit 7 = reverse
    uint16_t whatever2;
    uint8_t enter : 1;
    uint8_t dial_down : 1;
    uint8_t dial_up : 1;
    uint8_t minus : 1;
    uint8_t plus : 1;
    uint8_t whatever3 : 3;
    uint8_t whatever4[9];
} g29_report_t;

_Static_assert(sizeof(g29_report_t) == 63, "g29_report_t should be 63 bytes");

// Driving Force HID report
typedef struct __attribute__((packed)) {
    uint32_t wheel : 10;
    uint32_t cross : 1;
    uint32_t square : 1;
    uint32_t circle : 1;
    uint32_t triangle : 1;
    uint32_t R1 : 1;
    uint32_t L1 : 1;
    uint32_t R2 : 1;
    uint32_t L2 : 1;
    uint32_t select : 1;
    uint32_t start : 1;
    uint32_t R3 : 1;
    uint32_t L3 : 1;
    uint32_t whatever : 2;
    uint8_t y;
    uint32_t hat : 4;
    uint32_t whatever2 : 4;
    uint8_t throttle;
    uint8_t brake;
} df_report_t;

// Driving Force Pro HID report (native mode)
// The first 12 buttons are assumed to be in the same order as in compatibility mode.
typedef struct __attribute__((packed)) {
    uint32_t wheel : 14;
    uint32_t cross : 1;
    uint32_t square : 1;
    uint32_t circle : 1;
    uint32_t triangle : 1;
    uint32_t R1 : 1;
    uint32_t L1 : 1;
    uint32_t R2 : 1;
    uint32_t L2 : 1;
    uint32_t select : 1;
    uint32_t start : 1;
    uint32_t R3 : 1;
    uint32_t L3 : 1;
    uint32_t upshift : 1;  // gear lever
    uint32_t downshift : 1;
    uint32_t hat : 4;
    uint8_t y;  // combined pedals
    uint8_t throttle;
    uint8_t brake;
    uint8_t whatever;
} dfp_report_t;

_Static_assert(sizeof(dfp_report_t) == 8, "dfp_report_t should be 8 bytes");

// G25 and G27 HID report (native mode), from memory of the wheels' report descriptors,
// not checked against real hardware. The G27 uses all 22 buttons, the G25 only 19.
// Assumed button numbering (1-based):
//   1-12   same order as the 12 buttons in compatibility mode
//   13-18  shifter gears 1-6 (H-pattern)
//   19     G25: reverse, G27: extra wheel button
//   20-22  G27: extra wheel buttons
typedef struct __attribute__((packed)) {
    uint64_t hat : 4;
    uint64_t buttons : 22;  // button 1 is bit 0
    uint64_t wheel : 14;
    uint64_t throttle : 8;
    uint64_t brake : 8;
    uint64_t clutch : 8;
    uint8_t shifter[3];  // raw shifter position and flags
} g25_report_t;

_Static_assert(sizeof(g25_report_t) == 11, "g25_report_t should be 11 bytes");

#endif
