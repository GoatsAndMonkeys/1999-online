// milk.h — reads MilkDrop 1 preset files (.milk) and runs their equations.
//
// A MilkDrop 1 preset is a list of "key=value" numbers plus small programs in
// the plugin's expression language: per_frame_init (once), per_frame (once per
// frame) and per_pixel (once per mesh vertex). This file is the reader and an
// evaluator for that language; drawing is the visualizer's job (viz.h).
// Standard library only. MilkDrop 2 presets (pixel shaders) are recognised
// and refused: there is no GPU to run them on.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm::milk {

// Named doubles shared by a preset's programs. Indices stay valid for the
// life of the table, so callers can look a name up once and keep the index.
class Vars {
public:
    int index(const std::string& name);            // get or create (0.0); names are case-insensitive
    int find(const std::string& name) const;       // -1 if absent
    double& operator[](int i) { return values_[static_cast<size_t>(i)]; }
    double operator[](int i) const { return values_[static_cast<size_t>(i)]; }
    double get(const std::string& name, double fallback = 0.0) const;
    void set(const std::string& name, double v) { values_[static_cast<size_t>(index(name))] = v; }
    size_t size() const { return values_.size(); }

private:
    std::unordered_map<std::string, int> names_;
    std::vector<double> values_;
};

// One compiled program: "a = sin(time)*0.5; zoom = zoom + a;".
class Program {
public:
    // Compiles `source`. Statements that do not parse are skipped (counted in
    // errors()); the rest still run, as MilkDrop does.
    void compile(const std::string& source, Vars& vars);
    void run(Vars& vars, uint32_t& rng) const;
    bool empty() const { return code_.empty(); }
    int errors() const { return errors_; }
    size_t size() const { return code_.size(); }

private:
    friend class Compiler;
    struct Op {
        uint8_t op;
        int32_t arg;
        double value;
    };
    std::vector<Op> code_;
    int errors_ = 0;
};

struct CustomCode {            // wavecode_N / shapecode_N
    bool enabled = false;
    std::map<std::string, double> params;          // "samples", "sides", "r", "additive", ...
    std::string init, per_frame, per_point;        // per_point: custom waves only
};

struct Preset {
    std::string name;                              // file name without ".milk"
    std::map<std::string, double> params;          // lower-case keys: "zoom", "fdecay", "nwavemode", ...
    std::string init, per_frame, per_pixel;
    CustomCode waves[4], shapes[4];
    bool uses_shaders = false;                     // MilkDrop 2: cannot be shown here

    double get(const std::string& key, double fallback) const;
};

// False if the text is not a preset at all.
bool parse_preset(const std::string& text, Preset& out);
bool load_preset(const std::string& path, Preset& out);

} // namespace mm::milk
