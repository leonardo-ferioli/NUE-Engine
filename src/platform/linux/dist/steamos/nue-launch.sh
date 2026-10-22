#!/bin/sh
# Launcher for a NUE game shipped as a tarball on SteamOS and other Linux systems.
#
# Steam Deck / SteamOS:
#   1. Copy the extracted folder to the Deck (anywhere below $HOME, for example ~/Games/<game>).
#   2. Desktop Mode > Steam > Games > Add a Non-Steam Game to My Library > Browse > select this file.
#   3. Properties > Compatibility: force "Steam Linux Runtime 3.0 (sniper)" so the game runs in the same container as Steam's own native games.
#   4. Return to Gaming Mode. Steam applies its default "Gamepad" controller layout; ship a template for more.
#
# The script changes nothing system wide. It only starts the game from its own folder.
here="$(cd "$(dirname "$0")" && pwd)" || exit 1
cd "$here" || exit 1

# Never touch LD_PRELOAD (Steam's overlay lives there) and never set SDL_VIDEODRIVER here: NUE picks Wayland / X11 / gamescope itself
# and respects what the player exported. Debug switches players can use:
#   NUE_LOG_LEVEL=debug   more logging       SDL_VIDEO_DRIVER=x11   force X11
exec "$here/bin/game" "$@"
