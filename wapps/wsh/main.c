/* SPDX-License-Identifier: Apache-2.0 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "commands.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

/**
   @brief Execute shell built-in or launch program.
   @param args Null terminated list of arguments.
   @return 1 if the shell should continue running, 0 if it should terminate
 */
int wsh_execute(char **args)
{
    int i;

    if (args[0] == NULL) {
        // An empty command was entered.
        return 1;
    }

    for (i = 0; i < wsh_num_cmds(); i++) {
        if (strcmp(args[0], cmds[i].cmd) == 0) {
            return (*cmds[i].func)(args);
        }
    }

    fputs("wsh: ", stderr);
    fputs(args[0], stderr);
    fputs(" - command not found!\n", stderr);
    return 1;
}

/**
   @brief Read a line of input from stdin.
   @return The line from stdin.
 */
char *wsh_read_line(void)
{
    char *line = NULL;
    size_t bufsize = 0; // have getline allocate a buffer for us
    if (getline(&line, &bufsize, stdin) == -1) {
        if (feof(stdin)) {
            exit(EXIT_SUCCESS);  // We received an EOF
        } else  {
            perror("wsh: getline\n");
            exit(EXIT_FAILURE);
        }
    }
    return line;
}


#define LSH_TOK_BUFSIZE 64
#define LSH_TOK_DELIM " \t\r\n\a"
/**
   @brief Split a line into tokens (very naively).
   @param line The line.
   @return Null-terminated array of tokens.
 */
char **wsh_split_line(char *line)
{
    int bufsize = LSH_TOK_BUFSIZE, position = 0;
    char **tokens = malloc(bufsize * sizeof(char*));
    char *token, **tokens_backup;

    if (!tokens) {
        fputs("wsh: allocation error\n", stderr);
        exit(EXIT_FAILURE);
    }

    token = strtok(line, LSH_TOK_DELIM);
    while (token != NULL) {
        tokens[position] = token;
        position++;

        if (position >= bufsize) {
            bufsize += LSH_TOK_BUFSIZE;
            tokens_backup = tokens;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                free(tokens_backup);
                fputs("wsh: allocation error\n", stderr);
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, LSH_TOK_DELIM);
    }
    tokens[position] = NULL;
    return tokens;
}

/**
   @brief Loop getting input and executing it.
 */
void wsh_loop(void)
{
    char *line;
    char **args;
    int status;

    do {
        fputs("> ", stdout);
        fflush(stdout);
        line = wsh_read_line();
        args = wsh_split_line(line);
        status = wsh_execute(args);

        free(line);
        free(args);
    } while (status);
}

/**
   @brief Main entry point.
   @param argc Argument count.
   @param argv Argument vector.
   @return status code
 */
int main()
{
    // Load config files, if any.
    fputs("Wsh v ", stderr);
    fputs(VERSION, stderr);
    fputc('\n', stderr);

    // Run command loop.
    wsh_loop();

    fputs("Exiting Wsh. Bye\n", stderr);

    // Perform any shutdown/cleanup.

    return EXIT_SUCCESS;
}
