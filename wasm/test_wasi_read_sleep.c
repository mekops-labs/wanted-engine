/* SPDX-License-Identifier: Apache-2.0 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    ssize_t ret;
    int f;
    char buf[100];

    for (int i = 0;; i++) {

        sprintf(buf, "%5d: ", i);
        write(STDOUT_FILENO, buf, 7);

        f = open("rom/file", O_RDONLY);
        if (f < 0) {
            perror("Open");
            return 1;
        }

        ret = read(f, buf, 100);
        if (ret < 0) {
            perror("Read");
            return 1;
        }

        close(f);

        ret = write(STDOUT_FILENO, buf, ret);
        if (ret < 0) {
            perror("Write");
            return 1;
        }

        sleep(3);
    }

    return 0;
}
