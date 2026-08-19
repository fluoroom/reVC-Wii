# Shared by scripts/wii-*. Sourced, not executed.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SD_ROOT="$ROOT/sd"
SD_APP="$SD_ROOT/apps/reVC"
DOLPHIN_USER="$ROOT/.dolphin"
SD_IMAGE="$DOLPHIN_USER/Load/WiiSD.raw"
BOOT_DOL="$SD_APP/boot.dol"

DEV_BUILD_DIR="$ROOT/build-wii-dev"
RELEASE_BUILD_DIR="$ROOT/build-wii-release"

if [[ -z "${DEVKITPRO:-}" ]]; then
	export DEVKITPRO=/opt/devkitpro
	export DEVKITPPC="$DEVKITPRO/devkitPPC"
	export PATH="$DEVKITPRO/tools/bin:$PATH"
fi

wii_die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

wii_require_data() {
	[[ -f "$SD_APP/data/gta_vc.dat" ]] || wii_die \
		"missing $SD_APP/data/gta_vc.dat — put the PC Vice City files in sd/apps/reVC/"
}

wii_cmake() {
	local wrapper="$DEVKITPRO/portlibs/wii/bin/powerpc-eabi-cmake"
	if [[ -x "$wrapper" ]]; then
		"$wrapper" "$@"
		return
	fi
	local toolchain="$DEVKITPRO/cmake/Wii.cmake"
	[[ -f "$toolchain" ]] || wii_die "Wii cmake toolchain not found at $toolchain"
	cmake -DCMAKE_TOOLCHAIN_FILE="$toolchain" "$@"
}

wii_configure_and_build() {
	local build_dir="$1"
	local build_type="$2"
	local create_log="$3"

	mkdir -p "$build_dir"
	wii_cmake \
		-S "$ROOT" -B "$build_dir" \
		-DCMAKE_BUILD_TYPE="$build_type" \
		-DWII_GAME_BOOT=ON \
		-DLIBRW_PLATFORM=GX \
		-DWII_CREATE_LOG="$create_log"
	cmake --build "$build_dir" -j"$(nproc)"

	local built="$build_dir/src/reVC.dol"
	[[ -f "$built" ]] || wii_die "build finished but $built was not produced"
	mkdir -p "$SD_APP"
	cp -f "$built" "$BOOT_DOL"
	if [[ ! -f "$SD_APP/cheats.txt" ]]; then
		cp -f "$ROOT/gamefiles/cheats.txt" "$SD_APP/cheats.txt"
	fi
	if [[ ! -f "$SD_APP/cheats-ingame.txt" ]]; then
		cp -f "$ROOT/gamefiles/cheats-ingame.txt" "$SD_APP/cheats-ingame.txt"
	fi
	if [[ ! -f "$SD_APP/config.txt" ]]; then
		cp -f "$ROOT/gamefiles/config.txt" "$SD_APP/config.txt"
	fi
	cp -f "$ROOT/gamefiles/icon.png" "$SD_APP/icon.png"
	cp -f "$ROOT/gamefiles/meta.xml" "$SD_APP/meta.xml"
	# HBC lists this tiny folder on the SD card. USB Loader GX is a game ISO
	# loader and will not boot this DOL; a 1.5G apps/reVC on USB also times out
	# HBC's USB scan. Keep game data on USB as apps/reVC/ if the SD is small.
	local hbc_app="$ROOT/hbc/apps/reVC"
	mkdir -p "$hbc_app"
	cp -f "$BOOT_DOL" "$hbc_app/boot.dol"
	cp -f "$ROOT/gamefiles/meta.xml" "$hbc_app/meta.xml"
	cp -f "$ROOT/gamefiles/icon.png" "$hbc_app/icon.png"
	cp -f "$ROOT/gamefiles/cheats.txt" "$hbc_app/cheats.txt"
	cp -f "$ROOT/gamefiles/cheats-ingame.txt" "$hbc_app/cheats-ingame.txt"
	cp -f "$ROOT/gamefiles/config.txt" "$hbc_app/config.txt"
	if [[ -f "$SD_APP/cheats.txt" ]]; then
		cp -f "$SD_APP/cheats.txt" "$hbc_app/cheats.txt"
	fi
	if [[ -f "$SD_APP/cheats-ingame.txt" ]]; then
		cp -f "$SD_APP/cheats-ingame.txt" "$hbc_app/cheats-ingame.txt"
	fi
	if [[ -f "$SD_APP/config.txt" ]]; then
		cp -f "$SD_APP/config.txt" "$hbc_app/config.txt"
	fi
	printf 'tree %s\n' "$ROOT"
	printf 'hbc stub %s\n' "$hbc_app"
	printf 'installed %s -> %s\n' "$built" "$BOOT_DOL"
	stat -c 'dol  %y %s bytes' "$BOOT_DOL"
	# If this is older than the pad source, Dolphin would still be the previous mapping.
	local pad="$ROOT/src/wii-port/WiiPad.cpp"
	if [[ "$BOOT_DOL" -ot "$pad" ]]; then
		wii_die "boot.dol is older than $pad — the remap did not land in this build"
	fi
}

