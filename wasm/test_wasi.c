#include <unistd.h>

int main(void) {
    ssize_t ret;
    const char *hello = "hello\n";

    ret = write(STDOUT_FILENO, hello, 8);

    if (ret < 0)
        return -1;

    return 0;
}
