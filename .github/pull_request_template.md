## What this changes

<!-- One or two sentences. What is different after this merges? -->

## Why

<!-- The reason, not the restatement. If it fixes something, what was the symptom? -->

## How it was verified

<!-- Say what you actually ran, not what you intended to. For this repo that
     usually means: which backend, how many threads, on what hardware, and
     whether the result was compared against the serial reference. -->

## CI gates on this repo

These run automatically; they are listed so a reviewer can tell at a glance what
is and is not covered by the green tick:

- [ ] `build-test`
- [ ] `sanitizers`
- [ ] `strict-warnings`
- [ ] `clang-tidy`
- [ ] `cppcheck`
- [ ] `codeql`

## If this touches performance

<!-- A benchmark number with no baseline is not a result. Include the before, the
     after, the machine, and the losing rows too — this repo's READMEs report
     the cases where a change did not help, and that stays true. -->

## Anything a reviewer should push back on

<!-- Shortcuts taken, things left out, decisions you are not sure about. Leave
     "nothing" only if that is true. -->
