/* SPDX-License-Identifier: Apache-2.0 */

/* Registry image verification: the bytes handed to the runtime are checked
 * against the entry's metadata record and the signing keyring the firmware
 * carries. See docs/security-model.md. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <platform.h>
#include <wanted-api.h>

typedef enum {
    IMAGE_VERIFY_OK = 0,
    /* Firmware-carried, so the firmware signature already covers it. */
    IMAGE_VERIFY_SEEDED,
    IMAGE_VERIFY_NO_RECORD,
    IMAGE_VERIFY_DIGEST_MISMATCH,
    IMAGE_VERIFY_NO_SIGNATURE,
    IMAGE_VERIFY_UNKNOWN_KEY,
    IMAGE_VERIFY_BAD_SIGNATURE,
} image_verify_state_t;

/* Build the signed message binding an entry's identity to its content: a
 * length-prefixed name and version, the layer count, then the record's layer
 * digests base-first. Returns its length, or 0 where it does not fit. */
size_t WantedImageSignedMessage(const reg_entry_t *entry,
                                const registry_meta_t *meta, uint8_t *out,
                                size_t outLen);

/* Hash the layers mapped into `w` and check them against `entry`'s record and
 * signature. Always runs and always reports; refusing is the caller's call. */
image_verify_state_t WantedVerifyImage(const reg_entry_t *entry,
                                       const wapp_t *w);

/* A state's token, as logged and as reported at /proc/wanted. */
const char *WantedImageVerifyStateName(image_verify_state_t state);

/* The errno a refused load returns, distinct per state. 0 where the state
 * loads. */
int WantedImageVerifyErrno(image_verify_state_t state);

/* Whether a failed check refuses the load. Effective value: the compiled-in
 * floor OR every runtime source, so a source may raise it and none may lower
 * it — the runtime configuration lives on writable flash. */
bool WantedImageVerifyEnforced(void);

/* The compiled-in floor alone, which no configuration can clear. */
bool WantedImageVerifyFloor(void);

/* Raise enforcement from a runtime source. A false argument changes nothing. */
void WantedImageVerifyRaise(bool on);

/* Verify `w` and return the errno a refusal would use, or 0 to proceed. Logs
 * the state either way, so an unenforced build still shows what would fail. */
int WantedImageVerifyGate(const reg_entry_t *entry, const wapp_t *w);
