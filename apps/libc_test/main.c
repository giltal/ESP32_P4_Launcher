/*
 * libc_test — PAPP linked with newlib. Exercises the SDK libc glue and the
 * absolute (medlow-style) relocations that newlib/libgcc bring in.
 * Returns the number of failed checks (0 = all passed).
 */
#include "papp/papp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "test.libc", .name = "libc test", .version = "0.1.0", .main_stack = 16384, .flags = PAPP_MF_HIDDEN);

static int s_failures;
static int s_ctor_ran;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

__attribute__((constructor)) static void ctor(void) { s_ctor_ran = 1234; }

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    printf("libc_test on %s\n", papp_board_id());
    CHECK(s_ctor_ran == 1234);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d %s %.3f %x", 42, "str", 3.14159, 0xBEEF);
    CHECK(strcmp(buf, "42 str 3.142 beef") == 0);
    printf("snprintf: '%s'\n", buf);

    CHECK(fabs(strtod("2.5e3", NULL) - 2500.0) < 1e-9);
    CHECK(fabs(sqrt(2.0) - 1.41421356) < 1e-6);
    CHECK(fabsf(sinf(0.5f) - 0.4794255f) < 1e-5f);
    CHECK(atoi("-17") == -17);

    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof(int), cmp_int);
    CHECK(v[0] == 1 && v[4] == 9);

    /* heap churn through the launcher MEM interface */
    void *blocks[64];
    for (int i = 0; i < 64; i++) { blocks[i] = malloc(16 + i * 900); CHECK(blocks[i] != NULL); memset(blocks[i], i, 16); }
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    for (int i = 0; i < 64; i += 2) blocks[i] = realloc(NULL, 32);
    for (int i = 0; i < 64; i++) free(blocks[i]);
    char *s = strdup("duplicated");
    CHECK(s && strcmp(s, "duplicated") == 0);
    free(s);

    /* stdio file round trip on the SD card */
    mkdir("/sd/data", 0);
    mkdir("/sd/data/test.libc", 0);
    FILE *f = fopen("/sd/data/test.libc/t.txt", "w");
    CHECK(f != NULL);
    if (f) { fprintf(f, "line %d\nsecond\n", 1); fclose(f); }
    f = fopen("/sd/data/test.libc/t.txt", "r");
    CHECK(f != NULL);
    if (f) {
        char line[32] = "";
        fgets(line, sizeof(line), f);
        CHECK(strcmp(line, "line 1\n") == 0);
        fseek(f, 0, SEEK_END);
        CHECK(ftell(f) == 14);
        fclose(f);
    }
    remove("/sd/data/test.libc/t.txt");

    printf("%s: %d failure(s)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures;
}
