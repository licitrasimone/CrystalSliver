---
task: plan post-ex UX improvements for CrystalSliver
slug: 20260610-200618_postex-ux-plan
effort: standard
phase: plan
progress: 0/8
mode: interactive
started: 2026-06-10T20:06:18Z
updated: 2026-06-10T20:10:00Z
---

## Context
Operator post-ex flow currently requires 3 separate commands, manual env export, and temp file creation for args. Goal: collapse to 1 command + 1 paste into Sliver, no env setup required.

### Risks
- sliver-client non-interactive mode uncertain — postex.sh explicitly avoids calling Sliver to sidestep this
- Multi-version CRYSTAL_PALACE_HOME per engagement: .crystalenv is per-operator, env override always wins

## Criteria
- [x] ISC-1: Plan identifies every friction point in current post-ex flow
- [x] ISC-2: Plan proposes single-command wrapper for DLL → PICO step
- [x] ISC-3: Plan proposes inline args support (no temp file required)
- [x] ISC-4: Plan proposes persistent config for CRYSTAL_PALACE_HOME
- [x] ISC-5: Plan proposes idempotent extension install check
- [x] ISC-6: Plan proposes named PICO output (not generic default)
- [x] ISC-7: Plan specifies what scripts to change vs add
- [x] ISC-8: Plan specifies backward-compatibility constraints

## Decisions
- postex.sh prints Sliver command rather than calling sliver-client directly — avoids non-interactive CLI uncertainty
- .crystalenv is git-ignored per-operator — avoids committing engagement-specific paths
- Named output default uses input DLL basename — human-readable, no timestamp churn during rapid iteration
