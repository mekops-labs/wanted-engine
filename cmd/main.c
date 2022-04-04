#include <stdio.h>
#include <wanted.h>

/*

- load config
- WantedStart

*/

wantedConfig_t cfg = {
    .nWapps = 2,
    .wappsToRun = {"abc", "xyz"},
};

int main(int argc, char* argv[])
{
    WantedStart(cfg);

    printf("\nAll wapps ended, done...\n");

    return 0;
}
