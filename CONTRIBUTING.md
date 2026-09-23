# Contributing

This is a personal portfolio project. Issues and pull requests are welcome, and
so is "your README claims X and the code does Y" — that is the most useful kind
of report I can get.

## Running the checks locally

Everything CI runs, you can run. Nothing requires a cloud account:

```bash
pre-commit install          # then every commit runs the same gates as CI
pre-commit run --all-files  # or run them all now
```

Individually:

| What | Command |
|---|---|
| Python lint (incl. bandit security rules) | `ruff check .` |
| Terraform format + syntax | `terraform -chdir=terraform fmt -check && terraform -chdir=terraform validate` |
| Terraform lint | `tflint --chdir=terraform` |
| Terraform tests (mocked, no AWS) | `terraform -chdir=terraform test` |
| IaC security | `checkov` |
| Unit tests | `python -m pytest -q` |

`terraform init -backend=false` first — every check above is designed to run
without credentials, and none of them create anything or cost money.

## Conventions worth knowing

- **Every scanner exclusion carries its reason.** `ruff.toml`, `.checkov.yaml`
  and `spotbugs-exclude.xml` all explain each entry. A silent exclusion is
  indistinguishable from a bug nobody noticed, so if you add one, say why.
- **Third-party GitHub Actions are pinned to release tags, never `@main`.**
  `aquasecurity/trivy-action` was compromised in March 2026
  ([CVE-2026-33634](https://github.com/advisories/GHSA-69fq-xp46-6x23)) and a
  moving ref was the delivery path.
- **No formatter is imposed.** The style here is consistent and hand-tuned;
  `ruff format` / `clang-format` would rewrite large parts of it to no
  correctness benefit. The linters gate defects, not brace placement — the
  reasoning is written down in `ruff.toml` and `.clang-tidy`.
- **READMEs distinguish "validated" from "applied".** If you change what a repo
  actually proves, change the README's status line too. Claiming measured
  evidence that does not exist is the one thing I would rather not ship.
