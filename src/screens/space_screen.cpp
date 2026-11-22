// space_screen.cpp — "Mesh Space": a tiny Friendster-style personal page per
// mesh node (Ctx::peer, 0 = mine). Tabs: Profile | Wall (n) | Gym | Updates.
// Data comes from backend/mesh_space.h (demo pages until the mesh fills it).
//
// Gym tab: Challenge starts a BattleRequest::Kind::PlayerGym battle against
// the team the owner posted (asynchronous: the owner need not be online, the
// AI plays their team). The battle screen reports the outcome to
// MeshSpace::record_challenge(); when it pops, on_show() picks the result up
// and the Gym tab shows it until the tab is left.
#include "screens.h"

#include "game_state.h"
#include "mesh_space.h"
#include "router.h"
#include "widgets.h"
#include "win.h"

#include <algorithm>

namespace mm::ui {
namespace {

constexpr int kLine = 12;                // text row pitch

enum Tab { kProfile, kWall, kGym, kUpdates, kTabCount };

class SpaceScreen final : public Screen {
public:
    explicit SpaceScreen(Ctx& ctx) : Screen(ctx) {
        set_node(ctx.peer);
        seen_challenge_ = mesh_space().last_challenge().serial;   // only fights from here on
    }

    const char* title() const override { return title_.c_str(); }
    bool windowed() const override { return true; }

    // First content row: under the tab strip on the Wall, the top elsewhere.
    int top_y() const { return tab_ == kWall ? 16 : 0; }

    std::string hints() const override {
        switch (tab_) {
            case kProfile:
                if (picker_) return "Arrows:move  Enter:pick  D:default  Esc:cancel";
                return mine() ? "I:change icon  Tab:next tab  Esc:back" : "I:send IM  Tab:next tab  Esc:back";
            case kUpdates:
                return "Up/Dn:scroll  Tab:next tab  Esc:back";
            case kWall:
                return "Type + Enter:post  Up/Dn:scroll  Tab:next";
            default:
                if (ladder_) return "Up/Dn:champion  G/Esc:gym  Tab:next";
                if (has_result_) return "Enter:rematch  L:champions  Tab:next";
                return mine() ? "Enter:update party  L:champions  Tab:next"
                              : "Enter:challenge  L:champions  Tab:next";
        }
    }

    void build(lv_obj_t* body) override {
        const MeshSpace& ms = model();
        seen_rev_ = ms.revision();
        list_.reset();
        input_.reset();
        counter_ = nullptr;

        if (tab_ == kProfile && picker_) {     // full-body view, no tab strip
            build_picker(body);
            return;
        }
        const int posts = static_cast<int>(ms.wall(node_).size());
        // The tabs are on the soft keys, except on the Wall (the number keys
        // type there), which keeps its strip on top.
        if (tab_ == kWall) {
            win::tabs(body, 0, 0, {"Profile", fmt("Wall (%d)", posts), "Gym", "Updates"}, tab_);
            win::frame(body, 0, 14, kClientW, 1, win::Edge::None, skin().lo);   // rule under it
        }

        if (tab_ == kProfile) build_profile(body);
        else if (tab_ == kWall) build_wall(body);
        else if (tab_ == kUpdates) build_updates(body);
        else if (ladder_) build_ladder(body);
        else build_gym(body);
    }

    void tick(uint32_t) override {
        if (model().revision() != seen_rev_) ctx_.router.rebuild();
    }

    // Back from a gym battle: show its result on the Gym tab. (The model's
    // revision changed too, so the next tick() rebuilds the body.)
    void on_show() override {
        const GymChallenge& c = mesh_space().last_challenge();
        if (c.serial == seen_challenge_) return;
        seen_challenge_ = c.serial;
        if (c.node != node_ || mine()) return;
        result_ = c;
        has_result_ = true;
        tab_ = kGym;
        ladder_ = false;
    }

    // Soft keys: the four tabs on keys 4..7, the tab's action on key 8, the
    // champions ladder on the key 9 corner. None on the Wall tab: the number
    // keys type there.
    bool typing() const override { return tab_ == kWall && input_ != nullptr; }

