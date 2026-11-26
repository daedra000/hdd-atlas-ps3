// host mock: lv2 syscalls refuse (probe_test.c only)
#define lv2syscall2(n, a, b) long _r = -1; (void)(a); (void)(b)
#define lv2syscall3(n, a, b, c) long _r = -1; (void)(a); (void)(b); (void)(c)
#define return_to_user_prog(t) return (t)_r
