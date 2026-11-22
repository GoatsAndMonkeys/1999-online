#include "mesh_space.h"

#include "mm_json.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <utility>

namespace mm {
namespace {

std::string now_string(const char* format) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

std::string trimmed(const std::string& s, size_t max_bytes) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    std::string out = s.substr(a, b - a + 1);
    if (out.size() > max_bytes) {
        out.resize(max_bytes);
        // Do not leave half a UTF-8 sequence at the end.
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0xC0) out.pop_back();
    }
    return out;
}

std::vector<SpaceMon> capped(std::vector<SpaceMon> party) {
    if (party.size() > MeshSpace::kPartyMax) party.resize(MeshSpace::kPartyMax);
    return party;
}

} // namespace

MeshSpace& mesh_space() {
    static MeshSpace instance;
    return instance;
}

MeshSpace::MeshSpace() {
    seed_demo();
    // My saved page over the demo one. With no file the demo page is the
    // baseline, so only a real change ever creates space.json.
    if (!load()) saved_json_ = my_page_json();
    saved_rev_ = revision_;
}

MeshSpace::~MeshSpace() { save(); }

// ── Persistence (my page only) ──────────────────────────────────────────────

namespace {

const char* const kUpdateKindIds[] = {"gym_beaten", "ladder_move", "gym_defended",
                                      "wall_post", "new_champion", "badge"};
constexpr int kUpdateKindCount = sizeof(kUpdateKindIds) / sizeof(kUpdateKindIds[0]);

} // namespace

std::string MeshSpace::my_page_json() const {
    const Page& p = look(kMe);
    const auto q = [](const std::string& t) { return store::quote(t); };
    std::string o = "{\n  \"version\": 1,\n  \"profile\": {\"icon_dex\": " +
                    std::to_string(p.profile.icon_dex) + ", \"headline\": " + q(p.profile.headline) +
                    ", \"about\": " + q(p.profile.about) + ", \"mood\": " + q(p.profile.mood) +
                    "},\n  \"wall\": [";
    for (size_t i = 0; i < p.wall.size(); ++i) {
        o += std::string(i ? "," : "") + "\n    {\"author\": " + q(p.wall[i].author) +
             ", \"time\": " + q(p.wall[i].time) + ", \"text\": " + q(p.wall[i].text) + "}";
    }
    o += std::string(p.wall.empty() ? "" : "\n  ") + "],\n  \"gym\": {\"name\": " + q(p.gym.name) +
         ", \"badge\": " + q(p.gym.badge) + ", \"defended\": " + std::to_string(p.gym.defended) +
         ", \"lost\": " + std::to_string(p.gym.lost) + ", \"champions\": [";
    for (size_t i = 0; i < p.gym.champions.size(); ++i) {
        const Champion& c = p.gym.champions[i];
        o += std::string(i ? "," : "") + "\n    {\"name\": " + q(c.name) + ", \"turns\": " +
             std::to_string(c.turns) + ", \"date\": " + q(c.date) + ", \"party\": [";
        for (size_t k = 0; k < c.party.size(); ++k) {
            o += std::string(k ? ", " : "") + "[" + std::to_string(c.party[k].dex) + ", " +
                 std::to_string(c.party[k].level) + "]";
        }
        o += "]}";
    }
    o += std::string(p.gym.champions.empty() ? "" : "\n  ") + "]},\n  \"updates\": [";
    for (size_t i = 0; i < p.updates.size(); ++i) {
        const int kind = static_cast<int>(p.updates[i].kind);
        o += std::string(i ? "," : "") + "\n    {\"kind\": " +
             q(kind >= 0 && kind < kUpdateKindCount ? kUpdateKindIds[kind] : "") + ", \"time\": " +
             q(p.updates[i].time) + ", \"text\": " + q(p.updates[i].text) + "}";
    }
    o += std::string(p.updates.empty() ? "" : "\n  ") + "]\n}\n";
    return o;
}

