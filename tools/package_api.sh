#!/usr/bin/env bash
set -euo pipefail

print_usage() {
    cat <<'EOF'
Usage:
  tools/package_api.sh [options]

Options:
  --build-dir <dir>       CMake build directory (default: <repo>/build)
  --build-type <type>     CMake build type (default: Release)
  --version <ver>         Package version (default: parse ACCT_API_VERSION from CMakeLists.txt)
  --prefix-base <dir>     Install prefix root inside package (default: /opt/account_services_api)
  --output-dir <dir>      Output directory for tar.gz (default: <repo>)
  --pkgroot <dir>         Staging directory used by DESTDIR (default: <repo>/pkg)
  --package-name <name>   Package file basename (default: account_services_api)
  --source-date-epoch <s> Reproducible timestamp (unix seconds, default: git commit time)
  --native-arch           Enable -march=native for Release build (non-reproducible across machines)
  --skip-configure        Skip cmake -S/-B configure step
  --skip-build            Skip cmake --build step
  -h, --help              Show this help
EOF
}

infer_version() {
    local cmake_file="${repo_root}/CMakeLists.txt"
    local raw_version=""
    local major=""
    local minor=""
    local patch=""

    raw_version="$(
        sed -nE 's/^[[:space:]]*set\(ACCT_API_VERSION[[:space:]]*"([^"]+)"\).*/\1/p' "${cmake_file}" \
            | head -n 1
    )"
    if [[ -n "${raw_version}" && "${raw_version}" != *'$'* && "${raw_version}" != *'{'* ]]; then
        printf '%s\n' "${raw_version}"
        return 0
    fi

    major="$(
        sed -nE 's/^[[:space:]]*set\(ACCT_API_VERSION_MAJOR[[:space:]]+([0-9]+)\).*/\1/p' "${cmake_file}" \
            | head -n 1
    )"
    minor="$(
        sed -nE 's/^[[:space:]]*set\(ACCT_API_VERSION_MINOR[[:space:]]+([0-9]+)\).*/\1/p' "${cmake_file}" \
            | head -n 1
    )"
    patch="$(
        sed -nE 's/^[[:space:]]*set\(ACCT_API_VERSION_PATCH[[:space:]]+([0-9]+)\).*/\1/p' "${cmake_file}" \
            | head -n 1
    )"
    if [[ -n "${major}" && -n "${minor}" && -n "${patch}" ]]; then
        printf '%s.%s.%s\n' "${major}" "${minor}" "${patch}"
        return 0
    fi

    if [[ -f "${build_dir}/generated/version.h" ]]; then
        raw_version="$(
            sed -nE 's/^[[:space:]]*#define[[:space:]]+ACCT_API_VERSION[[:space:]]+"([^"]+)".*/\1/p' "${build_dir}/generated/version.h" \
                | head -n 1
        )"
        if [[ -n "${raw_version}" ]]; then
            printf '%s\n' "${raw_version}"
            return 0
        fi
    fi

    return 1
}

infer_source_date_epoch() {
    local inferred=""
    if command -v git >/dev/null 2>&1 && git -C "${repo_root}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        inferred="$(git -C "${repo_root}" log -1 --format=%ct 2>/dev/null || true)"
    fi
    if [[ "${inferred}" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "${inferred}"
        return 0
    fi
    printf '0\n'
}

create_reproducible_tarball() {
    local src_dir="$1"
    local out_file="$2"
    local epoch="$3"
    (
        export LC_ALL=C
        export TZ=UTC
        cd "${src_dir}"
        tar \
            --sort=name \
            --format=posix \
            --mtime="@${epoch}" \
            --owner=0 \
            --group=0 \
            --numeric-owner \
            --pax-option=delete=atime,delete=ctime \
            -cf - . | gzip -n > "${out_file}"
    )
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${repo_root}/build"
build_type="Release"
version=""
prefix_base="/opt/account_services_api"
output_dir="${repo_root}"
pkgroot="${repo_root}/pkg"
package_name="account_services_api"
source_date_epoch="${SOURCE_DATE_EPOCH:-}"
enable_native_arch=0
skip_configure=0
skip_build=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --build-type)
            build_type="$2"
            shift 2
            ;;
        --version)
            version="$2"
            shift 2
            ;;
        --prefix-base)
            prefix_base="$2"
            shift 2
            ;;
        --output-dir)
            output_dir="$2"
            shift 2
            ;;
        --pkgroot)
            pkgroot="$2"
            shift 2
            ;;
        --package-name)
            package_name="$2"
            shift 2
            ;;
        --source-date-epoch)
            source_date_epoch="$2"
            shift 2
            ;;
        --native-arch)
            enable_native_arch=1
            shift
            ;;
        --skip-configure)
            skip_configure=1
            shift
            ;;
        --skip-build)
            skip_build=1
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            print_usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${version}" ]]; then
    version="$(infer_version || true)"
fi

if [[ -z "${version}" ]]; then
    echo "Failed to infer version from ${repo_root}/CMakeLists.txt; pass --version <ver>." >&2
    exit 1
fi

if [[ -z "${source_date_epoch}" ]]; then
    source_date_epoch="$(infer_source_date_epoch)"
fi
if ! [[ "${source_date_epoch}" =~ ^[0-9]+$ ]]; then
    echo "invalid --source-date-epoch: ${source_date_epoch}" >&2
    exit 2
fi

native_arch_cmake=OFF
if [[ "${enable_native_arch}" -eq 1 ]]; then
    native_arch_cmake=ON
fi

install_prefix="${prefix_base%/}/${version}"
package_arch="$(uname -m)"
package_file="${output_dir%/}/${package_name}-${version}-linux-${package_arch}.tar.gz"

if [[ "${skip_configure}" -eq 0 ]]; then
    cmake -S "${repo_root}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DACCT_ENABLE_NATIVE_ARCH="${native_arch_cmake}"
fi

if [[ "${skip_build}" -eq 0 ]]; then
    cmake --build "${build_dir}" -j
fi

rm -rf "${pkgroot}"
DESTDIR="${pkgroot}" cmake --install "${build_dir}" --prefix "${install_prefix}"

install_root="${pkgroot}${install_prefix}"
if [[ ! -d "${install_root}/lib" || ! -d "${install_root}/include" ]]; then
    echo "API install artifacts are incomplete under ${install_root}; expected lib/ and include/." >&2
    exit 1
fi

mkdir -p "${output_dir}"
create_reproducible_tarball "${pkgroot}" "${package_file}" "${source_date_epoch}"

cat <<EOF
API package created:
  ${package_file}

Install prefix in package:
  ${install_prefix}
SOURCE_DATE_EPOCH:
  ${source_date_epoch}
ACCT_ENABLE_NATIVE_ARCH:
  ${native_arch_cmake}
EOF
