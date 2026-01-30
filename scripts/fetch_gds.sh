#!/bin/bash

# List of GDS files to fetch in the format <ship_repo>:<gds_filename>
GDS_TARGETS=(
    "08:tt_um_2048_vga_game.gds"
    "08:tt_um_a1k0n_vgadonut.gds"
    "08:tt_um_a1k0n_nyancat.gds"
    "09:tt_um_oscillating_bones.gds"
    "09:tt_um_znah_vga_ca.gds"
    "09:tt_um_rejunity_atari2600.gds.br"
    "09:tt_um_a1k0n_nyancat.gds"
    "ihp-25a:tt_um_znah_vga_ca.gds"
    "sky-25b:tt_um_vga_clock.oas"
    "sky-25b:tt_um_pongsagon_tinygpu_v2.oas"
    "gf-0p2:tt_um_2048_vga_game.oas"
)

# Base folder for downloads
DOWNLOAD_BASE="gds"

# Base URL for Tiny Tapeout GitHub raw content
BASE_URL="https://github.com/TinyTapeout/tinytapeout-"

for TARGET in "${GDS_TARGETS[@]}"; do
    # Split by colon
    REPO="${TARGET%%:*}"
    FILENAME="${TARGET##*:}"
    
    # Extract macro name from filename (removes .gds, .gds.br, or .oas)
    MACRO="${FILENAME%.gds*}"
    MACRO="${MACRO%.oas*}"
    
    # Construct target directory
    TARGET_DIR="${DOWNLOAD_BASE}/${REPO}"
    mkdir -p "${TARGET_DIR}"
    
    # Full path to the local file
    LOCAL_PATH="${TARGET_DIR}/${FILENAME}"

    if [[ -f "${LOCAL_PATH}" ]]; then
        echo "[OK] ${FILENAME}"
    else
        # Construct download URL
        # Format: https://github.com/TinyTapeout/<repo>/raw/main/projects/<macro>/<filename>
        URL="${BASE_URL}${REPO}/raw/main/projects/${MACRO}/${FILENAME}"
        
        echo "[FETCHING] ${FILENAME} -> ${TARGET_DIR}"
        curl -sSL -o "${LOCAL_PATH}" "${URL}"
        
        if [[ $? -ne 0 ]]; then
            echo "[ERROR] Failed to download ${FILENAME}"
        fi
    fi
done