    SoftKeys softkeys() const override {
        SoftKeys k;
        if (tab_ == kWall) return k;
        k.left.text = MM_SOFT_BACK;
        if (tab_ == kProfile && picker_) {
            k.set(0, "Pick").set(1, "Default");
            return k;
        }
        k.set(0, "Profile").set(1, "Wall").set(2, "Gym").set(3, "Updates");
        k.selected = tab_;
        if (tab_ == kProfile) k.set(4, mine() ? "Icon" : "IM");
        if (tab_ == kGym) {
            // Key 9 toggles the champions ladder; Gym (key 6) also leaves it.
            if (!ladder_) k.set(4, has_result_ ? "Rematch" : mine() ? "Update" : "Fight");
            k.right.text = LV_SYMBOL_LIST;
        }
        return k;
    }

    bool on_key(const KeyEvent& in) override {
        KeyEvent e = in;
        const int slot = tab_ == kWall ? kSoftNone : softkey_slot(in);
        if (slot == kSoftLeft) {
            if (!picker_) return false;          // Router: back
            e.key = Key::B;
        } else if (tab_ == kProfile && picker_ && slot != kSoftNone) {
            if (slot == 0) e.key = Key::A;
            else if (slot == 1) e.ch = 'd';
            else return true;
        } else if (slot >= 0 && slot < 4) {
            if (slot != tab_) return switch_tab(slot - tab_);
            if (tab_ != kGym || !ladder_) return true;
            e.key = Key::Char;                   // Gym again: leave the ladder
            e.ch = 'g';
        } else if (slot == 4) {
            e.key = Key::Char;
            if (tab_ == kProfile) e.ch = mine() ? 'c' : 'i';
            else if (tab_ == kGym && !ladder_) e.ch = mine() ? 'u' : 'c';
            else return true;
        } else if (slot == kSoftRight) {
            if (tab_ != kGym) return true;
            e.key = Key::Char;
            e.ch = ladder_ ? 'g' : 'l';
        }
        if (tab_ == kProfile && picker_) return picker_key(e);
        if (e.key == Key::Tab) return switch_tab(1);
        if (tab_ == kWall) return wall_key(e);
        if (e.key == Key::Left) return switch_tab(-1);
        if (e.key == Key::Right) return switch_tab(1);
        if (tab_ == kProfile) {
            if (e.key == Key::Char && e.ch == 'i' && !mine()) {
                ctx_.peer = node_;
                ctx_.router.push(ScreenId::Im);
                return true;
            }
            if (e.key == Key::Char && e.ch == 'i' && mine()) {
                const int cur = icon_dex();
                picker_ = true;
                picker_sel_ = cur >= 1 && cur <= kIconCount ? cur - 1 : 0;
                ctx_.router.rebuild();
                return true;
            }
            return false;
        }
        if (tab_ == kUpdates) {
            if (e.key != Key::Up && e.key != Key::Down) return false;
            const int n = static_cast<int>(model().updates(node_).size());
            const int top = std::clamp(feed_top_ + (e.key == Key::Down ? 1 : -1), 0,
                                       std::max(0, n - kFeedRows));
            if (top != feed_top_) {
                feed_top_ = top;
                ctx_.router.rebuild();
            }
            return true;
        }
        return gym_key(e);
    }

    // ── Demo states (screenshots) ───────────────────────────────────────────
    // 9-13: back from a fight at KD2ABC-m's gym, on a private copy of the model
    // (27-turn win: Gym tab, ladder, their Updates, my Updates; then a loss).
    int demo_state_count() const override { return 14; }
    const char* demo_state_name(int i) const override {
        static const char* const names[] = {"profile", "wall", "gym", "champions", "my-profile",
                                            "my-gym", "updates", "my-updates", "icon-picker",
                                            "gym-won", "gym-won-champions", "gym-won-updates",
                                            "gym-won-my-updates", "gym-lost"};
        return (i >= 0 && i < 14) ? names[i] : "main";
    }
    void set_demo_state(int i) override {
        demo_model_.reset();
        has_result_ = false;
        if (i >= 9) {
            demo_model_ = std::make_unique<MeshSpace>(mesh_space());
            std::vector<SpaceMon> team;
            for (const PartyMon& m : ctx_.game.party()) team.push_back({m.dex, m.level});
            result_ = demo_model_->record_challenge(MeshSpace::kDemoNode, demo_model_->my_name(),
                                                    i != 13, 27, team);
            has_result_ = true;
        }
        // 0-3, 6 and 9-13 show the KD2ABC-m demo page whatever Ctx::peer is; the rest are mine.
        set_node((i <= 3 || i == 6 || (i >= 9 && i != 12)) ? MeshSpace::kDemoNode : MeshSpace::kMe);
        tab_ = i == 1 ? kWall
             : (i == 2 || i == 3 || i == 5 || i == 9 || i == 10 || i == 13) ? kGym
             : (i == 6 || i == 7 || i == 11 || i == 12) ? kUpdates : kProfile;
        ladder_ = i == 3 || i == 10;
        ladder_sel_ = i == 10 ? std::max(0, result_.rank - 1) : 0;
        wall_top_ = 0;
        feed_top_ = 0;
        picker_ = i == 8;
        picker_sel_ = 24;                     // Pikachu
        draft_ = i == 1 ? "see u saturday!!" : "";
    }

private:
    bool mine() const { return node_ == MeshSpace::kMe; }

