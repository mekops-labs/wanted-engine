/* SPDX-License-Identifier: Apache-2.0 */

/* selftest — the WANTED engine test supervisor.
 *
 * Runs as the boot supervisor (like sheriff/wsh) and exercises the engine from
 * inside WASM, emitting TAP to stdout (the host runner scans the console). It
 * does two kinds of checks:
 *
 *   positive  — assert its own VFS namespace, /proc, the read-only TarFS, the
 *               inter-wapp pipe, and the control plane behave as expected;
 *   robustness — launch a deliberately misbehaving wapp via the control plane
 *               and assert the engine contains it (the wapp ends in a dead
 *               state while this supervisor keeps running).
 *
 * Everything here is plain WASI + the WANTED VFS/control-plane ABI, so the same
 * image runs on Linux, the NuttX sim, and future hardware. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "tap.h"

#define WANTED_CTL "/dev/wanted/ctl"
#define TRAPPER "trapper"
#define TRAPPER_CFG "/dev/wanted/wapps/" TRAPPER "/config"
#define TRAPPER_STATE "/dev/wanted/wapps/" TRAPPER "/state"
#define SUPERVISOR_STATE "/dev/wanted/wapps/supervisor/state"

/* Launched test wapps get a null stdin and the "log" console for stdout/stderr:
 * their output is captured per-wapp in the engine log store (read back below
 * via .../log) instead of sharing the platform console — so a launched wapp's
 * stdio teardown on exit cannot close the supervisor's own stdout. No interior
 * whitespace so the control-plane string parser keeps it as one value. */
#define LAUNCH_CFG                                                             \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}}}"
#define TRAPPER_LOG "/dev/wanted/wapps/" TRAPPER "/log"
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
#define ARGENV_LOG "/dev/wanted/wapps/" ARGENV "/log"
#define ARGENV_EXIT "/dev/wanted/wapps/" ARGENV "/exit_code"
#define ARGENV_CFG_BODY                                                        \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"args\":[\"alpha\",\"beta\"],\"envs\":[\"FOO=bar\",\"BAZ=qux\"]}"

/* volcheck mounts an engine-managed `volume` at /data. On a fresh store it
 * writes a marker and reports "vol-wrote"; on a store that already holds state
 * it reads the marker back and reports "vol-read:<payload>". Two runs of the
 * same instance prove the volume persists across a restart. */
#define VOLCHECK "volcheck"
#define VOLCHECK_CFG "/dev/wanted/wapps/" VOLCHECK "/config"
#define VOLCHECK_LOG "/dev/wanted/wapps/" VOLCHECK "/log"
#define VOLCHECK_PAYLOAD "persist-42"
#define VOLCHECK_CFG_BODY                                                      \
    "{\"console\":{\"in\":{\"name\":\"null\"},"                                \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\"}]}"

/* A shared volume is one store two wapps reach by name — the substrate for a
 * producer→processor→publisher pipeline. Two distinct instances (vprod, vcons)
 * both run the volcheck image against the same `name=stream,shared` volume: the
 * producer writes the marker on the fresh store, the consumer (a different
 * instance) re-opens it, proving the store crosses the wapp boundary. Both bind
 * the image via the config `image` field, since the instance names differ. */
#define VPROD "vprod"
#define VCONS "vcons"
#define VPROD_CFG "/dev/wanted/wapps/" VPROD "/config"
#define VCONS_CFG "/dev/wanted/wapps/" VCONS "/config"
#define VPROD_LOG "/dev/wanted/wapps/" VPROD "/log"
#define VCONS_LOG "/dev/wanted/wapps/" VCONS "/log"
#define SHARED_CFG_BODY                                                        \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=stream,shared\"}]}"

/* Isolation: a private and a shared volume of the *same name* must be different
 * stores. isoshr writes to a shared `name=iso`; isoprv then mounts a private
 * `name=iso` (no `shared`) — if the two namespaces mixed, isoprv would find the
 * shared marker (vol-open); kept disjoint, it sees a fresh store and writes
 * (vol-wrote). Both run the volcheck image. */
