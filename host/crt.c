// Program entry for 32-bit bionic, since Termux has no armv7 crtbegin. Same as the NDK's crtbegin.c: hand the raw stack to __libc_init.
typedef struct { void (**preinit_array)(void); void (**init_array)(void); void (**fini_array)(void); } structors_array_t;
extern void __libc_init(void *raw_args, void (*onexit)(void), int (*main)(int, char **, char **), structors_array_t const *structors);
extern int main(int, char **, char **);
__attribute__((used)) static void _start_main(void *raw_args) {
    structors_array_t a = {0, 0, 0};
    __libc_init(raw_args, 0, main, &a);
}
__asm__(".global _start\n_start:\n mov r0, sp\n b _start_main\n");
