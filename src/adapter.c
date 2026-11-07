// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "pico/stdio.h"
#include "pico/time.h"

#ifdef PICO_DEFAULT_PIO_USB_DP_PIN2
#include "pio_usb.h"
#endif

#include "reports.h"

uint8_t nonce_id;
uint8_t nonce[280];
uint8_t nonce_part = 0;
uint8_t signature[1064];
uint8_t signature_part = 0;
uint8_t signature_ready = 0;
uint8_t nonce_ready = 0;

uint8_t expected_part = 0;

uint8_t wheel_device = 0;
uint8_t wheel_instance = 0;
uint8_t auth_device = 0;
uint8_t auth_instance = 0;

bool busy = false;

enum {
    IDLE = 0,
    SENDING_RESET = 1,
    SENDING_NONCE = 2,
    WAITING_FOR_SIG = 3,
    RECEIVING_SIG = 4,
};

uint8_t state = IDLE;

bool initialized = true;

#define LOGITECH_VID 0x046d
#define PID_DRIVING_FORCE 0xc294  // Driving Force, or another wheel in compatibility mode
#define PID_DRIVING_FORCE_PRO 0xc298
#define PID_G25 0xc299
#define PID_G27 0xc29b

enum {
    WHEEL_NONE = 0,
    WHEEL_DF_COMPAT = 1,      // Driving Force compatibility mode (c294)
    WHEEL_SWITCHING = 2,      // wheel told to switch to native mode, waiting for it to reconnect
    WHEEL_DFP_NATIVE = 3,     // Driving Force Pro in native mode (c298)
    WHEEL_G25_NATIVE = 4,     // G25 (c299) or G27 (c29b) in native mode
};

uint8_t wheel_mode = WHEEL_NONE;
uint16_t wheel_bcd_device = 0;
uint32_t switch_sent_at_ms = 0;

// A G25 or G27 in compatibility mode only switches to native mode when L1+R1+L2+R2
// are pressed together. This remembers whether they were held in the previous report,
// so that holding them doesn't keep switching.
bool switch_combo_held = false;

// If a wheel we told to switch modes hasn't disconnected after this long, use it in compatibility mode.
#define SWITCH_TIMEOUT_MS 3000

// Commands waiting to be sent to the wheel, both our own and the ones from the console.
#define WHEEL_CMD_QUEUE_SIZE 32
uint8_t wheel_cmd_queue[WHEEL_CMD_QUEUE_SIZE][7];
bool wheel_cmd_from_console[WHEEL_CMD_QUEUE_SIZE];
uint8_t wheel_cmd_head = 0;
uint8_t wheel_cmd_count = 0;

dfp_report_t prev_dfp_report;
uint32_t prev_g25_buttons;
bool native_is_g25 = false;  // G25 or G27 in native mode

uint8_t get_buffer[64];
uint8_t set_buffer[64];

g29_report_t report;
g29_report_t prev_report;

// G29
const uint8_t output_0x03[] = {
    0x21, 0x27, 0x03, 0x11, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0D, 0x0D, 0x00, 0x00, 0x00, 0x00,
    0x0D, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t output_0xf3[] = { 0x0, 0x38, 0x38, 0, 0, 0, 0 };

void report_init() {
    memset(&report, 0, sizeof(report));
    report.lx = 0x80;
    report.ly = 0x80;
    report.rx = 0x80;
    report.ry = 0x80;
    report.clutch = 0xFFFF;
    memcpy(&prev_report, &report, sizeof(report));
}

// Repeated force updates would flood the UART, so a console command is only printed
// if its first two bytes differ from the previous one.
void print_wheel_cmd(bool from_console, uint8_t const* cmd) {
    static uint8_t last[2];
    if (from_console && (cmd[0] == last[0]) && (cmd[1] == last[1])) {
        return;
    }
    if (from_console) {
        last[0] = cmd[0];
        last[1] = cmd[1];
    }
    printf("%s -> wheel: %02x %02x %02x %02x %02x %02x %02x\n", from_console ? "console" : "adapter", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5], cmd[6]);
}

uint32_t now_ms() {
    return to_ms_since_boot(get_absolute_time());
}

void wheel_cmd_clear() {
    wheel_cmd_head = 0;
    wheel_cmd_count = 0;
}