wii_write_dolphin_config() {
	mkdir -p "$DOLPHIN_USER/Config" "$DOLPHIN_USER/Load" "$DOLPHIN_USER/Logs"

	# Keep GUI tweaks on later runs; only seed the files that do not exist yet.
	if [[ -f "$DOLPHIN_USER/Config/Dolphin.ini" ]]; then
		return
	fi

	cat >"$DOLPHIN_USER/Config/Dolphin.ini" <<EOF
[General]
WiiSDCardPath = $SD_IMAGE
WiiSDCardSyncFolder = $SD_ROOT
[Core]
WiiSDCard = True
WiiSDCardEnableFolderSync = False
WiiSDCardAllowWrites = True
WiimoteContinuousScanning = True
WiimoteEnableSpeaker = False
CPUThread = True
[BluetoothPassthrough]
Enabled = False
EOF

	cat >"$DOLPHIN_USER/Config/WiimoteNew.ini" <<'EOF'
[Wiimote1]
Source = 2
[Wiimote2]
Source = 0
[Wiimote3]
Source = 0
[Wiimote4]
Source = 0
[BalanceBoard]
Source = 0
EOF

	cat >"$DOLPHIN_USER/Config/Logger.ini" <<'EOF'
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = True
WriteToWindow = True
[Logs]
BOOT = True
CORE = True
OSREPORT = True
WIIMOTE = True
WII_IPC_SD = True
EOF
}

wii_image_bytes() {
	# 2 GiB leaves headroom over the ~1.5G asset tree and FAT metadata.
	printf '%s' $((2048 * 1024 * 1024))
}

wii_pack_sd_image() {
	command -v mkfs.vfat >/dev/null || wii_die "mkfs.vfat missing; install dosfstools"
	command -v mcopy >/dev/null || wii_die \
		"mcopy missing; install mtools (sudo pacman -S mtools) so the SD image can be packed without root"

	wii_require_data
	mkdir -p "$(dirname "$SD_IMAGE")"
	rm -f "$SD_IMAGE"
	truncate -s "$(wii_image_bytes)" "$SD_IMAGE"
	mkfs.vfat -F 32 -n REVC "$SD_IMAGE" >/dev/null

	export MTOOLS_SKIP_CHECK=1
	mcopy -i "$SD_IMAGE" -s "$SD_ROOT/apps" :: || wii_die "mcopy failed while packing $SD_IMAGE"
	printf 'packed %s from %s\n' "$SD_IMAGE" "$SD_ROOT"
}

wii_inject_boot_dol() {
	if [[ ! -f "$SD_IMAGE" ]]; then
		printf 'packing virtual SD from sd/ (once, ~1.5G)...\n'
		wii_pack_sd_image
	fi
	[[ -f "$BOOT_DOL" ]] || wii_die "no boot.dol at $BOOT_DOL"
	command -v mcopy >/dev/null || return 0
	export MTOOLS_SKIP_CHECK=1
	mcopy -i "$SD_IMAGE" -o "$BOOT_DOL" ::apps/reVC/boot.dol
	if [[ -f "$SD_APP/cheats.txt" ]]; then
		mcopy -i "$SD_IMAGE" -o "$SD_APP/cheats.txt" ::apps/reVC/cheats.txt
	fi
	if [[ -f "$SD_APP/cheats-ingame.txt" ]]; then
		mcopy -i "$SD_IMAGE" -o "$SD_APP/cheats-ingame.txt" ::apps/reVC/cheats-ingame.txt
	fi
	if [[ -f "$SD_APP/config.txt" ]]; then
		mcopy -i "$SD_IMAGE" -o "$SD_APP/config.txt" ::apps/reVC/config.txt
	fi
}

wii_extract_debug_log() {
	[[ -f "$SD_IMAGE" ]] || return 0
	command -v mcopy >/dev/null || return 0
	export MTOOLS_SKIP_CHECK=1
	mcopy -i "$SD_IMAGE" -n ::apps/reVC/debug.log "$SD_APP/debug.log" 2>/dev/null || true
}

wii_enable_bluetooth() {
	if command -v rfkill >/dev/null; then
		rfkill unblock bluetooth 2>/dev/null || true
	fi
	if command -v bluetoothctl >/dev/null; then
		bluetoothctl power on >/dev/null 2>&1 || true
	fi
}

wii_find_dolphin() {
	if command -v dolphin-emu >/dev/null; then
		command -v dolphin-emu
		return
	fi
	wii_die "dolphin-emu is not installed (the /usr/bin/dolphin binary is KDE's file manager). Install it with: sudo pacman -S dolphin-emu mtools"
}
