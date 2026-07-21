---
title: "Troubleshooting"
date: 2026-07-16T00:00:00+01:00
weight: 80
toc: true
description: "Common engine failure modes — symptom, cause, and fix — from a wapp that won't start to fd exhaustion and undersized stacks."
---

Symptom → cause → fix for the failures that come up most. Errnos link to the
[Error Reference](error-reference.md) for their engine-context meaning. Building
with `-DWANTED_DEBUG_TRACES=ON` (or `WANTED_DEBUG_TRACES=ON` for the NuttX app
build) adds a `DEBUG_TRACE` line at every decision point and is the first tool
for anything below.

## A wapp fails to start

**Symptom.** `start` succeeds but the instance never reaches `RUNNING`; its
`state` node reports `FAILURE` (or briefly `STARTING`, then a dead state).

**Causes & fixes.**

- **No config was written.** A bare `create` then `start` is rejected — the
  start gate requires either a launch config or an explicit `start <image>`.
  Write the config node (even `{}`) before `start`, or start with an image ref.
- **The image is not in the registry.** The loader can't resolve
  `config.image` (defaulting to the instance name). Confirm the image is staged
  in the registry with the exact name/tag.
- **A malformed image.** Missing `app.wasm`, invalid wasm, or a truncated
  archive → the loader rejects it and the slot ends `FAILURE`. Rebuild the image.
- **Initial memory over the cap.** An image whose *declared initial* linear
  memory exceeds `WASM_MAX_MEMORY_PAGES` is refused at load. Shrink the wapp or
  raise the cap via a profile.
- **A launch-config resource was rejected.** A mount under a reserved namespace
  (`/dev`, `/net`, `/proc`), a relative `platform` `src=`, or a bad socket
  address fails the install with [`EINVAL`](error-reference.md). Check the
  config against the [Control Plane Reference](control-plane-reference.md).

## `ENOENT` opening `/dev/wanted/…`

**Symptom.** Opening a control node returns [`ENOENT`](error-reference.md).

**Cause.** Namespaces are existence-gated — the per-wapp nodes
(`wapps/<name>/config`, `/ctl`, `/state`) exist only after `create <name>`.
Reaching for them first is the usual mistake.

**Fix.** Follow the lifecycle: `create <name>` on the root `ctl` first, then
write `config`, then `start` on the per-wapp `ctl`. Also confirm the wapp was
granted the `wanted` control-plane driver in its launch config — without it,
`/dev/wanted` is not even mounted (an observer wapp deliberately lacks it).

## fd exhaustion

**Symptom.** `open`/`socket` starts returning [`EMFILE`](error-reference.md); a
wapp misbehaves after running a while.

**Cause.** The wapp's per-instance fd table is bounded (`MAX_OPENED_FILES`); a
wapp that opens without closing exhausts it. This is contained to the wapp — the
engine and other wapps survive.

**Fix.** Close descriptors the wapp is done with. If the workload legitimately
needs more, raise the limit via a profile and rebuild — but audit for a leak
first.

## Stack or heap too small

**Symptom.** A wapp traps or the worker thread faults the moment real wasm runs;
on an RTOS, a hard fault instead of a clean trap.

**Causes & fixes.**

- **Native worker stack.** The WAMR classic interpreter is recursive and runs on
  the platform worker thread; an RTOS default (~2 KB on NuttX) overflows
  immediately. The platform sets `WASM_WORKER_STACK_SIZE` (64 KiB default) — a
  new port must size this explicitly rather than take the OS default.
- **WASM operand stack.** `WASM_STACK_SIZE` bounds the interpreter's operand
  stack (host memory, outside linear memory). A deeply recursive wapp needs more.
- **Linear memory.** The wapp's own C aux stack and libc heap live in linear
  memory, capped at `WASM_MAX_MEMORY_PAGES`. A wapp that needs a bigger heap hits
  [`ENOMEM`](error-reference.md) on `malloc`/`memory.grow`. Raise the cap via a
  profile (`constrained`/`small`/`big`) and rebuild.

All three are Kconfig symbols (`just menuconfig`); changing one resizes static
structures, so re-audit after any change.

## Volume/preopen read returns nothing on the NuttX sim

**Symptom.** A file written through a `volume`/`platform` preopen reads back
empty on the NuttX sim.

**Cause & fix.** This was a historical hostfs issue that no longer reproduces —
the sim reads back correctly. If you see it, first rule out a **stale sim
build**: the NuttX incremental build can run a stale engine ELF. Force a clean
rebuild with `NUTTX_CLEAN=1` and re-run before investigating further.

## Changes don't take effect on the NuttX sim

**Symptom.** An engine source edit doesn't change sim behaviour.

**Cause & fix.** The incremental NuttX build may keep a stale engine object;
rebuild with `NUTTX_CLEAN=1 just build` (or `nuttx-selftest`) to force a
full reconfigure. A clean sim TLS build also re-unpacks mbedTLS and needs
`unzip` in the build image.

## See also

- [Error Reference](error-reference.md) — the errno each symptom maps to.
- [Testing Guide](testing-guide.md) — running the unit, smoke, and selftest suites.
- [Platform Guide](platform-guide.md) — target-specific build and flash flows.
