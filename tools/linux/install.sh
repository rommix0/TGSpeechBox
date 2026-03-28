#!/bin/bash
# TGSpeechBox Linux Installer
# Installs to /usr/local by default, or a custom prefix

set -e

PREFIX="${1:-/usr/local}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo ""
echo "============================================"
echo "  TGSpeechBox Installer"
echo "============================================"
echo ""
echo "Installing to $PREFIX..."

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

# Symlink into /usr/bin if installing to /usr/local — systemd services
# (like speech-dispatcher via socket activation) often only have /usr/bin
# in their PATH, not /usr/local/bin.
if [ "$PREFIX" = "/usr/local" ] && [ -d /usr/bin ]; then
    for cmd in tgsbRender tgsp tgsb-speak tgsb nvsp; do
        if [ -f "$PREFIX/bin/$cmd" ] || [ -L "$PREFIX/bin/$cmd" ]; then
            ln -sf "$PREFIX/bin/$cmd" "/usr/bin/$cmd"
        fi
    done
    echo "  Symlinked binaries into /usr/bin (for systemd PATH compatibility)."
fi

echo ""
echo "  Core files installed."

# Optionally update library cache if installing to system location
if [ "$PREFIX" = "/usr" ] || [ "$PREFIX" = "/usr/local" ]; then
    if [ -x /sbin/ldconfig ] && [ -w /etc/ld.so.conf.d ]; then
        echo "$PREFIX/lib" > /etc/ld.so.conf.d/tgspeechbox.conf
        # Clean up old config if present
        rm -f /etc/ld.so.conf.d/nvspeechplayer.conf
        /sbin/ldconfig
        echo "  Library cache updated."
    else
        echo ""
        echo "  Note: You may need to run 'sudo ldconfig' or add $PREFIX/lib to LD_LIBRARY_PATH"
    fi
fi

# ============================================================================
# Dependency check
# ============================================================================

echo ""
echo "--------------------------------------------"
echo "  Checking dependencies"
echo "--------------------------------------------"
echo ""

dep_ok=true

# espeak-ng (required)
if command -v espeak-ng >/dev/null 2>&1; then
    echo "  [OK] espeak-ng"
else
    echo "  [MISSING] espeak-ng — required for text-to-phoneme conversion"
    echo "            Install: sudo apt install espeak-ng  (Debian/Ubuntu)"
    echo "                     sudo dnf install espeak-ng  (Fedora)"
    dep_ok=false
fi

# tgsbRender (just installed)
if command -v tgsbRender >/dev/null 2>&1; then
    echo "  [OK] tgsbRender"
else
    echo "  [WARNING] tgsbRender not found in PATH"
    echo "            This shouldn't happen — check that $PREFIX/bin is in your PATH."
    dep_ok=false
fi

# Audio output (paplay preferred, aplay as fallback)
if command -v paplay >/dev/null 2>&1; then
    echo "  [OK] paplay (PipeWire/PulseAudio audio — preferred)"
elif command -v aplay >/dev/null 2>&1; then
    echo "  [OK] aplay (ALSA audio)"
else
    echo "  [MISSING] No audio player found (need paplay or aplay)"
    echo "            Install: sudo apt install pulseaudio-utils  (for paplay)"
    echo "                     sudo apt install alsa-utils        (for aplay)"
    dep_ok=false
fi

# python3 (optional — no longer required for tgsb-speak)
if command -v python3 >/dev/null 2>&1; then
    echo "  [OK] python3"
fi

echo ""
if [ "$dep_ok" = true ]; then
    echo "  All required dependencies found."
else
    echo "  Some dependencies are missing (see above)."
    echo "  TGSpeechBox may not work until they are installed."
fi

# ============================================================================
# Quick self-test
# ============================================================================

echo ""
echo "--------------------------------------------"
echo "  Running self-test"
echo "--------------------------------------------"
echo ""

