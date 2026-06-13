/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

typedef struct {
    char *cmd;
    int (*func) (char **);
} cmd_t;

extern cmd_t cmds[];

int wsh_num_cmds();
