# Security policy

## Reporting a vulnerability

Please **do not open a public issue** for a security problem.

Use GitHub's [private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing/privately-reporting-a-security-vulnerability)
on this repository (Security → Report a vulnerability). If that is unavailable,
email the address on the maintainer's GitHub profile.

I'll acknowledge within 7 days. This is a personal project maintained in my own
time — I can't offer a commercial SLA, and I'd rather say that than imply one.

## Scope

This is a **learning and portfolio project**, not a library anyone should depend
on in production. It is concurrent C++ handling untrusted input in places, so
memory-safety and data-race findings are genuinely interesting and welcome.

Particularly in scope:

- data races or torn reads the test suite's sanitiser runs did not catch
- memory errors (use-after-free, buffer overrun) reachable from parsed input
- unbounded allocation driven by a remote peer

## What this repo already does

CI builds and runs the test suite under **ThreadSanitizer and
AddressSanitizer**. A finding that reproduces under either is a bug, not a
tuning question, and is treated as such.

Third-party GitHub Actions are pinned to release tags, never to `@master` or
`@main` — `aquasecurity/trivy-action` was compromised in March 2026
([CVE-2026-33634](https://github.com/advisories/GHSA-69fq-xp46-6x23)) and a
moving ref was the delivery path.
