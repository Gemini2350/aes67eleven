#!/bin/bash
# Remove driver, daemon and launch agent. coreaudiod restarts (audio blips).
set -uo pipefail

launchctl bootout "gui/$(id -u)/com.aes67vsc.daemon" 2>/dev/null
rm -f "$HOME/Library/LaunchAgents/com.aes67vsc.daemon.plist"

sudo rm -rf /Library/Audio/Plug-Ins/HAL/AES67VSC.driver
sudo rm -f /usr/local/libexec/aes67vsc-daemon
sudo rm -rf "/Applications/AES67 VSC.app" /Applications/AES67eleven.app
sudo rm -rf /usr/local/share/aes67-vsc
sudo killall coreaudiod

echo "removed. config/logs kept in ~/Library/Application Support/aes67-vsc and ~/Library/Logs/aes67-vsc"
