/* Pustaka "game" tiruan (seperti libGTASA.so): mengimpor dep_value dari pustaka lain. */
extern int dep_value(void);
extern int dep_other(void);
int (*volatile g_ptr)(void) = dep_value;             /* relokasi data (ABS/GLOB_DAT) */
int target_call(void)     { return dep_value(); }    /* lewat PLT (JUMP_SLOT)        */
int target_call_ptr(void) { return g_ptr(); }
int target_other(void)    { return dep_other(); }