bool MeshSpace::load() {
    JsonValue doc;
    if (store::load_json(store::path("space.json"), doc) != store::Load::Ok) return false;
    Page& p = page(kMe);
    const JsonValue& pr = doc["profile"];
    if (pr.is_object()) {
        const int64_t dex = pr.get_int("icon_dex");
        p.profile.icon_dex = dex > 0 && dex <= 0xffff ? static_cast<uint16_t>(dex) : 0;
        if (pr.has("headline")) p.profile.headline = pr.get_string("headline");
        if (pr.has("about")) p.profile.about = pr.get_string("about");
        if (pr.has("mood")) p.profile.mood = pr.get_string("mood");
    }
    if (doc["wall"].is_array()) {
        p.wall.clear();
        for (const JsonValue& j : doc["wall"].items()) {
            if (!j.is_object() || j.get_string("text").empty()) continue;
            p.wall.push_back({j.get_string("author"), j.get_string("text"), j.get_string("time")});
        }
        if (p.wall.size() > kWallMax) p.wall.erase(p.wall.begin(), p.wall.end() - static_cast<long>(kWallMax));
    }
    const JsonValue& g = doc["gym"];
    if (g.is_object()) {
        if (g.has("name")) p.gym.name = g.get_string("name");
        if (g.has("badge")) p.gym.badge = g.get_string("badge");
        p.gym.defended = static_cast<int>(g.get_int("defended", p.gym.defended));
        p.gym.lost = static_cast<int>(g.get_int("lost", p.gym.lost));
        if (g["champions"].is_array()) {
            p.gym.champions.clear();
            for (const JsonValue& j : g["champions"].items()) {
                Champion c;
                c.name = j.get_string("name");
                c.turns = static_cast<int>(j.get_int("turns"));
                c.date = j.get_string("date");
                if (c.name.empty() || c.turns <= 0) continue;
                for (const JsonValue& m : j["party"].items()) {
                    const int64_t dex = m[0].as_int(), level = m[1].as_int();
                    if (dex <= 0 || dex > 0xffff || level < 0 || level > 255) continue;
                    c.party.push_back({static_cast<uint16_t>(dex), static_cast<uint8_t>(level)});
                }
                c.party = capped(std::move(c.party));
                p.gym.champions.push_back(std::move(c));
            }
            sort_ladder(p.gym);
        }
    }
    if (doc["updates"].is_array()) {
        p.updates.clear();
        for (const JsonValue& j : doc["updates"].items()) {
            const std::string kind = j.get_string("kind"), text = j.get_string("text");
            int k = 0;
            while (k < kUpdateKindCount && kind != kUpdateKindIds[k]) ++k;
            if (k == kUpdateKindCount || text.empty()) continue;
            p.updates.push_back({static_cast<UpdateKind>(k), text, j.get_string("time")});
        }
        if (p.updates.size() > kUpdatesMax) {
            p.updates.erase(p.updates.begin(), p.updates.end() - static_cast<long>(kUpdatesMax));
        }
    }
    saved_json_ = my_page_json();   // as loaded: an unchanged page is never rewritten
    ++revision_;
    return true;
}

bool MeshSpace::save() {
    save_armed_ = false;
    saved_rev_ = revision_;
    if (!store::enabled()) return false;
    std::string text = my_page_json();
    if (text == saved_json_) return true;
    if (!store::write_atomic(store::path("space.json"), text)) return false;
    saved_json_ = std::move(text);
    return true;
}

void MeshSpace::tick(uint32_t now_ms) {
    if (revision_ == saved_rev_) {
        save_armed_ = false;
    } else if (!save_armed_) {
        save_armed_ = true;
        save_since_ms_ = now_ms;
    } else if (now_ms - save_since_ms_ >= 1000) {
        save();
    }
}

// ── Read ────────────────────────────────────────────────────────────────────

bool MeshSpace::known(uint32_t node) const { return pages_.count(node) != 0; }

const MeshSpace::Page& MeshSpace::look(uint32_t node) const {
    const auto it = pages_.find(node);
    if (it != pages_.end()) return it->second;
    char id[16];
    std::snprintf(id, sizeof(id), "!%08x", static_cast<unsigned>(node));
    blank_ = Page{};
    blank_.profile.name = id;
    blank_.profile.about = "This node has not shared a Mesh Space yet.";
    blank_.synced = "not synced yet";
    return blank_;
}

