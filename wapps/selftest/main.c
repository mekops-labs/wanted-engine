/* SPDX-License-Identifier: Apache-2.0 */

/* selftest — the engine test supervisor. It runs as the boot supervisor and
 * exercises the engine from inside WASM, emitting TAP. Everything here is plain
 * WASI plus the VFS and control-plane ABI, so one image runs everywhere. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tap.h"

#define WANTED_CTL "/dev/wanted/ctl"
#define TRAPPER "trapper"
#define TRAPPER_CFG "/dev/wanted/wapps/" TRAPPER "/config"
#define TRAPPER_STATE "/dev/wanted/wapps/" TRAPPER "/state"
#define SUPERVISOR_STATE "/dev/wanted/wapps/supervisor/state"

/* Launched test wapps get a null stdin and the "log" console, so their output
 * is captured per-wapp and a wapp's stdio teardown cannot close the
 * supervisor's stdout. No interior whitespace: the parser wants one value. */
#define LAUNCH_CFG                                                             \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}}}"

/* A launched wapp's captured stdout/stderr is read back through the `log` mount
 * the supervisor config binds at /log (one read-only node per wapp), not the
 * /dev/wanted control plane. */
#define LOG_MOUNT "/log"
#define TRAPPER_LOG LOG_MOUNT "/" TRAPPER
#define TRAPPER_MARKER "trapper-was-here"

#define LOOPER "looper"
#define LOOPER_CFG "/dev/wanted/wapps/" LOOPER "/config"
#define LOOPER_CTL "/dev/wanted/wapps/" LOOPER "/ctl"
#define LOOPER_STATE "/dev/wanted/wapps/" LOOPER "/state"

/* All-null console: every stdio slot discards/EOFs. A wapp still launches and
 * runs — it is just silent. No interior whitespace (see LAUNCH_CFG). */
#define NULL_CONSOLE_CFG                                                       \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"null\"},\"err\":{\"name\":\"null\"}}}"

/* argenv prints its argv + environ to the log console and exits with code 7.
 * Its config passes known args and envs (no interior whitespace per LAUNCH_CFG)
 * so the supervisor can read them back from the log and assert passthrough. */
#define ARGENV "argenv"
#define ARGENV_CFG "/dev/wanted/wapps/" ARGENV "/config"
#define ARGENV_LOG LOG_MOUNT "/" ARGENV
#define ARGENV_EXIT "/proc/wapps/" ARGENV "/exit_code"
#define ARGENV_CFG_BODY                                                        \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"args\":[\"alpha\",\"beta\"],\"envs\":[\"FOO=bar\",\"BAZ=qux\"]}"

/* A `pipe` console streams a wapp's stdout live to a peer. argpipe runs the
 * argenv image with its stdout backed by one, and the supervisor reads the
 * output back from /dev/pipe/argpipe.out. */
#define ARGPIPE "argpipe"
#define ARGPIPE_CFG "/dev/wanted/wapps/" ARGPIPE "/config"
#define ARGPIPE_PIPE "/dev/pipe/" ARGPIPE ".out"
#define ARGPIPE_CFG_BODY                                                       \
    "{\"image\":\"argenv\",\"console\":{\"in\":{\"name\":\"null\"},"           \
    "\"out\":{\"name\":\"pipe\"},\"err\":{\"name\":\"null\"}},"                \
    "\"args\":[\"alpha\",\"beta\"]}"

/* volcheck mounts an engine-managed `volume` at /data, writing a marker on a
 * fresh store and reading it back on a populated one. Two runs of the same
 * instance prove the volume persists across a restart. */
#define VOLCHECK "volcheck"
#define VOLCHECK_CFG "/dev/wanted/wapps/" VOLCHECK "/config"
#define VOLCHECK_LOG LOG_MOUNT "/" VOLCHECK
#define VOLCHECK_PAYLOAD "persist-42"
#define VOLCHECK_CFG_BODY                                                      \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\"}]}"

/* A shared volume is one store two wapps reach by name. Two instances run the
 * volcheck image against the same `name=stream,shared` volume: one writes the
 * marker, the other re-opens it, proving the store crosses wapp bounds. */
#define VPROD "vprod"
#define VCONS "vcons"
#define VPROD_CFG "/dev/wanted/wapps/" VPROD "/config"
#define VCONS_CFG "/dev/wanted/wapps/" VCONS "/config"
#define VPROD_LOG LOG_MOUNT "/" VPROD
#define VCONS_LOG LOG_MOUNT "/" VCONS
#define SHARED_CFG_BODY                                                        \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=stream,shared\"}]}"

/* Isolation: a private and a shared volume of the same name must be different
 * stores. isoshr writes to a shared `name=iso`, then isoprv mounts a private
 * one; disjoint namespaces mean isoprv sees a fresh store and writes. */
#define ISO_SHARE "isoshr"
#define ISO_PRIV "isoprv"
#define ISO_SHARE_CFG "/dev/wanted/wapps/" ISO_SHARE "/config"
#define ISO_PRIV_CFG "/dev/wanted/wapps/" ISO_PRIV "/config"
#define ISO_SHARE_LOG LOG_MOUNT "/" ISO_SHARE
#define ISO_PRIV_LOG LOG_MOUNT "/" ISO_PRIV
#define ISO_SHARE_CFG_BODY                                                     \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=iso,shared\"}]}"
#define ISO_PRIV_CFG_BODY                                                      \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=iso\"}]}"

/* A read-only shared volume must deny writes. vroro mounts a fresh
 * `name=roonly,shared,ro` store; volcheck finds no marker, tries to create one,
 * and the ro grant refuses, so it reports "vol-fail". */
#define VRORO "vroro"
#define VRORO_CFG "/dev/wanted/wapps/" VRORO "/config"
#define VRORO_LOG LOG_MOUNT "/" VRORO
#define VRORO_CFG_BODY                                                         \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=roonly,shared,ro\"}]}"

/* observer is the reference observability wapp: granted a `log` mount and the
 * ambient /proc but no `wanted` driver, so it watches the fleet yet cannot
 * command it. It reports each finding to its log. */
#define OBSERVER "observer"
#define OBSERVER_CFG "/dev/wanted/wapps/" OBSERVER "/config"
#define OBSERVER_LOG LOG_MOUNT "/" OBSERVER
#define OBSERVER_CFG_BODY                                                      \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"log\",\"path\":\"/log\"}]}"

