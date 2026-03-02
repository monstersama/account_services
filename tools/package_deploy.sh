#!/usr/bin/env bash
set -euo pipefail

print_usage() {
    cat <<'EOF'
Usage:
  tools/package_deploy.sh [options]

Options:
  --build-dir <dir>       CMake build directory (default: <repo>/build)
  --build-type <type>     CMake build type (default: Release)
  --version <ver>         Package version (default: parse ACCT_API_VERSION from CMakeLists.txt)
  --prefix-base <dir>     Install prefix root on target host (default: /opt/account_services)
  --output-dir <dir>      Output directory for tar.gz (default: <repo>)
  --pkgroot <dir>         Staging directory used by DESTDIR (default: <repo>/pkgroot)
  --package-name <name>   Package file basename (default: account_services)
  --no-db                 Exclude *.db files under data/
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${repo_root}/build"
build_type="Release"
version=""
prefix_base="/opt/account_services"
output_dir="${repo_root}"
pkgroot="${repo_root}/pkgroot"
package_name="account_services"
include_db=1
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
        --no-db)
            include_db=0
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

install_prefix="${prefix_base%/}/${version}"
package_arch="$(uname -m)"
package_file="${output_dir%/}/${package_name}-${version}-linux-${package_arch}.tar.gz"

if [[ "${skip_configure}" -eq 0 ]]; then
    cmake -S "${repo_root}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}"
fi

if [[ "${skip_build}" -eq 0 ]]; then
    cmake --build "${build_dir}" -j
fi

rm -rf "${pkgroot}"
DESTDIR="${pkgroot}" cmake --install "${build_dir}" --prefix "${install_prefix}"

install_root="${pkgroot}${install_prefix}"
mkdir -p "${install_root}/config" "${install_root}/data"

if [[ -d "${repo_root}/config" ]]; then
    cp -a "${repo_root}/config/." "${install_root}/config/"
fi

if [[ -d "${repo_root}/data" ]]; then
    cp -a "${repo_root}/data/." "${install_root}/data/"
    if [[ "${include_db}" -eq 0 ]]; then
        find "${install_root}/data" -type f -name '*.db' -delete
    fi
fi

mkdir -p "${install_root}/bin"
runtime_bins=(
    "src/acct_service_main"
    "gateway/acct_broker_gateway_main"
    "tools/full_chain_e2e/full_chain_observer"
    "tools/full_chain_e2e/order_submit_cli"
)
installed_bin_count=0
for rel_bin in "${runtime_bins[@]}"; do
    src_bin="${build_dir%/}/${rel_bin}"
    if [[ -f "${src_bin}" ]]; then
        cp -a "${src_bin}" "${install_root}/bin/"
        installed_bin_count=$((installed_bin_count + 1))
    fi
done
if [[ "${installed_bin_count}" -eq 0 ]]; then
    rmdir "${install_root}/bin" 2>/dev/null || true
fi

mkdir -p "${output_dir}"
tar -C "${pkgroot}" -czf "${package_file}" .

cat <<EOF
Package created:
  ${package_file}

Install prefix in package:
  ${install_prefix}
EOF
