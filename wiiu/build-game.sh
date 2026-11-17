#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# wiiu/build-game.sh - build WiiCompiled for the Wii U (Aroma .wuhb) from YOUR OWN
# copy of Mario Kart Wii. Runs on Linux, or on Windows in the devkitPro MSYS2 shell.
#
# Steps (the same translation upstream's Launcher/local-build.sh runs for the PC,
# base game only), then the Wii U build of runtime/ (runtime/cmake/WiiU.cmake):
#   0. with --image: extract your disc image into Assets/DATA with nodtool, exactly
#      like upstream's installer (same pinned nodtool, fetched by
#      Launcher/WiiCompiled.Setup.Common.Cli), and copy main.dol + StaticR.rel out
#   1. check the tools and your Assets/main.dol + Assets/StaticR.rel
#   2. build the translator (.NET 8)
#   3. translate-recursive, emit-base-manifest, generate-data-init, emit-build-shards
#   4. cmake with the devkitPro Wii U toolchain -> build-wiiu/WiiCompiled.wuhb
#
# The resulting .wuhb contains code translated from your game: it is for your own
# console only. Never share it. (The runtime smoke test and the test .wuhb files
# contain no game code.)
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)
build_dir="$workspace/build-wiiu"
jobs=""
skip_translation=0
image=""

usage() {
    cat <<'EOF'
Usage: wiiu/build-game.sh --image PATH [--jobs N] [--build-dir DIR] [--skip-translation]

  --image PATH         Your own Mario Kart Wii disc image (ISO, WBFS, RVZ, WIA, GCZ, CISO, ...).
                       Extracted into Assets/DATA; reused on later runs with the same image.
  --jobs N             Parallel jobs for the translator and the compiler (default: all cores)
  --build-dir DIR      CMake build directory (default: build-wiiu in the repository)
  --skip-translation   Reuse generated/ from an earlier run and only rebuild the .wuhb

Without --image, the script uses an earlier extraction: Assets/main.dol,
Assets/StaticR.rel and Assets/DATA (the extracted disc with sys/ and files/).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --skip-translation) skip_translation=1; shift ;;
        --image) image="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build-game.sh: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
done

