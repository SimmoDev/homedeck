# tools/

Supporting developer tooling for HomeDeck. Each script is a thin wrapper
around commands documented in full (including the "why" behind each
flag) in [DEVELOPMENT.md](../DEVELOPMENT.md)'s ESP-IDF setup section -
read that first if a script here does something unexpected.

- `flash.sh [serial-port]` - builds and flashes firmware onto real Tab5
  hardware (default port `/dev/ttyACM0`).
- `monitor.sh [serial-port]` - opens an interactive serial monitor.
- `factory-reset.sh [serial-port]` - erases the device's entire flash
  (settings, secrets, OTA state, the firmware image itself); confirms
  before proceeding since it's irreversible. Run `flash.sh` afterward to
  make the device bootable again.
- `set-password.sh [host]` - sets the Web UI admin password on a device
  that doesn't have one yet (default host `homedeck.local` - pass a real
  IP if mDNS resolution doesn't work on your machine).

All four are confirmed working against the K145 reference unit.