    // What the page is drawn from: the shared model, or a demo state's copy.
    const MeshSpace& model() const { return demo_model_ ? *demo_model_ : mesh_space(); }

    void set_node(uint32_t node) {
        node_ = node;
        title_ = model().profile(node_).name + "'s Mesh Space";
    }

    bool switch_tab(int step) {
        if (input_) draft_ = input_->value();
        tab_ = (tab_ + step + kTabCount) % kTabCount;
        ladder_ = false;
        has_result_ = false;                  // the fight result shows until the tab is left
        picker_ = false;
        ctx_.router.rebuild();
        return true;
    }

    // ── Profile ─────────────────────────────────────────────────────────────
    void build_profile(lv_obj_t* body) {
        const Skin& s = skin();
        const MeshSpace& ms = model();
        const SpaceProfile& p = ms.profile(node_);
        const SpaceGym& g = ms.gym(node_);

        // Left column: buddy icon, name, headline, mood, presence.
        const int lw = 100;
        // The icon is drawn at 1x. The field is as wide as a Gen 2 front (56px)
        // but only 50 tall (the column below has no line to spare), so the
        // tallest sprites lose up to 4 rows top and bottom, like any avatar
        // cropped to its frame. `clip` keeps the sprite off the bevel.
        const int iw = 60;
        lv_obj_t* icon = win::field(body, 0, top_y() + 1, iw, 50);
        (void)g;
        if (icon_dex() > 0) {
            lv_obj_update_layout(icon);
            const int cw = lv_obj_get_content_width(icon), ch = lv_obj_get_content_height(icon);
            lv_obj_t* clip = box(icon, 0, 0, cw, ch);
            sprite_box(clip, icon_dex(), (cw - 56) / 2, (ch - 56) / 2, 56, 56);
        } else {
            lv_obj_center(win::text(icon, "?", 0, 0, s.dim, 0, true));
        }
        // Presence sits beside the icon.
        if (p.online) {
            win::text(body, "Online", iw + 3, top_y() + 14, color::kGood, lw - iw - 3, true);
            win::text(body, "Now!", iw + 3, top_y() + 26, color::kGood, lw - iw - 3, true);
        } else {
            win::text(body, "Offline", iw + 3, top_y() + 20, s.dim, lw - iw - 3);
        }

        int y = top_y() + 53;
        win::text(body, p.name, 0, y, s.text, lw, true);
        y += kLine;
        if (!p.headline.empty()) {
            // Up to two lines.
            lv_obj_t* h = win::text_wrap(body, p.headline, 0, y, lw, s.them);
            lv_obj_set_style_text_line_space(h, 0, 0);
            lv_obj_update_layout(h);
            const int hh = std::min<int>(lv_obj_get_height(h), 2 * kLine + 1);
            lv_obj_set_height(h, hh);
            y += hh > kLine + 2 ? 2 * kLine : kLine;
        }
        win::text(body, "Mood: " + (p.mood.empty() ? std::string("-") : p.mood), 0, y, s.text, lw);

        // Right column: about me, top 8.
        const int rx = lw + 4, rw = kClientW - rx;
        lv_obj_t* about = win::field(body, rx, top_y() + 1, rw, 40);
        win::text(about, "About me", 3, 1, s.them, 0, true);
        lv_obj_t* at = win::text_wrap(about, p.about.empty() ? "(nothing here yet)" : p.about,
                                      3, 13, rw - 7, p.about.empty() ? s.dim : s.field_text);
        lv_obj_set_style_text_line_space(at, 0, 0);
        lv_obj_set_height(at, 25);            // two lines; the rest is cut off

        y = top_y() + 43;
        win::text(body, fmt("Top %d", static_cast<int>(MeshSpace::kTop)), rx, y, s.text, 0, true);
        {
            lv_obj_t* fc = win::text(body, fmt("%d friend%s", p.friends, p.friends == 1 ? "" : "s"),
                                     rx, y, s.dim, 0, false);
            lv_obj_update_layout(fc);
            lv_obj_set_x(fc, kClientW - 1 - lv_obj_get_width(fc));
        }
        if (p.top8.empty()) {
            win::text(body, "No friends added yet.", rx, y + kLine, s.dim, rw);
        } else {
            const int cw = rw / 2;
            const int n = std::min<int>(static_cast<int>(p.top8.size()), MeshSpace::kTop);
            for (int i = 0; i < n; ++i) {
                const int cx = rx + (i % 2) * cw, cy = y + kLine + 1 + (i / 2) * kLine;
                win::text(body, fmt("%d.", i + 1), cx, cy, s.dim, 12);
                win::text(body, p.top8[static_cast<size_t>(i)], cx + 13, cy, s.text, cw - 15);
            }
        }
    }

