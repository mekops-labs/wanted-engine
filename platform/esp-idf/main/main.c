//
//  Wasm3 - high performance WebAssembly interpreter written in C.
//
//  Copyright © 2019 Steven Massey, Volodymyr Shymanskyy.
//  All rights reserved.
//

#include "esp_system.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <wanted.h>


#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return; }
#define STR(...) #__VA_ARGS__

char* cfg = STR(
{
    "system": {
        "defaultWapps": [
            "a",
            "bb",
            "ccc"
        ]
    }
}
);


void app_main(void)
{
    printf("\nWanted on " CONFIG_IDF_TARGET ", build " __DATE__ " " __TIME__ "\n");

    clock_t start = clock();
    int ret = WantedStart(cfg, strlen(cfg));
    if (ret < 0) {
        errno = -ret;
        perror("wanted error");
    }

    printf("\nAll wapps ended, done...\n");

    clock_t end = clock();

    printf("Elapsed: %ld ms\n", (end - start)*1000 / CLOCKS_PER_SEC);

    sleep(3);
    printf("Restarting...\n\n\n");
    esp_restart();
}
