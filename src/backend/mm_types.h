// mm_types.h — plain data structs the MonsterMesh Cardputer UI reads.
//
// LVGL-free / SDL-free. Filled by GameState either from the live mmd daemon
// (see monster_mesh_pi/src/shared/IpcProtocol.h for the wire format) or from
// the built-in demo data when the daemon isn't reachable.
//
// NOTE on the wire format vs. the IpcProtocol.h comment block: the daemon
// actually sends party slots as
//   {"dex","name","nick","level","sav_level","total_xp_gained","total_hours",
//    "mood","moves":[4],"dvs":[2],"stat_exp":[5]}
// i.e. "nick" (not "nickname") and NO hp / max_hp / exp. Those three are
// derived client-side here (Gen 1 stat formula from base stats + DVs + stat
// exp; exp = growth-curve minimum for the level). `derived_stats` says so.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mm {

struct PartyMon {
    uint16_t    dex = 0;            // national dex (1..151 Gen1; up to 386 from a Gen2/3 save)
    uint8_t     level = 0;          // effective level (sav_level + daycare levels gained)
    std::string nickname;           // wire "nick"
    uint16_t    moves[4] = {0, 0, 0, 0};   // Gen 1 move ids (0 = empty)
    uint16_t    hp = 0;             // daemon doesn't send current HP -> == max_hp
    uint16_t    max_hp = 0;         // derived (see header note)
    uint32_t    exp = 0;            // derived: min total EXP for `level`
    uint32_t    total_xp_gained = 0;  // XP earned in the daycare

    // Extra fields the real protocol carries:
    std::string species;            // wire "name" (upper-case species name)
    uint8_t     sav_level = 0;      // level as read from the .sav
    uint32_t    total_hours = 0;    // hours spent in the daycare
    int         mood = 0;
    uint8_t     dvs[2] = {0x88, 0x88};               // packed Gen 1 DVs (atk|def, spd|spc)
    uint16_t    stat_exp[5] = {0, 0, 0, 0, 0};       // hp, atk, def, spd, spc
    bool        derived_stats = true;  // hp/max_hp/exp computed locally, not from the wire
};

struct Neighbor {                   // wire NEIGHBORS.list[] entry
    uint32_t    node_id = 0;
    std::string short_name;         // Meshtastic short name (<=4 chars)
    std::string game_name;          // trainer name from their .sav
    int         party_count = 0;
    std::string lead_nick;
    int         lead_level = 0;
    int         lead_dex = 0;
    uint32_t    last_seen_ms = 0;   // daemon millis() when last beacon heard
};

struct DaycareStatus {
    bool        connected_to_daemon = false;
    bool        active = false;     // daycare has a party checked in
    int         neighbors = 0;      // neighbour count
    std::string last_event;
    int         last_event_xp = 0;

    // Extra fields from STATUS / NODE_INFO / BEACON_RESULT:
    bool        serial_connected = false;   // radio (or MQTT relay) open
    std::string serial_port;
    uint32_t    total_events = 0;
    std::string sav_path;           // save file the daemon is watching
    uint32_t    node_id = 0;        // our Meshtastic node id (NODE_INFO)
    std::string short_name;         // our short name (NODE_INFO)
    std::string game_name;          // our trainer name (NODE_INFO)
    int         sav_gen = 0;        // 1/2/3 from MY_WIRE_PARTY (0 = unknown)
    std::string last_beacon_result; // "ok" / reason from BEACON_RESULT
    std::vector<Neighbor> neighbor_list;
};

struct DaycareEvent {
    std::string text;
    int         xp = 0;
    int         slot = 0;           // party slot the event targets
    time_t      timestamp = 0;      // local wall-clock time received
    uint32_t    from_node = 0;      // non-zero for mesh text forwarded as an event ("from")
};

struct IncomingChallenge {
    uint32_t    node_id = 0;
    std::string trainer;
    // The daemon's CHALLENGE_RECEIVED does NOT carry a party (despite the doc).
    // GameState asks for it with GET_NEIGHBOR_PARTY and fills this in when the
    // challenger is a known neighbour; may stay empty.
    std::vector<PartyMon> party;
};

struct Achievement {
    std::string name;
    std::string description;
};

}  // namespace mm
