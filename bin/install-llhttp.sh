#!/usr/bin/env bash
set -euo pipefail
# Install the same llhttp release as the runtime image into a caller-owned prefix.
# Ubuntu runners do not consistently provide the libllhttp-dev/pkg-config package.
prefix=${1:?Supply an absolute installation directory}
case "$prefix" in /*) ;; *) echo 'Use an absolute prefix' >&2; exit 2;; esac
source_dir=$(mktemp -d)
trap 'rm -rf "$source_dir"' EXIT
git clone --depth 1 --branch v6.0.7 https://github.com/nodejs/llhttp.git "$source_dir/source"
cat > "$source_dir/source/libllhttp.pc.in" <<'EOF'
prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

Name: llhttp
Description: llhttp
Version: @PROJECT_VERSION@
Libs: -L${libdir} -lllhttp
Cflags: -I${includedir}
EOF
cmake -S "$source_dir/source" -B "$source_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$source_dir/build" --parallel 3
cmake --install "$source_dir/build"