void wheel_cmd_enqueue(uint8_t const* cmd, bool from_console) {
    if (from_console && (wheel_cmd_count > 0)) {
        // If the last command still waiting is the console's and of the same kind (same
        // command and slots, same extended command), the new one replaces it. This keeps
        // repeated force updates from piling up when the wheel accepts them slower than the
        // console sends them, without reordering anything.
        uint8_t tail = (wheel_cmd_head + wheel_cmd_count - 1) % WHEEL_CMD_QUEUE_SIZE;
        uint8_t* last = wheel_cmd_queue[tail];
        if (wheel_cmd_from_console[tail] && (last[0] == cmd[0]) && (last[1] == cmd[1])) {
            memcpy(last, cmd, 7);
            return;
        }
    }
    if (wheel_cmd_count == WHEEL_CMD_QUEUE_SIZE) {
        printf("wheel command queue full, dropping %02x %02x\n", cmd[0], cmd[1]);
        return;
    }
    uint8_t idx = (wheel_cmd_head + wheel_cmd_count) % WHEEL_CMD_QUEUE_SIZE;
    memcpy(wheel_cmd_queue[idx], cmd, 7);
    wheel_cmd_from_console[idx] = from_console;
    wheel_cmd_count++;
}

void wheel_cmd_push(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6) {
    uint8_t cmd[7] = { b0, b1, b2, b3, b4, b5, b6 };
    wheel_cmd_enqueue(cmd, false);
}

// We only care about the Driving Force Pro for now.
bool is_dfp_in_compat_mode(uint16_t bcd_device) {
    if ((bcd_device & 0xfff8) == 0x1350 ||  // G29
        (bcd_device & 0xff00) == 0x8900 ||  // G29
        (bcd_device & 0xff00) == 0x1300 ||  // DFGT
        (bcd_device & 0xfff0) == 0x1230 ||  // G27
        (bcd_device & 0xff00) == 0x1200) {  // G25
        return false;
    }
    return (bcd_device & 0xf000) == 0x1000;
}

// Same bcdDevice checks as hid-lg4ff, the G27 one has to come first.
bool is_g27_in_compat_mode(uint16_t bcd_device) {
    return (bcd_device & 0xfff0) == 0x1230;
}

bool is_g25_in_compat_mode(uint16_t bcd_device) {
    return !is_g27_in_compat_mode(bcd_device) && ((bcd_device & 0xff00) == 0x1200);
}

// Called with every compatibility mode report, switches a G25 or G27 to native mode
// when L1+R1+L2+R2 are pressed together.
void check_switch_combo(df_report_t const* df) {
    bool held = df->L1 && df->R1 && df->L2 && df->R2;
    bool pressed = held && !switch_combo_held;
    switch_combo_held = held;
    if (!pressed) {
        return;
    }
    if (is_g27_in_compat_mode(wheel_bcd_device)) {
        printf("L1+R1+L2+R2 pressed, switching G27 to native mode\n");
        wheel_cmd_clear();
        wheel_cmd_push(0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00);
        wheel_cmd_push(0xf8, 0x09, 0x04, 0x01, 0x00, 0x00, 0x00);
    } else if (is_g25_in_compat_mode(wheel_bcd_device)) {
        printf("L1+R1+L2+R2 pressed, switching G25 to native mode\n");
        wheel_cmd_clear();
        wheel_cmd_push(0xf8, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00);
    } else {
        return;
    }
    wheel_mode = WHEEL_SWITCHING;
    initialized = true;
    switch_sent_at_ms = now_ms();
}

void hid_task() {
    if (!tud_hid_ready()) {
        return;
    }

    if (memcmp(&prev_report, &report, sizeof(report))) {
        tud_hid_report(1, &report, sizeof(report));
        memcpy(&prev_report, &report, sizeof(report));
    }
}

