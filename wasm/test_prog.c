#include <stddef.h>
#include <stdint.h>

#include "wasm3_libc.h"

#define WASM_EXPORT __attribute__((used)) __attribute__((visibility ("default")))

char *prefix = "hello ";

int WASM_EXPORT entry(int id) {
    char msg[10] = {'h', 'e', 'l', 'l','o', ' ', 'x', '\0'};

    msg[6] = id + 0x30;

    if (id == 0) {
        e_print("[%d] sending: %s\n", id, msg);
        w_send(msg, 10);
    }
    else
    {
        w_recv(msg, 10);
        e_print("[%d] received: %s\n", id, msg);
    }

    e_memset(msg, 0, 10);

    unsigned r = w_get_rand() % 5;

    e_print("[%d] sleeping for %d\n", id, r);

    w_sleep(r);

    if (id != 0) {
        msg[0] = 'a';
        msg[1] = 'c';
        msg[2] = 'k';
        msg[3] = ' ';
        msg[4] = id;
        e_print("[%d] sending: %s\n", id, msg);
        w_send(msg, 10);
    }
    else
    {
        w_recv(msg, 10);
        e_print("[%d] received: %s\n", id, msg);
    }

    return 0;
}
