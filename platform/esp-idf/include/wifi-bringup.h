/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Boot-path Wi-Fi association, for a board whose supervisor reaches its control
 * plane over the network: nothing else brings the radio up before the
 * supervisor starts, and a production supervisor is not a shell that could.
 * Runs the same bring-up and association a wapp drives through /dev/wifi, so
 * the driver stays usable afterwards.
 *
 * Waits up to `timeoutSec` for a DHCP lease. Returns 0 once addressed,
 * -ETIMEDOUT if the lease never arrives, -EINVAL on an empty SSID, -EIO if the
 * radio or the association failed. Credentials are used for this call only —
 * the radio is configured with RAM storage, so they never reach flash.
 */
int EspWifiBringup(const char *ssid, const char *pass, int timeoutSec);
