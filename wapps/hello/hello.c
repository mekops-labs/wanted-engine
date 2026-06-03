/* hello — a minimal WASI sample wapp used by the multi-wapp smoke test.
 *
 * It writes a recognizable marker to stdout, stays alive for a short, bounded
 * window so a concurrently issued `status` observes it alongside the running
 * supervisor, then exits cleanly so the engine drains and the CLI returns.
 *
 * The wapp talks to the outside world only through the VFS-backed stdio its
 * launch config grants it; it makes no other syscalls.
 */
#include <string.h>
#include <unistd.h>

#define MARKER_ALIVE "hello-wapp: alive\n"
#define MARKER_EXIT "hello-wapp: exit\n"
#define ALIVE_SECONDS 2

int main(void) {
    write(STDOUT_FILENO, MARKER_ALIVE, strlen(MARKER_ALIVE));

    sleep(ALIVE_SECONDS);

    write(STDOUT_FILENO, MARKER_EXIT, strlen(MARKER_EXIT));

    return 0;
}
