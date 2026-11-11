/* reColossus (from reKing): the app's entry from the PS5 loader: the C runtime's environment, the static constructors, then main.
 * From reUndr's ps5/src/native_start.c (MIT, bystritskiy). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <xmmintrin.h>

typedef void (*Init)(void);
extern void _init_env(void *);
extern Init __init_array_start[], __init_array_end[];
extern Init __fini_array_start[], __fini_array_end[];
extern int main(int, char **);

void _init(void) { for (Init *p = __init_array_start; p < __init_array_end; p++) (*p)(); }
void _fini(void) { for (Init *p = __fini_array_end; p > __fini_array_start;) (*--p)(); }

__attribute__((noreturn)) void _start(void *arguments, Init teardown) {
    _mm_setcsr(0x1f80);  /* SSE: round to nearest, exceptions masked */
    __asm__ volatile("fninit");
    _init_env(arguments);
    if (teardown) atexit(teardown);
    atexit(_fini);
    _init();
    exit(main(*(int *)arguments, (char **)((uint8_t *)arguments + 8)));
}
