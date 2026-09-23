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

## Repository layout

Every repo in this portfolio follows the same conventions. They are written down
here because a convention nobody states reads as an inconsistency.

| Directory | When it is used |
|---|---|
| `terraform/` | **Always** the infrastructure, in every repo that has any. Never `infra/`, `iac/` or `deploy/`. |
| `app/` | A **single containerised service** — one Dockerfile, one process. Matches the `WORKDIR /app` inside the image, so the import path is the same locally and in the container. |
| `src/` | **Multiple modules or functions** — several Lambdas, a library plus a CLI, a job plus its adapters. |
| `site/` | A **static site**'s served assets. Deliberately not `src/`: nothing here is compiled or imported, it is uploaded as-is. |
| `tests/` | Tests, except where a repo ships many independent functions — then each lives beside its own function (`src/<fn>/test_*.py`), because that is what gets packaged and deployed together. |
| `docs/` | Design notes, comparisons and runbooks. Repos with a richer operational story use named directories instead (`runbooks/`, `incidents/`, `architecture/`), which carry more meaning than a flat `docs/`. |

`app/` versus `src/` is the one that looks arbitrary and is not: **`app/` is one
service, `src/` is several things.** A container repo whose image sets
`WORKDIR /app` uses `app/` so `import main` resolves identically in a test and in
production. A repo with nine Lambdas has no single "app" to point at.

### Naming

Repo names describe **what the project demonstrates**, not where it runs — except
where the cloud *is* the demonstration (`azure-container-platform`,
`gcp-container-platform`), or where the service is inseparable from it
(`aws-landing-zone`, `aws-eks-platform`).
