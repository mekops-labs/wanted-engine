/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <registry-meta.h>
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
/* Stage layer `layer` of the image `ref` names. A write that starts fails with
 * the error the store gave; WantedCloseRegistryLayer ends it. */
int WantedWriteRegistryLayer(bool *cont, const char *ref, unsigned layer,
                             const uint8_t *buf, size_t bufLen);
int WantedCloseRegistryLayer(void);
/* Publish `ref` from its staged layers. `payload` is 1 byte, the layer count,
 * or 9 bytes, the count then the image build time as a big-endian int64. */
int WantedCommitRegistry(const char *ref, const uint8_t *payload,
                         size_t payloadLen);
int WantedRegistryRemove(const reg_entry_t *entry);
/* Attach a signature to the installed image `ref` ("<name>[:<version>]")
 * names. `payload` is REGISTRY_SIG_PAYLOAD_LEN bytes: a big-endian key id
 * then the raw signature. -ENOENT when no such image is installed. */
int WantedRegistrySetSignature(const char *ref, const uint8_t *payload,
                               size_t payloadLen);
/* Synthesize a small JSON descriptor for a registry entry into buf: its record
 * and the linear-memory profile read from the merged image. */
int WantedRenderRegistryDescriptor(const reg_entry_t *entry, uint8_t *buf,
                                   size_t bufLen);

/* Describe the blob `digest` names as a JSON object, or -ENOENT. */
int WantedRenderRegistryBlob(const uint8_t digest[REGISTRY_META_DIGEST_LEN],
                             char *buf, size_t bufLen);
/* Stage layer `layer` of `ref` as the blob named by `payload`: 64 hex digits,
 * then an optional newline. -EINVAL for any other payload. */
int WantedLinkRegistryLayer(const char *ref, unsigned layer,
                            const uint8_t *payload, size_t payloadLen);

/* Render the capacity of each registry store as a JSON array of objects, with
 * sizes in bytes. Returns the length, or -EMSGSIZE when `bufLen` is too small.
 */
int WantedRenderRegistryCapacity(char *buf, size_t bufLen);

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

/* /proc/net/<name> — read-only per-wapp directory reporting the calling
 * wapp's own sockets[] grants, unprivileged (see vfs-procfs-net.c). Only
 * registered when sockets are compiled in. */
extern const proc_dir_ops_t NetProcDirOps;

/* Upper bound (including NUL) on a control/config JSON payload the engine
 * copies to parse, which sizes the parse buffer. */
#define WANTED_CTRL_JSON_MAX 2048

/* Parse-table nodes, sized for the densest payload WANTED_CTRL_JSON_MAX admits
 * (about 20 bytes per two nodes); a payload that exhausts it is rejected. */
#define WANTED_CTRL_JSON_NODES 100

int WantedParseCtrlAction(json_t const *json, char *wappName,
                          wapp_action_t *act, wapp_config_t *cfg);
int WantedParseCtrlActionJson(const char *buf, size_t bufLen, char *wappName,
                              wapp_action_t *action, wapp_config_t *cfg);

/* Parse the bare launch-config body { console, drivers[], preopens } written
 * to wapps/<name>/config. Identity is supplied by the path, not the payload. */
int WantedParseWappConfigJson(const char *buf, size_t bufLen,
                              wapp_config_t *cfg);

/* Merge the overlay's socket addresses in `dir` into `cfg`, keeping any the
 * config already names; an absent overlay succeeds. On a malformed one it
 * returns negative and `cfg` must be discarded, so merge into a copy. */
int WantedMergeConfigOverlay(wapp_config_t *cfg, const char *dir);

/* Engine clock-quality state, exposed as text at /proc/clock_quality and read
 * as authoritative by a wapp deciding whether to trust the wall clock. An
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

/* Appends the firmware lines at offset `w` and returns the new offset; an
 * empty `board` or `variant` has no line. */
int WantedProcAppendFirmware(char *buf, size_t bufLen, int w, const char *board,
                             const char *variant);

/* Renders /proc/uptime: milliseconds since the engine started. */
int WantedProcReadUptime(vfs_ctx_t c, void *buf, size_t bufLen);
