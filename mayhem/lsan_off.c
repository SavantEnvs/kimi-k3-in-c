/* Leak detection off for every fuzz/standalone binary (ASan and UBSan stay on). */
int __lsan_is_turned_off(void) { return 1; }