/* The supervisor's own launch config wires all three resource sections, so they
 * are verified in this namespace: a `config` map mounted outside /dev, a named
 * socket, and the `wanted` device driver. */
#define CFGMAP_PATH "/etc/config"
#define CFGMAP_MARKER "selftest-cfgmap-v1"
#define SOCKET_NAME "uplink"

/* The server/client pairs the listen checks use, wired by the listen variant
 * of the supervisor config: a TCP listener with two clients on its port, and a
 * bound datagram socket with one. */
#define SERVER_SOCKET "/net/server"
#define CLIENT_SOCKET "/net/client"
#define CLIENT2_SOCKET "/net/client2"
#define DGRAM_SOCKET "/net/dgram"
#define DCLIENT_SOCKET "/net/dclient"
#define LISTEN_REQ "ping"
#define LISTEN_REQ2 "ping2"
#define LISTEN_RES "pong"

/* A `platform` bind mount at /host, backed by a host dir the runner populates:
 * an in-bounds file reads back, but a symlink planted inside it pointing
 * outside must not resolve — the confinement a read-only flag cannot give. */
#define BIND_INSIDE "/host/inside/data.txt"
#define BIND_INSIDE_MARKER "in-bounds-ok"
#define BIND_ESCAPE "/host/escape"

/* Read up to cap-1 bytes of a path into buf (NUL-terminated). <0 on open
 * error, else byte count. */
static int read_path(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    return n;
}

/* Open path write-only and write s. <0 on error. */
static int write_path(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int n = write(fd, s, strlen(s));
    close(fd);
    return n;
}

/* Reserve a wapp namespace via the root ctl `create` verb. The per-wapp nodes
 * (config, ctl, ...) exist only after this, so every launch creates first. */
static int create_wapp(const char *name) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "create %s", name);
    return write_path(WANTED_CTL, cmd) >= 0;
}

/* Launch an already-configured wapp through its own ctl node (defined below).
 */
static int start_wapp(const char *name);

/* True if directory `dir` contains an entry named `name`. */
static int dir_has(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, name) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static void positive_checks(void) {
    /* Large enough for the full /proc/wanted dump, whose `drivers` field pushes
     * it past 256 B; the node is one-shot, so a short read truncates. */
    char buf[512];

    tap_ok(read_path("/app.wasm", buf, sizeof(buf)) > 0,
           "TarFS: /app.wasm is readable");
    tap_ok(dir_has("/dev", "wanted"),
           "VFS: /dev exposes the wanted control plane");
    tap_ok(dir_has("/dev", "pipe"), "VFS: /dev exposes pipe");
    tap_ok(dir_has("/proc", "wapps"), "VFS: /proc exposes wapps");

    /* /dev/null reads as EOF. */
    int fd = open("/dev/null", O_RDONLY);
    int n = (fd >= 0) ? read(fd, buf, sizeof(buf)) : -1;
    if (fd >= 0)
        close(fd);
    tap_ok(fd >= 0 && n == 0, "VFS: /dev/null reads as EOF");

    /* /proc/wapps is a directory: one subdirectory per running wapp, each with
     * read-only status leaves. It enumerates the running supervisor, and the
     * supervisor's state leaf reports it running. */
    tap_ok(dir_has("/proc/wapps", "supervisor"),
           "proc: /proc/wapps enumerates the supervisor");
    tap_ok(read_path("/proc/wapps/supervisor/state", buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "proc: /proc/wapps/<name>/state reports the supervisor running");
    /* Per-wapp linear-memory accounting: a running wapp has committed at least
     * one page, so linear_cur and the page counts are present and non-empty. */
    tap_ok(read_path("/proc/wapps/supervisor/memory", buf, sizeof(buf)) > 0 &&
               strstr(buf, "linear_cur:") != NULL &&
               strstr(buf, "pages_cur:") != NULL,
           "proc: /proc/wapps/<name>/memory reports linear-memory accounting");

    /* /proc/wanted reports engine identity and the compile-time ceilings, and
     * is unprivileged. Platform and version vary by build, so only the stable
     * fields are asserted. */
    tap_ok(dir_has("/proc", "wanted"), "VFS: /proc exposes wanted");
    tap_ok(read_path("/proc/wanted", buf, sizeof(buf)) > 0 &&
               strstr(buf, "platform:") != NULL &&
               strstr(buf, "version:") != NULL &&
               strstr(buf, "max_wapps:\t3") != NULL &&
               strstr(buf, "wasm_max_pages:\t1") != NULL,
           "proc: /proc/wanted reports engine identity and limits");

    /* The drivers field lists the resolvable drivers on this build: the core
     * names plus the platform's own (the NuttX sim contributes gpio/wifi). */
    tap_ok(read_path("/proc/wanted", buf, sizeof(buf)) > 0 &&
               strstr(buf, "drivers:\t") != NULL &&
               strstr(buf, "wanted") != NULL,
           "proc: /proc/wanted reports available drivers");

    /* Inter-wapp pipe round-trip within our own namespace. */
    write_path("/dev/pipe/selftest", "ping");
    tap_ok(read_path("/dev/pipe/selftest", buf, sizeof(buf)) > 0 &&
               strncmp(buf, "ping", 4) == 0,
           "pipe: /dev/pipe round-trip");

    /* TarFS is read-only: opening app.wasm for write must fail. */
    int wfd = open("/app.wasm", O_WRONLY);
    if (wfd >= 0)
        close(wfd);
    tap_ok(wfd < 0, "TarFS: /app.wasm is read-only (write rejected)");

    /* Sandbox: a path escaping the root must not resolve to the host. */
    int efd = open("/../../../../etc/passwd", O_RDONLY);
    if (efd >= 0)
        close(efd);
    tap_ok(efd < 0, "sandbox: parent-traversal past root is denied");

    /* Our own control-plane state reads running. */
    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "control plane: supervisor state is running");
}

