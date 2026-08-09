/* SPDX-License-Identifier: Apache-2.0 */

/* Sim init shim. Running as the NuttX init task skips the NSH start-up script
 * that mounts /data over hostfs, so this mounts it and chdirs in, letting the
 * relative registry root resolve, then hands off to the standard entry. */

#include <stdio.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <unistd.h>

#define HOSTFS_TARGET "/data"
#define HOSTFS_DATA "fs=."

int wanted_main(int argc, char *argv[]);

int wanted_sim_main(int argc, char *argv[]) {
    if (mount(NULL, HOSTFS_TARGET, "hostfs", 0, HOSTFS_DATA) < 0) {
        perror("mount " HOSTFS_TARGET);
    } else if (chdir(HOSTFS_TARGET) < 0) {
        perror("chdir " HOSTFS_TARGET);
    }

    int rc = wanted_main(argc, argv);

    /* The engine loop returned without powering the board off, so power the
     * simulator off: otherwise NuttX idles as init returns and the raw-mode tty
     * hangs. Falls through when the config lacks BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
