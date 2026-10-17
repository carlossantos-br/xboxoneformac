#!/bin/bash
# xboxoneformac — installer.
#   ./install.sh                      set up Warframe for Mac (warframeformac)
#   ./install.sh /path/to/Game.app    set up another Sikarugir/Wineskin wrapper
#   ./install.sh --game Game.exe ...  games to watch (replaces the default list)
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
die()  { printf '\n\033[31mError:\033[0m %s\n' "$*" >&2; exit 1; }

valid_game() { [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._\ -]*\.[Ee][Xx][Ee]$ ]]; }

# Runs a command as root: sudo in a terminal, the macOS password dialog otherwise.
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
  do shell script cmd with prompt "xboxoneformac needs your password to install the controller service." with administrator privileges
end run
OSA
  fi
}

# ---------------------------------------------------------------- root part
if [ "${1:-}" = "--system" ]; then
  [ "$(id -u)" -eq 0 ] || die "--system must run as root."
  TARGET_UID="$2"; shift 2
  [[ "$TARGET_UID" =~ ^[0-9]+$ ]] && [ "$TARGET_UID" -ge 500 ] || die "invalid user id"
  ARGS=""
  for game in "$@"; do
    valid_game "$game" || die "invalid game name: $game"
    ARGS="$ARGS		<string>--game</string>
		<string>$game</string>
"
  done
  /bin/launchctl bootout "system/$LABEL" 2>/dev/null || true
  # Recreated by the service for the current user.
  /bin/rm -f "$DEST/state.bin" "$DEST/rumble.bin"
  /usr/bin/install -d -o root -g wheel -m 0755 "$DEST" "$DEST/wine"
  for f in xboxone-bridge "$SDL" libusb-1.0.0.dylib; do
    /usr/bin/install -o root -g wheel -m 0755 "$HERE/prebuilt/bridge/$f" "$DEST/$f"
  done
  /usr/bin/install -o root -g wheel -m 0644 "$HERE/prebuilt/wine/$SDL" "$DEST/wine/$SDL"
  /usr/bin/xattr -cr "$DEST" 2>/dev/null || true
  /usr/bin/codesign --verify "$DEST/xboxone-bridge"
  cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$DEST/xboxone-bridge</string>
		<string>--run</string>
		<string>$TARGET_UID</string>
$ARGS	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>10</integer>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>WorkingDirectory</key>
	<string>$DEST</string>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
PL
  /usr/sbin/chown root:wheel "$PLIST"; /bin/chmod 0644 "$PLIST"
  /usr/bin/plutil -lint "$PLIST" >/dev/null
  # Right after a bootout launchd can refuse for a moment (error 5): retry.
  for attempt in 1 2 3 4 5; do
    /bin/launchctl bootstrap system "$PLIST" 2>/dev/null && exit 0
    sleep 1
  done
  die "launchd did not start the service."
fi

# ---------------------------------------------------------------- user part
GAMES=(); WRAPPERS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --game) [ $# -ge 2 ] || die "--game needs a name"; valid_game "$2" || die "invalid game name: $2"; GAMES+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,5p' "$0" | sed 's/^# //'; exit 0 ;;
    *) WRAPPERS+=("${1%/}"); shift ;;
  esac
done
[ ${#GAMES[@]} -gt 0 ] || GAMES=("Warframe.x64.exe")
if [ ${#WRAPPERS[@]} -eq 0 ]; then
  [ -d "$WARFRAMEFORMAC" ] || die "Warframe for Mac was not found. Install it first (https://github.com/carlossantos-br/warframeformac) or pass the path of your Wine wrapper app."
  WRAPPERS=("$WARFRAMEFORMAC")
  # Setting up Warframe for Mac always watches Warframe.
  [[ " ${GAMES[*]} " == *" Warframe.x64.exe "* ]] || GAMES+=("Warframe.x64.exe")
fi

say "Checking"
[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = "1" ] || die "Apple Silicon is required."
( cd "$HERE/prebuilt" && shasum -a 256 -c -s SHA256SUMS ) || die "prebuilt files do not match SHA256SUMS."
for w in "${WRAPPERS[@]}"; do
  [ -f "$w/Contents/Frameworks/$SDL" ] || die "$w is not a Sikarugir/Wineskin wrapper (no Contents/Frameworks/$SDL)."
  lipo -archs "$w/Contents/Frameworks/$SDL" | grep -q x86_64 || die "$w uses an unsupported SDL build."
  info "wrapper: $w"
done
info "games: ${GAMES[*]}"

say "Installing the controller service (asks for your password)"
as_root /bin/bash "$HERE/install.sh" --system "$(id -u)" "${GAMES[@]}" || die "the controller service was not installed."
info "service running: $LABEL"

say "Adding controller support to Wine"
for w in "${WRAPPERS[@]}"; do
  fw="$w/Contents/Frameworks"
  # Keep the wrapper's own SDL once, so uninstall.sh can put it back.
  if [ ! -f "$fw/$SDL.orig" ] && ! grep -q XBOXONEFORMAC_STATE_FILE "$fw/$SDL"; then
    cp -p "$fw/$SDL" "$fw/$SDL.orig"
  fi
  cp "$HERE/prebuilt/wine/$SDL" "$fw/$SDL.new"
  xattr -c "$fw/$SDL.new" 2>/dev/null || true
  mv -f "$fw/$SDL.new" "$fw/$SDL"
  info "done: $(basename "$w")"
done

say "Ready!"
info "Plug the controller in by USB and start the game (restart it if it was open)."
info "Log: $LOG"