/* Launch the misbehaving wapp and assert the engine contains it. */
static void robustness_checks(void) {
    char buf[64];

    int cfg_ok =
        create_wapp(TRAPPER) && write_path(TRAPPER_CFG, LAUNCH_CFG) >= 0;
    int start_ok = start_wapp(TRAPPER);
    tap_ok(cfg_ok && start_ok, "control plane: launched the " TRAPPER " wapp");

    /* Poll until it leaves starting/running, meaning the engine reaped the
     * trap. Bounded, so a hang fails rather than blocks. Both "failure" and
     * "exited" count as dead. */
    const char *state = "";
    int contained = 0;
    for (int i = 0; i < 10; i++) {
        sleep(1);
        if (read_path(TRAPPER_STATE, buf, sizeof(buf)) <= 0)
            continue;
        state = buf;
        if (strstr(buf, "running") == NULL && strstr(buf, "starting") == NULL) {
            contained = 1;
            break;
        }
    }
    tap_ok(contained && (strstr(state, "failure") || strstr(state, "exited")),
           "robustness: trapping wapp is contained (dead, not running)");

    /* The supervisor — this code — survived the misbehaving child. */
    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "robustness: supervisor still running after the trap");

    /* The log console captured the wapp's output without touching the platform
     * console (this very TAP stream proves the supervisor's stdout survived).
     */
    tap_ok(read_path(TRAPPER_LOG, buf, sizeof(buf)) > 0 &&
               strstr(buf, TRAPPER_MARKER) != NULL,
           "log: supervisor reads the launched wapp's captured output");
}

/* Poll a wapp's state node until `want_running` matches, bounded to ~10 s.
 * An unreadable node counts as not-live, so a "wait until dead" poll on a
 * launch that never ran is satisfied at once rather than spinning out. */
static int wait_state(const char *state_path, int want_running) {
    char buf[64];
    for (int i = 0; i < 10; i++) {
        sleep(1);
        int n = read_path(state_path, buf, sizeof(buf));
        int live = (n > 0) && (strstr(buf, "running") != NULL ||
                               strstr(buf, "starting") != NULL);
        if (live == want_running)
            return 1;
    }
    return 0;
}

/* Build "/dev/wanted/wapps/<name>/<node>" into buf. */
static void wapp_node(char *buf, int cap, const char *name, const char *node) {
    snprintf(buf, cap, "/dev/wanted/wapps/%s/%s", name, node);
}

/* Build the wapp's log path under the `log` mount ("/log/<name>") into buf. */
static void log_path(char *buf, int cap, const char *name) {
    snprintf(buf, cap, LOG_MOUNT "/%s", name);
}

/* Launch an already-configured wapp through its own ctl node — the root ctl
 * does not start wapps (it only creates namespaces and drives power). Returns
 * true on a successful write. */
static int start_wapp(const char *name) {
    char ctl[96];
    wapp_node(ctl, sizeof(ctl), name, "ctl");
    return write_path(ctl, "start") >= 0;
}

/* Create the namespace, configure the wapp with the log console, and start it
 * via the control plane (the create → config → start lifecycle). Returns true
 * if every step succeeded. */
static int launch(const char *name) {
    char path[96];
    if (!create_wapp(name))
        return 0;
    wapp_node(path, sizeof(path), name, "config");
    if (write_path(path, LAUNCH_CFG) < 0)
        return 0;
    return start_wapp(name);
}

/* Poll a wapp's state until it reports a dead state (exited/failure), bounded
 * to ~10 s. Returns true once the engine has reaped it. */
static int wait_dead(const char *name) {
    char path[96];
    wapp_node(path, sizeof(path), name, "state");
    return wait_state(path, 0);
}

/* Launch each misbehaving wapp and assert the engine contains it: the wapp ends
 * dead on its own while this supervisor and the host survive. Covers the
 * stack-overflow and memory-exhaustion classes. */
static void containment_checks(void) {
    static const char *const wapps[] = {"stackbomb", "membomb"};
    char buf[64], desc[96];

    for (unsigned i = 0; i < sizeof(wapps) / sizeof(*wapps); i++) {
        int ok = launch(wapps[i]) && wait_dead(wapps[i]);
        snprintf(desc, sizeof(desc),
                 "robustness: %s is contained (dead, not running)", wapps[i]);
        tap_ok(ok, desc);
    }

    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "robustness: supervisor still running after the misbehaving wapps");
}

/* The per-wapp linear-memory cap is enforced two ways: bigmem's grow past the
 * cap is refused at runtime, and biginit's oversized initial memory is refused
 * at load. Both assume the constrained default cap; a wider one admits them. */
static void memcap_checks(void) {
    char path[96], buf[64];

    int bm = launch("bigmem") && wait_dead("bigmem");
    log_path(path, sizeof(path), "bigmem");
    int bounded = bm && read_path(path, buf, sizeof(buf)) > 0 &&
                  strstr(buf, "bigmem-bounded") != NULL;
    tap_ok(bounded, "memcap: bigmem linear-memory growth is bounded at the cap");

    launch("biginit");
    wapp_node(path, sizeof(path), "biginit", "state");
    int refused = wait_dead("biginit") &&
                  read_path(path, buf, sizeof(buf)) > 0 &&
                  strstr(buf, "failure") != NULL;
    tap_ok(refused,
           "memcap: biginit (initial memory over the cap) is refused at load");

    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "memcap: supervisor still running after the capped wapps");
}

/* A never-yielding wapp must still be stoppable: WAMR's per-instruction
 * terminate check unwinds even a tight compute loop that never blocks. Start
 * cpuhog, confirm it runs, stop it, confirm the engine terminated it. */
static void cpuhog_check(void) {
    char state[96], ctl[96];
    wapp_node(state, sizeof(state), "cpuhog", "state");
    wapp_node(ctl, sizeof(ctl), "cpuhog", "ctl");

    int ran = launch("cpuhog") && wait_state(state, 1);
    int stopped = ran && write_path(ctl, "stop") >= 0 && wait_state(state, 0);
    tap_ok(stopped, "robustness: a never-yielding cpuhog is stoppable");
}

/* Launch a long-running wapp, confirm it runs concurrently with the
 * supervisor, then stop it via the control plane and confirm the engine
 * terminated it. */
static void lifecycle_checks(void) {
    create_wapp(LOOPER);
    write_path(LOOPER_CFG, LAUNCH_CFG);
    int started = start_wapp(LOOPER);
    tap_ok(started && wait_state(LOOPER_STATE, 1),
           "lifecycle: looper runs concurrently with the supervisor");

    int stopped =
        write_path(LOOPER_CTL, "stop") >= 0 && wait_state(LOOPER_STATE, 0);
    tap_ok(stopped, "lifecycle: control-plane stop terminates the looper");
}

