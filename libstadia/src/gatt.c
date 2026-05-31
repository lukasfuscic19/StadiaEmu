/*
 * gatt.c -- BLE battery level for Stadia controller (SetupAPI device properties).
 */

#include "gatt.h"

#include <setupapi.h>
#include <devpropdef.h>
#include <stdio.h>
#include <windows.h>

#pragma comment(lib, "setupapi.lib")

INT gatt_get_battery_level(const WCHAR *bt_address)
{
    if (!bt_address || bt_address[0] == 0)
        return -1;

    /* DEVPKEY {104EA319-6EE2-4701-BD47-8DDBF425BBE5} pid=2 = battery % */
    static const DEVPROPKEY batt_key = {
        {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}},
        2
    };

    WCHAR prefix[64];
    swprintf_s(prefix, 64, L"BTHLE\\DEV_%s", bt_address);

    HDEVINFO hdev = SetupDiGetClassDevsW(NULL, NULL, NULL,
                                         DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hdev == INVALID_HANDLE_VALUE)
        return -1;

    INT result = -1;
    SP_DEVINFO_DATA dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cbSize = sizeof(dd);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hdev, i, &dd); i++) {
        WCHAR id[256] = {0};
        if (!SetupDiGetDeviceInstanceIdW(hdev, &dd, id, 256, NULL))
            continue;
        if (_wcsnicmp(id, prefix, wcslen(prefix)) != 0)
            continue;

        DEVPROPTYPE prop_type;
        DWORD val = 0, prop_size = sizeof(val);
        if (SetupDiGetDevicePropertyW(hdev, &dd, &batt_key,
                                      &prop_type, (PBYTE)&val, prop_size, &prop_size, 0))
            result = (INT)(val > 100 ? 100 : val);
        break;
    }

    SetupDiDestroyDeviceInfoList(hdev);
    return result;
}