    // Buddy icon: the picked one, else the gym lead (0 = none).
    int icon_dex() const {
        const MeshSpace& ms = model();
        const int picked = ms.profile(node_).icon_dex;
        if (picked > 0) return picked;
        const SpaceGym& g = ms.gym(node_);
        return g.party.empty() ? 0 : g.party[0].dex;
    }

    // ── Buddy icon picker (my profile) ──────────────────────────────────────
    // Monster sprites only: the shared game code has no human trainer art.
    static constexpr int kIconCount = 151;    // Gen 1 dex 1..151
    static constexpr int kIconCols = 9;
    static constexpr int kIconRows = 3;       // visible
    static constexpr int kIconCell = 34;      // 32px sprite + 1px margin

    void build_picker(lv_obj_t* body) {
        const Skin& s = skin();
        const int dex = picker_sel_ + 1;
        win::text(body, "Pick a buddy icon", 1, 0, s.text, 0, true);
        lv_obj_t* nm = win::text(body, fmt("#%03d ", dex) + title_case(species_name(dex)), 0, 0, s.dim, 0, false);
        lv_obj_update_layout(nm);
        lv_obj_set_x(nm, kClientW - 1 - lv_obj_get_width(nm));

        const int fy = 14, fh = kIconRows * kIconCell + 3;           // 105
        lv_obj_t* f = win::field(body, 0, fy, kClientW, fh);
        const int rows = (kIconCount + kIconCols - 1) / kIconCols;
        const int row = picker_sel_ / kIconCols;
        if (row < picker_top_) picker_top_ = row;
        if (row >= picker_top_ + kIconRows) picker_top_ = row - kIconRows + 1;
        picker_top_ = std::clamp(picker_top_, 0, std::max(0, rows - kIconRows));
        for (int r = 0; r < kIconRows; ++r) {
            for (int c = 0; c < kIconCols; ++c) {
                const int i = (picker_top_ + r) * kIconCols + c;
                if (i >= kIconCount) break;
                const int x = 2 + c * kIconCell, y = 2 + r * kIconCell;
                if (i == picker_sel_) win::frame(f, x, y, kIconCell, kIconCell, win::Edge::None, s.sel);
                sprite_box(f, i + 1, x + 1, y + 1, 32, 32);
            }
        }
        const int track = fh - 2;
        const int th = std::max(6, track * kIconRows / rows);
        const int ty = 1 + (track - th) * picker_top_ / std::max(1, rows - kIconRows);
        win::frame(f, kClientW - 3, ty, 2, th, win::Edge::None, s.dim);
    }

    bool picker_key(const KeyEvent& e) {
        int sel = picker_sel_;
        switch (e.key) {
            case Key::Left: if (sel > 0) --sel; break;
            case Key::Right: if (sel < kIconCount - 1) ++sel; break;
            case Key::Up: if (sel >= kIconCols) sel -= kIconCols; break;
            case Key::Down: sel = std::min(sel + kIconCols, kIconCount - 1); break;
            case Key::A:
                mesh_space().set_icon(MeshSpace::kMe, static_cast<uint16_t>(picker_sel_ + 1));
                toast("Buddy icon: " + title_case(species_name(picker_sel_ + 1)), 1400);
                picker_ = false;
                break;
            case Key::B:
                picker_ = false;
                break;
            case Key::Char:
                if (e.ch != 'd') return true;
                mesh_space().set_icon(MeshSpace::kMe, 0);
                toast("Buddy icon: your gym lead", 1400);
                picker_ = false;
                break;
            default: return true;   // Tab etc.: stay in the picker
        }
        if (sel == picker_sel_ && picker_) return true;
        picker_sel_ = sel;
        ctx_.router.rebuild();
        return true;
    }

