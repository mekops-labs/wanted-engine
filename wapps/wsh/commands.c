/* SPDX-License-Identifier: Apache-2.0 */

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "commands.h"

/* Control-plane paths used by the start/stop/status commands. Edit these to
 * match where the WANTED driver is mounted in this wapp's namespace; the
 * %s fields take the wapp name (and, for WANTED_WAPP_FIELD, the node name). */
#define WANTED_BASE        "/dev/wanted"
#define WANTED_CTL         WANTED_BASE "/ctl"          /* root create/launch */
#define WANTED_WAPPS       WANTED_BASE "/wapps"        /* wapp enumeration   */
#define WANTED_WAPP_CTL    WANTED_WAPPS "/%s/ctl"      /* per-wapp verb node */
#define WANTED_WAPP_STATE  WANTED_WAPPS "/%s/state"    /* per-wapp state     */
#define WANTED_WAPP_FIELD  WANTED_WAPPS "/%s/%s"       /* per-wapp read node */

/*
  Function Declarations for builtin shell commands:
 */
int wsh_cd(char **args);
int wsh_help(char **args);
int wsh_exit(char **args);
int wsh_ls(char **args);
int wsh_pwd(char **args);
int wsh_cat(char **args);
int wsh_write(char **args);
int wsh_cp(char **args);
int wsh_rm(char **args);
int wsh_start(char **args);
int wsh_stop(char **args);
int wsh_status(char **args);
int wsh_poweroff(char **args);
int wsh_reboot(char **args);

/*
  List of builtin commands, followed by their corresponding functions.
 */
cmd_t cmds[] = {
    { "help", wsh_help },
    { "exit", wsh_exit },
    { "cd", wsh_cd },
    { "ls", wsh_ls },
    { "pwd", wsh_pwd },
    { "cat", wsh_cat },
    { "write", wsh_write },
    { "cp", wsh_cp },
    { "rm", wsh_rm },
    { "start", wsh_start },
    { "stop", wsh_stop },
    { "status", wsh_status },
    { "poweroff", wsh_poweroff },
    { "reboot", wsh_reboot },
};

int wsh_num_cmds() {
  return sizeof(cmds) / sizeof(cmds[0]);
}

static void fprint_int(int num, FILE *f){
    int a = num;

    if (num < 0) {
       fputc('-', f);
       num = -num;
    }

    if (num > 9) fprint_int(num/10, f);

    fputc('0'+ (a%10), f);
}

/*
  Builtin function implementations.
*/

/**
   @brief Bultin command: change directory.
   @param args List of args.  args[0] is "cd".  args[1] is the directory.
   @return Always returns 1, to continue executing.
 */
int wsh_cd(char **args)
{
    if (args[1] == NULL) {
        fputs("wsh: expected argument to \"cd\"\n", stderr);
    } else {
        if (chdir(args[1]) != 0) {
        perror("wsh");
        }
    }
    return 1;
}

/**
   @brief Builtin command: print help.
   @param args List of args.  Not examined.
   @return Always returns 1, to continue executing.
 */
int wsh_help(char **args)
{
    int i;
    puts("Following commands are available:");

    for (i = 0; i < wsh_num_cmds(); i++) {
        putchar('\t');
        puts(cmds[i].cmd);
    }

    return 1;
}

/**
   @brief Builtin command: exit.
   @param args List of args.  Not examined.
   @return Always returns 0, to terminate execution.
 */
int wsh_exit(char **args)
{
    return 0;
}

