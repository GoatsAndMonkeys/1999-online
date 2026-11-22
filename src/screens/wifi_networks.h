// wifi_networks.h — where the WiFi Encounters wild mons live.
//
// Each "spot" is a network name plus its 6-byte id; the game only uses the id
// as a seed for wifi_enc::encounterForBssid() and the name as flavour text
// ("found near Moonbeam Bakery"). Nothing else is read or kept.
//
// Default: a built-in list of made-up spots (ids in the locally administered
// 02:xx range), so the game runs anywhere with no radio at all.
// Linux only, opt-in with env MM_WIFI_NEARBY=1: the list the OS already shows
// in its network menu is read via `nmcli` (no re-listing requested); any
// failure or empty result falls back to the built-in list. macOS: built-in only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm::wifi {

struct Spot {
    uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
    std::string ssid;     // may be empty (unnamed network)
};

// The built-in fictional spot list.
std::vector<Spot> demo_spots();

// Current spot list (see header comment). `*from_demo` is set to true when the
// built-in list was used. Sorted by name, duplicates (same id) removed.
std::vector<Spot> load_spots(bool* from_demo = nullptr);

// Flavour place name for a spot: its name, or a cosy stand-in such as
// "Hidden Grove" when the network has none.
std::string place_name(const Spot& s);

} // namespace mm::wifi
