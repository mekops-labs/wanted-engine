/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tiny-json.h>
#include <wanted-api.h>
#include <wanted.h>

int WantedInstallDriver(vfs_ctx_t c, const wapp_t *w, const char *name,
                        const char *path, const char *options);

int WantedParseConfig(const char *buf, size_t bufLen);
const wantedConfig_t *WantedGetConfig();

int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen);
int WantedCloseRegistry();
int WantedRegistryRemove(const reg_entry_t *entry);
int WantedReadManifest(reg_entry_t *entry, uint8_t *buf, size_t bufLen);

const char *statusToString(status_t state);

/* Upper bound (including NUL) on a control/config JSON payload the engine
 * copies onto the stack to parse, sizing the fixed parse buffer. The
 * compiled-in supervisor bootstrap config and any per-wapp launch config sit
 * comfortably under this. */
#define WANTED_CTRL_JSON_MAX 2048

int WantedParseCtrlAction(json_t const *json, char *wappName,
                          wapp_action_t *act, wapp_config_t *cfg);
int WantedParseCtrlActionJson(const char *buf, size_t bufLen, char *wappName,
                              wapp_action_t *action, wapp_config_t *cfg);

/* Parse the bare launch-config body { console, drivers[], preopens } written
 * to wapps/<name>/config. Identity is supplied by the path, not the payload. */
int WantedParseWappConfigJson(const char *buf, size_t bufLen,
                              wapp_config_t *cfg);

/* Engine clock-quality state. The byte exposed at /proc/clock_quality
 * reflects how the platform clock is calibrated. Wapp readers (e.g. agents
 * deciding whether to trust the wall clock for security decisions) consume
 * it as authoritative. Byte values:
 *   0 = HARDWARE_RTC         backed by a battery-backed RTC
 *   1 = SNTP_CALIBRATED      synced via SNTP
 *   2 = SIMPLE_CALIBRATION   set by a host-side time provider (no RTC, no NTP)
 *   3 = UNCALIBRATED         default; wall clock is meaningless
 * Updaters (RTC probe, SNTP daemon, host time provider) call
 * WantedSetClockQuality whenever the calibration state changes. */
#define WANTED_CLOCK_HARDWARE_RTC       0
#define WANTED_CLOCK_SNTP_CALIBRATED    1
#define WANTED_CLOCK_SIMPLE_CALIBRATION 2
#define WANTED_CLOCK_UNCALIBRATED       3

void    WantedSetClockQuality(uint8_t q);
uint8_t WantedGetClockQuality(void);
int     WantedProcReadClockQuality(vfs_ctx_t c, void *buf, size_t bufLen);