int wsh_ls(char **args)
{
    char cwd[300];
    char abspath[600];
    char *currentDir;
    struct stat s = { 0 };

    if (args[1] == NULL) {
        currentDir = getcwd(cwd, sizeof(cwd));
    } else {
        /* Resolve to absolute so lstat paths below are always correct. */
        if (args[1][0] == '/') {
            currentDir = args[1];
        } else {
            char *base = getcwd(cwd, sizeof(cwd));
            snprintf(abspath, sizeof(abspath), "%s/%s", base ? base : "", args[1]);
            currentDir = abspath;
        }
    }

    DIR *dr;
    dr = opendir(currentDir);
    struct dirent *de;

    if (dr == NULL) {
        fputs("ls: ", stderr);
        perror(currentDir);
        return 1;
    }

    printf("[%-20s] [%4s] [%8s] [%8s] [%8s]\n", "Name", "Type", "Size", "Dev", "Ino");
    while ((de = readdir(dr)) != NULL) {
        char type;
        switch (de->d_type) {
        case 3:
            type = '/';
            break;
        case 5:
        case 6:
            type = '@';
            break;
        default:
            type = ' ';
            break;
        }

        char entry_path[700];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", currentDir, de->d_name);
        if (lstat(entry_path, &s) < 0) {
            perror(args[0]);
            return 1;
        }

        printf("%c%-20s   %4d   %8lld   %-4.4s   %8llu\n", type, de->d_name, de->d_type, s.st_size, (char*)&s.st_dev, s.st_ino);
    }

    closedir(dr);

  return 1;
}

int wsh_pwd(char **argd)
{
    char cwd[300];
    char *currentDir;

    currentDir = getcwd(cwd, sizeof(cwd));
    puts(currentDir);

    return 1;
}

int wsh_cat(char **args)
{
    int fd_read;
    char line_buf[1024];
    char *buf = NULL;
    ssize_t count;

    if (args[1] == NULL) {
        fputs("cat: expected filename", stderr);
        return 1;
    }

    fd_read = open(args[1], O_RDONLY);
    if(fd_read < 0) {
        fputs("cat: ", stderr);
        perror(args[1]);
    } else {
        while((count = read(fd_read, line_buf, 1024)) > 0) {
            write(STDOUT_FILENO, line_buf, count);
        }

        if (count < 0) {
            fputs("cat: ", stderr);
            perror(args[1]);
        }
    }

    close(fd_read);
    return 1;
}

int wsh_write(char **args)
{
    int fd, cnt;
    char content[256];
    size_t pos = 0;

    if (args[1] == NULL) {
        fputs("write: expected filename\n", stderr);
        return 1;
    }

    if (args[2] == NULL) {
        fputs("write: expected content\n", stderr);
        return 1;
    }

    /* Join the remaining tokens with single spaces so multi-word payloads
     * (e.g. "start app1") survive the shell's whitespace tokenizer. */
    for (int i = 2; args[i] != NULL && pos < sizeof(content) - 1; i++) {
        if (i > 2 && pos < sizeof(content) - 1)
            content[pos++] = ' ';
        size_t len = strlen(args[i]);
        if (pos + len > sizeof(content) - 1)
            len = sizeof(content) - 1 - pos;
        memcpy(content + pos, args[i], len);
        pos += len;
    }
    content[pos] = '\0';

    fd = open(args[1], O_WRONLY);
    if(fd < 0) {
        fputs("write: ", stderr);
        perror(args[1]);
        return 1;
    }

    cnt = write(fd, content, pos);
    if (cnt < 0) {
        perror(args[1]);
    }

    fputs("write: written ", stderr);
    fprint_int(cnt, stderr);
    fputc('\n', stderr);

    close(fd);

    return 1;
}

int wsh_cp(char **args)
{
    int src, dest;
    unsigned char buf[4096];
    ssize_t cnt;

    if (args[1] == NULL || args[2] == NULL) {
        fputs("cp: need source and dest file\n", stderr);
        return 1;
    }

    src = open(args[1], O_RDONLY);
    if (src < 0) {
        fputs("cp: ", stderr);
        perror(args[1]);
        return 1;
    }

    dest = open(args[2], O_CREAT | O_WRONLY, 00777);
    if (dest < 0) {
        fputs("cp: ", stderr);
        perror(args[2]);
        return 1;
    }

    while((cnt = read(src, buf, 4096))) {
        if (cnt < 0) {
            fputs("cp: ", stderr);
            perror(args[1]);
            break;
        }
        cnt = write(dest, buf, cnt);

        if (cnt < 0) {
            fputs("cp: ", stderr);
            perror(args[2]);
            break;
        }
    }

    close(dest);
    close(src);

    return 1;
}

int wsh_rm(char **args)
{
    if (args[1] == NULL) {
        fputs("rm: need a filename\n", stderr);
        return 1;
    }

    int r = unlink(args[1]);
    if (r < 0) {
        fputs("rm: ", stderr);
        perror(args[1]);
    }

    return 1;
}

