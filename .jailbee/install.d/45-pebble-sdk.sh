#!/bin/bash
# Install the standalone Pebble toolchain during `jailbee base build`.
# Env: CONTAINER_USER, JAILBEE_USER_HOME
# Requires golden.stacks.node and the QEMU libraries in config.yaml.
set -euo pipefail

su - "${CONTAINER_USER}" <<'DEV_EOF'
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
command -v npm >/dev/null 2>&1 || {
  printf '%s\n' 'Pebble SDK needs golden.stacks.node in .jailbee/config.yaml' >&2
  exit 1
}

if ! command -v uv >/dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 -LsSf https://astral.sh/uv/install.sh | sh
fi
# pebble-tool requires Python <3.14; do not use Ubuntu's system interpreter.
uv tool install 'pebble-tool==5.1.2' --python 3.13
mkdir -p "$HOME/.pebble-sdk"
touch "$HOME/.pebble-sdk/NO_TRACKING"
# Building the image implies acceptance of the Pebble SDK terms (see README).
pebble sdk install 4.44 </dev/null

# pebble-tool requests SDL even without a display. Use null video/audio so
# installs, logs and screenshots work over the emulator's serial protocol.
mkdir -p "$HOME/.local/bin"
cat > "$HOME/.local/bin/qemu-pebble-headless" <<'WRAP_EOF'
#!/bin/bash
set -euo pipefail
args=()
skip_next=0
for arg in "$@"; do
  if [ "$skip_next" = 1 ]; then
    skip_next=0
    continue
  fi
  case "$arg" in
    -display|-audio) skip_next=1; continue ;;
  esac
  args+=("$arg")
done
exec qemu-pebble "${args[@]}" -display none -audio driver=none,id=audio0
WRAP_EOF
chmod +x "$HOME/.local/bin/qemu-pebble-headless"
DEV_EOF