#define ISO_SHARE "isoshr"
#define ISO_PRIV "isoprv"
#define ISO_SHARE_CFG "/dev/wanted/wapps/" ISO_SHARE "/config"
#define ISO_PRIV_CFG "/dev/wanted/wapps/" ISO_PRIV "/config"
#define ISO_SHARE_LOG "/dev/wanted/wapps/" ISO_SHARE "/log"
#define ISO_PRIV_LOG "/dev/wanted/wapps/" ISO_PRIV "/log"
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
 * `name=roonly,shared,ro` store; volcheck finds no marker and tries to create
 * one, which the ro grant rejects (-EROFS), so it reports "vol-fail". This is
 * the publisher's mount in a producer→processor→publisher chain — read the
 * shared feed, never mutate it. */
#define VRORO "vroro"
#define VRORO_CFG "/dev/wanted/wapps/" VRORO "/config"
#define VRORO_LOG "/dev/wanted/wapps/" VRORO "/log"
#define VRORO_CFG_BODY                                                         \
    "{\"image\":\"volcheck\",\"console\":{\"in\":{\"name\":\"null\"},"         \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"mounts\":[{\"name\":\"volume\",\"path\":\"/data\","                     \
    "\"options\":\"name=roonly,shared,ro\"}]}"

/* The supervisor's own launch config (selftest-config.json) wires the three
 * launch-config resource sections, so they are verified in our own namespace:
 * a `config` map mounted at an arbitrary path outside /dev, a named socket, and
 * the `wanted` device driver. */
#define CFGMAP_PATH "/etc/config"
#define CFGMAP_MARKER "selftest-cfgmap-v1"
#define SOCKET_NAME "uplink"

/* A `platform` bind mount at /host (selftest-config.json, backed by a host dir
 * the runner populates): an in-bounds file reads back, but a symlink the host
 * planted inside the dir that points OUTSIDE it must not resolve through the
 * mount — the bind-mount confinement the read-only flag cannot provide. */
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
     * it past 256 B; the node is one-shot, so a short read silently truncates. */
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

    /* /proc/wapps lists the running supervisor. */
    tap_ok(read_path("/proc/wapps", buf, sizeof(buf)) > 0 &&
               strstr(buf, "supervisor") != NULL,
           "proc: /proc/wapps reports the supervisor");

    /* /proc/wanted reports engine identity and the compile-time ceilings as
     * key:\tvalue lines. It is unprivileged. The platform string and version
     * vary by target/build, so assert the stable fields: the identity keys are
     * present and max_wapps carries the actual MAX_WAPPS ceiling. */
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

    /* Poll until it leaves starting/running, i.e. the engine has reaped the
     * trap. Bounded so a hang fails rather than blocks. (A trap now reports
     * "failure"; "exited" is also accepted for robustness — both count as
     * dead.) */
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

/* Poll a wapp's state node until `want_running` matches whether it is
 * running/starting, bounded to ~10 s. Returns true if the condition was met. A
 * node that can't be read (the wapp is unknown — e.g. a launch that failed
 * before it ever ran, leaving no slot) counts as not-live, so a "wait until
 * dead" poll is satisfied immediately rather than spinning out the bound. */
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
 * in a dead state on its own (trap or in-sandbox resource bound) while this
 * supervisor and the host survive. Covers the stack-overflow and
 * memory-exhaustion classes (the OOB trap is covered by robustness_checks). */
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

/* The per-wapp linear-memory cap (WASM_MAX_MEMORY_PAGES; constrained default =
 * one page) is enforced two ways. bigmem grows past one page: under the cap its
 * grow is refused, malloc returns NULL, and it logs "bigmem-bounded" (still
 * exiting cleanly). biginit declares four initial pages: the engine refuses to
 * load an image whose initial memory already exceeds the cap, so it ends in a
 * failure state without running. Both assume the constrained default cap; a
 * build with a wider cap (PROFILE=small/big) would admit them. */
