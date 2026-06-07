/*
 * Minimal PIE module — ON-DEVICE ABI-REJECTION TEST ONLY.
 *
 * scripts/tests/test_abi_reject.py rebuilds this at a MATCHING MODULE_ABI_VERSION
 * (DUMMY_ABI=1, accepted) and at MISMATCHED ones (DUMMY_ABI=2 / 0, rejected),
 * pushes the result over /modules/_selftest.bin, and asserts the core loader's
 * module-ABI gate accepts the former and rejects the latter (read back via the
 * ABI_SELFTEST hook in menu.c). It does nothing useful and is never built by
 * `all` or shipped in a release.
 */
#include "system/module.h"

MODULE_HEADER;

/* The loader's entry point (module.ld: ENTRY(init_module), _module_entry_offset
 * = init_module). The body is irrelevant: the test checks the loader's accept/
 * reject decision (vfs_read_module), not this code running. */
void init_module(void) { }
