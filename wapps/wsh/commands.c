#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "commands.h"

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

        printf("%c%-20s   %4d   %8lld   %8llx   %8llu\n", type, de->d_name, de->d_type, s.st_size, s.st_dev, s.st_ino);
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

    if (args[1] == NULL) {
        fputs("write: expected filename\n", stderr);
        return 1;
    }

    if (args[2] == NULL) {
        fputs("write: expected content\n", stderr);
        return 1;
    }

    fd = open(args[1], O_WRONLY);
    if(fd < 0) {
        fputs("write: ", stderr);
        perror(args[1]);
        return 1;
    }

    cnt = write(fd, args[2], strlen(args[2]));
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
