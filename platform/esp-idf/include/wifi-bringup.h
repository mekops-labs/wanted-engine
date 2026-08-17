/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Boot-path Wi-Fi association, for a supervisor whose control plane is on
 * the network. Waits up to `timeoutSec` for a DHCP lease: 0 once addressed,
 * -ETIMEDOUT, -EINVAL on an empty SSID, -EIO on a radio or join failure. */
int EspWifiBringup(const char *ssid, const char *pass, int timeoutSec);
