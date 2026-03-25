#!/bin/bash
# TGSpeechBox Linux Installer
# Installs to /usr/local by default, or a custom prefix

set -e

PREFIX="${1:-/usr/local}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Installing TGSpeechBox to $PREFIX..."

# Create directories
mkdir -p "$PREFIX/bin"
mkdir -p "$PREFIX/lib"
mkdir -p "$PREFIX/share/tgspeechbox"

# Copy files
cp "$SCRIPT_DIR/bin/tgsbRender" "$PREFIX/bin/tgsbRender"
cp "$SCRIPT_DIR/lib/"*.so "$PREFIX/lib/"
cp -r "$SCRIPT_DIR/share/tgspeechbox/"* "$PREFIX/share/tgspeechbox/"

# Backward-compat symlinks (keeps existing Speech Dispatcher configs working)
ln -sf tgsbRender "$PREFIX/bin/nvspRender"
ln -sf libtgspeechbox.so "$PREFIX/lib/libspeechPlayer.so"
ln -sf libtgsbFrontend.so "$PREFIX/lib/libnvspFrontend.so"
ln -sfn tgspeechbox "$PREFIX/share/nvspeechplayer"

# Install wrapper script
cp "$SCRIPT_DIR/bin/tgsp" "$PREFIX/bin/tgsp"
chmod +x "$PREFIX/bin/tgsp"
chmod +x "$PREFIX/bin/tgsbRender"

# Backward-compat wrapper symlinks
ln -sf tgsp "$PREFIX/bin/tgsb"
ln -sf tgsp "$PREFIX/bin/nvsp"

# Install tgsb-speak (clause-aware Speech Dispatcher wrapper)
EXTRAS_SD="$PREFIX/share/tgspeechbox/extras/speech-dispatcher"
if [ -f "$EXTRAS_SD/tgsb-speak" ]; then
    cp "$EXTRAS_SD/tgsb-speak" "$PREFIX/bin/tgsb-speak"
    chmod +x "$PREFIX/bin/tgsb-speak"
fi

echo ""
echo "Installation complete!"
echo ""
echo "Quick test:"
echo "  echo 'həˈloʊ wɜld' | $PREFIX/bin/tgsp --lang en-us | aplay -q -r 16000 -f S16_LE -t raw -"
echo ""
echo "Note: 'nvspRender', 'nvsp', and the old share path are symlinked"
echo "      for backward compatibility. Existing Speech Dispatcher configs"
echo "      will continue to work."

# Optionally update library cache if installing to system location
if [ "$PREFIX" = "/usr" ] || [ "$PREFIX" = "/usr/local" ]; then
    if [ -x /sbin/ldconfig ] && [ -w /etc/ld.so.conf.d ]; then
        echo "$PREFIX/lib" > /etc/ld.so.conf.d/tgspeechbox.conf
        # Clean up old config if present
        rm -f /etc/ld.so.conf.d/nvspeechplayer.conf
        /sbin/ldconfig
        echo "Library cache updated."
    else
        echo "Note: You may need to run 'sudo ldconfig' or add $PREFIX/lib to LD_LIBRARY_PATH"
    fi
fi

# ============================================================================
# Speech Dispatcher integration (optional)
# ============================================================================

