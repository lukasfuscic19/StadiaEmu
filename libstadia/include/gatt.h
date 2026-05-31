/*
 * gatt.h -- Win32 GATT interface for Stadia BLE controller.
 *
 * Uses SetupAPI + BluetoothGATT Win32 APIs — no Package manifest required
 * for device enumeration and input polling.
 *
 * Vibration write requires package identity (Sparse Package with
 * bluetooth.genericAttributeProfile capability). Without it, write
 * returns ERROR_INVALID_FUNCTION and is silently skipped.
 */

#ifndef GATT_H
#define GATT_H

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gatt_device;

typedef void (*gatt_input_cb)(const BYTE *data, size_t len, void *userdata);
typedef void (*gatt_disconnect_cb)(void *userdata);

struct gatt_device *gatt_open_stadia(const WCHAR        *bt_address,
                                     gatt_input_cb       on_input,
                                     gatt_disconnect_cb  on_disconnect,
                                     void               *userdata);

/* Open GATT for vibration output only — no input poll thread.
 * Used when BLE device is added via HID (input) but needs GATT for output. */
struct gatt_device *gatt_open_output_only(const WCHAR *bt_address);

BOOL gatt_send_output_report(struct gatt_device *dev,
                              const BYTE *data, size_t len);

void gatt_close(struct gatt_device *dev);

int  gatt_find_stadia_devices(WCHAR bt_addresses[][20], int max_count);

INT  gatt_get_battery_level(const WCHAR *bt_address);

/* Returns TRUE if the process has Sparse Package identity (BT vibration capable). */
BOOL gatt_has_package_identity(void);

#ifdef __cplusplus
}
#endif

#endif /* GATT_H */