    // ── Updates ─────────────────────────────────────────────────────────────
    static constexpr int kFeedRows = 4;       // updates visible at once

    struct KindLook { const char* label; uint32_t color; };
    static KindLook kind_look(UpdateKind k) {
        const Skin& s = skin();
        switch (k) {
            case UpdateKind::GymBeaten: return {"Gym beaten", color::kBad};
            case UpdateKind::LadderMove: return {"Ladder", s.them};
            case UpdateKind::GymDefended: return {"Gym defended", color::kGood};
            case UpdateKind::WallPost: return {"Wall", s.them};
            case UpdateKind::NewChampionElsewhere: return {"Champion", color::kGood};
            case UpdateKind::BadgeEarned: return {"Badge", color::kGood};
        }
        return {"News", s.field_text};
    }

    void build_updates(lv_obj_t* body) {
        const Skin& s = skin();
        const MeshSpace& ms = model();
        const auto& feed = ms.updates(node_);
        const int n = static_cast<int>(feed.size());
        const int fy = top_y() + 1, fh = kClientH - fy;                 // 104
        const int pitch = (fh - 2) / kFeedRows;                      // 25

        lv_obj_t* f = win::field(body, 0, fy, kClientW, fh);
        feed_top_ = std::clamp(feed_top_, 0, std::max(0, n - kFeedRows));
        if (n == 0) {
            win::text(f, mine() ? "No news yet. Go beat a gym!" : "No news from this node yet.", 4, 3, s.dim);
        }
        const int tw = kClientW - 12;
        for (int v = 0; v < kFeedRows && feed_top_ + v < n; ++v) {
            const SpaceUpdate& u = feed[static_cast<size_t>(n - 1 - (feed_top_ + v))];   // newest first
            const KindLook look = kind_look(u.kind);
            const int y = 1 + v * pitch;
            lv_obj_t* tl = win::text(f, u.time, 0, y, s.dim, 0, false);
            lv_obj_update_layout(tl);
            const int tlw = lv_obj_get_width(tl);
            lv_obj_set_x(tl, kClientW - 8 - tlw);
            win::text(f, look.label, 4, y, look.color, tw - tlw - 6, true);
            win::text(f, ms.render(node_, u), 4, y + kLine, s.field_text, tw);
        }
        if (n > kFeedRows) {
            const int track = fh - 2;
            const int th = std::max(6, track * kFeedRows / n);
            const int ty = 1 + (track - th) * feed_top_ / (n - kFeedRows);
            win::frame(f, kClientW - 3, ty, 2, th, win::Edge::None, s.dim);
        }
    }

    // ── Wall ────────────────────────────────────────────────────────────────
    static constexpr int kWallRows = 3;       // posts visible at once

    void build_wall(lv_obj_t* body) {
        const Skin& s = skin();
        const MeshSpace& ms = model();
        const auto& wall = ms.wall(node_);
        const int n = static_cast<int>(wall.size());
        const int fh = kWallRows * 2 * kLine + 3;          // 75

        lv_obj_t* f = win::field(body, 0, top_y() + 1, kClientW, fh);
        wall_top_ = std::clamp(wall_top_, 0, std::max(0, n - kWallRows));
        if (n == 0) {
            win::text(f, "No posts yet. Be the first!", 4, 3, s.dim);
        }
        const int tw = kClientW - 12;
        for (int v = 0; v < kWallRows && wall_top_ + v < n; ++v) {
            const WallPost& p = wall[static_cast<size_t>(n - 1 - (wall_top_ + v))];   // newest first
            const int y = 1 + v * 2 * kLine;
            lv_obj_t* tl = win::text(f, p.time, 0, y, s.dim, 0, false);
            lv_obj_update_layout(tl);
            const int tlw = lv_obj_get_width(tl);
            lv_obj_set_x(tl, kClientW - 8 - tlw);
            win::text(f, p.author, 4, y, p.author == ms.my_name() ? s.me : s.them, tw - tlw - 6, true);
            win::text(f, p.text, 4, y + kLine, s.field_text, tw);
        }
        if (n > kWallRows) {
            const int track = fh - 2;
            const int th = std::max(6, track * kWallRows / n);
            const int ty = 1 + (track - th) * wall_top_ / (n - kWallRows);
            win::frame(f, kClientW - 3, ty, 2, th, win::Edge::None, s.dim);
        }

        const int iy = top_y() + 1 + fh + 2;                  // 94
        const int bw = 44;
        input_ = std::make_unique<win::TextInput>(body, 0, iy, kClientW - bw - 3, 15,
                                                  static_cast<int>(MeshSpace::kPostMax));
        input_->set_placeholder(mine() ? "Write on your wall..." : "Write on this wall...");
        if (!draft_.empty()) input_->set_value(draft_);
        win::button(body, "Post", kClientW - bw, iy, bw, true, 15);

        std::string info = fmt("%d post%s", n, n == 1 ? "" : "s");
        const std::string synced = ms.synced(node_);
        if (!synced.empty()) info += " - " + synced;
        counter_ = win::text(body, "", 0, iy + 15, s.dim, 0, false);
        update_counter();
        lv_obj_update_layout(counter_);
        win::text(body, info, 1, iy + 15, s.dim, kClientW - 50);
    }

