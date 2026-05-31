/*
 * gatt.h -- BLE helpers for Stadia controller (battery level via SetupAPI).
 */

#ifndef GATT_H
#define GATT_H

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0-100, or -1 if unknown / unavailable. */
INT gatt_get_battery_level(const WCHAR *bt_address);

#ifdef __cplusplus
}
#endif

#endif /* GATT_H */
