#include "app/SettingsFile.h"

namespace pejeded {
namespace {

// Stage 1 subset of the section 16 sketch; later stages add their keys here.
const char* kDefaults = R"JSON({
  "version": 1,
  "devices": { "main": "MC-707 (emulator)", "extraInputs": [], "controller": "DJ-202 (emulator)",
               "tempoPath": "usb", "dj202TempoMode": "A1" },
  "audioOut": {
    "master": { "device": "", "channels": [1, 2] },
    "phones": { "device": "", "channels": [3, 4] },
    "masterGainDb": 0, "cueMix": 0.5, "phonesLevel": 0.8, "monitor": false, "blockSize": 256
  },
  "tracks": { "folder": "" },
  "patterns": { "folder": "" },
  "record": { "folder": "" },
  "visual": { "rate": 30 },
  "emulator": { "tempo": 0 },
  "console": { "height": 0.4 },
  "db": { "cleanAfterDays": 90, "suggestCleanAboveMb": 500 }
})JSON";

using Check = std::function<std::string(const std::string&)>;

std::string numberIn(const std::string& v, double lo, double hi) {
    auto s = juce::String(v).trim();
    if (s.isEmpty() || !s.containsOnly("-0123456789.")) return "must be a number";
    double x = s.getDoubleValue();
    if (x < lo || x > hi) return "must be " + juce::String(lo).toStdString() + " to " + juce::String(hi).toStdString();
    return "";
}

Check checkFor(const std::string& key, const juce::var& def) {
    if (key == "visual.rate") return [](const std::string& v) { return v == "30" || v == "60" ? std::string() : std::string("must be 30 or 60"); };
    if (key == "audioOut.blockSize")
        return [](const std::string& v) {
            return v == "128" || v == "256" || v == "512" || v == "1024" ? std::string() : std::string("must be 128, 256, 512 or 1024");
        };
    if (key == "devices.tempoPath") return [](const std::string& v) { return v == "usb" || v == "dj202" ? std::string() : std::string("must be usb or dj202"); };
    if (key == "devices.dj202TempoMode") return [](const std::string& v) { return v == "A1" || v == "A2" ? std::string() : std::string("must be A1 or A2"); };
    if (key == "emulator.tempo")
        return [](const std::string& v) {
            auto w = numberIn(v, 0, 300);
            if (w.empty() && juce::String(v).getDoubleValue() > 0 && juce::String(v).getDoubleValue() < 40) w = "must be 0 or 40 to 300";
            return w;
        };
    if (key == "audioOut.masterGainDb") return [](const std::string& v) { return numberIn(v, -24, 12); };
    if (key == "audioOut.cueMix" || key == "audioOut.phonesLevel") return [](const std::string& v) { return numberIn(v, 0, 1); };
    if (key == "console.height") return [](const std::string& v) { return numberIn(v, 0.1, 0.9); };
    if (key == "db.cleanAfterDays") return [](const std::string& v) { return numberIn(v, 30, 3650); };
    if (key == "db.suggestCleanAboveMb") return [](const std::string& v) { return numberIn(v, 50, 100000); };
    if (def.isBool()) return [](const std::string& v) { return v == "true" || v == "false" ? std::string() : std::string("must be true or false"); };
    if (def.isArray()) return [](const std::string& v) { return juce::JSON::parse(v).isArray() ? std::string() : std::string("must be a list like [1, 2]"); };
    if (def.isInt() || def.isDouble()) return [](const std::string& v) { return numberIn(v, -1e9, 1e9); };
    return {};
}

std::string text(const juce::var& v) {
    if (v.isString()) return v.toString().toStdString();
    if (v.isBool()) return static_cast<bool>(v) ? "true" : "false";
    return juce::JSON::toString(v, true).toStdString();
}

juce::var fromText(const std::string& s, const juce::var& like) {
    if (like.isString()) return juce::String(s);
    if (like.isBool()) return s == "true";
    // JSON::parse(String) reads only objects and arrays, so numbers are read here, or they come back as text
    if (like.isInt() || like.isInt64()) return juce::String(s).getLargeIntValue();
    if (like.isDouble()) return juce::String(s).getDoubleValue();
    auto p = juce::JSON::parse(juce::String(s));
    return p.isVoid() ? juce::var(juce::String(s)) : p;
}