    void update_counter() {
        if (!counter_ || !input_) return;
        set_text(counter_, fmt("%d/%d", static_cast<int>(input_->value().size()),
                               static_cast<int>(MeshSpace::kPostMax)));
        lv_obj_update_layout(counter_);
        lv_obj_set_x(counter_, kClientW - 1 - lv_obj_get_width(counter_));
    }

    bool wall_key(const KeyEvent& e) {
        const int n = static_cast<int>(model().wall(node_).size());
        // Only real arrow keys navigate here: ; . , / are text.
        switch (e.raw) {
            case LV_KEY_LEFT: return switch_tab(-1);
            case LV_KEY_RIGHT: return switch_tab(1);
            case LV_KEY_UP:
            case LV_KEY_DOWN: {
                const int top = std::clamp(wall_top_ + (e.raw == LV_KEY_DOWN ? 1 : -1), 0,
                                           std::max(0, n - kWallRows));
                if (top != wall_top_) {
                    wall_top_ = top;
                    if (input_) draft_ = input_->value();
                    ctx_.router.rebuild();
                }
                return true;
            }
            case LV_KEY_ENTER:
            case '\r': {
                MeshSpace& ms = mesh_space();
                const std::string text = input_ ? input_->value() : std::string();
                // TODO(mesh): post() only updates the local model; send the post
                // to the node over the mesh once the backend exists.
                if (ms.post(node_, ms.my_name(), text)) {
                    draft_.clear();
                    wall_top_ = 0;
                    ctx_.router.rebuild();
                }
                return true;
            }
            default: break;
        }
        if (input_ && input_->on_key(e)) {
            draft_ = input_->value();
            update_counter();
            return true;
        }
        return false;   // Esc (or Backspace on an empty box): back
    }

    // ── Gym ─────────────────────────────────────────────────────────────────
    // One party member: sprite with "Lv NN" centred under it.
    static void mon_cell(lv_obj_t* parent, const SpaceMon& m, int x, int y, int cell_w,
                         int px, uint32_t fg) {
        // Box as wide as the cell (half-size sprites are up to 28px wide).
        const int bw = std::max(px, std::min(cell_w, 28));
        sprite_box(parent, m.dex, x + (cell_w - bw) / 2, y, bw, px);
        lv_obj_t* l = win::text(parent, fmt("Lv %d", m.level), x, y + px - 1, fg, 0, false);
        lv_obj_update_layout(l);
        lv_obj_set_x(l, x + (cell_w - lv_obj_get_width(l)) / 2);
    }

