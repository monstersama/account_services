#!/usr/bin/env bash
set -euo pipefail

# Print the CLI usage for bootstrapping repositories declared in repos.lock.
print_usage() {
    cat <<'EOF'
Usage:
  tools/bootstrap_third_party.sh [options]

Options:
  --repo-root <dir>   Override repository root (default: auto-detect from script path)
  --manifest <file>   Override dependency manifest (default: <repo>/third_party/repos.lock)
  -h, --help          Show this help
EOF
}

# Emit a consistently formatted info log line.
log_info() {
    printf '[INFO] %s\n' "$*"
}

# Emit a consistently formatted warning log line.
log_warn() {
    printf '[WARN] %s\n' "$*" >&2
}

# Emit a consistently formatted error log line.
log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

# Ensure a required external command is available before syncing repositories.
require_command() {
    local command_name="$1"

    if ! command -v "${command_name}" >/dev/null 2>&1; then
        log_error "missing required command: ${command_name}"
        exit 1
    fi
}

# Remove leading and trailing whitespace from a manifest field.
trim_field() {
    local value="$1"

    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s\n' "${value}"
}

# Return success when the repository has tracked or untracked local changes.
repo_is_dirty() {
    local repo_dir="$1"

    if ! git -C "${repo_dir}" diff --quiet --ignore-submodules --; then
        return 0
    fi
    if ! git -C "${repo_dir}" diff --cached --quiet --ignore-submodules --; then
        return 0
    fi
    if [[ -n "$(git -C "${repo_dir}" ls-files --others --exclude-standard)" ]]; then
        return 0
    fi
    return 1
}

# Return success when HEAD is detached instead of attached to a branch ref.
repo_is_detached() {
    local repo_dir="$1"

    if git -C "${repo_dir}" symbolic-ref -q HEAD >/dev/null 2>&1; then
        return 1
    fi
    return 0
}