MeshSpace::Page& MeshSpace::page(uint32_t node) {
    const auto it = pages_.find(node);
    if (it != pages_.end()) return it->second;
    Page p = look(node);          // start from the empty page (name = node id)
    p.profile.about.clear();
    return pages_[node] = std::move(p);
}

const SpaceProfile& MeshSpace::profile(uint32_t node) const { return look(node).profile; }
const std::vector<WallPost>& MeshSpace::wall(uint32_t node) const { return look(node).wall; }
const SpaceGym& MeshSpace::gym(uint32_t node) const { return look(node).gym; }
const std::vector<SpaceUpdate>& MeshSpace::updates(uint32_t node) const { return look(node).updates; }

std::string MeshSpace::render(uint32_t node, const SpaceUpdate& u) const {
    const bool mine = node == kMe;
    const std::string& name = profile(node).name;
    const std::pair<const char*, std::string> tokens[] = {
        {"{You are}", mine ? "You are" : name + " is"},
        {"{You}", mine ? "You" : name},
        {"{Your}", mine ? "Your" : name + "'s"},
        {"{your}", mine ? "your" : name + "'s"},
    };
    std::string out = u.text;
    for (const auto& t : tokens) {
        const std::string key = t.first;
        for (size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + t.second.size())) {
            out.replace(at, key.size(), t.second);
        }
    }
    return out;
}

std::string MeshSpace::synced(uint32_t node) const { return look(node).synced; }

// ── Actions ─────────────────────────────────────────────────────────────────

bool MeshSpace::post(uint32_t node, const std::string& author, const std::string& text) {
    WallPost p;
    p.author = author;
    p.text = trimmed(text, kPostMax);
    if (p.text.empty()) return false;
    p.time = now_string("%b %d %I:%M %p");
    const bool by_owner = author == profile(node).name;
    add_post(node, p);
    if (!by_owner) add_update(node, UpdateKind::WallPost, author + " wrote on {your} wall", p.time);
    return true;
}

void MeshSpace::add_post(uint32_t node, const WallPost& p) {
    auto& w = page(node).wall;
    w.push_back(p);
    if (w.size() > kWallMax) w.erase(w.begin(), w.begin() + static_cast<long>(w.size() - kWallMax));
    ++revision_;
}

void MeshSpace::sort_ladder(SpaceGym& g) {
    std::stable_sort(g.champions.begin(), g.champions.end(),
                     [](const Champion& a, const Champion& b) { return a.turns < b.turns; });
    // One entry per person: the first (best) one wins.
    std::vector<Champion> out;
    for (auto& c : g.champions) {
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [&](const Champion& o) { return o.name == c.name; });
        if (!dup) out.push_back(std::move(c));
    }
    g.champions = std::move(out);
}

bool MeshSpace::record_win(uint32_t node, const std::string& name, int turns,
                           const std::vector<SpaceMon>& party, const std::string& date) {
    if (name.empty() || turns <= 0) return false;
    SpaceGym& g = page(node).gym;
    for (const Champion& c : g.champions) {
        if (c.name == name && c.turns <= turns) return false;   // not a better run
    }
    std::vector<std::string> before;                 // names by old rank
    for (const Champion& c : g.champions) before.push_back(c.name);
    g.champions.erase(std::remove_if(g.champions.begin(), g.champions.end(),
                                     [&](const Champion& c) { return c.name == name; }),
                      g.champions.end());
    Champion c;
    c.name = name;
    c.turns = turns;
    c.date = date.empty() ? now_string("%b %d") : date;
    c.party = capped(party);
    g.champions.push_back(std::move(c));
    sort_ladder(g);
    ++revision_;

    // Feed: the win itself, then everyone whose rank changed because of it.
    const auto rank_in = [](const std::vector<std::string>& names, const std::string& who) {
        const auto it = std::find(names.begin(), names.end(), who);
        return it == names.end() ? 0 : static_cast<int>(it - names.begin()) + 1;
    };
    std::vector<std::string> after;
    for (const Champion& ch : g.champions) after.push_back(ch.name);
    add_update(node, UpdateKind::GymBeaten,
               name + " beat {your} gym in " + std::to_string(turns) + " turns");
    for (size_t i = 0; i < after.size(); ++i) {
        const int now = static_cast<int>(i) + 1, was = rank_in(before, after[i]);
        if (was == 0 || was == now) continue;        // new on the ladder, or unmoved
        add_update(node, UpdateKind::LadderMove,
                   after[i] + (now < was ? " moved up to #" : " dropped to #") + std::to_string(now) +
                       " on the champions ladder");
    }
    if (node != kMe && name == my_name()) {
        const int now = rank_in(after, name);
        if (rank_in(before, name) != now) {
            add_update(kMe, UpdateKind::NewChampionElsewhere,
                       "{You are} now #" + std::to_string(now) + " on " + profile(node).name + "'s ladder");
        }
    }
    return true;
}

