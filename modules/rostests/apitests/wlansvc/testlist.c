#define STANDALONE
#include <apitest.h>

extern void func_crypto(void);

const struct test winetest_testlist[] =
{
    { "crypto", func_crypto },
    { 0, 0 }
};
