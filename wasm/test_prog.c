#include <stddef.h>
#include <stdint.h>

#include "wasm3_libc.h"

#define WASM_EXPORT __attribute__((used)) __attribute__((visibility ("default")))

char *prefix = "hello ";

int WASM_EXPORT entry(int id) {
    char msg[10] = {'h', 'e', 'l', 'l','o', ' ', 'x', '\0'};

    msg[6] = id + 0x30;

    if (id == 0) {
        print("[%d] sending: %s\n", id, msg);
        send(msg, 10);
    }
    else
    {
        recv(msg, 10);
        print("[%d] received: %s\n", id, msg);
    }

    memset(msg, 0, 10);

    unsigned r = get_rand() % 5;

    print("[%d] sleeping for %d\n", id, r);

    sleep(r);

    if (id != 0) {
        msg[0] = 'a';
        msg[1] = 'c';
        msg[2] = 'k';
        msg[3] = ' ';
        msg[4] = id;
        print("[%d] sending: %s\n", id, msg);
        send(msg, 10);
    }
    else
    {
        recv(msg, 10);
        print("[%d] received: %s\n", id, msg);
    }

    return 0;
}
