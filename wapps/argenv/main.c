/* argenv — prints its argv and environ, then exits with a fixed non-zero code.
 *
 * The selftest launches it with known args and envs in its launch config and
 * asserts they appear in its captured log, proving WASI argv/environ
 * passthrough. The deliberate non-zero return proves a clean, application-level
 * exit code surfaces on the exit_code control-plane node — distinct from a
 * trap, which leaves exit_code at its sentinel and the state at failure. */

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