# Reject malformed manifest entries before attempting to clone or checkout.
validate_entry() {
    local line_number="$1"
    local name="$2"
    local relative_path="$3"
    local repo_url="$4"
    local default_branch="$5"
    local commit="$6"

    if [[ -z "${name}" || -z "${relative_path}" || -z "${repo_url}" || -z "${default_branch}" || -z "${commit}" ]]; then
        log_error "manifest line ${line_number} is incomplete"
        exit 1
    fi

    case "${relative_path}" in
        /*|../*|*/../*|..)
            log_error "manifest line ${line_number} contains an invalid relative path: ${relative_path}"
            exit 1
            ;;
    esac

    if ! [[ "${commit}" =~ ^[0-9a-f]{40}$ ]]; then
        log_error "manifest line ${line_number} contains a non-commit revision: ${commit}"
        exit 1
    fi
}

# Sync a dependency repository to the commit pinned in repos.lock.
sync_dependency() {
    local name="$1"
    local relative_path="$2"
    local repo_url="$3"
    local default_branch="$4"
    local commit="$5"
    local target_dir="${repo_root}/${relative_path}"
    local current_head=""
    local actual_head=""

    if [[ -e "${target_dir}" && ! -d "${target_dir}" ]]; then
        log_error "${name}: ${relative_path} exists but is not a directory"
        exit 1
    fi

    mkdir -p "$(dirname "${target_dir}")"

    if [[ ! -e "${target_dir}" ]]; then
        # Clone without pinning to a branch so a renamed default branch does not break bootstrap.
        log_info "${name}: cloning ${repo_url} into ${relative_path}"
        git clone "${repo_url}" "${target_dir}"
    fi

    if [[ ! -d "${target_dir}/.git" ]]; then
        log_error "${name}: ${relative_path} exists but is not a Git repository"
        exit 1
    fi

    current_head="$(git -C "${target_dir}" rev-parse HEAD 2>/dev/null || true)"
    if [[ "${current_head}" == "${commit}" ]]; then
        if repo_is_dirty "${target_dir}"; then
            log_error "${name}: local changes detected under ${relative_path}; clean the repository before bootstrapping"
            exit 1
        fi

        if repo_is_detached "${target_dir}"; then
            log_info "${name}: already at pinned commit ${commit}"
            return 0
        fi

        # Detach HEAD even when the branch tip already matches the pinned revision.
        log_info "${name}: detaching HEAD at pinned commit ${commit}"
        git -C "${target_dir}" checkout --detach "${commit}" >/dev/null
        actual_head="$(git -C "${target_dir}" rev-parse HEAD)"
        if [[ "${actual_head}" != "${commit}" ]]; then
            log_error "${name}: expected ${commit}, but repository resolved to ${actual_head}"
            exit 1
        fi
        log_info "${name}: synced to ${actual_head}"
        return 0
    fi

    if repo_is_dirty "${target_dir}"; then
        log_error "${name}: local changes detected under ${relative_path}; clean the repository before bootstrapping"
        exit 1
    fi

    if ! git -C "${target_dir}" remote get-url origin >/dev/null 2>&1; then
        log_error "${name}: ${relative_path} is missing the origin remote"
        exit 1
    fi

    # Refresh refs from origin without depending on the remote branch name.
    log_info "${name}: fetching from origin"
    git -C "${target_dir}" fetch --tags --prune origin

    if ! git -C "${target_dir}" cat-file -e "${commit}^{commit}" 2>/dev/null; then
        log_error "${name}: pinned commit ${commit} was not found in origin (manifest branch hint: ${default_branch})"
        exit 1
    fi

    # Use a detached HEAD so the worktree exactly matches the pinned revision.
    log_info "${name}: checking out pinned commit ${commit}"
    git -C "${target_dir}" checkout --detach "${commit}" >/dev/null

    actual_head="$(git -C "${target_dir}" rev-parse HEAD)"
    if [[ "${actual_head}" != "${commit}" ]]; then
        log_error "${name}: expected ${commit}, but repository resolved to ${actual_head}"
        exit 1
    fi

    log_info "${name}: synced to ${actual_head}"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
manifest_path=""
manifest_path_explicit=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo-root)
            repo_root="$2"
            shift 2
            ;;
        --manifest)
            manifest_path="$2"
            manifest_path_explicit=1
            shift 2
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            log_error "unknown option: $1"
            print_usage >&2
            exit 2
            ;;
    esac
done

require_command git

if [[ "${manifest_path_explicit}" -eq 0 ]]; then
    manifest_path="${repo_root}/third_party/repos.lock"
fi

if [[ ! -d "${repo_root}" ]]; then
    log_error "repository root does not exist: ${repo_root}"
    exit 1
fi

if [[ ! -f "${manifest_path}" ]]; then
    log_error "dependency manifest not found: ${manifest_path}"
    exit 1
fi

line_number=0
processed_count=0

while IFS= read -r raw_line || [[ -n "${raw_line}" ]]; do
    line_number=$((line_number + 1))

    trimmed_line="$(trim_field "${raw_line}")"
    if [[ -z "${trimmed_line}" || "${trimmed_line:0:1}" == "#" ]]; then
        continue
    fi

    IFS='|' read -r raw_name raw_relative_path raw_repo_url raw_default_branch raw_commit extra_field <<< "${raw_line}"

    if [[ -n "${extra_field:-}" ]]; then
        log_error "manifest line ${line_number} contains too many fields"
        exit 1
    fi

    name="$(trim_field "${raw_name}")"
    relative_path="$(trim_field "${raw_relative_path}")"
    repo_url="$(trim_field "${raw_repo_url}")"
    default_branch="$(trim_field "${raw_default_branch}")"
    commit="$(trim_field "${raw_commit}")"

    validate_entry "${line_number}" "${name}" "${relative_path}" "${repo_url}" "${default_branch}" "${commit}"
    sync_dependency "${name}" "${relative_path}" "${repo_url}" "${default_branch}" "${commit}"
    processed_count=$((processed_count + 1))
done < "${manifest_path}"

if [[ "${processed_count}" -eq 0 ]]; then
    log_error "no dependency entries found in ${manifest_path}"
    exit 1
fi

log_info "bootstrapped ${processed_count} dependenc(ies) from ${manifest_path}"