if command -v espeak-ng >/dev/null 2>&1 && command -v tgsbRender >/dev/null 2>&1; then
    test_output=$(echo 'həˈloʊ' | tgsbRender --packdir "$PREFIX/share/tgspeechbox" --lang en-us 2>/dev/null | wc -c)
    if [ "$test_output" -gt 1000 ] 2>/dev/null; then
        echo "  [OK] tgsbRender produces audio ($test_output bytes)"
    else
        echo "  [FAIL] tgsbRender produced no audio or too little ($test_output bytes)"
        echo "         This may indicate a glibc incompatibility or missing packs."
        echo "         Try: ldd $PREFIX/bin/tgsbRender"
    fi

    # Test in-process espeak mode (preferred — no pipe chain)
    test_espeak=$(tgsbRender --espeak --text 'hello' --packdir "$PREFIX/share/tgspeechbox" --lang en-us 2>/dev/null | wc -c)
    if [ "$test_espeak" -gt 1000 ] 2>/dev/null; then
        echo "  [OK] In-process espeak works (tgsbRender --espeak: $test_espeak bytes)"
    else
        echo "  [INFO] In-process espeak not available — using pipe fallback"
        # Test pipe chain fallback
        test_pipeline=$(echo 'hello' | espeak-ng -q -v en-us --ipa=1 --stdin 2>/dev/null | tgsbRender --packdir "$PREFIX/share/tgspeechbox" --lang en-us 2>/dev/null | wc -c)
        if [ "$test_pipeline" -gt 1000 ] 2>/dev/null; then
            echo "  [OK] Pipe fallback works (espeak-ng → tgsbRender: $test_pipeline bytes)"
        else
            echo "  [FAIL] Neither in-process nor pipe mode produced audio"
            echo "         Check espeak-ng installation and language data."
        fi
    fi
else
    echo "  Skipped (missing espeak-ng or tgsbRender)."
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
        echo "  Could not find speechd.conf in:"
        echo "    $user_conf"
        echo "    $sys_conf"
        echo ""
        echo "  If your speechd.conf is elsewhere, configure manually."
        echo "  See: $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
        return 1
    fi

    echo ""
    echo "  Found config: $sd_conf_file"
    echo "  Modules dir:  $sd_modules_dir"

    # --- Install the generic module config ---
    mkdir -p "$sd_modules_dir"

    local src_conf="$PREFIX/share/tgspeechbox/extras/speech-dispatcher/tgsb-generic.conf"
    local dst_conf="$sd_modules_dir/tgsb-generic.conf"

    if [ ! -f "$src_conf" ]; then
        echo "  Error: tgsb-generic.conf not found at $src_conf"
        return 1
    fi

    cp "$src_conf" "$dst_conf"
    echo "  Installed module config: $dst_conf"

    # --- Ensure espeak-ng module is enabled ---
    # Many distros ship speechd.conf with all AddModule lines commented out.
    # If espeak-ng is commented out, uncomment it so users always have a
    # working fallback synthesizer.
    if grep -q '^#.*AddModule "espeak-ng".*"sd_espeak-ng"' "$sd_conf_file" 2>/dev/null; then
        if ! grep -q '^AddModule "espeak-ng".*"sd_espeak-ng"' "$sd_conf_file" 2>/dev/null; then
            # Uncomment the first commented espeak-ng line
            sed -i '0,/^#.*AddModule "espeak-ng".*"sd_espeak-ng"/{s/^#\s*//}' "$sd_conf_file"
            echo "  Enabled espeak-ng module (was commented out)."
        fi
    fi

    # If there's still no espeak-ng AddModule at all, add one
    if ! grep -q '^AddModule "espeak-ng"' "$sd_conf_file" 2>/dev/null; then
        if command -v sd_espeak-ng >/dev/null 2>&1 || [ -f "$sd_modules_dir/espeak-ng.conf" ] || [ -f "/usr/lib/speech-dispatcher-modules/sd_espeak-ng" ]; then
            sed -i '/# --- TGSpeechBox/i AddModule "espeak-ng" "sd_espeak-ng" "espeak-ng.conf"' "$sd_conf_file" 2>/dev/null || \
                echo 'AddModule "espeak-ng" "sd_espeak-ng" "espeak-ng.conf"' >> "$sd_conf_file"
            echo "  Added espeak-ng module (was missing)."
        fi
    fi

    # --- Enable the TGSpeechBox module ---

    # Check if tgsb module is already configured
    if grep -q '^AddModule "tgsb"' "$sd_conf_file" 2>/dev/null; then
        echo "  TGSpeechBox module already present."
    else
        {
            echo ""
            echo "# --- TGSpeechBox (added by install.sh) ---"
            echo 'AddModule "tgsb" "sd_generic" "tgsb-generic.conf"'
        } >> "$sd_conf_file"
        echo "  Added TGSpeechBox module."
    fi

    # --- Ensure there is an active DefaultModule ---
    # If no DefaultModule is set (all commented out), set espeak-ng as default
    # so the user always has a working voice.
    if ! grep -q '^DefaultModule' "$sd_conf_file" 2>/dev/null; then
        echo 'DefaultModule espeak-ng' >> "$sd_conf_file"
        echo "  Set espeak-ng as default (no default was configured)."
    fi

    # --- Ask if they want TGSpeechBox as default ---
    echo ""
    echo "  Your current default synthesizer:"
    local current_default
    current_default=$(grep '^DefaultModule' "$sd_conf_file" 2>/dev/null | tail -1 | awk '{print $2}')
    echo "    $current_default"
    echo ""
    echo "  You can set TGSpeechBox as the default, or keep $current_default."
    echo "  Either way, both will be available — you can switch in Orca's settings."
    echo ""
    read -r -p "  Set TGSpeechBox as the default synthesizer? [y/N] " set_default
    case "$set_default" in
        [yY]|[yY][eE][sS])
            # Set DefaultModule to tgsb (skip if already set)
            if grep -q '^DefaultModule tgsb$' "$sd_conf_file" 2>/dev/null; then
                echo "  DefaultModule already set to tgsb."
            else
                # Comment out any existing DefaultModule line and add ours
                if grep -q '^DefaultModule' "$sd_conf_file" 2>/dev/null; then
                    sed -i 's/^DefaultModule/# DefaultModule/' "$sd_conf_file"
                fi
                echo 'DefaultModule tgsb' >> "$sd_conf_file"
                echo "  Set DefaultModule to tgsb."
            fi
            echo ""
            echo "  Tip: If you ever need to switch back, run:"
            echo "    sudo sed -i 's/^DefaultModule.*/DefaultModule espeak-ng/' $sd_conf_file"
            echo "    killall speech-dispatcher"
            ;;
        *)
            echo "  Kept $current_default as default."
            echo "  You can select TGSpeechBox in Orca: Preferences → Speech → Speech Synthesizer."
            ;;
    esac

    # --- Final summary ---
    echo ""
    echo "--------------------------------------------"
    echo "  Speech Dispatcher setup complete!"
    echo "--------------------------------------------"
    echo ""
    echo "  Modules enabled:"
    grep '^AddModule' "$sd_conf_file" | while read -r line; do
        local name
        name=$(echo "$line" | sed 's/AddModule "\([^"]*\)".*/\1/')
        echo "    - $name"
    done
    echo ""
    echo "  Default: $(grep '^DefaultModule' "$sd_conf_file" | tail -1 | awk '{print $2}')"
    echo ""
    echo "  To apply changes:"
    echo "    killall speech-dispatcher"
    echo ""
    echo "  To test:"
    echo "    spd-say 'Hello from TGSpeechBox'"
    echo ""
    echo "  To switch synthesizer in Orca:"
    echo "    Orca Preferences → Speech → Speech Synthesizer"
    echo ""
    echo "  If you ever lose your voice, run:"
    echo "    sudo sed -i 's/^DefaultModule.*/DefaultModule espeak-ng/' $sd_conf_file"
    echo "    killall speech-dispatcher"
    echo ""

    return 0
}