static void memcap_checks(void) {
    char path[96], buf[64];

    int bm = launch("bigmem") && wait_dead("bigmem");
    wapp_node(path, sizeof(path), "bigmem", "log");
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
 * termination check unwinds even a tight compute loop that never blocks. Start
 * the cpuhog, confirm it runs, stop it via the control plane, confirm the
 * engine terminated it. */
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

/* Console backing: a wapp's stdio slots default when the launch config omits
 * them (stdin->null, stdout/stderr->log), and an explicit all-null console is
 * also valid. Either way the wapp must launch — a wapp with unwired stdio fds
 * fails to start. Reuses the looper (a clean long-runner), stopped after each.
 */
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

/* Launch a wapp that parks in a blocking host call, stop it, and report whether
 * the stop *interrupted* the call promptly. cpuhog covers the other axis (a
 * wapp busy in the interpreter, where the terminate flag is checked per
 * instruction); these cover a wapp with no instruction boundaries to check
 * because it is parked in a host call. The stop must reach it anyway: the
 * engine sets the terminate flag and signals the worker to EINTR the call, so
 * the interpreter regains control and unwinds. Promptness is judged in a 2 s
 * window — well under any self-return — so it isolates the interrupt path;
 * *alive_out reports whether the supervisor survived. On a non-prompt result
 * the wapp is reaped (bounded) so the suite can continue and the failure is
 * recorded rather than hanging. */
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

/* Launch a non-privileged wapp that tries to break out of its namespace, and
 * assert every escape was denied. Where positive_checks probes isolation from
 * the supervisor's privileged view, this probes it from a launched wapp — the
 * actual sandbox boundary. The escaper reports a single verdict on its log
 * console: "sandbox-OK" if all attempts were denied, "sandbox-LEAK" if any
 * succeeded. */
static void sandbox_check(void) {
    char log[96], buf[128];
    wapp_node(log, sizeof(log), "escaper", "log");

    launch("escaper");
    wait_dead("escaper");

    int got = read_path(log, buf, sizeof(buf)) > 0;
    tap_ok(got && strstr(buf, "sandbox-OK") != NULL &&
               strstr(buf, "sandbox-LEAK") == NULL,
           "sandbox: a launched wapp cannot escape its namespace");
}

/* Launch a wapp that exhausts a sandbox resource (file descriptors) and assert
 * the abuse is contained: the wapp is reaped and the supervisor survives —
 * never a host crash. Whether the engine bounded the fd table below the wapp's
 * probe cap is reported as a diagnostic. */
static void resource_check(void) {
    char log[96], verdict[64], buf[64];
    wapp_node(log, sizeof(log), "fdhog", "log");

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

/* Try to start a battery of malformed registry images (no app.wasm entrypoint,
 * invalid wasm, truncated archive). The engine must reject each cleanly — none
 * reaches a running state — and stay up; a crash in the loader would take the
 * whole engine down and the TAP plan would never print. */
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

/* Prove /dev/pipe is a process-wide channel between two distinct wapps (the
 * positive_checks round-trip is within one namespace). Two instances —
 * `reader` and `writer` — run the single `duplex` image (config `image`):
 * `reader` blocks reading the shared channel and echoes what it got to its log
 * console; `writer` writes the payload. Each picks its side from the ROLE env
 * var in its launch config (the env-passthrough path) — the supervisor verifies
 * the payload reached the reader's log. */
#define DUPLEX_PAYLOAD "duplex-ok"
#define READER_CFG "/dev/wanted/wapps/reader/config"
#define WRITER_CFG "/dev/wanted/wapps/writer/config"
#define READER_LOG "/dev/wanted/wapps/reader/log"
#define READER_CFG_BODY                                                        \
    "{\"image\":\"duplex\","                                                   \
    "\"console\":{\"in\":{\"name\":\"null\"},"                                 \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}},"                  \
    "\"envs\":[\"ROLE=reader\"]}"
