#!/usr/bin/env bash
# Checks that the tools and hardware needed to build and run the DON port are available.
# Prints one line per check: "OK <name> <version>" or "MISSING <name>: <reason>".
# Exits with 0 if every check passes, 1 if anything is missing. Installs nothing.
# Extra tools can be required with CHECK_ENV_EXTRA="tool1 tool2".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
failures=0

ok() {
  echo "OK $1 $2"
}

missing() {
  echo "MISSING $1: $2"
  failures=$((failures + 1))
}

has_tool() {
  command -v "$1" >/dev/null 2>&1
}

# First line of a tool's version output that contains a digit.
first_digit_line() {
  grep -m1 '[0-9]' || true
}

cmake_ver() {
  cmake --version 2>&1 | grep -m1 -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' || true
}

java_ver() {
  java -version 2>&1 | grep -m1 -oE '"[^"]+"' | tr -d '"' || true
}

gxx_ver() {
  g++ --version 2>&1 | first_digit_line
}

# check_tool <name> [version function]
check_tool() {
  local name="$1"
  local ver=""
  if ! has_tool "$name"; then
    missing "$name" "not found in PATH"
    return
  fi
  if [ "$#" -gt 1 ]; then
    ver=$("$2") || ver=""
  fi
  ok "$name" "${ver:-present}"
}

# 1. Required tools in PATH.
check_tool git
check_tool cmake cmake_ver
check_tool ninja
check_tool g++ gxx_ver
check_tool 7z
check_tool uv
check_tool java java_ver
check_tool xvfb-run
check_tool pkg-config
check_tool vulkaninfo
check_tool docker

# 2. cmake >= 3.21.
if has_tool cmake; then
  cmake_v=$(cmake_ver)
  if [ -z "$cmake_v" ]; then
    missing "cmake>=3.21" "could not read the cmake version"
  elif [ "$(printf '%s\n%s\n' "3.21" "$cmake_v" | sort -V | head -n 1)" = "3.21" ]; then
    ok "cmake>=3.21" "$cmake_v"
  else
    missing "cmake>=3.21" "found cmake $cmake_v, need 3.21 or newer"
  fi
else
  missing "cmake>=3.21" "cmake not found in PATH"
fi

# 3. java major version >= 21.
if has_tool java; then
  java_v=$(java_ver)
  java_major=${java_v%%.*}
  if [ "$java_major" = "1" ]; then
    # Old version scheme: "1.8.0_392" means Java 8.
    java_major=$(echo "$java_v" | cut -d. -f2)
  fi
  java_major=${java_major%%[!0-9]*}
  if [ -z "$java_major" ]; then
    missing "java>=21" "could not read the java version"
  elif [ "$java_major" -ge 21 ]; then
    ok "java>=21" "$java_v"
  else
    missing "java>=21" "found java $java_v, need 21 or newer"
  fi
else
  missing "java>=21" "java not found in PATH"
fi

# 4. g++ accepts -std=c++20 -march=x86-64-v3 and compiles a trivial program.
if has_tool g++; then
  probe_dir=$(mktemp -d)
  printf 'int main(){}\n' > "$probe_dir/probe.cpp"
  if g++ -std=c++20 -march=x86-64-v3 "$probe_dir/probe.cpp" -o "$probe_dir/probe" \
      > "$probe_dir/log" 2>&1; then
    ok "g++-std=c++20-march=x86-64-v3" "$(gxx_ver)"
  else
    missing "g++-std=c++20-march=x86-64-v3" "trivial program does not compile: $(head -n 3 "$probe_dir/log" | tr '\n' ' ')"
  fi
  rm -rf "$probe_dir"
else
  missing "g++-std=c++20-march=x86-64-v3" "g++ not found in PATH"
fi

# 5. The CPU supports AVX2.
if [ -r /proc/cpuinfo ] && grep -qw avx2 /proc/cpuinfo; then
  ok "cpu-avx2" "present"
else
  missing "cpu-avx2" "avx2 is not listed in /proc/cpuinfo"
fi

# 6. At least 20 GiB free on the file system of the repository.
need_kb=$((20 * 1024 * 1024))
avail_kb=$(df -Pk "$ROOT" | awk 'NR==2 {print $4}') || avail_kb=""
if ! [[ "$avail_kb" =~ ^[0-9]+$ ]]; then
  missing "disk-free-20GiB" "could not read free space of $ROOT"
elif [ "$avail_kb" -ge "$need_kb" ]; then
  ok "disk-free-20GiB" "$((avail_kb / 1024 / 1024)) GiB free"
else
  missing "disk-free-20GiB" "$((avail_kb / 1024 / 1024)) GiB free, need 20 GiB"
fi

# 7. The docker daemon answers without sudo.
if has_tool docker && docker info >/dev/null 2>&1; then
  ok "docker-daemon" "$(docker info --format '{{.ServerVersion}}' 2>/dev/null || echo present)"
else
  missing "docker-daemon" "'docker info' fails without sudo (daemon not running or no permission)"
fi

# 8. Extra tools requested by the caller, separated by spaces.
if [ -n "${CHECK_ENV_EXTRA:-}" ]; then
  read -ra extra_tools <<< "$CHECK_ENV_EXTRA"
  for extra in "${extra_tools[@]}"; do
    check_tool "$extra"
  done
fi

echo
if [ "$failures" -eq 0 ]; then
  echo "All checks passed."
  exit 0
fi
echo "$failures check(s) failed."
exit 1
