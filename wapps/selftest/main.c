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
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "tap.h"

#define WANTED_CTL   "/dev/wanted/ctl"
#define TRAPPER      "trapper"
#define TRAPPER_CFG  "/dev/wanted/wapps/" TRAPPER "/config"
#define TRAPPER_STATE "/dev/wanted/wapps/" TRAPPER "/state"
#define SUPERVISOR_STATE "/dev/wanted/wapps/supervisor/state"

/* Launched test wapps get a null stdin and the "log" console for stdout/stderr:
 * their output is captured per-wapp in the engine log store (read back below
 * via .../log) instead of sharing the platform console — so a launched wapp's
 * stdio teardown on exit cannot close the supervisor's own stdout. No interior
 * whitespace so the control-plane string parser keeps it as one value. */
#define LAUNCH_CFG \
    "{\"console\":{\"in\":{\"name\":\"null\"}," \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}}}"
#define TRAPPER_LOG "/dev/wanted/wapps/" TRAPPER "/log"
#define TRAPPER_MARKER "trapper-was-here"

#define LOOPER       "looper"
#define LOOPER_CFG   "/dev/wanted/wapps/" LOOPER "/config"
#define LOOPER_CTL   "/dev/wanted/wapps/" LOOPER "/ctl"
#define LOOPER_STATE "/dev/wanted/wapps/" LOOPER "/state"

/* All-null console: every stdio slot discards/EOFs. A wapp still launches and
 * runs — it is just silent. No interior whitespace (see LAUNCH_CFG). */
#define NULL_CONSOLE_CFG \
    "{\"console\":{\"in\":{\"name\":\"null\"}," \
    "\"out\":{\"name\":\"null\"},\"err\":{\"name\":\"null\"}}}"

/* argenv prints its argv + environ to the log console and exits with code 7.
 * Its config passes known args and envs (no interior whitespace per LAUNCH_CFG)
 * so the supervisor can read them back from the log and assert passthrough. */
#define ARGENV        "argenv"
#define ARGENV_CFG    "/dev/wanted/wapps/" ARGENV "/config"
#define ARGENV_LOG    "/dev/wanted/wapps/" ARGENV "/log"
#define ARGENV_EXIT   "/dev/wanted/wapps/" ARGENV "/exit_code"
#define ARGENV_CFG_BODY \
    "{\"console\":{\"in\":{\"name\":\"null\"}," \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}}," \
    "\"args\":[\"alpha\",\"beta\"],\"envs\":[\"FOO=bar\",\"BAZ=qux\"]}"

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

/* Launch an already-configured wapp through its own ctl node (defined below). */
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
    char buf[256];

    tap_ok(read_path("/app.wasm", buf, sizeof(buf)) > 0,
           "TarFS: /app.wasm is readable");
    tap_ok(dir_has("/dev", "wanted"), "VFS: /dev exposes the wanted control plane");
    tap_ok(dir_has("/dev", "pipe"), "VFS: /dev exposes pipe");
    tap_ok(dir_has("/proc", "wapps"), "VFS: /proc exposes wapps");

    /* /dev/null reads as EOF. */
    int fd = open("/dev/null", O_RDONLY);
    int n = (fd >= 0) ? read(fd, buf, sizeof(buf)) : -1;
    if (fd >= 0) close(fd);
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
               strstr(buf, "max_wapps:\t3") != NULL,
           "proc: /proc/wanted reports engine identity and limits");

    /* Inter-wapp pipe round-trip within our own namespace. */
    write_path("/dev/pipe/selftest", "ping");
    tap_ok(read_path("/dev/pipe/selftest", buf, sizeof(buf)) > 0 &&
               strncmp(buf, "ping", 4) == 0,
           "pipe: /dev/pipe round-trip");

    /* TarFS is read-only: opening app.wasm for write must fail. */
    int wfd = open("/app.wasm", O_WRONLY);
    if (wfd >= 0) close(wfd);
    tap_ok(wfd < 0, "TarFS: /app.wasm is read-only (write rejected)");

    /* Sandbox: a path escaping the root must not resolve to the host. */
    int efd = open("/../../../../etc/passwd", O_RDONLY);
    if (efd >= 0) close(efd);
    tap_ok(efd < 0, "sandbox: parent-traversal past root is denied");

    /* Our own control-plane state reads running. */
    tap_ok(read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
               strstr(buf, "running") != NULL,
           "control plane: supervisor state is running");
}

