#!/bin/sh
# VeloPlay for Linux: fetches the newest version and installs it for this user.
# Nothing here needs root. The program lands in ~/.local/share/VeloPlay with an
# entry in the app menu; later versions it fetches itself.
#
#   curl -fsSL https://raw.githubusercontent.com/FynnXland/VeloPlay/main/install.sh | sh
#
# To remove it again: ~/.local/share/VeloPlay/VeloPlay --uninstall
set -eu

# Everything happens inside main, called on the last line: a download of this
# script that breaks off halfway runs nothing at all.
main() {
    package="VeloPlay-linux-x64.zip"
    url="${VELOPLAY_PACKAGE_URL:-https://github.com/FynnXland/VeloPlay/releases/latest/download/$package}"

    case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in
        de*) german=1 ;;
        *) german=0 ;;
    esac

    [ "$(uname -s)" = "Linux" ] || fail \
        "This is the installer for Linux. For Windows there is a setup file on the download page." \
        "Das ist der Installer für Linux. Für Windows gibt es eine Setup-Datei auf der Download-Seite."
    case "$(uname -m)" in
        x86_64 | amd64) ;;
        *) fail "VeloPlay for Linux needs a 64-bit Intel or AMD processor; this one is $(uname -m)." \
            "VeloPlay für Linux braucht einen 64-Bit-Prozessor von Intel oder AMD; dieser hier ist $(uname -m)." ;;
    esac

    if have curl; then fetch=curl
    elif have wget; then fetch=wget
    else fail "curl or wget is needed to download VeloPlay. Install one of them and run this again." \
        "Zum Herunterladen braucht es curl oder wget. Installiere eins davon und starte den Befehl noch einmal."
    fi
    if have unzip; then unpack=unzip
    elif have bsdtar; then unpack=bsdtar
    elif have python3; then unpack=python3
    else fail "unzip is needed to unpack VeloPlay. Install it (for example: sudo apt install unzip) and run this again." \
        "Zum Entpacken braucht es unzip. Installiere es (zum Beispiel: sudo apt install unzip) und starte den Befehl noch einmal."
    fi

    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    trap 'exit 1' INT TERM

    say "Downloading VeloPlay ..." "VeloPlay wird heruntergeladen ..."
    if [ "$fetch" = curl ]; then
        curl -fL --retry 2 --progress-bar -o "$work/$package" "$url" || no_download
    else
        wget -q --show-progress -O "$work/$package" "$url" || no_download
    fi

    mkdir "$work/app"
    case "$unpack" in
        unzip) unzip -q "$work/$package" -d "$work/app" ;;
        bsdtar) bsdtar -xf "$work/$package" -C "$work/app" ;;
        python3) python3 -m zipfile -e "$work/$package" "$work/app" ;;
    esac || fail "The download could not be unpacked. Run this again." \
        "Der Download ließ sich nicht entpacken. Starte den Befehl noch einmal."
    [ -f "$work/app/VeloPlay" ] || fail "The download is not VeloPlay for Linux. Run this again." \
        "Der Download ist nicht VeloPlay für Linux. Starte den Befehl noch einmal."
    # Not every unpacker keeps the mark that a file may be run.
    chmod +x "$work/app/VeloPlay"
    [ ! -f "$work/app/veloplay-cli" ] || chmod +x "$work/app/veloplay-cli"

    # The program installs itself: it copies its files, writes the menu entry
    # and says where it went.
    "$work/app/VeloPlay" --install || fail "VeloPlay could not install itself; the line above says why." \
        "VeloPlay konnte sich nicht installieren; die Zeile darüber sagt, warum."

    say "Done. Start VeloPlay from the app menu." "Fertig. Starte VeloPlay aus dem App-Menü."
}

have() { command -v "$1" >/dev/null 2>&1; }

say() {
    if [ "${german:-0}" = 1 ]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi
}

fail() {
    say "$1" "$2" >&2
    exit 1
}

no_download() {
    # The Linux file of a new version follows a few minutes after the Windows one.
    fail "The download failed. Right after a new version the Linux file can be a few minutes late: try again in ten minutes." \
        "Der Download ist fehlgeschlagen. Direkt nach einer neuen Version kommt die Linux-Datei ein paar Minuten später: versuch es in zehn Minuten noch einmal."
}

main "$@"