GymChallenge MeshSpace::record_challenge(uint32_t node, const std::string& name, bool won,
                                         int turns, const std::vector<SpaceMon>& party) {
    GymChallenge r;
    r.node = node;
    r.won = won;
    r.turns = std::max(1, turns);
    if (name.empty() || name == profile(node).name) return r;    // your own gym: no fight
    const bool me = name == my_name();

    SpaceGym& g = page(node).gym;       // std::map: stays valid while other pages are added
    const auto entry = [&]() -> std::pair<int, int> {            // {rank, turns} of `name`
        for (size_t i = 0; i < g.champions.size(); ++i) {
            if (g.champions[i].name == name) return {static_cast<int>(i) + 1, g.champions[i].turns};
        }
        return {0, 0};
    };
    r.old_rank = entry().first;
    if (won) {
        ++g.lost;
        r.new_best = record_win(node, name, r.turns, party);
        if (!r.new_best) {
            // Beaten again, but not faster than their ladder entry: news, no ladder change.
            add_update(node, UpdateKind::GymBeaten,
                       name + " beat {your} gym again in " + std::to_string(r.turns) + " turns");
        }
        if (r.new_best && r.old_rank == 0 && !g.badge.empty()) {
            r.badge = true;
            if (me && node != kMe) add_update(kMe, UpdateKind::BadgeEarned, "{You} earned the " + g.badge);
        }
    } else {
        ++g.defended;
        add_update(node, UpdateKind::GymDefended, "{Your} gym held off " + name);
    }
    r.rank = entry().first;
    r.best_turns = entry().second;
    ++revision_;
    // TODO(mesh): this only changes the local copy of `node`'s page. When I am
    // the challenger, send the result (name, won, turns, party) to the gym
    // owner over the radio here; the owner's backend then calls
    // record_challenge() on its own page and re-broadcasts the gym, and
    // set_gym()/add_update() bring everybody else's copy in line.
    if (me) {
        r.serial = ++challenge_serial_;
        last_challenge_ = r;
    }
    return r;
}

void MeshSpace::set_icon(uint32_t node, uint16_t dex) {
    page(node).profile.icon_dex = dex;
    ++revision_;
}

void MeshSpace::add_update(uint32_t node, UpdateKind kind, const std::string& text,
                           const std::string& time) {
    if (text.empty()) return;
    auto& u = page(node).updates;
    u.push_back({kind, text, time.empty() ? now_string("%b %d %I:%M %p") : time});
    if (u.size() > kUpdatesMax) u.erase(u.begin(), u.begin() + static_cast<long>(u.size() - kUpdatesMax));
    ++revision_;
}

void MeshSpace::set_leader_party(uint32_t node, const std::vector<SpaceMon>& party) {
    page(node).gym.party = capped(party);
    ++revision_;
}

// ── Backend feed ────────────────────────────────────────────────────────────

void MeshSpace::set_profile(uint32_t node, const SpaceProfile& p) {
    SpaceProfile& dst = page(node).profile;
    dst = p;
    if (dst.top8.size() > kTop) dst.top8.resize(kTop);
    ++revision_;
}

void MeshSpace::set_wall(uint32_t node, std::vector<WallPost> posts) {
    if (posts.size() > kWallMax) posts.erase(posts.begin(), posts.end() - static_cast<long>(kWallMax));
    page(node).wall = std::move(posts);
    ++revision_;
}