configure_speech_dispatcher() {
    local sd_conf_file=""
    local sd_modules_dir=""

    # --- Locate speechd.conf and modules directory ---

    # Check user-level config first
    local user_conf="$HOME/.config/speech-dispatcher/speechd.conf"
    local user_modules="$HOME/.config/speech-dispatcher/modules"

    # Check system-level config
    local sys_conf="/etc/speech-dispatcher/speechd.conf"
    local sys_modules="/etc/speech-dispatcher/modules"

    # Prefer user config if it exists; otherwise fall back to system
    if [ -f "$user_conf" ]; then
        sd_conf_file="$user_conf"
        sd_modules_dir="$user_modules"
    elif [ -f "$sys_conf" ]; then
        sd_conf_file="$sys_conf"
        sd_modules_dir="$sys_modules"
    else
        echo ""
        echo "Could not find speechd.conf in:"
        echo "  $user_conf"
        echo "  $sys_conf"
        echo ""
        echo "If your speechd.conf is elsewhere, configure manually."
        echo "See: $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
        return 1
    fi

    echo ""
    echo "Found Speech Dispatcher config: $sd_conf_file"
    echo "Modules directory: $sd_modules_dir"

    # --- Install the generic module config ---
    mkdir -p "$sd_modules_dir"

    local src_conf="$PREFIX/share/tgspeechbox/extras/speech-dispatcher/tgsb-generic.conf"
    local dst_conf="$sd_modules_dir/tgsb-generic.conf"

    if [ ! -f "$src_conf" ]; then
        echo "Error: tgsb-generic.conf not found at $src_conf"
        return 1
    fi

    cp "$src_conf" "$dst_conf"
    echo "Installed: $dst_conf"

    # --- Enable the module in speechd.conf ---

    # Check if tgsb module is already configured
    if grep -q 'AddModule "tgsb"' "$sd_conf_file" 2>/dev/null; then
        echo "TGSpeechBox module already present in $sd_conf_file"
    else
        # Add the module line after the last AddModule line, or at the end
        # We use a marker comment so we can find our additions later
        {
            echo ""
            echo "# --- TGSpeechBox (added by install.sh) ---"
            echo 'AddModule "tgsb" "sd_generic" "tgsb-generic.conf"'
        } >> "$sd_conf_file"
        echo "Added TGSpeechBox module to $sd_conf_file"
    fi

    # Ask if they want it as default
    echo ""
    read -r -p "Set TGSpeechBox as the default synthesizer? [y/N] " set_default
    case "$set_default" in
        [yY]|[yY][eE][sS])
            # Set DefaultModule to tgsb (skip if already set)
            if grep -q '^DefaultModule tgsb$' "$sd_conf_file" 2>/dev/null; then
                echo "DefaultModule already set to tgsb."
            else
                # Comment out any existing DefaultModule line and add ours
                if grep -q '^DefaultModule' "$sd_conf_file" 2>/dev/null; then
                    sed -i 's/^DefaultModule/# DefaultModule/' "$sd_conf_file"
                fi
                echo 'DefaultModule tgsb' >> "$sd_conf_file"
                echo "Set DefaultModule to tgsb."
            fi
            ;;
        *)
            echo "Skipped. You can set it manually later:"
            echo "  DefaultModule tgsb"
            echo "in $sd_conf_file"
            ;;
    esac

    # --- Verify dependencies ---
    echo ""
    echo "Checking pipeline dependencies..."
    local missing=""
    for cmd in espeak-ng tgsbRender aplay; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing="$missing $cmd"
        fi
    done

    if [ -n "$missing" ]; then
        echo "Warning: missing commands:$missing"
        echo "The synthesis pipeline needs these in PATH."
        if echo "$missing" | grep -q "espeak-ng"; then
            echo "  Install espeak-ng: sudo apt install espeak-ng (Debian/Ubuntu)"
            echo "                     sudo dnf install espeak-ng (Fedora)"
        fi
        if echo "$missing" | grep -q "aplay"; then
            echo "  Install aplay: sudo apt install alsa-utils (Debian/Ubuntu)"
            echo "                 sudo dnf install alsa-utils (Fedora)"
        fi
    else
        echo "All dependencies found."
    fi

    echo ""
    echo "Speech Dispatcher configuration complete!"
    echo ""
    echo "Restart Speech Dispatcher to apply:"
    echo "  systemctl --user restart speech-dispatcher"
    echo "  # or: killall speech-dispatcher"
    echo ""
    echo "Test with:"
    echo "  spd-say 'Hello from TGSpeechBox'"

    return 0
}

# --- Ask the user ---

# Only offer if speech-dispatcher appears to be installed
if command -v spd-say >/dev/null 2>&1 || [ -f /etc/speech-dispatcher/speechd.conf ] || [ -f "$HOME/.config/speech-dispatcher/speechd.conf" ]; then
    echo ""
    read -r -p "Would you like to configure Speech Dispatcher to use TGSpeechBox? [y/N] " do_sd
    case "$do_sd" in
        [yY]|[yY][eE][sS])
            configure_speech_dispatcher || true
            ;;
        *)
            echo ""
            echo "Skipped Speech Dispatcher setup."
            echo "To configure manually later, see:"
            echo "  $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
            ;;
    esac
else
    echo ""
    echo "Speech Dispatcher not detected. To integrate later, see:"
    echo "  $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
fi