/* Console backing: a wapp's stdio slots default when the config omits them, and
 * an explicit all-null console is also valid. Either way it must launch, since
 * a wapp with unwired stdio fds fails to start. */
static void console_checks(void) {
    /* Empty config (no console block): the unset slots resolve to their
     * defaults. A start still requires a config to have been written, so the
     * empty object is the minimal "use all defaults" config. */
    int dflt = create_wapp(LOOPER) && write_path(LOOPER_CFG, "{}") >= 0 &&
               start_wapp(LOOPER) && wait_state(LOOPER_STATE, 1);
    tap_ok(dflt, "console: a wapp with no console config launches on defaults");
    if (dflt) {
        write_path(LOOPER_CTL, "stop");
        wait_state(LOOPER_STATE, 0);
    }

    /* Explicit all-null console: silent, but still runs. */
    int nul = create_wapp(LOOPER) &&
              write_path(LOOPER_CFG, NULL_CONSOLE_CFG) >= 0 &&
              start_wapp(LOOPER) && wait_state(LOOPER_STATE, 1);
    tap_ok(nul, "console: an all-null console launches a (silent) wapp");
    if (nul) {
        write_path(LOOPER_CTL, "stop");
        wait_state(LOOPER_STATE, 0);
    }
}

/* Stop verb on a wapp's control node. Returns the write result (<0 on error,
 * e.g. no such node). */
static int stop_wapp(const char *name) {
    char ctl[96];
    wapp_node(ctl, sizeof(ctl), name, "ctl");
    return write_path(ctl, "stop");
}

/* Launch a wapp parked in a blocking host call, stop it, and report whether the
 * stop interrupted the call promptly. Promptness is judged in a 2 s window,
 * well under any self-return, so it isolates the interrupt path. */
static int stop_interrupts(const char *name, int *alive_out) {
    char state[96], buf[64];
    wapp_node(state, sizeof(state), name, "state");

    launch(name);
    wait_state(state, 1); /* running, inside the blocking call */
    stop_wapp(name);

    int prompt = 0;
    for (int i = 0; i < 2; i++) {
        sleep(1);
        if (read_path(state, buf, sizeof(buf)) > 0 && !strstr(buf, "running") &&
            !strstr(buf, "starting")) {
            prompt = 1;
            break;
        }
    }
    if (!prompt)
        wait_dead(name); /* bound a stuck slot so the suite goes on */

    *alive_out = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                 strstr(buf, "running") != NULL;

    printf("# %s: stop interrupts the blocked host call: %s\n", name,
           prompt ? "yes" : "no");
    fflush(stdout);
    return prompt;
}

/* blocker parks in a single timed sleep; the stop must interrupt that host call
 * (not wait it out) and the supervisor must survive. */
static void blocker_check(void) {
    int alive = 0;
    int prompt = stop_interrupts("blocker", &alive);
    tap_ok(prompt && alive, "robustness: stop interrupts a sleep-blocked wapp "
                            "and reaps it promptly");
}

/* pblock parks in a read on an empty pipe that never completes on its own, so
 * it can only be ended by the stop interrupting the host call — the strict form
 * of the blocker check (no self-return to fall back on). */
static void ioblock_check(void) {
    int alive = 0;
    int prompt = stop_interrupts("pblock", &alive);
    tap_ok(prompt && alive, "robustness: stop interrupts an I/O-blocked wapp "
                            "(read on an empty pipe)");
}

/* Control-plane edge cases that must not crash the engine: stopping a wapp that
 * has already exited, and stopping one that was never launched. */
static void edge_checks(void) {
    char buf[64];

    /* blocker is dead by now; stopping it again is a no-op, not a crash. */
    stop_wapp("blocker");
    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "edge: stopping an already-dead wapp is harmless");

    /* No node exists for a wapp that was never launched, so the stop fails to
     * open cleanly and the supervisor keeps running. */
    int rc = stop_wapp("ghost");
    tap_ok(rc < 0 && read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "edge: stopping an unknown wapp errors cleanly");
}

/* Launch a non-privileged wapp that tries to break out of its namespace and
 * assert every escape was denied. This probes the sandbox boundary from a
 * launched wapp, which is where it actually is. */
static void sandbox_check(void) {
    char log[96], buf[128];
    log_path(log, sizeof(log), "escaper");

    launch("escaper");
    wait_dead("escaper");

    int got = read_path(log, buf, sizeof(buf)) > 0;
    tap_ok(got && strstr(buf, "sandbox-OK") != NULL &&
               strstr(buf, "sandbox-LEAK") == NULL,
           "sandbox: a launched wapp cannot escape its namespace");
}

/* Launch a wapp that exhausts its file descriptors and assert the abuse is
 * contained: the wapp is reaped and the supervisor survives. Whether the fd
 * table was bounded below the probe cap is a diagnostic. */
static void resource_check(void) {
    char log[96], verdict[64], buf[64];
    log_path(log, sizeof(log), "fdhog");

    launch("fdhog");
    int reaped = wait_dead("fdhog");

    verdict[0] = '\0';
    read_path(log, verdict, sizeof(verdict));
    int alive = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                strstr(buf, "running") != NULL;

    printf("# fdhog: %s", verdict[0] ? verdict : "(no verdict)\n");
    fflush(stdout);

    tap_ok(reaped && alive,
           "robustness: fd exhaustion is contained to the wapp, host survives");
}

/* Try to start a battery of malformed registry images. The engine must reject
 * each cleanly and stay up: a crash in the loader would take the engine down
 * and the TAP plan would never print. */
static void malformed_check(void) {
    static const char *const bad[] = {"noappwasm", "badwasm", "truncated"};
    char state[96], cfg[96], buf[64];
    int contained = 1;

    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        /* create → config → start: the empty config satisfies the start gate so
         * the loader is actually reached and gets to reject the bad image. */
        create_wapp(bad[i]);
        wapp_node(cfg, sizeof(cfg), bad[i], "config");
        write_path(cfg, "{}");
        start_wapp(bad[i]);
        wapp_node(state, sizeof(state), bad[i], "state");
        wait_dead(bad[i]); /* never lingers running/starting */
        if (read_path(state, buf, sizeof(buf)) > 0 &&
            (strstr(buf, "running") || strstr(buf, "starting")))
            contained = 0;
    }

    int alive = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                strstr(buf, "running") != NULL;
    tap_ok(contained && alive, "robustness: malformed images are rejected "
                               "without crashing the engine");
}