    void build_gym(lv_obj_t* body) {
        const Skin& s = skin();
        const MeshSpace& ms = model();
        const SpaceGym& g = ms.gym(node_);
        const std::string& owner = ms.profile(node_).name;
        const int champs = static_cast<int>(g.champions.size());

        win::text(body, "Leader's party", 1, top_y(), s.text, 0, false);
        const int fw = 116, fy = top_y() + 13, fh = kClientH - fy;      // 92
        lv_obj_t* f = win::field(body, 0, fy, fw, fh);
        if (g.party.empty()) {
            win::text_wrap(f, mine() ? "No party yet. Press Enter to copy your team."
                                     : "No party posted yet.", 4, 4, fw - 8, s.dim);
        }
        const int cw = (fw - 2) / 3, ch = (fh - 2) / 2;              // 38 x 45
        for (size_t i = 0; i < g.party.size() && i < MeshSpace::kPartyMax; ++i) {
            mon_cell(f, g.party[i], 1 + static_cast<int>(i % 3) * cw,
                     1 + static_cast<int>(i / 3) * ch, cw, 32, s.field_text);
        }

        const int rx = fw + 6, rw = kClientW - rx;                   // 190
        int y = top_y();
        win::text(body, g.name.empty() ? "No gym yet" : g.name, rx, y, s.text, rw, true);
        y += 13;
        const bool result = has_result_ && !mine() && result_.node == node_;
        if (result) {
            // Just back from a fight here: the outcome replaces the gym facts.
            const GymChallenge& r = result_;
            if (r.won) {
                win::text(body, fmt("You won in %d turns!", r.turns), rx, y, color::kGood, rw, true);
                y += 13;
                win::text(body, r.new_best ? fmt("You are #%d on the champions ladder", r.rank)
                                           : fmt("Your %d-turn run stays #%d", r.best_turns, r.rank),
                          rx, y, s.text, rw);
                y += 13;
                if (r.badge) win::text(body, "You earned the " + g.badge, rx, y, s.them, rw);
                else win::text(body, "Leader: " + owner, rx, y, s.text, rw);
            } else {
                win::text(body, "The gym held you off", rx, y, color::kBad, rw, true);
                y += 13;
                win::text(body, r.rank > 0 ? fmt("Your %d-turn run stays #%d", r.best_turns, r.rank)
                                           : std::string("Beat the leader to join the ladder"),
                          rx, y, s.text, rw);
                y += 13;
                win::text(body, "Leader: " + owner, rx, y, s.text, rw);
            }
            y += 13;
        } else {
            win::text(body, "Leader: " + owner, rx, y, s.text, rw);
            y += 13;
            if (!g.badge.empty()) win::text(body, "Prize: " + g.badge, rx, y, s.text, rw);
            y += 13;
        }
        win::text(body, fmt("Defended %d, lost %d", g.defended, g.lost), rx, y, s.text, rw);
        y += 13;
        win::text(body, "Fights run while the leader is away", rx, y, s.dim, rw);

        const std::string ladder = fmt("Champions (%d)", champs);
        if (mine()) {
            win::text(body, "This is your gym", rx, 87, s.them, 0, true);
            win::button(body, ladder, kClientW - 92, 85, 92);
            win::button(body, "Update party from my team", rx, 104, rw, true);
        } else {
            win::button(body, result ? "Rematch" : "Challenge", rx, 98, 80, true);
            win::button(body, ladder, rx + 86, 98, rw - 86);
        }
    }

    void build_ladder(lv_obj_t* body) {
        const Skin& s = skin();
        const MeshSpace& ms = model();
        const SpaceGym& g = ms.gym(node_);
        const int n = static_cast<int>(g.champions.size());

        win::text(body, (g.name.empty() ? std::string("Gym") : g.name) + " - Champions",
                  1, top_y(), s.text, 190, true);
        win::text(body, "fewest turns first", 200, top_y(), s.dim, kClientW - 200);
        const int ly = top_y() + 13, lh = kClientH - ly;                // 92
        if (n == 0) {
            lv_obj_t* f = win::field(body, 0, ly, kClientW, lh);
            win::text(f, "Nobody has beaten this gym yet.", 5, 4, s.field_text);
            if (!mine()) win::text(f, "Press G, then Challenge, to be the first.", 5, 18, s.dim);
            return;
        }

        const int lw = 146;
        list_ = std::make_unique<win::ListBox>(body, 0, ly, lw, lh);
        std::vector<win::Row> rows;
        for (int i = 0; i < n; ++i) {
            win::Row r;
            r.text = fmt("%d. ", i + 1) + g.champions[static_cast<size_t>(i)].name;
            r.right = fmt("%d turns", g.champions[static_cast<size_t>(i)].turns);
            rows.push_back(r);
        }
        list_->set_rows(rows);
        ladder_sel_ = std::clamp(ladder_sel_, 0, n - 1);
        list_->select(ladder_sel_);

        // Selected champion's winning party.
        const Champion& c = g.champions[static_cast<size_t>(ladder_sel_)];
        const int px = lw + 4, pw = kClientW - px, ph = 77;          // 162 wide
        lv_obj_t* f = win::field(body, px, ly, pw, ph);
        const int cw = (pw - 2) / 3, ch = (ph - 2) / 2;              // 53 x 37
        for (size_t i = 0; i < c.party.size() && i < MeshSpace::kPartyMax; ++i) {
            mon_cell(f, c.party[i], 1 + static_cast<int>(i % 3) * cw,
                     1 + static_cast<int>(i / 3) * ch, cw, 26, s.field_text);
        }
        if (c.party.empty()) win::text(f, "Party not shared.", 5, 4, s.dim);
        win::text(body, fmt("Won in %d turns on ", c.turns) + c.date, px + 1, ly + ph + 2, s.dim, pw - 1);
    }

