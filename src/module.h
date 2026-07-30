/*
 * module.h - module system for cross-file imports
 *
 * Takes an entry file and resolves all import statements recursively,
 * merging functions/variables/externs from every module into one ND_PROGRAM for codegen.
 *
 * Supported import forms:
 *   import { factorial } from "./mathlib.mvs";  // import from a file (names used directly)
 *   import { io } from "std";                    // import from a package as a namespace (io.out)
 */
#ifndef MVS_MODULE_H
#define MVS_MODULE_H

#include "ast.h"

/* Load the entry file and resolve all imports; returns an ND_PROGRAM merging every module.
 *   entry_path  - the initial .mvs file
 *   stddir      - standard library folder (used when importing from the "std" package)
 *   coredir     - core library folder (the "core" package: pure MVS, no CRT/OS,
 *                 so it stays importable under --nostd)
 *   nostd       - 1 = freestanding mode: "std" imports are forbidden ("core" is fine)
 *   target_os   - current target OS ("windows" or "linux"); items whose
 *                 @compile(target_os = ...) does not match are dropped here
 *   target_arch - current target arch ("x86_64" or "aarch64"); same filtering
 *   had_error   - set to 1 if an error occurred
 */
Node *module_load(const char *entry_path, const char *stddir, const char *coredir, int nostd,
                  const char *target_os, const char *target_arch, int *had_error);

#endif /* MVS_MODULE_H */