/* Read a control-plane node into `buf` (NUL-terminated, trailing newline
 * stripped). Returns the byte count, or -1 on error. */
static int wanted_read(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = '\0';
    return (int)n;
}

/* Write a verb/payload to a control-plane node. Returns bytes written or -1. */
static int wanted_write(const char *path, const char *payload)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;

    int n = write(fd, payload, strlen(payload));
    close(fd);
    return n;
}

/**
   @brief Create-and-launch a wapp via the root control node.
   @param args args[1] is the wapp name.
 */
int wsh_start(char **args)
{
    char payload[64];

    if (args[1] == NULL) {
        fputs("start: need a wapp name\n", stderr);
        return 1;
    }

    snprintf(payload, sizeof(payload), "start %s", args[1]);
    if (wanted_write(WANTED_CTL, payload) < 0) {
        fputs("start: ", stderr);
        perror(WANTED_CTL);
    }
    return 1;
}

/**
   @brief Stop a wapp via its per-wapp control node.
   @param args args[1] is the wapp name.
 */
int wsh_stop(char **args)
{
    char path[128];

    if (args[1] == NULL) {
        fputs("stop: need a wapp name\n", stderr);
        return 1;
    }

    snprintf(path, sizeof(path), WANTED_WAPP_CTL, args[1]);
    if (wanted_write(path, "stop") < 0) {
        fputs("stop: ", stderr);
        perror(path);
    }
    return 1;
}

/**
   @brief Report wapp state. With a name, print that wapp's state/version/id;
          without one, list every wapp under wapps/ with its state.
 */
int wsh_status(char **args)
{
    char path[160];
    char val[64];

    if (args[1] != NULL) {
        static const char *fields[] = { "state", "version", "id" };
        printf("%s:\n", args[1]);
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), WANTED_WAPP_FIELD, args[1], fields[i]);
            if (wanted_read(path, val, sizeof(val)) >= 0)
                printf("  %-8s %s\n", fields[i], val);
            else
                printf("  %-8s <unavailable>\n", fields[i]);
        }
        return 1;
    }

    DIR *dr = opendir(WANTED_WAPPS);
    if (dr == NULL) {
        fputs("status: ", stderr);
        perror(WANTED_WAPPS);
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(dr)) != NULL) {
        snprintf(path, sizeof(path), WANTED_WAPP_STATE, de->d_name);
        if (wanted_read(path, val, sizeof(val)) >= 0)
            printf("%-15s %s\n", de->d_name, val);
    }
    closedir(dr);
    return 1;
}

/* Stop every running child wapp (everything but the supervisor itself) via the
 * control plane, so a poweroff/reboot drains cleanly. Best-effort: a failed
 * stop is ignored — the engine tears remaining wapps down when the run loop
 * ends regardless. */
static void wsh_stop_children(void)
{
    DIR *dr = opendir(WANTED_WAPPS);
    if (dr == NULL)
        return;

    struct dirent *de;
    char path[160];
    while ((de = readdir(dr)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
            strcmp(de->d_name, "supervisor") == 0)
            continue;
        snprintf(path, sizeof(path), WANTED_WAPP_CTL, de->d_name);
        wanted_write(path, "stop");
    }
    closedir(dr);
}

/**
   @brief Drain child wapps, then ask the engine to power off. Returns 0 so the
          shell loop exits; the engine stops without respawning the supervisor.
 */
int wsh_poweroff(char **args)
{
    (void)args;
    wsh_stop_children();
    if (wanted_write(WANTED_CTL, "poweroff") < 0) {
        fputs("poweroff: ", stderr);
        perror(WANTED_CTL);
        return 1;
    }
    return 0;
}

/**
   @brief Drain child wapps, then ask the engine to reboot (host re-exec / board
          reset). Returns 0 so the shell loop exits; this wsh instance is not
          respawned.
 */
int wsh_reboot(char **args)
{
    (void)args;
    wsh_stop_children();
    if (wanted_write(WANTED_CTL, "reboot") < 0) {
        fputs("reboot: ", stderr);
        perror(WANTED_CTL);
        return 1;
    }
    return 0;
}
