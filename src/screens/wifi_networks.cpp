// wifi_networks.cpp — spot list for WiFi Encounters (see wifi_networks.h).
#include "wifi_networks.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm::wifi {
namespace {

struct DemoSpot {
    uint8_t id[6];
    const char* name;
};

// Made-up spots. The ids were picked so a Lv 32 lead (the demo party) meets a
// few fancy skins here: Shiny Raichu, Pink Pikachu, Dark Voltorb, Dark-Shiny
// Magneton. Other lead levels roll other zones, so other species/skins.
constexpr DemoSpot kDemo[] = {
    {{0x02, 0x4d, 0x00, 0x00, 0x0b, 0xbf}, "Moonbeam Bakery"},
    {{0x02, 0x4d, 0x00, 0x00, 0x24, 0x9f}, "Pidgey_Loft_5G"},
    {{0x02, 0x4d, 0x00, 0x00, 0x0c, 0x75}, "Totally Not A Router"},
    {{0x02, 0x4d, 0x02, 0x82, 0x31, 0x41}, "Grandmas_Teapot"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x6f, 0x24}, "LibraryOfSnacks"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x94, 0x2f}, "Snorlax_Nap_Pod"},
    {{0x02, 0x4d, 0x4d, 0x00, 0xb9, 0x3a}, "Lighthouse-Lounge"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x72, 0x71}, "Thunder Hill Picnic"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x4d, 0x66}, "ByteSizedDiner"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x4a, 0x19}, "Underground Hamster Net"},
    {{0x02, 0x4d, 0x4d, 0x00, 0x03, 0x50}, ""},   // unnamed -> flavour name
};

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Sort by name (never by anything else), drop repeated ids.
void tidy(std::vector<Spot>& v) {
    std::stable_sort(v.begin(), v.end(), [](const Spot& a, const Spot& b) {
        return lower(place_name(a)) < lower(place_name(b));
    });
    std::vector<Spot> out;
    for (const Spot& s : v) {
        bool dup = false;
        for (const Spot& o : out) {
            if (std::memcmp(o.bssid, s.bssid, 6) == 0) { dup = true; break; }
        }
        if (!dup) out.push_back(s);
    }
    v.swap(out);
}

#if defined(__linux__)
// Split one terse nmcli line on unescaped ':' and undo its "\:" / "\\" escapes.
std::vector<std::string> split_terse(const std::string& line) {
    std::vector<std::string> f(1);
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\\' && i + 1 < line.size()) {
            f.back().push_back(line[++i]);
        } else if (ch == ':') {
            f.emplace_back();
        } else {
            f.back().push_back(ch);
        }
    }
    return f;
}

bool parse_id(const std::string& s, uint8_t out[6]) {
    unsigned v[6];
    if (std::sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

// The list the OS already has (same one its network menu shows). Read only.
std::vector<Spot> os_spots() {
    std::vector<Spot> v;
    FILE* p = popen("nmcli -t -f BSSID,SSID dev wifi list --rescan no 2>/dev/null", "r");
    if (!p) return v;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p) && v.size() < 40) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        const auto f = split_terse(line);
        if (f.empty()) continue;
        Spot s;
        if (!parse_id(f[0], s.bssid)) continue;
        if (f.size() > 1) s.ssid = f[1];
        if (s.ssid == "--") s.ssid.clear();   // nmcli's "no name"
        v.push_back(std::move(s));
    }
    pclose(p);
    return v;
}
#endif

} // namespace

std::vector<Spot> demo_spots() {
    std::vector<Spot> v;
    for (const DemoSpot& d : kDemo) {
        Spot s;
        std::memcpy(s.bssid, d.id, 6);
        s.ssid = d.name;
        v.push_back(std::move(s));
    }
    tidy(v);
    return v;
}

std::vector<Spot> load_spots(bool* from_demo) {
#if defined(__linux__)
    const char* opt = std::getenv("MM_WIFI_NEARBY");
    if (opt && opt[0] == '1') {
        std::vector<Spot> v = os_spots();
        if (!v.empty()) {
            tidy(v);
            if (from_demo) *from_demo = false;
            return v;
        }
    }
#endif
    if (from_demo) *from_demo = true;
    return demo_spots();
}

std::string place_name(const Spot& s) {
    // The panel fonts are ASCII-only: keep printable ASCII, swap the rest.
    std::string out;
    bool blank = true;
    for (char ch : s.ssid) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u >= 32 && u < 127) {
            out.push_back(ch);
            if (ch != ' ') blank = false;
        } else if (u >= 0xC0 || u < 0x80) {   // start of a non-ASCII char (or control)
            out.push_back('?');
        }
    }
    if (!blank) return out;
    static const char* const kNames[] = {"Hidden Grove", "Quiet Hollow", "Misty Glade", "Secret Meadow"};
    return kNames[(s.bssid[4] ^ s.bssid[5]) % 4];
}

} // namespace mm::wifi