step() { printf '\n== %s\n' "$*"; }
fail() { printf '\nbuild-game.sh: error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "'$1' was not found. $2"; }

# ---------------------------------------------------------------------------------------------
step "Checking tools"
[[ -n "${DEVKITPRO:-}" && -d "${DEVKITPRO}/devkitPPC" ]] ||
    fail "DEVKITPRO is not set or devkitPPC is missing. Install devkitPro with: dkp-pacman -S wiiu-dev wiiu-cmake"
[[ -f "${DEVKITPRO}/cmake/WiiU.cmake" ]] || fail "devkitPro's Wii U CMake support is missing: dkp-pacman -S wiiu-cmake"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
need cmake "Install CMake 3.25 or newer (MSYS2: pacman -S cmake)."
need git "Install git (MSYS2: pacman -S git)."
need python3 "Install Python 3 (MSYS2: pacman -S python)."
cmake_version=$(cmake --version | awk 'NR==1 {print $3}')
if [[ "$(printf '%s\n3.25\n' "$cmake_version" | sort -V | head -n1)" != "3.25" ]]; then
    fail "CMake $cmake_version is too old; 3.25 or newer is needed."
fi
if [[ -z "$jobs" ]]; then
    jobs=$(nproc 2>/dev/null || echo 4)
fi
echo "devkitPro: $DEVKITPRO   cmake: $cmake_version   jobs: $jobs"

generated="$workspace/generated"
if (( ! skip_translation )); then
    need dotnet "Install the .NET 8 SDK (8.0.4xx or newer): https://dotnet.microsoft.com/download/dotnet/8.0"
    dotnet --list-sdks | grep -q '^8\.' || fail "The .NET 8 SDK is required (dotnet --list-sdks shows none)."

    # shellcheck source=../Launcher/build-cache.sh
    source "$workspace/Launcher/build-cache.sh"
    assets="$workspace/Assets"

    if [[ -n "$image" ]]; then
        [[ -f "$image" ]] || fail "Disc image not found: $image"
        step "Extracting your disc image"
        # Reuse an earlier extraction of the same file (path, size and modification time).
        image_id="$(cd "$(dirname "$image")" && pwd)/$(basename "$image") $(wc -c < "$image" | tr -d ' ') $(date -r "$image" +%s 2>/dev/null || echo 0)"
        marker="$assets/.wiiu-extracted-from"
        if [[ -f "$marker" && "$(cat "$marker")" == "$image_id" && -f "$assets/DATA/sys/main.dol" &&
              -f "$assets/main.dol" && -f "$assets/StaticR.rel" ]]; then
            echo "Reusing the earlier extraction of this image in $assets/DATA"
        else
            echo "Fetching nodtool (the disc tool upstream's installer uses)..."
            nodtool=$(dotnet run --project "$workspace/Launcher/WiiCompiled.Setup.Common.Cli" -c Release -- \
                --workspace "$workspace" | tail -n1 | tr -d '\r')
            if command -v cygpath >/dev/null 2>&1; then
                nodtool=$(cygpath -u "$nodtool")  # MSYS2: dotnet prints a Windows path
            fi
            [[ -x "$nodtool" ]] || fail "Could not obtain nodtool ($nodtool)."
            mkdir -p "$assets"
            rm -f "$marker"
            rm -rf "$assets/DATA"
            echo "Extracting (several GB; takes a few minutes)..."
            "$nodtool" extract "$image" "$assets/DATA" -q || fail "nodtool could not extract $image"
            [[ -f "$assets/DATA/sys/main.dol" && -f "$assets/DATA/files/rel/StaticR.rel" ]] ||
                fail "The image did not contain sys/main.dol and files/rel/StaticR.rel. Is it Mario Kart Wii?"
            cp -f "$assets/DATA/sys/main.dol" "$assets/main.dol"
            cp -f "$assets/DATA/files/rel/StaticR.rel" "$assets/StaticR.rel"
            printf '%s' "$image_id" > "$marker"
        fi
    fi
    [[ -f "$assets/main.dol" && -f "$assets/StaticR.rel" ]] ||
        fail "No game files yet. Run with --image <your disc image> (see --help)."

    step "Checking your game files"
    project=$(mkw_resolve_project "$workspace" "$assets") ||
        fail "main.dol/StaticR.rel are not a clean PAL, NTSC-U or NTSC-J Mario Kart Wii release."
    region=$(mkw_project_pin "$project" region)
    game_id=$(mkw_project_pin "$project" game_id)
    entry_point=$(awk '
        /^translation:/ { in_translation = 1 }
        in_translation && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/ {
            gsub(/^[[:space:]]*-[[:space:]]*/, ""); gsub(/[[:space:]]*$/, ""); print; exit
        }
    ' "$project")
    [[ -n "$entry_point" && -n "$region" ]] || fail "Could not read the entry point/region from $project"
    echo "game: $game_id (region $region), project $project"

    step "Building the translator"
    dotnet build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    translator_dll="$workspace/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
    [[ -f "$translator_dll" ]] || fail "The translator did not build ($translator_dll is missing)."
    translator() { dotnet "$translator_dll" "$@"; }

    functions="$generated/functions"
    base_metadata="$generated/base_translation_output.json"
    base_manifest_dir="$workspace/build/base"
    mkdir -p "$generated" "$base_manifest_dir"

    step "Translating your game (this takes a while)"
    translator translate-recursive "$entry_point" --project "$project" \
        --outdir "$functions" --output-metadata "$base_metadata" \
        --production-source-bundle "$generated/base_translation_sources.bin" \
        --no-function-files --prune-stale --threads "$jobs"

    step "Creating the base translation manifest"
    translator emit-base-manifest --project "$project" --out "$base_manifest_dir" \
        --functions-dir "$functions" --translation-output-metadata "$base_metadata" --region "$region"

    step "Generating game data initialisation"
    translator generate-data-init --project "$project"

    step "Preparing the build graph"
    translator emit-build-shards --project "$project" --base-metadata "$base_metadata" \
        --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" \
        --out "$generated/build_shards"
fi
[[ -f "$generated/build_shards/shards.cmake" ]] ||
    fail "generated/build_shards/shards.cmake is missing; run without --skip-translation first."

# ---------------------------------------------------------------------------------------------
step "Preparing aurora's headers"
if [[ ! -f "$workspace/aurora-main/include/dolphin/gx.h" ]]; then
    git -C "$workspace" submodule update --init aurora-main
fi

step "Building the Wii U app"
cmake -S "$workspace/runtime" -B "$build_dir" -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/WiiU.cmake"
cmake --build "$build_dir" --target WiiCompiled_wuhb --parallel "$jobs"

wuhb="$build_dir/WiiCompiled.wuhb"
[[ -f "$wuhb" ]] || fail "The build finished but $wuhb is missing."

data_hint="the extracted disc (the folder with sys/ and files/)"
if [[ -d "$workspace/Assets/DATA/files" ]]; then
    data_hint="$workspace/Assets/DATA  (copy this whole folder)"
fi
cat <<EOF

Done: $wuhb

Put on the Wii U's SD card:
  SD:/wiiu/apps/WiiCompiled.wuhb   <- $wuhb
  SD:/WiiCompiled/DATA/            <- $data_hint
  SD:/WiiCompiled/Config.toml      <- optional; the defaults are fine for now

This .wuhb contains code translated from your game: keep it for your own console.
EOF