// Copies default leaves into `into` where they are missing; keeps unknown keys.
void fill(juce::var& into, const juce::var& def) {
    auto* d = def.getDynamicObject();
    auto* o = into.getDynamicObject();
    if (!d || !o) return;
    for (const auto& p : d->getProperties()) {
        if (!o->hasProperty(p.name)) o->setProperty(p.name, p.value.clone());
        else if (p.value.getDynamicObject()) {
            juce::var child = o->getProperty(p.name);
            if (!child.getDynamicObject()) o->setProperty(p.name, p.value.clone());
            else fill(child, p.value);
        }
    }
}

void leaves(const juce::var& def, const std::string& prefix, std::vector<std::pair<std::string, juce::var>>& out) {
    auto* d = def.getDynamicObject();
    if (!d) return;
    for (const auto& p : d->getProperties()) {
        std::string k = prefix + p.name.toString().toStdString();
        if (p.value.getDynamicObject()) leaves(p.value, k + ".", out);
        else if (k != "version") out.emplace_back(k, p.value);
    }
}

// Object that holds the last part of `key` ("a.b.c" -> object a.b); creates missing parts.
juce::DynamicObject* parentOf(juce::var& root, const std::string& key) {
    auto parts = juce::StringArray::fromTokens(juce::String(key), ".", "");
    juce::DynamicObject* cur = root.getDynamicObject();
    for (int i = 0; cur && i < parts.size() - 1; ++i) {
        juce::var child = cur->getProperty(juce::Identifier(parts[i]));
        if (!child.getDynamicObject()) {
            child = juce::var(new juce::DynamicObject());
            cur->setProperty(juce::Identifier(parts[i]), child);
        }
        cur = child.getDynamicObject();
    }
    return cur;
}

} // namespace

juce::var SettingsFile::defaults() { return juce::JSON::parse(juce::String(kDefaults)); }

std::vector<std::pair<std::string, std::string>> SettingsFile::defaultValues() {
    std::vector<std::pair<std::string, juce::var>> all;
    leaves(defaults(), "", all);
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [k, v] : all) out.emplace_back(k, text(v));
    return out;
}

SettingsFile::Loaded SettingsFile::load(const juce::File& file, Settings& s) {
    path = file;
    readOnly = false;
    Loaded how = Loaded::Fresh;
    tree = defaults();
    if (file.existsAsFile()) {
        juce::var read;
        auto r = juce::JSON::parse(file.loadFileAsString(), read);
        if (r.failed() || !read.getDynamicObject()) {
            file.moveFileTo(file.getSiblingFile("settings.broken.json"));
            how = Loaded::Broken;
        } else if (static_cast<int>(read.getProperty("version", 1)) > kVersion) {
            readOnly = true;
            how = Loaded::Newer;
        } else {
            fill(read, defaults());
            tree = read;
            how = Loaded::Read;
        }
    }
    std::vector<std::pair<std::string, juce::var>> keys;
    leaves(defaults(), "", keys);
    for (const auto& [k, def] : keys) {
        auto* parent = parentOf(tree, k);
        juce::var value = def;
        if (parent) value = parent->getProperty(juce::Identifier(juce::String(k).fromLastOccurrenceOf(".", false, false)));
        if (value.isVoid()) value = def;
        auto check = checkFor(k, def);
        std::string v = text(value);
        if (check && !check(v).empty()) v = text(def);  // a bad value in the file falls back to the default
        s.define(k, v, "", check);
    }
    return how;
}

bool SettingsFile::save(const Settings& s) {
    if (readOnly || path == juce::File()) return false;
    auto def = defaults();
    std::vector<std::pair<std::string, juce::var>> keys;
    leaves(def, "", keys);
    for (const auto& [k, like] : keys) {
        auto* parent = parentOf(tree, k);
        if (!parent) continue;
        parent->setProperty(juce::Identifier(juce::String(k).fromLastOccurrenceOf(".", false, false)), fromText(s.get(k), like));
    }
    tree.getDynamicObject()->setProperty("version", kVersion);
    path.getParentDirectory().createDirectory();
    auto tmp = path.getSiblingFile(path.getFileName() + ".tmp");
    if (!tmp.replaceWithText(juce::JSON::toString(tree))) return false;
    return tmp.moveFileTo(path);
}

} // namespace pejeded
