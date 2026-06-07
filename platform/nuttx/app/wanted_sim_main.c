/* Sim init shim.
 *
 * When wsh runs as the NuttX init task (CONFIG_INIT_ENTRYPOINT=wanted_sim_main)
 * the NSH start-up script that normally mounts /data over hostfs does not run,
 * so the engine cannot reach its config or supervisor image. Mount hostfs here
 * (board rcS does `mount -t hostfs -o fs=. /data`), then chdir into it so the
 * relative registry root (REGISTRY_ROOT "./wapps") resolves onto hostfs — a
 * supervisor that launches wapps can then find their images. Finally hand off
 * to the standard entry point. Used only by the sim test / interactive board
 * config; the production NSH built-in entry remains wanted_main. */

#include <sys/boardctl.h>
#include <sys/mount.h>
#include <stdio.h>
#include <unistd.h>

#define HOSTFS_TARGET "/data"
#define HOSTFS_DATA   "fs=."

int wanted_main(int argc, char *argv[]);

int wanted_sim_main(int argc, char *argv[]) {
    if (mount(NULL, HOSTFS_TARGET, "hostfs", 0, HOSTFS_DATA) < 0) {
        perror("mount " HOSTFS_TARGET);
    } else if (chdir(HOSTFS_TARGET) < 0) {
        perror("chdir " HOSTFS_TARGET);
    }

    int rc = wanted_main(argc, argv);

    /* The engine loop returned — every wapp, including the supervisor, has ended
     * (e.g. the user typed `exit` in wsh). Power the simulator off so the host
     * process exits and the controlling terminal is restored; otherwise NuttX
     * idles as init returns and the raw-mode tty hangs. Falls through if the
     * config lacks BOARDCTL_POWEROFF (then init just returns, as before). */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
