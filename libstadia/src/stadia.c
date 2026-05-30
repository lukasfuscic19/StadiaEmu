/*
 * stadia.c -- Routines for interacting with a Stadia controller.
 *
 * Supports two modes:
 *   USB  — HID via hid_device (overlapped ReadFile / WriteFile)
 *   BLE  — GATT via gatt_device (Win32 poll + WinRT write)
 */

#include "stadia.h"
#include "hid.h"
#include "gatt.h"

#include <stdio.h>
#include <synchapi.h>
#include <tchar.h>
#include <windows.h>

#pragma comment(lib, "kernel32.lib")

#define STADIA_READ_TIMEOUT        10
#define STADIA_VIBRATION_REPORT_ID 0x05

static const BYTE init_vibration[5] = {STADIA_VIBRATION_REPORT_ID, 0x00, 0x00, 0x00, 0x00};

static const DWORD dpad_map[8] =
{
    STADIA_BUTTON_UP,
    STADIA_BUTTON_UP    | STADIA_BUTTON_RIGHT,
    STADIA_BUTTON_RIGHT,
    STADIA_BUTTON_RIGHT | STADIA_BUTTON_DOWN,
    STADIA_BUTTON_DOWN,
    STADIA_BUTTON_DOWN  | STADIA_BUTTON_LEFT,
    STADIA_BUTTON_LEFT,
    STADIA_BUTTON_LEFT  | STADIA_BUTTON_UP
};

/* -----------------------------------------------------------------------
 * Input packet parser — shared by USB thread and BLE callback
 * ----------------------------------------------------------------------- */
static void parse_input(struct stadia_controller *c, const BYTE *buf, size_t len)
{
    if (len < 10) return;
    if (buf[0] != 0x03) return;

    AcquireSRWLockExclusive(&c->state_lock);

    c->state.buttons = STADIA_BUTTON_NONE;
    c->state.buttons |= buf[1] < 8 ? dpad_map[buf[1]] : 0;

    c->state.buttons |= (buf[2] & (1<<7)) ? STADIA_BUTTON_RS          : 0;
    c->state.buttons |= (buf[2] & (1<<6)) ? STADIA_BUTTON_OPTIONS     : 0;
    c->state.buttons |= (buf[2] & (1<<5)) ? STADIA_BUTTON_MENU        : 0;
    c->state.buttons |= (buf[2] & (1<<4)) ? STADIA_BUTTON_STADIA_BTN  : 0;

    c->state.buttons |= (buf[3] & (1<<6)) ? STADIA_BUTTON_A  : 0;
    c->state.buttons |= (buf[3] & (1<<5)) ? STADIA_BUTTON_B  : 0;
    c->state.buttons |= (buf[3] & (1<<4)) ? STADIA_BUTTON_X  : 0;
    c->state.buttons |= (buf[3] & (1<<3)) ? STADIA_BUTTON_Y  : 0;
    c->state.buttons |= (buf[3] & (1<<2)) ? STADIA_BUTTON_LB : 0;
    c->state.buttons |= (buf[3] & (1<<1)) ? STADIA_BUTTON_RB : 0;
    c->state.buttons |= (buf[3] & (1<<0)) ? STADIA_BUTTON_LS : 0;

    c->state.left_stick_x  = buf[4];
    c->state.left_stick_y  = buf[5];
    c->state.right_stick_x = buf[6];
    c->state.right_stick_y = buf[7];
    c->state.left_trigger  = buf[8];
    c->state.right_trigger = buf[9];

    ReleaseSRWLockExclusive(&c->state_lock);

    stadia_update_callback(c, &c->state);
}

/* -----------------------------------------------------------------------
 * USB input thread
 * ----------------------------------------------------------------------- */
static DWORD WINAPI _stadia_input_thread(LPVOID lparam)
{
    struct stadia_controller *c = (struct stadia_controller*)lparam;
    INT bytes_read;

    while (c->active) {
        bytes_read = hid_get_input_report(c->device, STADIA_READ_TIMEOUT);

        if (bytes_read == 0) {
            /* Timeout — check stop signal and retry */
            if (WaitForSingleObject(c->stopping_event, 0) == WAIT_OBJECT_0)
                break;
            continue;
        }

        if (bytes_read < 0)
            break; /* Device disconnected or I/O error */

        parse_input(c, c->device->input_buffer, (size_t)bytes_read);
    }

    /*
     * Do NOT call stadia_controller_destroy() from this thread —
     * that would deadlock (destroy waits for this thread to exit).
     * Instead signal active=FALSE and call the destroy callback,
     * letting main thread do the cleanup.
     */
    c->active = FALSE;
    SetEvent(c->stopping_event);
    stadia_destroy_callback(c);
    return 0;
}

/* -----------------------------------------------------------------------
 * Output thread — vibration (USB and BLE)
 * ----------------------------------------------------------------------- */
