# third_party

External repositories under this directory are bootstrapped on demand and are
not tracked by the main repository.

- Edit `third_party/repos.lock` to update pinned dependency commits.
- Run `tools/bootstrap_third_party.sh` before configuring or building.
- Keep each dependency pinned to a commit for reproducible builds.
