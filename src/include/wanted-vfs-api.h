#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wanted.h>
#include <wanted-api.h>
#include <tiny-json.h>

int WantedInstallDriver(vfs_ctx_t c, const wapp_t *w, const char *name, const char *path, const char *options);

int WantedParseConfig(const char* buf, size_t bufLen);
const wantedConfig_t *WantedGetConfig();
int WantedGetConfigJson(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen);
int WantedCloseRegistry();
int WantedRegistryRemove(const reg_entry_t *entry);
int WantedReadManifest(reg_entry_t *entry, uint8_t *buf, size_t bufLen);

int WantedReadState(uint8_t *buf, size_t bufLen);

int WantedParseCtrlAction(json_t const* json, char *wappName, wapp_action_t *act, wapp_config_t *cfg);
int WantedParseCtrlActionJson(const char *buf, size_t bufLen, char *wappName, wapp_action_t *action, wapp_config_t *cfg);