    bool gym_key(const KeyEvent& e) {
        if (ladder_) {
            if (list_ && list_->on_key(e)) {
                ladder_sel_ = list_->selected();
                ctx_.router.rebuild();
                return true;
            }
            if (e.key == Key::Up || e.key == Key::Down) return true;
            if (e.key == Key::B || (e.key == Key::Char && e.ch == 'g')) {
                ladder_ = false;
                ctx_.router.rebuild();
                return true;
            }
            return false;
        }
        if (e.key == Key::Char && e.ch == 'l') {
            ladder_ = true;
            // After a win the ladder opens on my own entry.
            ladder_sel_ = has_result_ && result_.rank > 0 ? result_.rank - 1 : 0;
            ctx_.router.rebuild();
            return true;
        }
        if (mine()) {
            if (e.key == Key::A || (e.key == Key::Char && e.ch == 'u')) {
                update_my_party();
                return true;
            }
            return false;
        }
        if (e.key == Key::A || (e.key == Key::Char && e.ch == 'c')) {
            challenge();
            return true;
        }
        return false;
    }

    void update_my_party() {
        std::vector<SpaceMon> party;
        for (const PartyMon& m : ctx_.game.party()) {
            if (m.dex == 0) continue;
            party.push_back({m.dex, m.level});
        }
        if (party.empty()) {
            toast("Your team is empty");
            return;
        }
        // TODO(mesh): broadcast the new gym party once the backend exists.
        mesh_space().set_leader_party(MeshSpace::kMe, party);
        toast(fmt("Gym party updated (%d)", static_cast<int>(std::min(party.size(), MeshSpace::kPartyMax))));
        ctx_.router.rebuild();
    }

    // Fight the gym now: the owner is not asked and need not be online, the
    // team on their page is AI-played. The battle screen records the outcome
    // (MeshSpace::record_challenge) and on_show() shows it when it pops.
    void challenge() {
        const MeshSpace& ms = mesh_space();
        const SpaceGym& g = ms.gym(node_);
        if (mine() || ms.profile(node_).name == ms.my_name()) {
            toast("You cannot challenge your own gym");
            return;
        }
        if (g.party.empty()) {
            toast("This gym has no party yet");
            return;
        }
        if (ctx_.game.party().empty()) {
            toast("You have no team to battle with");
            return;
        }
        BattleRequest r;
        r.kind = BattleRequest::Kind::PlayerGym;
        r.node_id = node_;
        for (const SpaceMon& m : g.party) {
            if (r.team_count >= 6) break;
            r.team[r.team_count++] = {m.dex, m.level};
        }
        r.trainer = ms.profile(node_).name;
        r.place = g.name;
        r.prize = g.badge;
        // TODO(mesh): the team is the last synced copy of the owner's gym; the
        // result is sent back to them from the battle screen / record_challenge().
        has_result_ = false;
        demo_model_.reset();
        seen_challenge_ = ms.last_challenge().serial;
        ctx_.battle = r;
        ctx_.router.push(ScreenId::Battle);
    }

    uint32_t node_ = 0;
    std::string title_;
    int tab_ = kProfile;
    bool ladder_ = false;
    int ladder_sel_ = 0;
    int wall_top_ = 0;                   // first visible post (0 = newest)
    int feed_top_ = 0;                   // first visible update (0 = newest)
    bool picker_ = false;                // buddy icon picker open (my profile)
    int picker_sel_ = 0, picker_top_ = 0;
    std::string draft_;                  // wall input text, kept across rebuilds
    uint32_t seen_rev_ = 0;
    GymChallenge result_;                // my last fight at this gym
    bool has_result_ = false;            // ... shown on the Gym tab
    uint32_t seen_challenge_ = 0;        // MeshSpace::last_challenge().serial already handled
    std::unique_ptr<MeshSpace> demo_model_;   // demo states 9+: a copy with a fight recorded

    std::unique_ptr<win::ListBox> list_;
    std::unique_ptr<win::TextInput> input_;
    lv_obj_t* counter_ = nullptr;
};

} // namespace

std::unique_ptr<Screen> make_space_screen(Ctx& ctx) {
    return std::make_unique<SpaceScreen>(ctx);
}

} // namespace mm::ui
