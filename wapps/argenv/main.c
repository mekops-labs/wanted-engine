/* SPDX-License-Identifier: Apache-2.0 */

/* argenv — prints its argv and environ, then exits with a fixed non-zero code.
 * The selftest asserts they reach the captured log, proving WASI passthrough,
 * and that a clean exit code surfaces on the exit_code control node. */

#include <stdio.h>

extern char **environ;

#define EXIT_CODE 7

int main(int argc, char **argv) {
    for (int i = 0; i < argc; i++)
        printf("arg %d=%s\n", i, argv[i]);
    for (char **e = environ; *e != NULL; e++)
        printf("env %s\n", *e);
    fflush(stdout); /* -> log console */
    return EXIT_CODE;
}
