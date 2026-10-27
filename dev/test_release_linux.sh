#!/bin/sh

set -eu

artifacts=$1
logs=$2

chmod +x "$artifacts/st" "$artifacts/pt"
for binary in st pt; do
    path="$artifacts/$binary"
    file -L "$path"
    file -L "$path" | grep -q 'ELF 64-bit LSB executable, x86-64'
    if readelf -lW "$path" | grep -q INTERP; then
        echo "release binary has an interpreter: $binary" >&2
        exit 1
    fi
    if readelf -dW "$path" | grep -q NEEDED; then
        echo "release binary has dynamic dependencies: $binary" >&2
        exit 1
    fi
done
"$artifacts/st" -version | grep -q '^Shitty '
"$artifacts/pt" -version | grep -q '^Pretty '

runtime="$logs/xdg-runtime"
display_file="$logs/xvfb-display"
weston_log="$logs/weston.log"
vulkan_log="$logs/vulkan-wayland.log"

export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=wayland-shitty-test
export VK_DRIVER_FILES="$(find /usr/share/vulkan/icd.d -name 'lvp_icd*.json' -print -quit)"
# SoLo 11 uses the ld.so-style debug switches; DL_STUB_DEBUG was the
# older standalone dlfcn wrapper's variable and is ignored by the new bridge.
export LD_DEBUG=dlfcn
export LD_DEBUG_OUTPUT="$logs/dlfcn.log"

test -n "$VK_DRIVER_FILES"
install -d -m 700 "$runtime"

xvfb_pid=
weston_pid=
cleanup() {
    if test -n "$weston_pid"; then
        kill "$weston_pid" 2>/dev/null || true
        wait "$weston_pid" 2>/dev/null || true
    fi
    if test -n "$xvfb_pid"; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

Xvfb \
    -displayfd 3 \
    -screen 0 1024x768x24 \
    -nolisten tcp \
    3>"$display_file" &
xvfb_pid=$!

attempt=0
while test "$attempt" -lt 100 && ! test -s "$display_file"; do
    kill -0 "$xvfb_pid" 2>/dev/null
    sleep 0.1
    attempt=$((attempt + 1))
done
test -s "$display_file"
export DISPLAY=":$(cat "$display_file")"

weston \
    --backend=x11-backend.so \
    --renderer=pixman \
    --shell=kiosk-shell.so \
    --no-config \
    --socket="$WAYLAND_DISPLAY" \
    --idle-time=0 \
    --log="$weston_log" &
weston_pid=$!

attempt=0
while test "$attempt" -lt 100 && ! test -S "$runtime/$WAYLAND_DISPLAY"; do
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        cat "$weston_log"
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
test -S "$runtime/$WAYLAND_DISPLAY"

if ! WAYLAND_DEBUG=client timeout 30 "$artifacts/st" \
    -vulkanInfo \
    -geometry 20x5 \
    -e /bin/sh -c 'printf "Vulkan Wayland smoke\n"; sleep 2' \
    >"$vulkan_log" 2>&1; then
    cat "$vulkan_log"
    exit 1
fi
cat "$vulkan_log"

grep -q '^Vulkan device: llvmpipe' "$vulkan_log"
grep -q '^Vulkan presentation:' "$vulkan_log"
grep -Eq 'wl_surface[#@][0-9]+\.attach\(wl_buffer[#@][0-9]+' "$vulkan_log"
grep -q 'try open handle libwayland-client.so.0' "$DL_STUB_DEBUG"
grep -q 'found handle wayland-client' "$DL_STUB_DEBUG"