static DWORD WINAPI _stadia_output_thread(LPVOID lparam)
{
    struct stadia_controller *c = (struct stadia_controller*)lparam;
    BYTE vibration[5] = {STADIA_VIBRATION_REPORT_ID, 0x00, 0x00, 0x00, 0x00};
    HANDLE events[2] = {c->output_event, c->stopping_event};

    while (c->active) {
        WaitForMultipleObjects(2, events, FALSE, INFINITE);

        AcquireSRWLockShared(&c->vibration_lock);
        vibration[2] = c->big_motor;
        vibration[4] = c->small_motor;
        ReleaseSRWLockShared(&c->vibration_lock);

        if (c->is_bluetooth) {
            if (c->gatt)
                gatt_send_output_report(c->gatt, vibration, sizeof(vibration));
        } else {
            hid_send_output_report(c->device, vibration, sizeof(vibration),
                                   STADIA_READ_TIMEOUT);
        }

        ResetEvent(c->output_event);
    }

    /* Stop motors on exit */
    if (c->is_bluetooth && c->gatt)
        gatt_send_output_report(c->gatt, init_vibration, sizeof(init_vibration));
    else if (!c->is_bluetooth && c->device)
        hid_send_output_report(c->device, init_vibration, sizeof(init_vibration),
                               STADIA_READ_TIMEOUT);
    return 0;
}

/* -----------------------------------------------------------------------
 * BLE input callback — called from GATT poll thread
 * ----------------------------------------------------------------------- */
static void _gatt_input_cb(const BYTE *data, size_t len, void *userdata)
{
    struct stadia_controller *c = (struct stadia_controller*)userdata;
    if (!c->active) return;
    parse_input(c, data, len);
}

static void _gatt_disconnect_cb(void *userdata)
{
    struct stadia_controller *c = (struct stadia_controller*)userdata;
    if (!c->active) return;
    c->active = FALSE;
    SetEvent(c->stopping_event);
    stadia_destroy_callback(c);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

struct stadia_controller *stadia_controller_create(struct hid_device *device,
                                                   BOOL               is_bluetooth,
                                                   const WCHAR       *bt_address)
{
    SECURITY_ATTRIBUTES sa = {
        .nLength              = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = NULL,
        .bInheritHandle       = TRUE
    };

    struct stadia_controller *c =
        (struct stadia_controller*)calloc(1, sizeof(struct stadia_controller));

    c->device       = device;
    c->is_bluetooth = is_bluetooth;
    c->active       = TRUE;

    InitializeSRWLock(&c->state_lock);
    InitializeSRWLock(&c->vibration_lock);
    c->stopping_event = CreateEvent(&sa, TRUE, FALSE, NULL);
    c->output_event   = CreateEvent(&sa, TRUE, FALSE, NULL);

    if (is_bluetooth) {
        if (!bt_address) { stadia_controller_destroy(c); return NULL; }

        c->gatt = gatt_open_stadia(bt_address, _gatt_input_cb, _gatt_disconnect_cb, c);
        if (!c->gatt) { stadia_controller_destroy(c); return NULL; }

        /* Init vibration (non-fatal) */
        gatt_send_output_report(c->gatt, init_vibration, sizeof(init_vibration));

        /* BLE input arrives via callback — only output thread needed */
        c->input_thread  = NULL;
        c->output_thread = CreateThread(&sa, 0, _stadia_output_thread, c, 0, NULL);
    } else {
        /* USB: test vibration init (non-fatal) */
        hid_send_output_report(device, init_vibration, sizeof(init_vibration),
                               STADIA_READ_TIMEOUT);

        c->input_thread  = CreateThread(&sa, 0, _stadia_input_thread,  c,
                                        CREATE_SUSPENDED, NULL);
        c->output_thread = CreateThread(&sa, 0, _stadia_output_thread, c,
                                        CREATE_SUSPENDED, NULL);

        if (!c->input_thread || !c->output_thread) {
            stadia_controller_destroy(c);
            return NULL;
        }

        ResumeThread(c->input_thread);
        ResumeThread(c->output_thread);
    }

    return c;
}

void stadia_controller_set_vibration(struct stadia_controller *c,
                                     BYTE small_motor, BYTE big_motor)
{
    AcquireSRWLockExclusive(&c->vibration_lock);
    c->small_motor = small_motor;
    c->big_motor   = big_motor;
    ReleaseSRWLockExclusive(&c->vibration_lock);
    SetEvent(c->output_event);
}

void stadia_controller_destroy(struct stadia_controller *c)
{
    if (!c) return;

    c->active = FALSE;

    if (c->device)
        CancelIoEx(c->device->handle, &c->device->input_ol);

    SetEvent(c->stopping_event);
    SetEvent(c->output_event);

    /*
     * Wait for threads — but never wait on the calling thread itself
     * (input thread calls stadia_destroy_callback, not this function).
     */
    DWORD calling_tid = GetCurrentThreadId();
    HANDLE wait[2];
    int wc = 0;

    if (c->input_thread  && GetThreadId(c->input_thread)  != calling_tid)
        wait[wc++] = c->input_thread;
    if (c->output_thread && GetThreadId(c->output_thread) != calling_tid)
        wait[wc++] = c->output_thread;

    if (wc > 0)
        WaitForMultipleObjects(wc, wait, TRUE, 3000);

    if (c->gatt) { gatt_close(c->gatt); c->gatt = NULL; }

    CloseHandle(c->stopping_event);
    CloseHandle(c->output_event);
    if (c->input_thread)  CloseHandle(c->input_thread);
    if (c->output_thread) CloseHandle(c->output_thread);

    free(c);
}