/* Launch the misbehaving wapp and assert the engine contains it. */
static void robustness_checks(void) {
    char buf[64];

    int cfg_ok = create_wapp(TRAPPER) && write_path(TRAPPER_CFG, LAUNCH_CFG) >= 0;
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
     * console (this very TAP stream proves the supervisor's stdout survived). */
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
    static const char *const wapps[] = { "stackbomb", "membomb" };
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

    int stopped = write_path(LOOPER_CTL, "stop") >= 0 &&
                  wait_state(LOOPER_STATE, 0);
    tap_ok(stopped, "lifecycle: control-plane stop terminates the looper");
}

/* Console backing: a wapp's stdio slots default when the launch config omits
 * them (stdin->null, stdout/stderr->log), and an explicit all-null console is
 * also valid. Either way the wapp must launch — a wapp with unwired stdio fds
 * fails to start. Reuses the looper (a clean long-runner), stopped after each. */
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
              start_wapp(LOOPER) &&
              wait_state(LOOPER_STATE, 1);
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
 * the stop *interrupted* the call promptly. cpuhog covers the other axis (a wapp
 * busy in the interpreter, where the terminate flag is checked per instruction);
 * these cover a wapp with no instruction boundaries to check because it is parked
 * in a host call. The stop must reach it anyway: the engine sets the terminate
 * flag and signals the worker to EINTR the call, so the interpreter regains
 * control and unwinds. Promptness is judged in a 2 s window — well under any
 * self-return — so it isolates the interrupt path; *alive_out reports whether the
 * supervisor survived. On a non-prompt result the wapp is reaped (bounded) so the
 * suite can continue and the failure is recorded rather than hanging. */
static int stop_interrupts(const char *name, int *alive_out) {
    char state[96], buf[64];
    wapp_node(state, sizeof(state), name, "state");

    launch(name);
    wait_state(state, 1);                /* running, inside the blocking call */
    stop_wapp(name);

    int prompt = 0;
    for (int i = 0; i < 2; i++) {
        sleep(1);
        if (read_path(state, buf, sizeof(buf)) > 0 &&
            !strstr(buf, "running") && !strstr(buf, "starting")) {
            prompt = 1;
            break;
        }
    }
    if (!prompt)
        wait_dead(name);                 /* bound a stuck slot so the suite goes on */

    *alive_out = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                 strstr(buf, "running") != NULL;

    printf("# %s: stop interrupts the blocked host call: %s\n",
           name, prompt ? "yes" : "no");
    fflush(stdout);
    return prompt;
}

/* blocker parks in a single timed sleep; the stop must interrupt that host call
 * (not wait it out) and the supervisor must survive. */
static void blocker_check(void) {
    int alive = 0;
    int prompt = stop_interrupts("blocker", &alive);
    tap_ok(prompt && alive,
           "robustness: stop interrupts a sleep-blocked wapp and reaps it promptly");
}

/* pblock parks in a read on an empty pipe that never completes on its own, so it
 * can only be ended by the stop interrupting the host call — the strict form of
 * the blocker check (no self-return to fall back on). */
static void ioblock_check(void) {
    int alive = 0;
    int prompt = stop_interrupts("pblock", &alive);
    tap_ok(prompt && alive,
           "robustness: stop interrupts an I/O-blocked wapp (read on an empty pipe)");
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
    tap_ok(rc < 0 &&
               read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
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
 * the abuse is contained: the wapp is reaped and the supervisor survives — never
 * a host crash. Whether the engine bounded the fd table below the wapp's probe
 * cap is reported as a diagnostic. */
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
    static const char *const bad[] = {
        "noappwasm", "badwasm", "truncated"
    };
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
        wait_dead(bad[i]);               /* never lingers running/starting */
        if (read_path(state, buf, sizeof(buf)) > 0 &&
            (strstr(buf, "running") || strstr(buf, "starting")))
            contained = 0;
    }

    int alive = read_path(SUPERVISOR_STATE, buf, sizeof(buf)) > 0 &&
                strstr(buf, "running") != NULL;
    tap_ok(contained && alive,
           "robustness: malformed images are rejected without crashing the engine");
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
    tap_ok(cycles == CRASH_CYCLES && alive,
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
#define READER_CFG_BODY \
    "{\"image\":\"duplex\"," \
    "\"console\":{\"in\":{\"name\":\"null\"}," \
    "\"out\":{\"name\":\"log\"},\"err\":{\"name\":\"log\"}}," \
    "\"envs\":[\"ROLE=reader\"]}"
#define WRITER_CFG_BODY "{\"image\":\"duplex\",\"envs\":[\"ROLE=writer\"]}"
static void pipe_duplex_check(void) {
    char buf[128];

    create_wapp("reader");
    create_wapp("writer");
    write_path(READER_CFG, READER_CFG_BODY); /* log console + ROLE=reader */
    write_path(WRITER_CFG, WRITER_CFG_BODY); /* ROLE=writer */
    start_wapp("reader");                    /* blocks reading /dev/pipe/duplex */
    start_wapp("writer");                    /* writes the payload to it */
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
               strstr(buf, "FOO=bar") != NULL &&
               strstr(buf, "BAZ=qux") != NULL,
           "argv/env: configured args and envs reach the launched wapp");

    int n = read_path(ARGENV_EXIT, buf, sizeof(buf));
    tap_ok(n > 0 && strstr(buf, "7") != NULL,
           "exit_code: a clean non-zero exit surfaces on the exit_code node");
}

int main(void) {
    /* Phases run in order. Announce each before running it (with a current/total
     * counter) so a long, mostly-sleeping check — the control-plane stop/wait
     * phases take seconds — is visibly progressing rather than looking hung
     * between `ok` lines. The table is the single source for the order and the
     * total, so adding a phase updates the counter automatically. */
    static const struct {
        const char *name;
        void (*run)(void);
    } phases[] = {
        { "positive_checks",    positive_checks    },
        { "pipe_duplex_check",  pipe_duplex_check  },
        { "robustness_checks",  robustness_checks  },
        { "containment_checks", containment_checks },
        { "cpuhog_check",       cpuhog_check       },
        { "console_checks",     console_checks     },
        { "argenv_check",       argenv_check       },
        { "lifecycle_checks",   lifecycle_checks   },
        { "blocker_check",      blocker_check      },
        { "ioblock_check",      ioblock_check      },
        { "edge_checks",        edge_checks        },
        { "sandbox_check",      sandbox_check      },
        { "resource_check",     resource_check     },
        { "malformed_check",    malformed_check    },
        { "crashloop_check",    crashloop_check    },
    };
    const int total = (int)(sizeof(phases) / sizeof(phases[0]));

    printf("# WANTED engine selftest\n");
    fflush(stdout);

    for (int i = 0; i < total; i++) {
        char label[64];
        snprintf(label, sizeof(label), "phase %d/%d: %s",
                 i + 1, total, phases[i].name);
        tap_diag(label);
        phases[i].run();
    }
    return tap_plan();
}