/* Rapidly restart a wapp that dies the instant it starts. Each cycle must
 * reclaim the slot and complete, and the supervisor must stay healthy — the
 * start/reap path must not thrash or leak across a crash loop. */
#define CRASH_CYCLES 8
static void crashloop_check(void) {
    char buf[64];
    int cycles = 0;

    for (int i = 0; i < CRASH_CYCLES; i++) {
        if (!launch("crasher"))
            break;
        if (!wait_dead("crasher"))
            break;
        cycles++;
    }

    int alive = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                strstr(buf, "running") != NULL;
    tap_ok(
        cycles == CRASH_CYCLES && alive,
        "robustness: a crash-looping wapp does not thrash or wedge the engine");
}

/* Prove /dev/pipe is a process-wide channel between two distinct wapps. Two
 * instances run the single `duplex` image and pick their side from the ROLE env
 * var; the supervisor verifies the payload reached the reader's log. */
#define DUPLEX_PAYLOAD "duplex-ok"
#define READER_CFG "/dev/wanted/wapps/reader/config"
#define WRITER_CFG "/dev/wanted/wapps/writer/config"
#define READER_LOG LOG_MOUNT "/reader"
#define READER_CFG_BODY                                                        \
    "{\"image\":\"duplex\","                                                   \
    "\"console\":{\"in\":{\"name\":\"null\"},"                                 \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"envs\":[\"ROLE=reader\"]}"
/* writer pins the image by tag ("duplex:0.0.1-1") — exact resolution — while
 * reader uses the bare name ("duplex") — first-match. Both run the one image.
 * The version separator is '@' (VFAT-legal), not ':'. */
#define WRITER_CFG_BODY                                                        \
    "{\"image\":\"duplex:0.0.1-1\",\"envs\":[\"ROLE=writer\"]}"
static void pipe_duplex_check(void) {
    char buf[128];

    create_wapp("reader");
    create_wapp("writer");
    write_path(READER_CFG, READER_CFG_BODY); /* log console + ROLE=reader */
    write_path(WRITER_CFG, WRITER_CFG_BODY); /* ROLE=writer */
    start_wapp("reader"); /* blocks reading /dev/pipe/duplex */
    start_wapp("writer"); /* writes the payload to it */
    wait_dead("reader");

    int got = read_path(READER_LOG, buf, sizeof(buf)) > 0;
    tap_ok(got && strstr(buf, DUPLEX_PAYLOAD) != NULL,
           "pipe: a payload crosses between two wapps via /dev/pipe");
}

/* argv and environ passthrough plus exit-code exposure. argenv prints its known
 * args and envs to its log and exits non-zero; assert the values reached it and
 * that the clean exit surfaces on exit_code, where a trap would leave -1. */
static void argenv_check(void) {
    char buf[256];

    int started = create_wapp(ARGENV) &&
                  write_path(ARGENV_CFG, ARGENV_CFG_BODY) >= 0 &&
                  start_wapp(ARGENV);
    wait_dead(ARGENV);

    int got = read_path(ARGENV_LOG, buf, sizeof(buf)) > 0;
    tap_ok(started && got &&
               strstr(buf, "arg 0=argenv") != NULL && /* engine-set argv[0] */
               strstr(buf, "arg 1=alpha") != NULL &&
               strstr(buf, "arg 2=beta") != NULL &&
               strstr(buf, "FOO=bar") != NULL && strstr(buf, "BAZ=qux") != NULL,
           "argv/env: configured args and envs reach the launched wapp");

    int n = read_path(ARGENV_EXIT, buf, sizeof(buf));
    tap_ok(n > 0 && strstr(buf, "7") != NULL,
           "exit_code: a clean non-zero exit surfaces on the exit_code node");
}

/* Capability separation: launch observer with a `log` mount but no `wanted`
 * control mount, and assert it can read /proc/wapps and tail logs while every
 * attempt to reach the control plane is denied. */
static void observer_check(void) {
    char buf[1024];

    int started = create_wapp(OBSERVER) &&
                  write_path(OBSERVER_CFG, OBSERVER_CFG_BODY) >= 0 &&
                  start_wapp(OBSERVER);
    wait_dead(OBSERVER);

    int got = read_path(OBSERVER_LOG, buf, sizeof(buf)) > 0;
    tap_ok(started && got && strstr(buf, "obs-wapp:supervisor=") != NULL,
           "observe: a non-control wapp reads the fleet via /proc/wapps");
    tap_ok(got && strstr(buf, "obs-control:denied") != NULL &&
               strstr(buf, "obs-control:reachable") == NULL,
           "observe: the control plane is unreachable without the wanted mount");
    tap_ok(got && strstr(buf, "obs-done") != NULL,
           "observe: the observability wapp ran to completion");
}

/* A `pipe` console is a live stream to a peer. Launch argpipe with stdout
 * backed by one, then read its output from /dev/pipe/argpipe.out, proving a
 * wapp's stdout can be consumed live by another wapp. */
static void console_pipe_check(void) {
    char buf[256];

    int started = create_wapp(ARGPIPE) &&
                  write_path(ARGPIPE_CFG, ARGPIPE_CFG_BODY) >= 0 &&
                  start_wapp(ARGPIPE);
    wait_dead(ARGPIPE);

    int got = read_path(ARGPIPE_PIPE, buf, sizeof(buf)) > 0;
    tap_ok(started && got && strstr(buf, "arg 0=" ARGPIPE) != NULL &&
               strstr(buf, "arg 1=alpha") != NULL,
           "console: a pipe console streams a wapp's stdout to a peer reader");
}

/* The launch-config resource sections, verified in the supervisor's own
 * namespace: a `config` map mounted outside /dev that also surfaces a synthetic
 * parent in the root listing, a socket at /net/<name>, and `wanted`. */
