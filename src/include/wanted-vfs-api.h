#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wanted.h>
#include <wanted-api.h>

int WantedInstallDriver(vfs_ctx_t c, const wapp_t *w, const char *name, const char *path, const char *options);

int WantedSetConfig(wantedConfig_t cfg);
int WantedGetConfig(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen);
int WantedCloseRegistry();
int WantedRegistryRemove(const reg_entry_t *entry);
int WantedReadManifest(reg_entry_t *entry, uint8_t *buf, size_t bufLen);

int WantedReadState(uint8_t *buf, size_t bufLen);

int WantedParseCtrlAction(const char *buf, size_t bufLen, char *wappName, wapp_action_t *action, wapp_config_t *cfg);
