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
 *   entry_path - the initial .mvs file
 *   stddir     - standard library folder (used when importing from a package such as "std")
 *   nostd      - 1 = freestanding mode: package imports (such as "std") are forbidden
 *   had_error  - set to 1 if an error occurred
 */
Node *module_load(const char *entry_path, const char *stddir, int nostd, int *had_error);

#endif /* MVS_MODULE_H */