static void mounts_check(void) {
    char buf[256];

    int n = read_path(CFGMAP_PATH, buf, sizeof(buf));
    tap_ok(n > 0 && strstr(buf, CFGMAP_MARKER) != NULL,
           "mounts: config-map reads back its content at /etc/config (outside "
           "/dev)");

    tap_ok(dir_has("/", "etc"),
           "mounts: a deep mount surfaces a synthetic parent in ls /");

    /* A socket needs an IP netstack even to enumerate the node, because
     * listing /net stats each entry and stat'ing a socket opens it. A readdir
     * abort means no netstack and skips; enumerable but absent is a failure. */
    int found = 0, aborted = 0;
    DIR *nd = opendir("/net");
    if (nd) {
        struct dirent *e;
        for (;;) {
            errno = 0;
            e = readdir(nd);
            if (e == NULL) {
                aborted = (errno != 0);
                break;
            }
            if (strcmp(e->d_name, SOCKET_NAME) == 0)
                found = 1;
        }
        closedir(nd);
    } else {
        aborted = 1;
    }

    if (found || !aborted) {
        tap_ok(found, "sockets: a named socket is created at /net/<name>");
    } else {
        tap_diag("sockets: skipped — /net enumeration needs an IP netstack "
                 "(absent on this build, e.g. the sim:wanted board)");
    }
}

/* Serving from inside the sandbox: a granted listening socket binds at open and
 * accepts connections onto fds of their own, with the clients connecting back
 * to the same port. Skipped on a build without the listen role. */
