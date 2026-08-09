/* SPDX-License-Identifier: Apache-2.0 */

/* looper — a well-behaved long-running test wapp. It loops forever, so the
 * supervisor can prove both that it runs concurrently and that a "stop" verb
 * terminates the in-flight call. The sleep keeps it off the CPU. */

#include <unistd.h>

int main(void) {
    for (;;)
        sleep(1);
    return 0;
}
