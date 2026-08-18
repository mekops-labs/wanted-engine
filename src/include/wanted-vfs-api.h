/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tiny-json.h>
#include <vfs-procfs.h>
#include <wanted-api.h>
#include <wanted.h>

int WantedInstallDriver(vfs_ctx_t c, const wapp_t *w, const char *name,
                        const char *path, const char *options);

/* Write the space-separated names of every resolvable driver into buf,
 * truncated to bufLen, iterating the same tables WantedInstallDriver resolves
 * against. Returns bytes written, or a negative errno on bad args. */
int WantedListDrivers(char *buf, size_t bufLen);

int WantedParseConfig(const char *buf, size_t bufLen);
const wantedConfig_t *WantedGetConfig(void);

int WantedWriteRegistry(bool *cont, const char *ref, const uint8_t *buf,
                        size_t bufLen);
int WantedCloseRegistry(void);
int WantedRegistryRemove(const reg_entry_t *entry);
/* Synthesize a small JSON descriptor (name/version/size) for a registry entry
 * into buf — the entry alone is the source, with no image load. */
int WantedRenderRegistryDescriptor(const reg_entry_t *entry, uint8_t *buf,
                                   size_t bufLen);

/* Parse a wasm module's declared linear-memory limits from the leading `len`
 * bytes of its app.wasm, filling *init, *has_max and *max. -ENOENT when those
 * bytes hold no memory section, -EINVAL on a bad header. Allocation-free. */
int WantedWasmMemoryProfile(const uint8_t *buf, size_t len, uint32_t *init,
                            bool *has_max, uint32_t *max);

const char *StatusToString(status_t state);

/* /proc/wapps/<name>/<leaf> — read-only per-wapp observability directory,
 * registered as a privileged ProcFS directory entry. Its leaves are rendered
 * from PlatformWappGetState. */
extern const proc_dir_ops_t WappsProcDirOps;

/* Upper bound (including NUL) on a control/config JSON payload the engine
 * copies onto the stack to parse, which sizes the fixed parse buffer. */
#define WANTED_CTRL_JSON_MAX 2048

int WantedParseCtrlAction(json_t const *json, char *wappName,
                          wapp_action_t *act, wapp_config_t *cfg);
int WantedParseCtrlActionJson(const char *buf, size_t bufLen, char *wappName,
                              wapp_action_t *action, wapp_config_t *cfg);

/* Parse the bare launch-config body { console, drivers[], preopens } written
 * to wapps/<name>/config. Identity is supplied by the path, not the payload. */
int WantedParseWappConfigJson(const char *buf, size_t bufLen,
                              wapp_config_t *cfg);

/* Engine clock-quality state, exposed as one byte at /proc/clock_quality and
 * read as authoritative by a wapp deciding whether to trust the wall clock. An
 * updater calls WantedSetClockQuality whenever the calibration changes. */
#define WANTED_CLOCK_HARDWARE_RTC 0
#define WANTED_CLOCK_SNTP_CALIBRATED 1
#define WANTED_CLOCK_SIMPLE_CALIBRATION 2
#define WANTED_CLOCK_UNCALIBRATED 3

void WantedSetClockQuality(uint8_t q);
uint8_t WantedGetClockQuality(void);
int WantedProcReadClockQuality(vfs_ctx_t c, void *buf, size_t bufLen);

/* Renders /proc/wanted: engine identity and the compile-time ceilings a
 * supervisor checks a launch config against. */
int WantedProcReadInfo(vfs_ctx_t c, void *buf, size_t bufLen);