void wheel_task() {
    if (!wheel_device) {
        return;
    }

    if ((wheel_mode == WHEEL_SWITCHING) && (now_ms() - switch_sent_at_ms > SWITCH_TIMEOUT_MS)) {
        printf("Wheel didn't switch to native mode, using it in compatibility mode\n");
        wheel_mode = WHEEL_DF_COMPAT;
        initialized = false;
        tuh_hid_receive_report(wheel_device, wheel_instance);
    }

    if (!initialized) {
        initialized = true;
        // A G29 starts at 900 degrees and games don't necessarily set the range, but
        // a DFP starts at 200.
        if (wheel_mode == WHEEL_DFP_NATIVE) {
            wheel_cmd_push(0xf8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00);  // 900 degree coarse range
            wheel_cmd_push(0x81, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00);  // no fine limit
        }
        if (wheel_mode == WHEEL_G25_NATIVE) {
            wheel_cmd_push(0xf8, 0x81, 0x84, 0x03, 0x00, 0x00, 0x00);  // 900 degrees
        }
        // Disable autocenter. This has to come after the range commands, they turn it back on.
        wheel_cmd_push(0xf5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    }

    if ((wheel_cmd_count > 0) && tuh_hid_send_ready(wheel_device, wheel_instance)) {
        if (tuh_hid_send_report(wheel_device, wheel_instance, 0, wheel_cmd_queue[wheel_cmd_head], 7)) {
            print_wheel_cmd(wheel_cmd_from_console[wheel_cmd_head], wheel_cmd_queue[wheel_cmd_head]);
            wheel_cmd_head = (wheel_cmd_head + 1) % WHEEL_CMD_QUEUE_SIZE;
            wheel_cmd_count--;
            if ((wheel_mode == WHEEL_SWITCHING) && (wheel_cmd_count == 0)) {
                switch_sent_at_ms = now_ms();
            }
        }
    }
}

void auth_task() {
    if (!busy && auth_device) {
        switch (state) {
            case IDLE:
                break;
            case SENDING_RESET:
                tuh_hid_get_report(auth_device, auth_instance, 0xF3, HID_REPORT_TYPE_FEATURE, get_buffer, 7 + 1);
                busy = true;
                break;
            case SENDING_NONCE:
                set_buffer[0] = 0xF0;
                set_buffer[1] = nonce_id;
                set_buffer[2] = nonce_part;
                set_buffer[3] = 0;
                memcpy(set_buffer + 4, nonce + (nonce_part * 56), 56);
                printf(".");
                tuh_hid_set_report(auth_device, auth_instance, 0xF0, HID_REPORT_TYPE_FEATURE, set_buffer, 64);
                busy = true;
                nonce_part++;
                break;
            case WAITING_FOR_SIG:
                tuh_hid_get_report(auth_device, auth_instance, 0xF2, HID_REPORT_TYPE_FEATURE, get_buffer, 15 + 1);
                busy = true;
                break;
            case RECEIVING_SIG:
                tuh_hid_get_report(auth_device, auth_instance, 0xF1, HID_REPORT_TYPE_FEATURE, get_buffer, 63 + 1);
                busy = true;
                break;
        }
    }
}

int main() {
    board_init();
    report_init();
    tusb_init();
#ifdef PICO_DEFAULT_PIO_USB_DP_PIN2
    pio_usb_host_add_port(PICO_DEFAULT_PIO_USB_DP_PIN2, PIO_USB_PINOUT_DPDM);
#endif
    stdio_init_all();

    while (1) {
        tuh_task();
        tud_task();
        hid_task();
        auth_task();
        wheel_task();
    }

    return 0;
}

void tuh_hid_get_report_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len) {
    if (dev_addr == auth_device) {
        busy = false;
        switch (report_id) {
            case 0xF3:
                printf("Sending nonce to auth controller");
                state = SENDING_NONCE;
                break;
            case 0xF2:
                // printf(".");
                if (get_buffer[2] == 0) {
                    signature_part = 0;
                    state = RECEIVING_SIG;
                    printf("\n");
                    printf("Receiving signature from auth controller");
                }
                break;
            case 0xF1:
                memcpy(signature + (signature_part * 56), get_buffer + 4, 56);
                signature_part++;
                printf(".");
                if (signature_part == 19) {
                    state = IDLE;
                    expected_part = 0;
                    signature_ready = true;
                    signature_part = 0;
                    printf("\n");
                }
                break;
        }
    }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t report_id, uint8_t report_type, uint16_t len) {
    if ((dev_addr == auth_device) && (report_id == 0xF0)) {
        busy = false;
        if (nonce_part == 5) {
            printf("\n");
            printf("Waiting for auth controller to sign...\n");
            state = WAITING_FOR_SIG;
        }
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    switch (report_id) {
        case 0x03:
            memcpy(buffer, output_0x03, reqlen);
            board_led_write(false);
            return reqlen;
        case 0xF3:
            memcpy(buffer, output_0xf3, reqlen);
            signature_ready = false;
            return reqlen;
        case 0xF1: {  // GET_SIGNATURE_NONCE
            buffer[0] = nonce_id;
            buffer[1] = signature_part;
            buffer[2] = 0;
            if (signature_part == 0) {
                printf("Sending signature to PS5");
            }
            printf(".");
            memcpy(&buffer[3], &signature[signature_part * 56], 56);
            signature_part++;
            if (signature_part == 19) {
                signature_part = 0;
                printf("\n");
                board_led_write(true);
            }
            return reqlen;
        }
        case 0xF2: {  // GET_SIGNING_STATE
            printf("PS5 asks if signature ready (%s).\n", signature_ready ? "yes" : "no");
            buffer[0] = nonce_id;
            buffer[1] = signature_ready ? 0 : 16;
            memset(&buffer[2], 0, 9);
            return reqlen;
        }
    }
    return reqlen;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (report_id == 0xF0) {  // SET_AUTH_PAYLOAD
        uint8_t part = expected_part;
        if (bufsize == 63) {
            nonce_id = buffer[0];
            part = buffer[1];
        }
        if (part == 0) {
            printf("Getting nonce from PS5");
        }
        printf(".");
        if (part > 4) {
            return;
        }
        expected_part = part + 1;
        memcpy(&nonce[part * 56], &buffer[3], 56);
        if (part == 4) {
            nonce_ready = 1;
            printf("\n");
            printf("Sending reset to auth controller...\n");
            state = SENDING_RESET;
            nonce_part = 0;
        }
    } else {
        // Pass everything through to the wheel. Commands are 7 bytes, after the report ID.
        // Until the wheel is ready to be used they're dropped, the wheel gets initialized
        // on its own when it connects.
        if ((bufsize >= 8) && (wheel_mode == WHEEL_DF_COMPAT || wheel_mode == WHEEL_DFP_NATIVE || wheel_mode == WHEEL_G25_NATIVE)) {
            wheel_cmd_enqueue(buffer + 1, true);
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    printf("tuh_hid_mount_cb %04x:%04x %d %d\n", vid, pid, dev_addr, instance);

    if ((vid == LOGITECH_VID) && (pid == PID_DRIVING_FORCE)) {
        tusb_desc_device_t desc;
        uint16_t bcd_device = 0;
        if (tuh_descriptor_get_device_local(dev_addr, &desc)) {
            bcd_device = desc.bcdDevice;
        }
        printf("bcdDevice %04x\n", bcd_device);

        wheel_device = dev_addr;
        wheel_instance = instance;
        wheel_bcd_device = bcd_device;
        wheel_cmd_clear();
        // Don't switch right away if the buttons are already held when the wheel connects.
        switch_combo_held = true;

        if (is_dfp_in_compat_mode(bcd_device)) {
            // The wheel disconnects and comes back as a Driving Force Pro (c298).
            printf("Driving Force Pro in compatibility mode, switching to native mode\n");
            wheel_mode = WHEEL_SWITCHING;
            initialized = true;
            switch_sent_at_ms = now_ms();
            wheel_cmd_push(0xf8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
        } else {
            if (is_g25_in_compat_mode(bcd_device) || is_g27_in_compat_mode(bcd_device)) {
                printf("G25/G27 in compatibility mode, press L1+R1+L2+R2 to switch to native mode\n");
            }
            wheel_mode = WHEEL_DF_COMPAT;
            initialized = false;
            tuh_hid_receive_report(dev_addr, instance);
        }
    } else if ((vid == LOGITECH_VID) && (pid == PID_DRIVING_FORCE_PRO)) {
        printf("Driving Force Pro in native mode\n");
        wheel_device = dev_addr;
        wheel_instance = instance;
        wheel_mode = WHEEL_DFP_NATIVE;
        wheel_cmd_clear();
        memset(&prev_dfp_report, 0, sizeof(prev_dfp_report));
        initialized = false;
        tuh_hid_receive_report(dev_addr, instance);
    } else if ((vid == LOGITECH_VID) && ((pid == PID_G25) || (pid == PID_G27))) {
        printf("%s in native mode\n", (pid == PID_G25) ? "G25" : "G27");
        wheel_device = dev_addr;
        wheel_instance = instance;
        wheel_mode = WHEEL_G25_NATIVE;
        wheel_cmd_clear();
        prev_g25_buttons = 0;
        native_is_g25 = (pid == PID_G25);
        initialized = false;
        tuh_hid_receive_report(dev_addr, instance);
    } else {  // assume everything else is the controller we use for authentication
        auth_device = dev_addr;
        auth_instance = instance;
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("tuh_hid_umount_cb\n");
    if (dev_addr == wheel_device) {
        wheel_device = 0;
        wheel_instance = 0;
        wheel_mode = WHEEL_NONE;
        wheel_cmd_clear();
    }
    if (dev_addr == auth_device) {
        auth_device = 0;
        auth_instance = 0;
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report_, uint16_t len) {
    if (len > 0) {
        if ((dev_addr == wheel_device) && (wheel_mode == WHEEL_DFP_NATIVE) && (len >= sizeof(dfp_report_t))) {
            dfp_report_t* dfp = (dfp_report_t*) report_;
            report.wheel = dfp->wheel << 2;
            report.throttle = dfp->throttle << 8;
            report.brake = dfp->brake << 8;
            report.dpad = dfp->hat;
            report.cross = dfp->cross;
            report.square = dfp->square;
            report.circle = dfp->circle;
            report.triangle = dfp->triangle;
            report.L2 = dfp->L2;
            report.L1 = dfp->L1;
            report.R2 = dfp->R2;
            report.R1 = dfp->R1;
            // The gear lever gets its own buttons, which can be mapped separately in games.
            report.plus = dfp->upshift;
            report.minus = dfp->downshift;
            report.PS = dfp->select;  // select works as the PS button, the console never sees select
            report.start = dfp->start;
            report.R3 = dfp->R3;
            report.L3 = dfp->L3;

            // Print button changes so that we can check the mapping (wheel and pedals left out).
            dfp_report_t masked = *dfp;
            masked.wheel = 0;
            masked.y = 0;
            masked.throttle = 0;
            masked.brake = 0;
            if (memcmp(&masked, &prev_dfp_report, sizeof(masked))) {
                uint8_t const* b = (uint8_t const*) dfp;
                printf("DFP: %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
                prev_dfp_report = masked;
            }
        } else if ((dev_addr == wheel_device) && (wheel_mode == WHEEL_G25_NATIVE) && (len >= sizeof(g25_report_t))) {
            g25_report_t const* g25 = (g25_report_t const*) report_;
            uint32_t buttons = g25->buttons;
            report.wheel = g25->wheel << 2;
            report.throttle = g25->throttle << 8;
            report.brake = g25->brake << 8;
            report.clutch = g25->clutch << 8;
            report.dpad = g25->hat;
            report.cross = (buttons >> 0) & 1;
            report.square = (buttons >> 1) & 1;
            report.circle = (buttons >> 2) & 1;
            report.triangle = (buttons >> 3) & 1;
            report.R1 = (buttons >> 4) & 1;
            report.L1 = (buttons >> 5) & 1;
            report.R2 = (buttons >> 6) & 1;
            report.L2 = (buttons >> 7) & 1;
            report.PS = (buttons >> 8) & 1;  // select works as the PS button, the console never sees select
            report.start = (buttons >> 9) & 1;
            report.R3 = (buttons >> 10) & 1;
            report.L3 = (buttons >> 11) & 1;
            report.gears = (buttons >> 12) & 0x3f;  // gears 1-6
            if (native_is_g25) {
                report.gears |= ((buttons >> 18) & 1) << 7;  // reverse
            } else {
                // The four extra buttons on the G27 wheel.
                report.plus = (buttons >> 18) & 1;
                report.minus = (buttons >> 19) & 1;
                report.dial_up = (buttons >> 20) & 1;
                report.dial_down = (buttons >> 21) & 1;
            }

            // Print button changes so that we can check the mapping.
            if (buttons != prev_g25_buttons) {
                printf("%s buttons: %06lx\n", native_is_g25 ? "G25" : "G27", (unsigned long) buttons);
                prev_g25_buttons = buttons;
            }
        } else if ((dev_addr == wheel_device) && (wheel_mode == WHEEL_DF_COMPAT)) {
            df_report_t* df = (df_report_t*) report_;
            report.wheel = df->wheel << 6;
            report.throttle = df->throttle << 8;
            report.brake = df->brake << 8;
            report.dpad = df->hat;
            report.cross = df->cross;
            report.square = df->square;
            report.circle = df->circle;
            report.triangle = df->triangle;
            report.L2 = df->L2;
            report.L1 = df->L1;
            report.R2 = df->R2;
            report.R1 = df->R1;
            report.PS = df->select;  // select works as the PS button, the console never sees select
            report.start = df->start;
            report.R3 = df->R3;
            report.L3 = df->L3;
            check_switch_combo(df);
        }
    }

    tuh_hid_receive_report(dev_addr, instance);
}
