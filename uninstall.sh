#!/bin/bash
# xboxoneformac — removes the controller service and puts back each wrapper's
# original SDL library. Pass extra wrapper paths if you set up other games.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/XboxOneForMac"
LABEL="org.xboxoneformac.bridge"
PLIST="/Library/LaunchDaemons/$LABEL.plist"
LOG="/Library/Logs/XboxOneForMac.log"
WARFRAMEFORMAC="${HOME:-}/Library/Application Support/WarframeForMac/Warframe.app"
SDL="libSDL2-2.0.0.dylib"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }

as_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"
  elif [ -t 0 ]; then sudo "$@"
  else
    /usr/bin/osascript - "$@" <<'OSA'
on run argv
  set cmd to ""
  repeat with a in argv
    set cmd to cmd & quoted form of (a as text) & " "
  end repeat
  do shell script cmd with prompt "xboxoneformac needs your password to remove the controller service." with administrator privileges
end run
OSA
  fi
}

if [ "${1:-}" = "--system" ]; then
  /bin/launchctl bootout "system/$LABEL" 2>/dev/null || true
  # Only the files this project installs; the folder is removed if left empty.
  /bin/rm -f "$PLIST" "$LOG" "$DEST/xboxone-bridge" "$DEST/$SDL" "$DEST/libusb-1.0.0.dylib" \
             "$DEST/state.bin" "$DEST/rumble.bin" "$DEST/wine/$SDL"
  /bin/rmdir "$DEST/wine" "$DEST" 2>/dev/null || true
  exit 0
fi

say "Restoring Wine's own SDL"
WRAPPERS=("$@")
[ -d "$WARFRAMEFORMAC" ] && WRAPPERS+=("$WARFRAMEFORMAC")
for w in "${WRAPPERS[@]+"${WRAPPERS[@]}"}"; do
  fw="${w%/}/Contents/Frameworks"
  [ -f "$fw/$SDL" ] && grep -q XBOXONEFORMAC_STATE_FILE "$fw/$SDL" || continue
  if [ -f "$fw/$SDL.orig" ]; then
    mv -f "$fw/$SDL.orig" "$fw/$SDL"
    info "restored: $(basename "${w%/}")"
  else
    info "$(basename "${w%/}"): original not found — reinstall that wrapper to get it back."
  fi
done

say "Removing the controller service (asks for your password)"
if [ -e "$PLIST" ] || [ -d "$DEST" ]; then
  as_root /bin/bash "$HERE/uninstall.sh" --system
fi
[ ! -e "$PLIST" ] && [ ! -d "$DEST" ] && info "removed" || info "some files were left in $DEST"
