/*
 * Intel SDV / HP i2000 board hardware monitor
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_I2000_HWMON_H
#define HW_IA64_I2000_HWMON_H

#define TYPE_IA64_I2000_HWMON "ia64-i2000-hwmon"

/*
 * The two monitor chips the i2000's I/O board hangs off the south bridge's
 * SMBus; the vendor firmware initialises both during POST.
 */
#define IA64_I2000_HWMON_ADDR_0 0x2c
#define IA64_I2000_HWMON_ADDR_1 0x4e

#endif
