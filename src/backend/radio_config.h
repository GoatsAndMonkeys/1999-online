// radio_config.h — Meshtastic settings as a data table for the Radio Setup screen.
//
// mm::radio_config() holds every section (User, LoRa, channels, device, ...,
// module configs, admin actions) as a list of typed fields. Each field records
// the protobuf field name and number it maps to, and each section records the
// AdminMessage variant that carries it, so a later step can build real
// AdminMessages from this table without another lookup.
//
// Names, numbers, enum values and ranges were taken from meshtastic/protobufs
// (master, read 2026-11-20): config.proto, module_config.proto, channel.proto,
// mesh.proto (User, Position), admin.proto and the nanopb *.options files
// (string / bytes sizes). Where the protobufs give no range, the range is the
// protobuf type's (or its nanopb int_size's); GPIO pins are limited to 0..255
// here, which is this file's choice, not Meshtastic's.
//
// NOTHING HERE TALKS TO A RADIO YET. Values live in memory, start from
// firmware-like defaults (not read from the device), and apply() / revert() /
// run_action() only update the in-memory state and log. See TODO(daemon) in
// radio_config.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

enum class FieldType : uint8_t {
    Enum,     // one of FieldDef::options; value = the protobuf enum number
    Bool,     // value 0 / 1
    Int,      // value within [min, max], shown with `unit`
    Text,     // string of at most max_len bytes (bytes fields: base64 text)
    Action,   // AdminMessage command; nothing to edit, run with run_action()
};

struct EnumOption {
    int value;            // protobuf enum number
    const char* name;     // protobuf enum value name, e.g. "EU_868"
    const char* label;    // human label
};

struct FieldDef {
    const char* label;            // UI label (short: it shares a 200px row with the value)
    FieldType type;
    // Protobuf mapping. `pb_field` / `pb_number` are the field inside the
    // section's message (SectionDef::pb_message) unless `pb_carrier` is set,
    // in which case they are a field of that message instead (e.g. the fixed
    // position, which travels in AdminMessage.set_fixed_position). For an
    // Action they are the AdminMessage field itself.
    const char* pb_field;
    int pb_number;
    const char* pb_type;          // "uint32", "bool", "string", "bytes", "RegionCode", ...
    const char* pb_carrier = nullptr;

    const EnumOption* options = nullptr;
    int option_count = 0;
    int64_t min = 0;
    int64_t max = 0;
    const char* unit = "";
    int max_len = 0;              // Text: bytes, without the NUL nanopb reserves
    bool secret = false;          // shown masked in lists

    int64_t def_int = 0;          // Enum / Bool / Int default; Action: the argument sent
    const char* def_text = "";    // Text default
    const char* help = "";        // one line; Action: the confirmation question
};

struct Field {
    const FieldDef* def = nullptr;
    int64_t ival = 0;             // Enum / Bool / Int
    std::string sval;             // Text
    int64_t saved_ival = 0;       // value as of the last apply() / revert target
    std::string saved_sval;

    bool changed() const { return ival != saved_ival || sval != saved_sval; }
};

enum class SectionKind : uint8_t {
    Owner,      // AdminMessage.set_owner (User)
    Config,     // AdminMessage.set_config (Config.<oneof>)
    Channel,    // AdminMessage.set_channel (Channel, index = channel_index)
    Module,     // AdminMessage.set_module_config (ModuleConfig.<oneof>)
    Actions,    // one AdminMessage field per action
};

struct SectionDef {
    const char* id;               // "lora", "mqtt", "channel0", ...
    const char* name;             // short, for the section list
    const char* title;            // full
    SectionKind kind;
    const char* pb_message;       // "Config.LoRaConfig", "User", "Channel", ...
    const char* admin_field;      // "set_config", "set_owner", ...
    int admin_number;             // its field number in AdminMessage
    const char* oneof_field;      // "lora" in Config.payload_variant; "" if none
    int oneof_number;             // its field number; 0 if none
    int channel_index;            // Channel sections; else -1
};

struct Section {
    SectionDef def;
    std::vector<Field> fields;
    bool dirty = false;           // some field differs from its saved value
};

class RadioConfig {
public:
    RadioConfig();

    int section_count() const { return static_cast<int>(sections_.size()); }
    const Section& section(int i) const { return sections_[static_cast<size_t>(i)]; }
    int find_section(const char* id) const;                   // -1 if unknown
    int find_field(int section, const char* pb_field) const;  // -1 if unknown

    // Setters clamp / truncate to the field's limits, update the section's
    // dirty flag and return false if the field is of the wrong type.
    bool set_int(int section, int field, int64_t v);          // Enum / Bool / Int
    bool set_text(int section, int field, const std::string& v);

    // Value as shown in the UI: enum value name, "on"/"off", number + unit,
    // text ("****" when secret and not `reveal`), "" for actions.
    std::string display(int section, int field, bool reveal = false) const;

    // Save / drop the pending edits of one section. Today both are local only.
    bool apply(int section);
    void revert(int section);
    // Run an Action field. Today: logs only. Returns false if not an action.
    bool run_action(int section, int field);

    bool any_dirty() const;
    uint32_t revision() const { return revision_; }           // bumps on every change

private:
    void refresh_dirty(Section& s);

    std::vector<Section> sections_;
    uint32_t revision_ = 0;
};

RadioConfig& radio_config();

} // namespace mm