void MeshSpace::set_gym(uint32_t node, const SpaceGym& g) {
    SpaceGym& dst = page(node).gym;
    dst = g;
    dst.party = capped(dst.party);
    for (Champion& c : dst.champions) c.party = capped(c.party);
    sort_ladder(dst);
    ++revision_;
}

void MeshSpace::set_synced(uint32_t node, const std::string& text) {
    page(node).synced = text;
    ++revision_;
}

// ── Demo data ───────────────────────────────────────────────────────────────

void MeshSpace::seed_demo() {
    {   // me
        Page& p = pages_[kMe];
        p.profile = {"xXmeshKid03Xx", "~ 915 MHz 4 life ~",
                     "just a kid w/ a cardputer + a dream. will battle 4 snacks. LoRa > wifi!!",
                     "hyper", true, 23,
                     {"KD2ABC-m", "Trailhead-R", "Ridge_Gym", "base_stn",
                      "ash_k3tchup", "N0CALL-7", "sk8rmesh", "Tom"}};
        p.wall = {
            {"Tom", "welcome 2 mesh space :)", "Nov 12 09:00 AM"},
            {"Ridge_Gym", "rematch?? my gym is waiting", "Nov 18 06:41 PM"},
            {"KD2ABC-m", "sandwich is in the mail lol", "Nov 20 11:09 AM"},
        };
        p.gym.name = "Basement Gym";
        p.gym.badge = "Solder Badge";
        p.gym.party = {{25, 31}, {6, 36}, {130, 30}, {94, 29}, {143, 28}, {65, 27}};
        p.gym.defended = 6;
        p.gym.lost = 2;
        p.gym.champions = {
            {"Ridge_Gym", 38, "Nov 16", {{76, 36}, {95, 35}, {112, 34}, {141, 33}, {142, 33}, {34, 32}}},
            {"KD2ABC-m", 47, "Nov 09", {{149, 34}, {59, 33}, {131, 31}, {103, 30}, {26, 29}, {123, 28}}},
        };
        using K = UpdateKind;
        p.updates = {
            {K::GymBeaten, "KD2ABC-m beat {your} gym in 47 turns", "Nov 09 05:12 PM"},
            {K::NewChampionElsewhere, "{You are} now #1 on Trailhead-R's ladder", "Nov 12 04:30 PM"},
            {K::BadgeEarned, "{You} earned the Trail Badge", "Nov 12 04:30 PM"},
            {K::GymBeaten, "Ridge_Gym beat {your} gym in 38 turns", "Nov 16 08:44 PM"},
            {K::LadderMove, "KD2ABC-m dropped to #2 on the champions ladder", "Nov 16 08:44 PM"},
            {K::WallPost, "Ridge_Gym wrote on {your} wall", "Nov 18 06:41 PM"},
            {K::GymDefended, "{Your} gym held off sk8rmesh", "Nov 19 03:20 PM"},
            {K::WallPost, "KD2ABC-m wrote on {your} wall", "Nov 20 11:09 AM"},
        };
    }
    {   // KD2ABC-m
        Page& p = pages_[kDemoNode];
        p.profile = {"KD2ABC-m", "~*~ summit or bust ~*~",
                     "ham op, trail runner. node lives on my pack. msg me 4 trail conditions!!",
                     "stoked", true, 41,
                     {"xXmeshKid03Xx", "Trailhead-R", "Ridge_Gym", "base_stn",
                      "ash_k3tchup", "W2XYZ", "peak_bagr", "Tom"}};
        p.wall = {
            {"Ridge_Gym", "gg on the ridge. ur dragonite is unfair", "Nov 17 08:15 PM"},
            {"W2XYZ", "73 from the valley, copy u 5x9", "Nov 18 07:30 AM"},
            {"base_stn", "great signal from ur new antenna", "Nov 19 04:47 PM"},
            {"xXmeshKid03Xx", "u still owe me a sandwich lol", "Nov 20 09:26 AM"},
            {"Trailhead-R", "thx 4 the add!! c u at the ridge sat", "Nov 20 11:02 AM"},
        };
        p.synced = "synced 2 min ago, 2 hops";
        p.gym.name = "Summit Gym";
        p.gym.badge = "Summit Badge";
        p.gym.party = {{149, 34}, {59, 33}, {131, 31}, {103, 30}, {26, 29}, {123, 28}};
        p.gym.defended = 14;
        p.gym.lost = 3;
        p.gym.champions = {
            {"Trailhead-R", 31, "Nov 14", {{9, 36}, {65, 34}, {128, 33}, {135, 32}, {38, 31}, {87, 30}}},
            {"ash_k3tchup", 44, "Nov 08", {{25, 38}, {6, 36}, {1, 30}, {7, 30}, {12, 29}, {17, 28}}},
            {"Ridge_Gym", 52, "Oct 31", {{76, 36}, {95, 35}, {112, 34}, {141, 33}, {142, 33}, {34, 32}}},
        };
        using K = UpdateKind;
        p.updates = {
            {K::GymBeaten, "Ridge_Gym beat {your} gym in 52 turns", "Oct 31 09:05 PM"},
            {K::NewChampionElsewhere, "{You are} now #1 on Ridge_Gym's ladder", "Nov 02 01:15 PM"},
            {K::BadgeEarned, "{You} earned the Ridge Badge", "Nov 02 01:15 PM"},
            {K::GymBeaten, "Trailhead-R beat {your} gym in 31 turns", "Nov 14 06:20 PM"},
            {K::LadderMove, "ash_k3tchup moved up to #2 on the champions ladder", "Nov 16 10:02 AM"},
            {K::GymDefended, "{Your} gym held off W2XYZ", "Nov 18 07:55 AM"},
            {K::WallPost, "base_stn wrote on {your} wall", "Nov 19 04:47 PM"},
            {K::WallPost, "Trailhead-R wrote on {your} wall", "Nov 20 11:02 AM"},
        };
    }
    {   // Ridge_Gym
        Page& p = pages_[kDemoRidge];
        p.profile = {"Ridge_Gym", "rock solid since 1999",
                     "solar node on the north ridge. gym is open 24/7, bring water types (pls dont).",
                     "unbothered", true, 17,
                     {"KD2ABC-m", "xXmeshKid03Xx", "Trailhead-R", "base_stn"}};
        p.wall = {
            {"KD2ABC-m", "nice battery uptime!!", "Nov 15 02:12 PM"},
            {"xXmeshKid03Xx", "ur onix hits 2 hard :(", "Nov 19 08:03 PM"},
        };
        p.synced = "synced 11 min ago, 1 hop";
        p.gym.name = "Ridge Gym";
        p.gym.badge = "Ridge Badge";
        p.gym.party = {{76, 36}, {95, 35}, {112, 34}, {141, 33}, {142, 33}, {34, 32}};
        p.gym.defended = 22;
        p.gym.lost = 1;
        p.gym.champions = {
            {"KD2ABC-m", 58, "Nov 02", {{149, 34}, {59, 33}, {131, 31}, {103, 30}, {26, 29}, {123, 28}}},
        };
    }
    {   // Trailhead-R
        Page& p = pages_[kDemoTrail];
        p.profile = {"Trailhead-R", "last repeater b4 the woods",
                     "router node at the trailhead lot. leave a msg on my wall if the gate is locked.",
                     "sleepy", false, 9,
                     {"KD2ABC-m", "Ridge_Gym", "xXmeshKid03Xx"}};
        p.wall = {
            {"KD2ABC-m", "gate was open at 6am fyi", "Nov 20 06:10 AM"},
        };
        p.synced = "synced 3 hr ago, 3 hops";
        p.gym.name = "Trailhead Gym";
        p.gym.badge = "Trail Badge";
        p.gym.party = {{9, 36}, {65, 34}, {128, 33}, {135, 32}, {38, 31}, {87, 30}};
        p.gym.defended = 4;
        p.gym.lost = 1;
        p.gym.champions = {
            {"xXmeshKid03Xx", 40, "Nov 12", {{25, 31}, {6, 36}, {130, 30}, {94, 29}, {143, 28}, {65, 27}}},
        };
    }
}

} // namespace mm