/* writer pins the image by tag ("duplex@0.0.1-1") — exact resolution — while
 * reader uses the bare name ("duplex") — first-match. Both run the one image.
 * The version separator is '@' (VFAT-legal), not ':'. */
#define WRITER_CFG_BODY                                                        \
    "{\"image\":\"duplex@0.0.1-1\",\"envs\":[\"ROLE=writer\"]}"
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

/* argv / environ passthrough + exit-code exposure. Configure argenv with known
 * args and envs, launch it via its own ctl, and let it print them to its log
 * and exit with a fixed non-zero code. Assert the values reached the wapp
 * (argv[0] is the engine-set name) and that the clean non-zero exit surfaces on
 * the exit_code node — distinct from a trap, which would leave it at -1. */
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

/* The launch-config resource sections, verified in the supervisor's own
 * namespace (wired by selftest-config.json):
 *   - mounts[]:  a `config` map mounts at an arbitrary path OUTSIDE /dev
 *                (/etc/config), reads back its configured content, and surfaces
 *                a synthetic parent (/etc) in the root listing — exercising the
 *                general single-driver VFS mount;
 *   - sockets[]: a socket is created at /net/<name> by name;
 *   - drivers[]: the `wanted` device driver mounts at /dev/<name> — already
 *                asserted by positive_checks via /dev/wanted. */
static void mounts_check(void) {
    char buf[256];

    int n = read_path(CFGMAP_PATH, buf, sizeof(buf));
    tap_ok(n > 0 && strstr(buf, CFGMAP_MARKER) != NULL,
           "mounts: config-map reads back its content at /etc/config (outside "
           "/dev)");

    tap_ok(dir_has("/", "etc"),
           "mounts: a deep mount surfaces a synthetic parent in ls /");

    /* A socket needs an IP netstack: socket() must succeed even to enumerate
     * the node, because listing /net stats each entry and stat'ing a socket
     * node opens it. The sim:wanted board is built without CONFIG_NET, so
     * socket() fails there and /net enumeration aborts. Distinguish three
     * outcomes:
     *   - found        → the socket is present (assert pass);
     *   - readdir abort → no netstack on this build (skip with a diagnostic);
     *   - enumerable but absent → a real regression (assert fail). */
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

/* A `platform` bind mount must confine path resolution to its host directory.
 * An in-bounds file reads back; a symlink the host planted inside the mount
 * that points outside it must not resolve through the mount. (Host-side
 * symlinks only exist on Linux; on a target without them the escape simply
 * cannot be set up, so a missing escape node is not a failure.) */
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

/* Configure instance `name` with `cfg` and return true if the engine REJECTED
 * it — the wapp never reached running/starting. Each `cfg` pins image:looper (a
 * known-good image) so the image loads and the ONLY failure source is the
 * launch config itself. */
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
 * driver or socket, a mount under a reserved namespace (/dev, /net, /proc), and
 * a malformed socket address are each rejected at install — the wapp fails to
 * start rather than coming up half-configured. A valid config mount on a
 * launched wapp still runs, proving the rejection is specific. */
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

/* An engine-managed `volume` is a writable, persistent, named store: the engine
 * owns the host location (the wapp names only the volume) and it survives a
 * wapp restart. volcheck writes a marker on a fresh store and, on a populated
 * one, re-opens it and reads it back. Running the same instance twice — the
 * engine names the volume by instance, so both runs see the same store — proves
 * the first run's write persists into the second.
 *
 * Persistence (the marker re-opens after the restart) is asserted on every
 * platform. The byte-level read-back is asserted where the host-fs preopen
 * returns content; a build whose preopen opens but reads back nothing (the
 * NuttX sim's hostfs) skips that one assertion with a diagnostic, the same way
 * the socket check skips on a netless build. */
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

    if (n > 0 && strstr(buf, "vol-read:" VOLCHECK_PAYLOAD) != NULL) {
        tap_ok(1, "volume: the persisted bytes read back through the preopen");
    } else {
        tap_diag(
            "volume: byte read-back skipped — this build's host-fs preopen "
            "opens the persisted file but reads back no content (e.g. the "
            "NuttX sim hostfs)");
    }
}