static void listen_check(void) {
    char buf[64];

    int lfd = open(SERVER_SOCKET, O_RDWR);
    if (lfd < 0) {
        tap_diag("listen: skipped — no listening socket in this config "
                 "(engine built without the listen role)");
        return;
    }

    /* A stream listener carries no payload of its own. */
    tap_ok(read(lfd, buf, sizeof(buf)) < 0,
           "listen: reading the listener itself fails — only its connections "
           "carry data");

    /* The client's first write connects and queues a request the listener has
     * not accepted yet; accept then hands back the connection that carries it.
     */
    int cfd = open(CLIENT_SOCKET, O_RDWR);
    int wrote = cfd >= 0 && write(cfd, LISTEN_REQ, strlen(LISTEN_REQ)) > 0;

    int afd = accept(lfd, NULL, NULL);
    tap_ok(wrote && afd >= 0, "listen: accept yields a connection fd");

    int n = afd >= 0 ? (int)read(afd, buf, sizeof(buf) - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    tap_ok(n > 0 && strcmp(buf, LISTEN_REQ) == 0,
           "listen: the accepted connection reads what the client sent");

    if (afd >= 0)
        write(afd, LISTEN_RES, strlen(LISTEN_RES));
    n = cfd >= 0 ? (int)read(cfd, buf, sizeof(buf) - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    tap_ok(n > 0 && strcmp(buf, LISTEN_RES) == 0,
           "listen: the server's answer reaches the client (round trip)");

    /* A second client is served alongside the first: two live connections, two
     * fds, no cross-talk. */
    int c2fd = open(CLIENT2_SOCKET, O_RDWR);
    int wrote2 = c2fd >= 0 && write(c2fd, LISTEN_REQ2, strlen(LISTEN_REQ2)) > 0;
    int a2fd = accept(lfd, NULL, NULL);
    tap_ok(wrote2 && a2fd >= 0 && a2fd != afd,
           "listen: a second connection accepts onto an fd of its own");

    n = a2fd >= 0 ? (int)read(a2fd, buf, sizeof(buf) - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    tap_ok(n > 0 && strcmp(buf, LISTEN_REQ2) == 0,
           "listen: concurrent connections stay isolated");

    /* Closing one connection leaves the other serving. */
    if (afd >= 0)
        close(afd);
    if (cfd >= 0)
        close(cfd);
    int served = a2fd >= 0 && write(a2fd, LISTEN_RES, strlen(LISTEN_RES)) > 0;
    n = c2fd >= 0 ? (int)read(c2fd, buf, sizeof(buf) - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    tap_ok(served && n > 0 && strcmp(buf, LISTEN_RES) == 0,
           "listen: closing one connection leaves the other serving");

    if (a2fd >= 0)
        close(a2fd);
    if (c2fd >= 0)
        close(c2fd);
    close(lfd);
}

/* A bound datagram socket serves without an accept step: it reads a datagram
 * and answers the sender on the socket itself. */
static void dgram_listen_check(void) {
    char buf[64];

    int sfd = open(DGRAM_SOCKET, O_RDWR);
    if (sfd < 0) {
        tap_diag("listen: skipped — no bound datagram socket in this config");
        return;
    }

    int cfd = open(DCLIENT_SOCKET, O_RDWR);
    int sent = cfd >= 0 && write(cfd, LISTEN_REQ, strlen(LISTEN_REQ)) > 0;

    int n = (int)read(sfd, buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
    tap_ok(sent && n > 0 && strcmp(buf, LISTEN_REQ) == 0,
           "listen: a bound datagram socket reads a datagram with no accept");

    write(sfd, LISTEN_RES, strlen(LISTEN_RES));
    n = cfd >= 0 ? (int)read(cfd, buf, sizeof(buf) - 1) : -1;
    if (n > 0)
        buf[n] = '\0';
    tap_ok(n > 0 && strcmp(buf, LISTEN_RES) == 0,
           "listen: the datagram answer reaches the sender");

    if (cfd >= 0)
        close(cfd);
    close(sfd);
}

/* A `platform` bind mount must confine path resolution to its host directory:
 * an in-bounds file reads back, a symlink pointing outside must not resolve.
 * A missing escape node is not a failure, since the setup needs symlinks. */
static void bind_mount_escape_check(void) {
    /* The /host bind mount is wired only in the Linux selftest config; a build
     * without it (e.g. the NuttX sim) has nothing to confine. */
    int probe = open("/host", O_RDONLY | O_DIRECTORY);
    if (probe < 0) {
        tap_diag("bind mount: skipped — no /host platform mount on this build");
        return;
    }
    close(probe);

    char buf[64];
    int n = read_path(BIND_INSIDE, buf, sizeof(buf));
    tap_ok(n > 0 && strstr(buf, BIND_INSIDE_MARKER) != NULL,
           "bind mount: an in-bounds file reads through the platform mount");

    int fd = open(BIND_ESCAPE, O_RDONLY);
    if (fd >= 0)
        close(fd);
    tap_ok(fd < 0, "bind mount: a symlink escaping the mount root is denied");
}

/* Configure instance `name` with `cfg` and return true if the engine rejected
 * it. Each `cfg` pins a known-good image, so the image loads and the only
 * failure source is the launch config itself. */
static int rejects_config(const char *name, const char *cfg) {
    char path[96], state[96], buf[64];
    if (!create_wapp(name))
        return 0;
    wapp_node(path, sizeof(path), name, "config");
    if (write_path(path, cfg) < 0)
        return 0;
    start_wapp(name);
    wapp_node(state, sizeof(state), name, "state");
    wait_dead(name);
    int n = read_path(state, buf, sizeof(buf));
    return !(n > 0 && (strstr(buf, "running") || strstr(buf, "starting")));
}

/* Per-section launch-config validation must fail loudly: a path on a device
 * driver or socket, a mount under a reserved namespace, and a malformed socket
 * address are each rejected at install, not left half-configured. */
static void launch_config_validation_check(void) {
    static const struct {
        const char *name, *cfg;
    } bad[] = {
        {"m_dev", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"config\","
                  "\"path\":\"/dev/x\",\"options\":\"y\"}]}"},
        {"m_net", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"config\","
                  "\"path\":\"/net/x\",\"options\":\"y\"}]}"},
        {"m_proc", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"config\","
                   "\"path\":\"/proc/x\",\"options\":\"y\"}]}"},
        {"d_path", "{\"image\":\"looper\",\"drivers\":[{\"name\":\"null\","
                   "\"path\":\"/x\"}]}"},
        {"s_path",
         "{\"image\":\"looper\",\"sockets\":[{\"name\":\"s\",\"path\":\"/net/"
         "x\",\"address\":\"tcp://localhost:9999\"}]}"},
        {"s_addr", "{\"image\":\"looper\",\"sockets\":[{\"name\":\"s\","
                   "\"address\":\"bogus\"}]}"},
        {"m_psrc", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"platform\","
                   "\"path\":\"/p\",\"options\":\"src=relative\"}]}"},
        {"m_popt", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"platform\","
                   "\"path\":\"/p\",\"options\":\"bogus\"}]}"},
        {"m_vnam", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"volume\","
                   "\"path\":\"/d\",\"options\":\"name=../escape\"}]}"},
        {"m_vopt", "{\"image\":\"looper\",\"mounts\":[{\"name\":\"volume\","
                   "\"path\":\"/d\",\"options\":\"bogus\"}]}"},
    };
    char buf[80], desc[96];
    int all = 1;

    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        int rejected = rejects_config(bad[i].name, bad[i].cfg);
        snprintf(desc, sizeof(desc), "launch-config reject %s: %s", bad[i].name,
                 rejected ? "ok" : "ACCEPTED");
        tap_diag(desc);
        if (!rejected)
            all = 0;
    }
    tap_ok(all, "launch config: malformed drivers/mounts/sockets are rejected "
                "at install");

    /* A valid config mount on a launched wapp must still come up. */
    int ok =
        create_wapp("cfgok") &&
        write_path("/dev/wanted/wapps/cfgok/config",
                   "{\"image\":\"looper\",\"mounts\":[{\"name\":\"config\","
                   "\"path\":\"/etc/cfg\",\"options\":\"z\"}]}") >= 0 &&
        start_wapp("cfgok") && wait_state("/dev/wanted/wapps/cfgok/state", 1);
    tap_ok(ok, "launch config: a valid config mount launches the wapp");
    if (ok) {
        write_path("/dev/wanted/wapps/cfgok/ctl", "stop");
        wait_state("/dev/wanted/wapps/cfgok/state", 0);
    }

    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "launch config: supervisor survives the rejected configs");
}

/* An engine-managed `volume` is a writable, persistent, named store that
 * survives a wapp restart. Running volcheck twice as the same instance proves
 * the first run's write persists, and the payload reads back byte for byte. */
static void volume_check(void) {
    char buf[160];

    int r1 = create_wapp(VOLCHECK) &&
             write_path(VOLCHECK_CFG, VOLCHECK_CFG_BODY) >= 0 &&
             start_wapp(VOLCHECK) && wait_dead(VOLCHECK);
    int wrote = r1 && read_path(VOLCHECK_LOG, buf, sizeof(buf)) > 0 &&
                strstr(buf, "vol-wrote") != NULL;
    tap_ok(
        wrote,
        "volume: a fresh volume mounts writable and the wapp writes its state");

    /* The launch config is consumed on start, so re-arm it before relaunching
     * the same instance. The store is named by the instance, not the config. */
    int r2 = write_path(VOLCHECK_CFG, VOLCHECK_CFG_BODY) >= 0 &&
             start_wapp(VOLCHECK) && wait_dead(VOLCHECK);
    int n = r2 ? read_path(VOLCHECK_LOG, buf, sizeof(buf)) : -1;

    tap_ok(n > 0 && strstr(buf, "vol-open") != NULL,
           "volume: state persists across a wapp restart (marker re-opens)");

    tap_ok(n > 0 && strstr(buf, "vol-read:" VOLCHECK_PAYLOAD) != NULL,
           "volume: the persisted bytes read back through the preopen");
}

/* A shared volume crosses the wapp isolation boundary by design: two instances
 * naming the same `shared` volume see one store. The producer writes a marker,
 * the consumer re-opens it, and the payload is read back byte for byte. */
static void shared_volume_check(void) {
    char buf[160];

    int p = create_wapp(VPROD) && write_path(VPROD_CFG, SHARED_CFG_BODY) >= 0 &&
            start_wapp(VPROD) && wait_dead(VPROD);
    int wrote = p && read_path(VPROD_LOG, buf, sizeof(buf)) > 0 &&
                strstr(buf, "vol-wrote") != NULL;
    tap_ok(wrote, "shared volume: a producer writes to a fresh shared volume");

    /* A different instance names the same shared volume and must see the marker
     * the producer wrote — the store crossed the wapp boundary. */
    int c = create_wapp(VCONS) && write_path(VCONS_CFG, SHARED_CFG_BODY) >= 0 &&
            start_wapp(VCONS) && wait_dead(VCONS);
    int n = c ? read_path(VCONS_LOG, buf, sizeof(buf)) : -1;

    tap_ok(n > 0 && strstr(buf, "vol-open") != NULL,
           "shared volume: a second wapp reaches the producer's store "
           "(cross-wapp share)");

    tap_ok(n > 0 && strstr(buf, "vol-read:" VOLCHECK_PAYLOAD) != NULL,
           "shared volume: the shared bytes read back through the preopen");
}

/* Private and shared namespaces must never alias: a `name=iso` private volume
 * and a `name=iso` shared volume are different stores. The private instance
 * must see a fresh store; the shared marker there would be a leak. */
static void volume_isolation_check(void) {
    char buf[160];

    int s = create_wapp(ISO_SHARE) &&
            write_path(ISO_SHARE_CFG, ISO_SHARE_CFG_BODY) >= 0 &&
            start_wapp(ISO_SHARE) && wait_dead(ISO_SHARE);
    int shared_wrote = s && read_path(ISO_SHARE_LOG, buf, sizeof(buf)) > 0 &&
                       strstr(buf, "vol-wrote") != NULL;

    int p = create_wapp(ISO_PRIV) &&
            write_path(ISO_PRIV_CFG, ISO_PRIV_CFG_BODY) >= 0 &&
            start_wapp(ISO_PRIV) && wait_dead(ISO_PRIV);
    int priv_fresh = p && read_path(ISO_PRIV_LOG, buf, sizeof(buf)) > 0 &&
                     strstr(buf, "vol-wrote") != NULL &&
                     strstr(buf, "vol-open") == NULL;

    tap_ok(shared_wrote && priv_fresh, "volume: a private volume never aliases "
                                       "a shared volume of the same name");
}

/* `ro` is orthogonal to `shared`: a read-only shared volume is provisioned by
 * the engine but denies the wapp every write. vroro tries to create a marker on
 * a fresh ro shared store and is refused, reporting "vol-fail". */
static void volume_readonly_check(void) {
    char buf[160];

    int r = create_wapp(VRORO) && write_path(VRORO_CFG, VRORO_CFG_BODY) >= 0 &&
            start_wapp(VRORO) && wait_dead(VRORO);
    int denied = r && read_path(VRORO_LOG, buf, sizeof(buf)) > 0 &&
                 strstr(buf, "vol-fail") != NULL &&
                 strstr(buf, "vol-wrote") == NULL;
    tap_ok(denied, "volume: a read-only shared volume denies writes");
}

/* Multiple readers on one pipe. A named pipe is a single consume-once ring, so
 * with two readers blocked and one payload written, exactly one receives it and
 * the other reaches EOF: multi-reader attach is safe, each byte once. */
#define MREAD_A "mreadA"
#define MREAD_B "mreadB"
#define MREAD_A_LOG LOG_MOUNT "/" MREAD_A
#define MREAD_B_LOG LOG_MOUNT "/" MREAD_B
#define DUPLEX_CHAN "/dev/pipe/duplex"
static void multi_reader_pipe_check(void) {
    char buf[128];

    create_wapp(MREAD_A);
    create_wapp(MREAD_B);
    write_path("/dev/wanted/wapps/" MREAD_A "/config", READER_CFG_BODY);
    write_path("/dev/wanted/wapps/" MREAD_B "/config", READER_CFG_BODY);
    start_wapp(MREAD_A);
    start_wapp(MREAD_B);

    /* Both readers must be attached (blocked in their read) before the writer
     * closes, so the delivery is deterministic: one drains the payload, the
     * other sees the closed writer and gets EOF. */
    wait_state("/dev/wanted/wapps/" MREAD_A "/state", 1);
    wait_state("/dev/wanted/wapps/" MREAD_B "/state", 1);

    /* The supervisor is the single writer: one payload into the shared ring. */
    write_path(DUPLEX_CHAN, DUPLEX_PAYLOAD);

    wait_dead(MREAD_A);
    wait_dead(MREAD_B);

    int got_a = read_path(MREAD_A_LOG, buf, sizeof(buf)) > 0 &&
                strstr(buf, DUPLEX_PAYLOAD) != NULL;
    int got_b = read_path(MREAD_B_LOG, buf, sizeof(buf)) > 0 &&
                strstr(buf, DUPLEX_PAYLOAD) != NULL;
    tap_ok(got_a != got_b, "pipe: two readers on one pipe — payload reaches "
                           "exactly one (consume-once)");
}

int main(void) {
    /* Phases run in order, each announced with a current/total counter before
     * it runs, so a long mostly-sleeping check is visibly progressing. The
     * table is the single source for both the order and the total. */
    static const struct {
        const char *name;
        void (*run)(void);
    } phases[] = {
        {"positive_checks", positive_checks},
        {"mounts_check", mounts_check},
        {"bind_mount_escape_check", bind_mount_escape_check},
        {"listen_check", listen_check},
        {"dgram_listen_check", dgram_listen_check},
        {"pipe_duplex_check", pipe_duplex_check},
        {"multi_reader_pipe_check", multi_reader_pipe_check},
        {"robustness_checks", robustness_checks},
        {"containment_checks", containment_checks},
        {"memcap_checks", memcap_checks},
        {"cpuhog_check", cpuhog_check},
        {"console_checks", console_checks},
        {"argenv_check", argenv_check},
        {"observer_check", observer_check},
        {"console_pipe_check", console_pipe_check},
        {"lifecycle_checks", lifecycle_checks},
        {"blocker_check", blocker_check},
        {"ioblock_check", ioblock_check},
        {"edge_checks", edge_checks},
        {"sandbox_check", sandbox_check},
        {"resource_check", resource_check},
        {"malformed_check", malformed_check},
        {"crashloop_check", crashloop_check},
        {"launch_config_validation_check", launch_config_validation_check},
        {"volume_check", volume_check},
        {"shared_volume_check", shared_volume_check},
        {"volume_isolation_check", volume_isolation_check},
        {"volume_readonly_check", volume_readonly_check},
    };
    const int total = (int)(sizeof(phases) / sizeof(phases[0]));

    printf("# WANTED engine selftest\n");
    fflush(stdout);

    for (int i = 0; i < total; i++) {
        char label[64];
        snprintf(label, sizeof(label), "phase %d/%d: %s", i + 1, total,
                 phases[i].name);
        tap_diag(label);
        phases[i].run();
    }
    return tap_plan();
}
