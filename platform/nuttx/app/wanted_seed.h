/* SPDX-License-Identifier: Apache-2.0 */

#ifndef WANTED_SEED_H
#define WANTED_SEED_H

/* Installs each "<name>@<version>.wapp" under `dir` that the registry does not
 * hold. A freshly flashed board can then start its bundled wapps with no
 * network. Best-effort: an image that fails is left out. */
void WantedSeedRegistry(const char *dir);

#endif /* WANTED_SEED_H */