/* A shared volume crosses the wapp isolation boundary by design: two instances
 * that name the same `shared` volume see one store. The producer writes a
 * marker to a fresh shared volume; the consumer — a separate instance —
 * re-opens that marker, proving the store is shared, not per-wapp. The byte
 * read-back is asserted only where the host-fs preopen returns content (skipped
 * on the NuttX sim hostfs, like the persistence check). */
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

    if (n > 0 && strstr(buf, "vol-read:" VOLCHECK_PAYLOAD) != NULL) {
        tap_ok(1,
               "shared volume: the shared bytes read back through the preopen");
    } else {
        tap_diag(
            "shared volume: byte read-back skipped — host-fs preopen opens "
            "the shared file but reads back no content (e.g. the NuttX sim)");
    }
}

/* Private and shared namespaces must never alias: a `name=iso` private volume
 * and a `name=iso` shared volume are different stores. The shared instance
 * writes its marker; the private instance, naming the same volume, must see a
 * fresh store (write, not read) — finding the shared marker would be a
 * namespace leak. This is the open-based proof (it holds even where byte
 * read-back does not). */
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
 * a fresh ro shared store and is refused (-EROFS), reporting "vol-fail". */
static void volume_readonly_check(void) {
    char buf[160];

    int r = create_wapp(VRORO) && write_path(VRORO_CFG, VRORO_CFG_BODY) >= 0 &&
            start_wapp(VRORO) && wait_dead(VRORO);
    int denied = r && read_path(VRORO_LOG, buf, sizeof(buf)) > 0 &&
                 strstr(buf, "vol-fail") != NULL &&
                 strstr(buf, "vol-wrote") == NULL;
    tap_ok(denied, "volume: a read-only shared volume denies writes (-EROFS)");
}

/* Multiple readers on one pipe. A named pipe is a single consume-once ring, not
 * a broadcast: with two readers blocked on /dev/pipe/duplex and one writer
 * (this supervisor) writing a single payload, exactly one reader receives it
 * and the other reaches EOF — proving multi-reader attach is safe and each byte
 * is delivered once. MAX_WAPPS=3, so the supervisor is the writer and the two
 * readers are the only launched wapps. Both reader instances run the one duplex
 * image (ROLE=reader) and echo what they read to their log. */
#define MREAD_A "mreadA"
#define MREAD_B "mreadB"
#define MREAD_A_LOG "/dev/wanted/wapps/" MREAD_A "/log"
#define MREAD_B_LOG "/dev/wanted/wapps/" MREAD_B "/log"
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
    /* Phases run in order. Announce each before running it (with a
     * current/total counter) so a long, mostly-sleeping check — the
     * control-plane stop/wait phases take seconds — is visibly progressing
     * rather than looking hung between `ok` lines. The table is the single
     * source for the order and the total, so adding a phase updates the counter
     * automatically. */
    static const struct {
        const char *name;
        void (*run)(void);
    } phases[] = {
        {"positive_checks", positive_checks},
        {"mounts_check", mounts_check},
        {"bind_mount_escape_check", bind_mount_escape_check},
        {"pipe_duplex_check", pipe_duplex_check},
        {"multi_reader_pipe_check", multi_reader_pipe_check},
        {"robustness_checks", robustness_checks},
        {"containment_checks", containment_checks},
        {"memcap_checks", memcap_checks},
        {"cpuhog_check", cpuhog_check},
        {"console_checks", console_checks},
        {"argenv_check", argenv_check},
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