# --- Ask the user ---

# Only offer if speech-dispatcher appears to be installed
if command -v spd-say >/dev/null 2>&1 || [ -f /etc/speech-dispatcher/speechd.conf ] || [ -f "$HOME/.config/speech-dispatcher/speechd.conf" ]; then
    echo ""
    echo "--------------------------------------------"
    echo "  Speech Dispatcher integration"
    echo "--------------------------------------------"
    echo ""
    echo "  Speech Dispatcher detected on this system."
    echo "  TGSpeechBox can register as a synthesizer so"
    echo "  screen readers like Orca can use it."
    echo ""
    read -r -p "  Configure Speech Dispatcher? [Y/n] " do_sd
    case "$do_sd" in
        [nN]|[nN][oO])
            echo ""
            echo "  Skipped Speech Dispatcher setup."
            echo "  To configure manually later, see:"
            echo "    $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
            ;;
        *)
            configure_speech_dispatcher || true
            ;;
    esac
else
    echo ""
    echo "  Speech Dispatcher not detected."
    echo "  To integrate later, see:"
    echo "    $PREFIX/share/tgspeechbox/extras/speech-dispatcher/README.md"
fi

echo ""
echo "============================================"
echo "  Installation complete!"
echo "============================================"
echo ""
echo "  Quick manual test:"
echo "    echo 'hello world' | espeak-ng -q -v en-us --ipa=1 --stdin \\"
echo "      | tgsbRender --packdir $PREFIX/share/tgspeechbox --lang en-us \\"
echo "      | paplay --raw --rate=22050 --channels=1 --format=s16le"
echo ""
echo "  Backward-compat symlinks (nvspRender, nvsp) are installed"
echo "  for existing Speech Dispatcher configs."
echo ""
