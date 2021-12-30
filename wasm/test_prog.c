#include <stddef.h>
#include <stdint.h>


#define WASM_IMPORT(MODULE,NAME) __attribute__((import_module(MODULE))) __attribute__((import_name(NAME)))

WASM_IMPORT("*", "sum") int sum(int, int);
WASM_IMPORT("*", "ext_memcpy") int ext_memcpy(void*, const void*, size_t);

#define WASM_EXPORT __attribute__((used)) __attribute__((visibility ("default")))

int WASM_EXPORT test(int32_t arg1, int32_t arg2)
{
    int x = arg1 + arg2;
    int y = arg1 - arg2;
    return sum(x, y) / 2;
}

int64_t WASM_EXPORT test_memcpy(void)
{
    int64_t x = 0;
    int32_t low = 0x01234567;
    int32_t high = 0x89abcdef;
    ext_memcpy(&x, &low, 4);
    ext_memcpy(((uint8_t*)&x) + 4, &high, 4);
    return x;
}

int WASM_EXPORT _start(int argc, char* argv[]) {
    test(10,20);
    test_memcpy();

    return 0;
}
