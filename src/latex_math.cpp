#include "latex_math.hpp"
#include "geometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <unordered_map>

// ─── Segment Splitting ───────────────────────────────────────────────────────

static bool is_escaped_at(const std::string& s, size_t idx) {
    size_t bs = 0;
    while (idx > bs && s[idx - 1 - bs] == '\\') {
        ++bs;
    }
    return (bs % 2) == 1;
}

static std::string unescape_dollars(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '$') {
            out += '$';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<TextSegment> split_latex_segments(const std::string& text) {
    std::vector<TextSegment> segs;
    const size_t n = text.size();
    std::string plain_buf;
    bool found_math = false;

    size_t i = 0;
    while (i < n) {
        if (text[i] == '$' && !is_escaped_at(text, i)) {
            if (i + 1 < n && text[i + 1] == '$') {
                // Search for closing $$
                size_t j = i + 2;
                bool closed = false;
                while (j + 1 < n) {
                    if (text[j] == '$' && text[j + 1] == '$' && !is_escaped_at(text, j)) {
                        closed = true;
                        break;
                    }
                    ++j;
                }
                if (closed && j > i + 2) {
                    if (!plain_buf.empty()) {
                        segs.push_back({TextSegment::Kind::Plain, plain_buf});
                        plain_buf.clear();
                    }
                    segs.push_back({TextSegment::Kind::DisplayMath, text.substr(i + 2, j - (i + 2))});
                    found_math = true;
                    i = j + 2;
                    continue;
                }
                // Unclosed $$ or empty $$$$: treat literally
                plain_buf += "$$";
                i += 2;
                continue;
            } else {
                // Single $ ... $
                size_t j = i + 1;
                bool closed = false;
                while (j < n) {
                    if (text[j] == '$' && !is_escaped_at(text, j) &&
                        (j + 1 >= n || text[j + 1] != '$')) {
                        closed = true;
                        break;
                    }
                    ++j;
                }
                if (closed && j > i + 1) {
                    if (!plain_buf.empty()) {
                        segs.push_back({TextSegment::Kind::Plain, plain_buf});
                        plain_buf.clear();
                    }
                    segs.push_back({TextSegment::Kind::InlineMath, text.substr(i + 1, j - (i + 1))});
                    found_math = true;
                    i = j + 1;
                    continue;
                }
                plain_buf += '$';
                ++i;
                continue;
            }
        }
        plain_buf += text[i];
        ++i;
    }

    if (!found_math) {
        return {{TextSegment::Kind::Plain, text}};
    }
    if (!plain_buf.empty()) {
        segs.push_back({TextSegment::Kind::Plain, plain_buf});
    }
    for (auto& seg : segs) {
        if (seg.kind == TextSegment::Kind::Plain) {
            seg.content = unescape_dollars(seg.content);
        }
    }
    return segs;
}

bool contains_latex_math(const std::string& text) {
    if (text.find('$') == std::string::npos) return false;
    auto segs = split_latex_segments(text);
    for (const auto& s : segs) {
        if (s.kind != TextSegment::Kind::Plain) return true;
    }
    return false;
}

// ─── Font Probing & Resolution ───────────────────────────────────────────────

enum class MathStyle {
    PlainText = 0,
    MathItalic,
    MathUpright,
    MathBold,
    MathBoldItalic,
    MathSans,
    MathMono,
    MathSymbol
};

struct ResolvedFont {
    std::string family;
    cairo_font_slant_t slant = CAIRO_FONT_SLANT_NORMAL;
    cairo_font_weight_t weight = CAIRO_FONT_WEIGHT_NORMAL;
};

static bool font_supports_utf8(cairo_t* cr, const char* family,
                               cairo_font_slant_t slant, cairo_font_weight_t weight,
                               const std::string& utf8) {
    if (utf8.empty()) return true;
    cairo_save(cr);
    cairo_select_font_face(cr, family, slant, weight);
    cairo_set_font_size(cr, 20.0);
    cairo_scaled_font_t* sf = cairo_get_scaled_font(cr);
    cairo_glyph_t* glyphs = nullptr;
    int num_glyphs = 0;
    cairo_status_t st = cairo_scaled_font_text_to_glyphs(
        sf, 0.0, 0.0, utf8.c_str(), (int)utf8.size(),
        &glyphs, &num_glyphs, nullptr, nullptr, nullptr);
    bool ok = (st == CAIRO_STATUS_SUCCESS && num_glyphs > 0);
    if (ok) {
        for (int i = 0; i < num_glyphs; ++i) {
            if (glyphs[i].index == 0) {
                ok = false;
                break;
            }
        }
    }
    if (glyphs) cairo_glyph_free(glyphs);
    cairo_restore(cr);
    return ok;
}

static ResolvedFont resolve_font_for(cairo_t* cr, MathStyle style, const std::string& utf8) {
    static std::unordered_map<std::string, ResolvedFont> cache;
    std::string key = std::to_string((int)style) + "|" + utf8;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    cairo_font_slant_t slant = CAIRO_FONT_SLANT_NORMAL;
    cairo_font_weight_t weight = CAIRO_FONT_WEIGHT_NORMAL;
    std::vector<const char*> primary;

    switch (style) {
        case MathStyle::PlainText:
            primary = {"Inter", "DejaVu Sans", "Liberation Sans", "Noto Sans", "sans-serif"};
            break;
        case MathStyle::MathItalic:
            slant = CAIRO_FONT_SLANT_ITALIC;
            primary = {"DejaVu Serif", "STIX Two Text", "Liberation Serif", "FreeSerif", "Noto Serif", "serif"};
            break;
        case MathStyle::MathUpright:
            primary = {"DejaVu Serif", "STIX Two Text", "Liberation Serif", "FreeSerif", "Noto Serif", "serif"};
            break;
        case MathStyle::MathBold:
            weight = CAIRO_FONT_WEIGHT_BOLD;
            primary = {"DejaVu Serif", "STIX Two Text", "Liberation Serif", "FreeSerif", "Noto Serif", "serif"};
            break;
        case MathStyle::MathBoldItalic:
            slant = CAIRO_FONT_SLANT_ITALIC;
            weight = CAIRO_FONT_WEIGHT_BOLD;
            primary = {"DejaVu Serif", "STIX Two Text", "Liberation Serif", "FreeSerif", "Noto Serif", "serif"};
            break;
        case MathStyle::MathSans:
            primary = {"DejaVu Sans", "Liberation Sans", "Noto Sans", "Inter", "sans-serif"};
            break;
        case MathStyle::MathMono:
            primary = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "monospace"};
            break;
        case MathStyle::MathSymbol:
            primary = {"Latin Modern Math", "STIX Two Math", "Noto Sans Math",
                       "DejaVu Serif", "DejaVu Sans", "FreeSerif",
                       "Noto Sans Symbols", "Noto Sans Symbols 2", "serif", "sans-serif"};
            break;
    }

    for (const char* fam : primary) {
        if (font_supports_utf8(cr, fam, slant, weight, utf8)) {
            ResolvedFont rf{fam, slant, weight};
            if (cache.size() < 4096) cache[key] = rf;
            return rf;
        }
    }

    static const char* symbol_fallbacks[] = {
        "Latin Modern Math", "STIX Two Math", "Noto Sans Math",
        "DejaVu Serif", "DejaVu Sans", "FreeSerif",
        "Noto Sans Symbols", "Noto Sans Symbols 2", "serif", "sans-serif"
    };
    for (const char* fam : symbol_fallbacks) {
        if (font_supports_utf8(cr, fam, CAIRO_FONT_SLANT_NORMAL, weight, utf8)) {
            ResolvedFont rf{fam, CAIRO_FONT_SLANT_NORMAL, weight};
            if (cache.size() < 4096) cache[key] = rf;
            return rf;
        }
    }

    ResolvedFont rf{primary.front(), slant, weight};
    if (cache.size() < 4096) cache[key] = rf;
    return rf;
}

// ─── Unicode Helpers for Math Alphabets ──────────────────────────────────────

static std::string to_blackboard_bold(const std::string& s) {
    static const std::unordered_map<char, const char*> bb = {
        {'C', "\xE2\x84\x82"}, // ℂ
        {'H', "\xE2\x84\x8D"}, // ℍ
        {'N', "\xE2\x84\x95"}, // ℕ
        {'P', "\xE2\x84\x99"}, // ℙ
        {'Q', "\xE2\x84\x9A"}, // ℚ
        {'R', "\xE2\x84\x9D"}, // ℝ
        {'Z', "\xE2\x84\xA4"}, // ℤ
    };
    std::string out;
    for (unsigned char c : s) {
        auto it = bb.find((char)c);
        if (it != bb.end()) {
            out += it->second;
        } else if (c >= 'A' && c <= 'Z') {
            append_utf8(out, 0x1D538u + (uint32_t)(c - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            append_utf8(out, 0x1D552u + (uint32_t)(c - 'a'));
        } else if (c >= '0' && c <= '9') {
            append_utf8(out, 0x1D7D8u + (uint32_t)(c - '0'));
        } else {
            out += (char)c;
        }
    }
    return out;
}

static std::string to_calligraphic(const std::string& s) {
    static const std::unordered_map<char, const char*> cal = {
        {'B', "\xE2\x84\xAC"}, // ℬ
        {'E', "\xE2\x84\xB0"}, // ℰ
        {'F', "\xE2\x84\xB1"}, // ℱ
        {'H', "\xE2\x84\x8B"}, // ℋ
        {'I', "\xE2\x84\x90"}, // ℐ
        {'L', "\xE2\x84\x92"}, // ℒ
        {'M', "\xE2\x84\xB3"}, // ℳ
        {'R', "\xE2\x84\x9B"}, // ℛ
        {'e', "\xE2\x84\xAF"}, // ℯ
        {'g', "\xE2\x84\x8A"}, // ℊ
        {'o', "\xE2\x84\xB4"}, // ℴ
    };
    std::string out;
    for (unsigned char c : s) {
        auto it = cal.find((char)c);
        if (it != cal.end()) {
            out += it->second;
        } else if (c >= 'A' && c <= 'Z') {
            append_utf8(out, 0x1D49Cu + (uint32_t)(c - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            append_utf8(out, 0x1D4B6u + (uint32_t)(c - 'a'));
        } else {
            out += (char)c;
        }
    }
    return out;
}

static std::string to_fraktur(const std::string& s) {
    static const std::unordered_map<char, const char*> frak = {
        {'C', "\xE2\x84\xAD"}, // ℭ
        {'H', "\xE2\x84\x8C"}, // ℌ
        {'I', "\xE2\x84\x91"}, // ℑ
        {'R', "\xE2\x84\x9C"}, // ℜ
        {'Z', "\xE2\x84\xA8"}, // ℨ
    };
    std::string out;
    for (unsigned char c : s) {
        auto it = frak.find((char)c);
        if (it != frak.end()) {
            out += it->second;
        } else if (c >= 'A' && c <= 'Z') {
            append_utf8(out, 0x1D504u + (uint32_t)(c - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            append_utf8(out, 0x1D51Eu + (uint32_t)(c - 'a'));
        } else {
            out += (char)c;
        }
    }
    return out;
}

// ─── LaTeX Command Tables ────────────────────────────────────────────────────

enum class AtomRole {
    Ordinary,
    BinaryOp,
    Relation,
    OpenDelim,
    CloseDelim,
    Punct,
    LargeOp,
    FunctionName
};

struct SymbolDef {
    const char* utf8;
    AtomRole role;
    MathStyle style;
    bool stack_limits_in_display;
};

static const std::unordered_map<std::string, SymbolDef>& symbol_table() {
    static const std::unordered_map<std::string, SymbolDef> tbl = {
        // Lowercase Greek
        {"alpha",      {"α", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"beta",       {"β", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"gamma",      {"γ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"delta",      {"δ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"epsilon",    {"ϵ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"varepsilon", {"ε", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"zeta",       {"ζ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"eta",        {"η", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"theta",      {"θ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"vartheta",   {"ϑ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"iota",       {"ι", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"kappa",      {"κ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"lambda",     {"λ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"mu",         {"μ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"nu",         {"ν", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"xi",         {"ξ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"pi",         {"π", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"varpi",      {"ϖ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"rho",        {"ρ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"varrho",     {"ϱ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"sigma",      {"σ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"varsigma",   {"ς", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"tau",        {"τ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"upsilon",    {"υ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"phi",        {"ϕ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"varphi",     {"φ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"chi",        {"χ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"psi",        {"ψ", AtomRole::Ordinary, MathStyle::MathItalic, false}},
        {"omega",      {"ω", AtomRole::Ordinary, MathStyle::MathItalic, false}},

        // Uppercase Greek
        {"Alpha",   {"Α", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Beta",    {"Β", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Gamma",   {"Γ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Delta",   {"Δ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Epsilon", {"Ε", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Zeta",    {"Ζ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Eta",     {"Η", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Theta",   {"Θ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Iota",    {"Ι", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Kappa",   {"Κ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Lambda",  {"Λ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Mu",      {"Μ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Nu",      {"Ν", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Xi",      {"Ξ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Pi",      {"Π", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Rho",     {"Ρ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Sigma",   {"Σ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Tau",     {"Τ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Upsilon", {"Υ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Phi",     {"Φ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Chi",     {"Χ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Psi",     {"Ψ", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"Omega",   {"Ω", AtomRole::Ordinary, MathStyle::MathUpright, false}},

        // Large Operators
        {"sum",       {"∑", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"prod",      {"∏", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"coprod",    {"∐", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"int",       {"∫", AtomRole::LargeOp, MathStyle::MathSymbol, false}},
        {"iint",      {"∬", AtomRole::LargeOp, MathStyle::MathSymbol, false}},
        {"iiint",     {"∭", AtomRole::LargeOp, MathStyle::MathSymbol, false}},
        {"oint",      {"∮", AtomRole::LargeOp, MathStyle::MathSymbol, false}},
        {"bigcup",    {"⋃", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigcap",    {"⋂", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigvee",    {"⋁", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigwedge",  {"⋀", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigoplus",  {"⨁", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigotimes", {"⨂", AtomRole::LargeOp, MathStyle::MathSymbol, true}},
        {"bigsqcup",  {"⨆", AtomRole::LargeOp, MathStyle::MathSymbol, true}},

        // Binary Operators
        {"pm",        {"±", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"mp",        {"∓", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"times",     {"×", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"div",       {"÷", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"cdot",      {"·", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"ast",       {"∗", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"star",      {"⋆", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"circ",      {"∘", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"bullet",    {"•", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"oplus",     {"⊕", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"ominus",    {"⊖", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"otimes",    {"⊗", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"oslash",    {"⊘", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"odot",      {"⊙", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"cup",       {"∪", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"cap",       {"∩", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"setminus",  {"∖", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"wedge",     {"∧", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"land",      {"∧", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"vee",       {"∨", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"lor",       {"∨", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"triangle",  {"△", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"diamond",   {"⋄", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"dagger",    {"†", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},
        {"ddagger",   {"‡", AtomRole::BinaryOp, MathStyle::MathSymbol, false}},

        // Relations
        {"leq",        {"≤", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"le",         {"≤", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"geq",        {"≥", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"ge",         {"≥", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"neq",        {"≠", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"ne",         {"≠", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"approx",     {"≈", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"equiv",      {"≡", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"sim",        {"∼", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"simeq",      {"≃", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"cong",       {"≅", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"propto",     {"∝", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"ll",         {"≪", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"gg",         {"≫", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"prec",       {"≺", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"succ",       {"≻", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"preceq",     {"⪯", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"succeq",     {"⪰", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"subset",     {"⊂", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"supset",     {"⊃", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"subseteq",   {"⊆", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"supseteq",   {"⊇", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"subsetneq",  {"⊊", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"supsetneq",  {"⊋", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"in",         {"∈", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"notin",      {"∉", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"ni",         {"∋", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"mid",        {"∣", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"nmid",       {"∤", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"parallel",   {"∥", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"perp",       {"⊥", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"models",     {"⊨", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"vdash",      {"⊢", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"dashv",      {"⊣", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"triangleq",  {"≜", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"doteq",      {"≐", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"coloneq",    {"≔", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"coloneqq",   {"≔", AtomRole::Relation, MathStyle::MathSymbol, false}},

        // Arrows
        {"to",                 {"→", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"rightarrow",         {"→", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"leftarrow",          {"←", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"gets",               {"←", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"leftrightarrow",     {"↔", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Rightarrow",         {"⇒", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"implies",            {"⇒", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Leftarrow",          {"⇐", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Leftrightarrow",     {"⇔", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"iff",                {"⇔", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"mapsto",             {"↦", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"longmapsto",         {"⟼", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"longrightarrow",     {"⟶", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"longleftarrow",      {"⟵", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Longrightarrow",     {"⟹", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Longleftarrow",      {"⟸", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Longleftrightarrow", {"⟺", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"uparrow",            {"↑", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"downarrow",          {"↓", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"updownarrow",        {"↕", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Uparrow",            {"⇑", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"Downarrow",          {"⇓", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"nearrow",            {"↗", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"searrow",            {"↘", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"swarrow",            {"↙", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"nwarrow",            {"↖", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"hookrightarrow",     {"↪", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"hookleftarrow",      {"↩", AtomRole::Relation, MathStyle::MathSymbol, false}},

        // Logic, Calculus, Misc
        {"forall",        {"∀", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"exists",        {"∃", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"nexists",       {"∄", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"neg",           {"¬", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"lnot",          {"¬", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"emptyset",      {"∅", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"varnothing",    {"∅", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"infty",         {"∞", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"partial",       {"∂", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"nabla",         {"∇", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"hbar",          {"ℏ", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"ell",           {"ℓ", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"wp",            {"℘", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"Re",            {"ℜ", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"Im",            {"ℑ", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"aleph",         {"ℵ", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"therefore",     {"∴", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"because",       {"∵", AtomRole::Relation, MathStyle::MathSymbol, false}},
        {"cdots",         {"⋯", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"ldots",         {"…", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"dots",          {"…", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"vdots",         {"⋮", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"ddots",         {"⋱", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"angle",         {"∠", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"measuredangle", {"∡", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"degree",        {"°", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"prime",         {"′", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"checkmark",     {"✓", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"square",        {"□", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"blacksquare",   {"■", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"clubsuit",      {"♣", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"diamondsuit",   {"♦", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"heartsuit",     {"♥", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"spadesuit",     {"♠", AtomRole::Ordinary, MathStyle::MathSymbol, false}},

        // Delimiter symbols when used directly
        {"langle", {"⟨", AtomRole::OpenDelim, MathStyle::MathSymbol, false}},
        {"rangle", {"⟩", AtomRole::CloseDelim, MathStyle::MathSymbol, false}},
        {"lfloor", {"⌊", AtomRole::OpenDelim, MathStyle::MathSymbol, false}},
        {"rfloor", {"⌋", AtomRole::CloseDelim, MathStyle::MathSymbol, false}},
        {"lceil",  {"⌈", AtomRole::OpenDelim, MathStyle::MathSymbol, false}},
        {"rceil",  {"⌉", AtomRole::CloseDelim, MathStyle::MathSymbol, false}},
        {"|",      {"‖", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"Vert",   {"‖", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"vert",   {"|", AtomRole::Ordinary, MathStyle::MathSymbol, false}},
        {"{",      {"{", AtomRole::OpenDelim, MathStyle::MathUpright, false}},
        {"}",      {"}", AtomRole::CloseDelim, MathStyle::MathUpright, false}},
        {"lbrace", {"{", AtomRole::OpenDelim, MathStyle::MathUpright, false}},
        {"rbrace", {"}", AtomRole::CloseDelim, MathStyle::MathUpright, false}},
        {"$",      {"$", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"%",      {"%", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"&",      {"&", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"#",      {"#", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"_",      {"_", AtomRole::Ordinary, MathStyle::MathUpright, false}},
        {"backslash", {"\\", AtomRole::Ordinary, MathStyle::MathUpright, false}},
    };
    return tbl;
}

struct FunctionDef {
    const char* name;
    bool stack_limits_in_display;
};

static const std::unordered_map<std::string, FunctionDef>& function_table() {
    static const std::unordered_map<std::string, FunctionDef> tbl = {
        {"sin",    {"sin",    false}},
        {"cos",    {"cos",    false}},
        {"tan",    {"tan",    false}},
        {"cot",    {"cot",    false}},
        {"sec",    {"sec",    false}},
        {"csc",    {"csc",    false}},
        {"arcsin", {"arcsin", false}},
        {"arccos", {"arccos", false}},
        {"arctan", {"arctan", false}},
        {"sinh",   {"sinh",   false}},
        {"cosh",   {"cosh",   false}},
        {"tanh",   {"tanh",   false}},
        {"coth",   {"coth",   false}},
        {"log",    {"log",    false}},
        {"ln",     {"ln",     false}},
        {"lg",     {"lg",     false}},
        {"exp",    {"exp",    false}},
        {"lim",    {"lim",    true}},
        {"limsup", {"lim sup", true}},
        {"liminf", {"lim inf", true}},
        {"sup",    {"sup",    true}},
        {"inf",    {"inf",    true}},
        {"max",    {"max",    true}},
        {"min",    {"min",    true}},
        {"argmax", {"arg max", true}},
        {"argmin", {"arg min", true}},
        {"arg",    {"arg",    false}},
        {"det",    {"det",    false}},
        {"dim",    {"dim",    false}},
        {"gcd",    {"gcd",    false}},
        {"hom",    {"hom",    false}},
        {"ker",    {"ker",    false}},
        {"Pr",     {"Pr",     true}},
        {"deg",    {"deg",    false}},
        {"mod",    {"mod",    false}},
        {"bmod",   {"mod",    false}},
        {"pmod",   {"mod",    false}},
    };
    return tbl;
}

// ─── Math AST ────────────────────────────────────────────────────────────────

struct MathNode;
using NodePtr = std::shared_ptr<MathNode>;

enum class LimitsMode {
    Auto,
    Limits,
    NoLimits
};

enum class AccentKind {
    Hat,
    WideHat,
    Tilde,
    WideTilde,
    Bar,
    Overline,
    Underline,
    Vec,
    Dot,
    DDot,
    DDDot,
    Overbrace,
    Underbrace,
    OverRightArrow,
    OverLeftArrow
};

struct MathNode {
    enum class Type {
        Row,
        Symbol,
        Space,
        Fraction,
        Sqrt,
        Scripts,
        Delimited,
        Accent,
        Matrix,
        ColorGroup,
        TextRun
    };

    Type type = Type::Row;

    // Row
    std::vector<NodePtr> children;

    // Symbol / TextRun
    std::string text;
    AtomRole role = AtomRole::Ordinary;
    MathStyle style = MathStyle::MathItalic;
    bool stack_limits = false;

    // Space (in em)
    double space_em = 0.0;

    // Fraction
    NodePtr num;
    NodePtr den;
    bool draw_line = true;
    bool has_parens = false;

    // Sqrt
    NodePtr root_index;
    NodePtr radicand;

    // Scripts
    NodePtr base;
    NodePtr sub;
    NodePtr sup;
    LimitsMode limits_mode = LimitsMode::Auto;

    // Delimited
    std::string left_delim;
    std::string right_delim;
    NodePtr inner;

    // Accent
    AccentKind accent_kind = AccentKind::Hat;

    // Matrix
    std::string env_name;
    std::vector<std::vector<NodePtr>> rows;

    // ColorGroup
    std::string color_str;
};

static NodePtr make_node(MathNode::Type t) {
    auto n = std::make_shared<MathNode>();
    n->type = t;
    return n;
}

// ─── Math Tokenizer & Parser ─────────────────────────────────────────────────

struct MathTok {
    enum class Kind {
        Command,
        LBrace,
        RBrace,
        LBracket,
        RBracket,
        Caret,
        Underscore,
        Ampersand,
        Prime,
        TextChar
    };
    Kind kind = Kind::TextChar;
    std::string val;
    size_t src_end = 0;
};

static size_t next_utf8_len(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t len = 1;
    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    return std::min(len, s.size() - i);
}

static std::vector<MathTok> tokenize_math(const std::string& s) {
    std::vector<MathTok> toks;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '\\') {
            ++i;
            if (i >= n) break;
            if (std::isalpha((unsigned char)s[i])) {
                size_t start = i;
                while (i < n && std::isalpha((unsigned char)s[i])) ++i;
                toks.push_back({MathTok::Kind::Command, s.substr(start, i - start), i});
            } else {
                size_t len = next_utf8_len(s, i);
                i += len;
                toks.push_back({MathTok::Kind::Command, s.substr(i - len, len), i});
            }
            continue;
        }
        if (c == '{') { ++i; toks.push_back({MathTok::Kind::LBrace, "{", i}); continue; }
        if (c == '}') { ++i; toks.push_back({MathTok::Kind::RBrace, "}", i}); continue; }
        if (c == '[') { ++i; toks.push_back({MathTok::Kind::LBracket, "[", i}); continue; }
        if (c == ']') { ++i; toks.push_back({MathTok::Kind::RBracket, "]", i}); continue; }
        if (c == '^') { ++i; toks.push_back({MathTok::Kind::Caret, "^", i}); continue; }
        if (c == '_') { ++i; toks.push_back({MathTok::Kind::Underscore, "_", i}); continue; }
        if (c == '&') { ++i; toks.push_back({MathTok::Kind::Ampersand, "&", i}); continue; }
        if (c == '\'') {
            size_t start = i;
            while (i < n && s[i] == '\'') ++i;
            toks.push_back({MathTok::Kind::Prime, s.substr(start, i - start), i});
            continue;
        }
        size_t len = next_utf8_len(s, i);
        i += len;
        toks.push_back({MathTok::Kind::TextChar, s.substr(i - len, len), i});
    }
    return toks;
}

class MathParser {
public:
    explicit MathParser(const std::string& src) : src_(src), toks_(tokenize_math(src)) {}

    NodePtr parse() {
        return parse_row(StopMode::None, 0);
    }

private:
    enum class StopMode {
        None,
        Brace,
        RightDelim,
        EnvEnd
    };

    const std::string& src_;
    std::vector<MathTok> toks_;
    size_t pos_ = 0;

    bool at_end() const { return pos_ >= toks_.size(); }
    const MathTok& peek() const { return toks_[pos_]; }

    // Extract raw braced text directly from src_ for \text{...} (preserving spaces)
    // Fallback to token-based reconstruction if needed.
    std::string read_braced_raw_from_tokens() {
        if (at_end()) return "";
        if (peek().kind != MathTok::Kind::LBrace) {
            std::string out = peek().val;
            ++pos_;
            return out;
        }
        ++pos_;
        int depth = 1;
        std::string out;
        while (!at_end() && depth > 0) {
            const auto& t = peek();
            if (t.kind == MathTok::Kind::LBrace) {
                ++depth;
                out += "{";
            } else if (t.kind == MathTok::Kind::RBrace) {
                --depth;
                if (depth > 0) out += "}";
            } else if (t.kind == MathTok::Kind::Command) {
                if (t.val == " " || t.val == "$" || t.val == "%" || t.val == "&" ||
                    t.val == "#" || t.val == "_" || t.val == "{" || t.val == "}") {
                    out += t.val;
                } else {
                    out += "\\" + t.val;
                }
            } else {
                if (!out.empty() && t.kind == MathTok::Kind::TextChar) {
                    // Add space if needed? Better to parse raw from src_ when possible!
                }
                out += t.val;
            }
            ++pos_;
        }
        return out;
    }

    // For \text{...}, \textrm{...}, \mbox{...}, \operatorname{...}, read the braced
    // text directly from src_ using the command token's src_end to preserve spaces.
    std::string read_braced_text_preserving_spaces(const std::string& /*cmd_name*/) {
        size_t cmd_end = (pos_ > 0 && pos_ - 1 < toks_.size()) ? toks_[pos_ - 1].src_end : src_.size();
        // Always advance token cursor first
        std::string tok_fallback = read_braced_raw_from_tokens();

        size_t p = cmd_end;
        while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t' || src_[p] == '\n')) ++p;
        if (p >= src_.size() || src_[p] != '{') return tok_fallback;
        ++p;
        int depth = 1;
        std::string out;
        while (p < src_.size() && depth > 0) {
            if (src_[p] == '\\' && p + 1 < src_.size()) {
                char nx = src_[p + 1];
                if (nx == '{' || nx == '}' || nx == '$' || nx == '%' || nx == '&' || nx == '#' || nx == '_' || nx == ' ') {
                    out += nx;
                    p += 2;
                    continue;
                }
            }
            if (src_[p] == '{') {
                ++depth;
                out += '{';
            } else if (src_[p] == '}') {
                --depth;
                if (depth > 0) out += '}';
            } else {
                out += src_[p];
            }
            ++p;
        }
        return out;
    }

    std::string parse_delim_token() {
        if (at_end()) return ".";
        const auto& t = peek();
        if (t.kind == MathTok::Kind::Command) {
            std::string v = t.val;
            ++pos_;
            if (v == "{" || v == "lbrace") return "{";
            if (v == "}" || v == "rbrace") return "}";
            if (v == "|" || v == "Vert") return "||";
            if (v == "vert") return "|";
            if (v == "langle") return "<";
            if (v == "rangle") return ">";
            if (v == "lfloor") return "lfloor";
            if (v == "rfloor") return "rfloor";
            if (v == "lceil") return "lceil";
            if (v == "rceil") return "rceil";
            return ".";
        }
        if (t.kind == MathTok::Kind::LBracket) { ++pos_; return "["; }
        if (t.kind == MathTok::Kind::RBracket) { ++pos_; return "]"; }
        if (t.kind == MathTok::Kind::TextChar) {
            std::string v = t.val;
            ++pos_;
            if (v == "(" || v == ")" || v == "|" || v == ".") return v;
            if (v == "<" || v == ">") return v;
            return v;
        }
        return ".";
    }

    NodePtr parse_row(StopMode stop, int depth) {
        auto row = make_node(MathNode::Type::Row);
        if (depth > 64) return row;

        while (!at_end()) {
            const auto& t = peek();
            if (stop == StopMode::Brace && t.kind == MathTok::Kind::RBrace) {
                ++pos_;
                break;
            }
            if (stop == StopMode::RightDelim && t.kind == MathTok::Kind::Command && t.val == "right") {
                break;
            }
            if (stop == StopMode::EnvEnd &&
                ((t.kind == MathTok::Kind::Command && (t.val == "end" || t.val == "\\")) ||
                 t.kind == MathTok::Kind::Ampersand)) {
                break;
            }
            if (t.kind == MathTok::Kind::RBrace) {
                // Stray '}' at top level: consume and ignore
                ++pos_;
                continue;
            }
            if (t.kind == MathTok::Kind::Ampersand ||
                (t.kind == MathTok::Kind::Command && t.val == "\\")) {
                if (stop == StopMode::EnvEnd) break;
                ++pos_;
                continue;
            }
            if (t.kind == MathTok::Kind::Command && t.val == "right") {
                if (stop == StopMode::RightDelim) break;
                ++pos_;
                parse_delim_token();
                continue;
            }

            NodePtr atom = parse_with_scripts(depth + 1);
            if (atom) row->children.push_back(atom);
        }
        return row;
    }

    NodePtr parse_with_scripts(int depth) {
        NodePtr base;
        if (!at_end() && (peek().kind == MathTok::Kind::Caret || peek().kind == MathTok::Kind::Underscore)) {
            auto empty = make_node(MathNode::Type::Space);
            empty->space_em = 0.0;
            base = empty;
        } else {
            base = parse_atom(depth + 1);
        }
        if (!base || depth > 64) return base;

        LimitsMode lmode = LimitsMode::Auto;
        while (!at_end() && peek().kind == MathTok::Kind::Command &&
               (peek().val == "limits" || peek().val == "nolimits")) {
            lmode = (peek().val == "limits") ? LimitsMode::Limits : LimitsMode::NoLimits;
            ++pos_;
        }

        NodePtr sub;
        NodePtr sup;
        std::string primes;

        while (!at_end()) {
            const auto& t = peek();
            if (t.kind == MathTok::Kind::Prime) {
                for (size_t k = 0; k < t.val.size(); ++k) primes += "′";
                ++pos_;
            } else if (t.kind == MathTok::Kind::Caret) {
                ++pos_;
                sup = parse_script_arg(depth + 1);
            } else if (t.kind == MathTok::Kind::Underscore) {
                ++pos_;
                sub = parse_script_arg(depth + 1);
            } else {
                break;
            }
        }

        if (!primes.empty()) {
            auto pnode = make_node(MathNode::Type::Symbol);
            pnode->text = primes;
            pnode->role = AtomRole::Ordinary;
            pnode->style = MathStyle::MathSymbol;
            if (!sup) {
                sup = pnode;
            } else {
                auto row = make_node(MathNode::Type::Row);
                row->children.push_back(pnode);
                row->children.push_back(sup);
                sup = row;
            }
        }

        if (!sub && !sup && lmode == LimitsMode::Auto) {
            return base;
        }
        auto sc = make_node(MathNode::Type::Scripts);
        sc->base = base;
        sc->sub = sub;
        sc->sup = sup;
        sc->limits_mode = lmode;
        return sc;
    }

    NodePtr parse_script_arg(int depth) {
        if (at_end() || depth > 64) return make_node(MathNode::Type::Row);
        if (peek().kind == MathTok::Kind::LBrace) {
            ++pos_;
            return parse_row(StopMode::Brace, depth + 1);
        }
        return parse_atom(depth + 1);
    }

    NodePtr parse_arg(int depth) {
        if (at_end() || depth > 64) return make_node(MathNode::Type::Row);
        if (peek().kind == MathTok::Kind::LBrace) {
            ++pos_;
            return parse_row(StopMode::Brace, depth + 1);
        }
        return parse_atom(depth + 1);
    }

    NodePtr parse_styled_group(MathStyle style, int depth) {
        NodePtr inner = parse_arg(depth + 1);
        apply_style_recursive(inner, style);
        return inner;
    }

    static void apply_style_recursive(const NodePtr& node, MathStyle style) {
        if (!node) return;
        if (node->type == MathNode::Type::Symbol) {
            if (style == MathStyle::MathUpright && node->style == MathStyle::MathItalic) {
                node->style = MathStyle::MathUpright;
            } else if (style == MathStyle::MathBold) {
                node->style = MathStyle::MathBold;
            } else if (style == MathStyle::MathBoldItalic) {
                node->style = MathStyle::MathBoldItalic;
            } else if (style == MathStyle::MathSans) {
                node->style = MathStyle::MathSans;
            } else if (style == MathStyle::MathMono) {
                node->style = MathStyle::MathMono;
            } else if (style == MathStyle::MathItalic) {
                node->style = MathStyle::MathItalic;
            }
        }
        for (const auto& c : node->children) apply_style_recursive(c, style);
        apply_style_recursive(node->num, style);
        apply_style_recursive(node->den, style);
        apply_style_recursive(node->base, style);
        apply_style_recursive(node->sub, style);
        apply_style_recursive(node->sup, style);
        apply_style_recursive(node->inner, style);
        apply_style_recursive(node->radicand, style);
    }

    NodePtr parse_atom(int depth) {
        if (at_end() || depth > 64) return nullptr;
        const MathTok t = peek();

        if (t.kind == MathTok::Kind::LBrace) {
            ++pos_;
            return parse_row(StopMode::Brace, depth + 1);
        }

        if (t.kind == MathTok::Kind::LBracket || t.kind == MathTok::Kind::RBracket) {
            ++pos_;
            auto n = make_node(MathNode::Type::Symbol);
            n->text = t.val;
            n->style = MathStyle::MathUpright;
            n->role = (t.kind == MathTok::Kind::LBracket) ? AtomRole::OpenDelim : AtomRole::CloseDelim;
            return n;
        }

        if (t.kind == MathTok::Kind::TextChar) {
            ++pos_;
            auto n = make_node(MathNode::Type::Symbol);
            n->text = t.val;
            if (t.val.size() == 1) {
                char c = t.val[0];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                    n->style = MathStyle::MathItalic;
                    n->role = AtomRole::Ordinary;
                } else if (c >= '0' && c <= '9') {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::Ordinary;
                } else if (c == '+' || c == '*' || c == '/') {
                    n->style = MathStyle::MathUpright;
                    n->role = (c == '/') ? AtomRole::Ordinary : AtomRole::BinaryOp;
                } else if (c == '-') {
                    n->text = "−"; // Mathematical minus U+2212
                    n->style = MathStyle::MathSymbol;
                    n->role = AtomRole::BinaryOp;
                } else if (c == '=' || c == '<' || c == '>') {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::Relation;
                } else if (c == '(') {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::OpenDelim;
                } else if (c == ')') {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::CloseDelim;
                } else if (c == ',' || c == ';') {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::Punct;
                } else {
                    n->style = MathStyle::MathUpright;
                    n->role = AtomRole::Ordinary;
                }
            } else {
                n->style = MathStyle::MathSymbol;
                n->role = AtomRole::Ordinary;
            }
            return n;
        }

        if (t.kind == MathTok::Kind::Command) {
            std::string cmd = t.val;
            ++pos_;

            // Fractions
            if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac" || cmd == "cfrac") {
                auto f = make_node(MathNode::Type::Fraction);
                f->num = parse_arg(depth + 1);
                f->den = parse_arg(depth + 1);
                f->draw_line = true;
                f->has_parens = false;
                return f;
            }
            if (cmd == "binom" || cmd == "dbinom" || cmd == "tbinom") {
                auto f = make_node(MathNode::Type::Fraction);
                f->num = parse_arg(depth + 1);
                f->den = parse_arg(depth + 1);
                f->draw_line = false;
                f->has_parens = true;
                return f;
            }

            // Roots
            if (cmd == "sqrt") {
                auto r = make_node(MathNode::Type::Sqrt);
                if (!at_end() && peek().kind == MathTok::Kind::LBracket) {
                    ++pos_;
                    auto idx_row = make_node(MathNode::Type::Row);
                    while (!at_end() && peek().kind != MathTok::Kind::RBracket) {
                        NodePtr item = parse_with_scripts(depth + 1);
                        if (item) idx_row->children.push_back(item);
                    }
                    if (!at_end() && peek().kind == MathTok::Kind::RBracket) ++pos_;
                    r->root_index = idx_row;
                }
                r->radicand = parse_arg(depth + 1);
                return r;
            }

            // Delimiters \left ... \right
            if (cmd == "left") {
                auto d = make_node(MathNode::Type::Delimited);
                d->left_delim = parse_delim_token();
                d->inner = parse_row(StopMode::RightDelim, depth + 1);
                if (!at_end() && peek().kind == MathTok::Kind::Command && peek().val == "right") {
                    ++pos_;
                    d->right_delim = parse_delim_token();
                } else {
                    d->right_delim = ".";
                }
                return d;
            }

            // Big delimiter sizing prefixes (\big, \Big, \bigg, \Bigg) -> pass through next token
            if (cmd == "big" || cmd == "Big" || cmd == "bigg" || cmd == "Bigg" ||
                cmd == "bigl" || cmd == "Bigl" || cmd == "biggl" || cmd == "Biggl" ||
                cmd == "bigr" || cmd == "Bigr" || cmd == "biggr" || cmd == "Biggr") {
                return parse_atom(depth + 1);
            }

            // Accents & decorations
            auto make_accent = [&](AccentKind k) {
                auto a = make_node(MathNode::Type::Accent);
                a->accent_kind = k;
                a->inner = parse_arg(depth + 1);
                return a;
            };
            if (cmd == "hat") return make_accent(AccentKind::Hat);
            if (cmd == "widehat") return make_accent(AccentKind::WideHat);
            if (cmd == "tilde") return make_accent(AccentKind::Tilde);
            if (cmd == "widetilde") return make_accent(AccentKind::WideTilde);
            if (cmd == "bar") return make_accent(AccentKind::Bar);
            if (cmd == "overline") return make_accent(AccentKind::Overline);
            if (cmd == "underline") return make_accent(AccentKind::Underline);
            if (cmd == "vec") return make_accent(AccentKind::Vec);
            if (cmd == "dot") return make_accent(AccentKind::Dot);
            if (cmd == "ddot") return make_accent(AccentKind::DDot);
            if (cmd == "dddot") return make_accent(AccentKind::DDDot);
            if (cmd == "overbrace") return make_accent(AccentKind::Overbrace);
            if (cmd == "underbrace") return make_accent(AccentKind::Underbrace);
            if (cmd == "overrightarrow") return make_accent(AccentKind::OverRightArrow);
            if (cmd == "overleftarrow") return make_accent(AccentKind::OverLeftArrow);

            // Spacing commands
            if (cmd == ",") { auto s = make_node(MathNode::Type::Space); s->space_em = 0.167; return s; }
            if (cmd == ":" || cmd == ">") { auto s = make_node(MathNode::Type::Space); s->space_em = 0.222; return s; }
            if (cmd == ";") { auto s = make_node(MathNode::Type::Space); s->space_em = 0.278; return s; }
            if (cmd == "!") { auto s = make_node(MathNode::Type::Space); s->space_em = -0.167; return s; }
            if (cmd == " ") { auto s = make_node(MathNode::Type::Space); s->space_em = 0.33; return s; }
            if (cmd == "quad") { auto s = make_node(MathNode::Type::Space); s->space_em = 1.0; return s; }
            if (cmd == "qquad") { auto s = make_node(MathNode::Type::Space); s->space_em = 2.0; return s; }

            // Text & Font commands
            if (cmd == "text" || cmd == "textrm" || cmd == "mbox") {
                auto n = make_node(MathNode::Type::TextRun);
                n->text = read_braced_text_preserving_spaces(cmd);
                n->style = MathStyle::PlainText;
                n->role = AtomRole::Ordinary;
                return n;
            }
            if (cmd == "operatorname") {
                auto n = make_node(MathNode::Type::Symbol);
                n->text = read_braced_text_preserving_spaces(cmd);
                n->style = MathStyle::MathUpright;
                n->role = AtomRole::FunctionName;
                return n;
            }
            if (cmd == "mathrm") return parse_styled_group(MathStyle::MathUpright, depth);
            if (cmd == "mathbf" || cmd == "textbf") return parse_styled_group(MathStyle::MathBold, depth);
            if (cmd == "boldsymbol" || cmd == "bm") return parse_styled_group(MathStyle::MathBoldItalic, depth);
            if (cmd == "mathit" || cmd == "textit") return parse_styled_group(MathStyle::MathItalic, depth);
            if (cmd == "mathsf" || cmd == "textsf") return parse_styled_group(MathStyle::MathSans, depth);
            if (cmd == "mathtt" || cmd == "texttt") return parse_styled_group(MathStyle::MathMono, depth);

            if (cmd == "mathbb") {
                std::string raw = read_braced_text_preserving_spaces(cmd);
                auto n = make_node(MathNode::Type::Symbol);
                n->text = to_blackboard_bold(raw);
                n->style = MathStyle::MathSymbol;
                n->role = AtomRole::Ordinary;
                return n;
            }
            if (cmd == "mathcal" || cmd == "mathscr") {
                std::string raw = read_braced_text_preserving_spaces(cmd);
                auto n = make_node(MathNode::Type::Symbol);
                n->text = to_calligraphic(raw);
                n->style = MathStyle::MathSymbol;
                n->role = AtomRole::Ordinary;
                return n;
            }
            if (cmd == "mathfrak") {
                std::string raw = read_braced_text_preserving_spaces(cmd);
                auto n = make_node(MathNode::Type::Symbol);
                n->text = to_fraktur(raw);
                n->style = MathStyle::MathSymbol;
                n->role = AtomRole::Ordinary;
                return n;
            }

            // Color commands
            if (cmd == "color" || cmd == "textcolor") {
                std::string cname = read_braced_text_preserving_spaces(cmd);
                auto cg = make_node(MathNode::Type::ColorGroup);
                cg->color_str = cname;
                cg->inner = parse_arg(depth + 1);
                return cg;
            }

            // Environments \begin{...} ... \end{...}
            if (cmd == "begin") {
                std::string env = read_braced_text_preserving_spaces("begin");
                // Trim whitespace in env name
                while (!env.empty() && env.front() == ' ') env.erase(env.begin());
                while (!env.empty() && env.back() == ' ') env.pop_back();

                // Optional column spec for array: \begin{array}{cc}
                if (env == "array" && !at_end() && peek().kind == MathTok::Kind::LBrace) {
                    read_braced_raw_from_tokens();
                }

                auto mat = make_node(MathNode::Type::Matrix);
                mat->env_name = env;

                std::vector<NodePtr> cur_row;
                while (!at_end()) {
                    if (peek().kind == MathTok::Kind::Command && peek().val == "end") {
                        ++pos_;
                        read_braced_raw_from_tokens();
                        break;
                    }
                    NodePtr cell = parse_row(StopMode::EnvEnd, depth + 1);
                    cur_row.push_back(cell);
                    if (!at_end() && peek().kind == MathTok::Kind::Ampersand) {
                        ++pos_;
                        continue;
                    }
                    if (!at_end() && peek().kind == MathTok::Kind::Command && peek().val == "\\") {
                        ++pos_;
                        mat->rows.push_back(std::move(cur_row));
                        cur_row.clear();
                        continue;
                    }
                }
                if (!cur_row.empty()) {
                    mat->rows.push_back(std::move(cur_row));
                }
                return mat;
            }

            // Named math functions
            auto fit = function_table().find(cmd);
            if (fit != function_table().end()) {
                auto n = make_node(MathNode::Type::Symbol);
                n->text = fit->second.name;
                n->role = AtomRole::FunctionName;
                n->style = MathStyle::MathUpright;
                n->stack_limits = fit->second.stack_limits_in_display;
                return n;
            }

            // Known symbols
            auto sit = symbol_table().find(cmd);
            if (sit != symbol_table().end()) {
                auto n = make_node(MathNode::Type::Symbol);
                n->text = sit->second.utf8;
                n->role = sit->second.role;
                n->style = sit->second.style;
                n->stack_limits = sit->second.stack_limits_in_display;
                return n;
            }

            // Unknown command fallback: render literally as upright text so typos remain visible
            auto n = make_node(MathNode::Type::Symbol);
            n->text = "\\" + cmd;
            n->role = AtomRole::Ordinary;
            n->style = MathStyle::MathMono;
            return n;
        }

        ++pos_;
        return nullptr;
    }
};

// ─── Color Parsing Helper ────────────────────────────────────────────────────

static bool parse_latex_color(const std::string& name, RGBA& out) {
    if (name.empty()) return false;
    if (name[0] == '#' && name.size() >= 7) {
        out = hex_to_rgba(name);
        return true;
    }
    std::string low;
    for (char c : name) {
        if (c != ' ') low += (char)std::tolower((unsigned char)c);
    }
    static const std::unordered_map<std::string, const char*> named = {
        {"red",     "#FF3B30"},
        {"orange",  "#FF9500"},
        {"yellow",  "#FFCC00"},
        {"green",   "#34C759"},
        {"blue",    "#007AFF"},
        {"indigo",  "#5856D6"},
        {"purple",  "#AF52DE"},
        {"pink",    "#FF2D55"},
        {"white",   "#FFFFFF"},
        {"gray",    "#8E8E93"},
        {"grey",    "#8E8E93"},
        {"black",   "#000000"},
        {"cyan",    "#32ADE6"},
        {"magenta", "#FF2D55"},
        {"teal",    "#30B0C7"},
        {"brown",   "#A2845E"},
    };
    auto it = named.find(low);
    if (it != named.end()) {
        out = hex_to_rgba(it->second);
        return true;
    }
    return false;
}

// ─── Layout Box Tree & Rendering ─────────────────────────────────────────────

struct LayoutBox {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    double left_ext = 0.0;  // left ink overhang (<= 0)
    double right_ext = 0.0; // right ink extent (>= width)

    enum class DrawKind {
        Container,
        Glyphs,
        HLine,
        Radical,
        Delimiter,
        AccentDraw,
        ColorWrap
    };

    DrawKind draw_kind = DrawKind::Container;

    struct Child {
        double dx = 0.0;
        double dy = 0.0;
        std::shared_ptr<LayoutBox> box;
    };
    std::vector<Child> children;

    // Glyphs
    std::string text;
    std::string font_family;
    cairo_font_slant_t slant = CAIRO_FONT_SLANT_NORMAL;
    cairo_font_weight_t weight = CAIRO_FONT_WEIGHT_NORMAL;
    double font_size = 24.0;
    double glyph_dx = 0.0; // shift if x_bearing < 0

    // HLine
    double line_thickness = 1.5;

    // Radical
    double rad_w = 0.0;
    double rad_top_y = 0.0;
    double rad_bot_y = 0.0;
    double rad_bar_w = 0.0;

    // Delimiter
    std::string delim_type;
    double delim_top_y = 0.0;
    double delim_bot_y = 0.0;

    // Accent
    AccentKind accent_kind = AccentKind::Hat;
    double accent_y = 0.0;
    double accent_w = 0.0;

    // ColorWrap
    RGBA color{1, 1, 1, 1};
};

using BoxPtr = std::shared_ptr<LayoutBox>;

static BoxPtr make_box(LayoutBox::DrawKind k = LayoutBox::DrawKind::Container) {
    auto b = std::make_shared<LayoutBox>();
    b->draw_kind = k;
    return b;
}

static void add_child(const BoxPtr& parent, double dx, double dy, const BoxPtr& child) {
    if (!child) return;
    parent->children.push_back({dx, dy, child});
    parent->ascent = std::max(parent->ascent, child->ascent - dy);
    parent->descent = std::max(parent->descent, child->descent + dy);
    parent->left_ext = std::min(parent->left_ext, dx + child->left_ext);
    parent->right_ext = std::max(parent->right_ext, dx + std::max(child->width, child->right_ext));
}

static BoxPtr layout_text_glyphs(cairo_t* cr, const std::string& utf8,
                                 MathStyle style, double font_size) {
    auto b = make_box(LayoutBox::DrawKind::Glyphs);
    b->text = utf8;
    b->font_size = font_size;

    if (utf8.empty()) {
        b->ascent = font_size * 0.72;
        b->descent = font_size * 0.18;
        return b;
    }

    ResolvedFont rf = resolve_font_for(cr, style, utf8);
    b->font_family = rf.family;
    b->slant = rf.slant;
    b->weight = rf.weight;

    cairo_save(cr);
    cairo_select_font_face(cr, rf.family.c_str(), rf.slant, rf.weight);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, utf8.c_str(), &ext);
    cairo_restore(cr);

    double pad_l = (ext.x_bearing < 0) ? -ext.x_bearing : 0.0;
    b->glyph_dx = pad_l;
    b->width = std::max(ext.x_advance + pad_l, ext.width + pad_l);
    if (style == MathStyle::MathItalic && utf8.size() == 1) {
        // Italic right overhang protection
        b->width = std::max(b->width, ext.x_advance + pad_l + font_size * 0.04);
    }
    b->ascent = std::max(-ext.y_bearing, font_size * 0.45);
    b->descent = std::max(ext.height + ext.y_bearing, 0.0);
    b->left_ext = 0.0;
    b->right_ext = std::max(b->width, pad_l + ext.x_bearing + ext.width);
    return b;
}

static BoxPtr layout_delimiter_box(const std::string& delim, double top_y, double bot_y, double font_size) {
    auto b = make_box(LayoutBox::DrawKind::Delimiter);
    b->delim_type = delim;
    b->delim_top_y = top_y;
    b->delim_bot_y = bot_y;
    b->font_size = font_size;
    b->line_thickness = std::max(1.2, font_size * 0.055);
    double h = std::max(bot_y - top_y, font_size);
    if (delim == ".") {
        b->width = 0.0;
    } else if (delim == "|") {
        b->width = std::max(font_size * 0.28, b->line_thickness * 3.0);
    } else if (delim == "||") {
        b->width = std::max(font_size * 0.42, b->line_thickness * 5.0);
    } else if (delim == "(" || delim == ")") {
        b->width = std::clamp(h * 0.22, font_size * 0.30, font_size * 0.65);
    } else if (delim == "[" || delim == "]" || delim == "lfloor" || delim == "rfloor" ||
               delim == "lceil" || delim == "rceil") {
        b->width = std::clamp(h * 0.20, font_size * 0.30, font_size * 0.60);
    } else if (delim == "{" || delim == "}") {
        b->width = std::clamp(h * 0.26, font_size * 0.36, font_size * 0.72);
    } else {
        b->width = std::clamp(h * 0.24, font_size * 0.34, font_size * 0.68);
    }
    b->ascent = std::max(0.0, -top_y);
    b->descent = std::max(0.0, bot_y);
    b->right_ext = b->width;
    return b;
}

static BoxPtr layout_math_node(cairo_t* cr, const NodePtr& node, double font_size, bool is_display, int depth = 0);

static double inter_atom_space(AtomRole prev_role, AtomRole cur_role, bool prev_was_open_or_start, double font_size) {
    if (cur_role == AtomRole::BinaryOp && prev_was_open_or_start) {
        return 0.0; // Unary + / -
    }
    if (prev_role == AtomRole::BinaryOp || cur_role == AtomRole::BinaryOp) {
        return font_size * 0.22;
    }
    if (prev_role == AtomRole::Relation || cur_role == AtomRole::Relation) {
        return font_size * 0.28;
    }
    if (prev_role == AtomRole::Punct) {
        return font_size * 0.16;
    }
    if (prev_role == AtomRole::FunctionName && cur_role != AtomRole::OpenDelim) {
        return font_size * 0.16;
    }
    if (prev_role == AtomRole::LargeOp && cur_role != AtomRole::OpenDelim) {
        return font_size * 0.14;
    }
    return 0.0;
}

static AtomRole node_role(const NodePtr& node) {
    if (!node) return AtomRole::Ordinary;
    if (node->type == MathNode::Type::Symbol) return node->role;
    if (node->type == MathNode::Type::Scripts) return node_role(node->base);
    if (node->type == MathNode::Type::Delimited) return AtomRole::Ordinary;
    return AtomRole::Ordinary;
}

static bool node_is_large_op(const NodePtr& node, bool& stack_in_display) {
    if (!node || node->type != MathNode::Type::Symbol) return false;
    if (node->role == AtomRole::LargeOp || node->role == AtomRole::FunctionName) {
        stack_in_display = node->stack_limits;
        return node->role == AtomRole::LargeOp || node->stack_limits;
    }
    return false;
}

static BoxPtr layout_math_node(cairo_t* cr, const NodePtr& node, double font_size, bool is_display, int depth) {
    if (!node || depth > 64) {
        auto empty = make_box();
        empty->ascent = font_size * 0.7;
        return empty;
    }

    const double axis_h = font_size * 0.28;

    switch (node->type) {
        case MathNode::Type::Space: {
            auto b = make_box();
            b->width = node->space_em * font_size;
            b->ascent = font_size * 0.5;
            b->descent = 0.0;
            return b;
        }

        case MathNode::Type::TextRun: {
            return layout_text_glyphs(cr, node->text, MathStyle::PlainText, font_size);
        }

        case MathNode::Type::Symbol: {
            double sz = font_size;
            if (node->role == AtomRole::LargeOp && is_display) {
                sz = font_size * 1.38;
            } else if (node->role == AtomRole::LargeOp) {
                sz = font_size * 1.15;
            }
            BoxPtr b = layout_text_glyphs(cr, node->text, node->style, sz);
            if (node->role == AtomRole::LargeOp) {
                // Vertically center large operator around the math axis
                double mid = (b->ascent - b->descent) * 0.5;
                double shift = mid - axis_h;
                if (std::abs(shift) > 0.5) {
                    auto wrap = make_box();
                    wrap->width = b->width;
                    add_child(wrap, 0.0, shift, b);
                    return wrap;
                }
            }
            return b;
        }

        case MathNode::Type::Row: {
            auto row = make_box();
            if (node->children.empty()) {
                row->ascent = font_size * 0.72;
                row->descent = font_size * 0.18;
                return row;
            }

            double x = 0.0;
            AtomRole prev_role = AtomRole::OpenDelim;
            bool prev_open_or_start = true;

            for (size_t i = 0; i < node->children.size(); ++i) {
                const NodePtr& ch = node->children[i];
                if (ch->type == MathNode::Type::Space) {
                    x = std::max(0.0, x + ch->space_em * font_size);
                    continue;
                }
                AtomRole cur_role = node_role(ch);
                bool is_unary = (cur_role == AtomRole::BinaryOp && prev_open_or_start);
                if (i > 0) {
                    x += inter_atom_space(prev_role, cur_role, prev_open_or_start, font_size);
                }
                BoxPtr cb = layout_math_node(cr, ch, font_size, is_display, depth + 1);
                add_child(row, x, 0.0, cb);
                x += cb->width;
                prev_role = is_unary ? AtomRole::OpenDelim : cur_role;
                prev_open_or_start = (cur_role == AtomRole::OpenDelim ||
                                      cur_role == AtomRole::BinaryOp ||
                                      cur_role == AtomRole::Relation ||
                                      cur_role == AtomRole::Punct);
            }
            row->width = std::max(x, 0.0);
            row->right_ext = std::max(row->right_ext, row->width);
            return row;
        }

        case MathNode::Type::Fraction: {
            double child_sz = std::max(font_size * (is_display ? 0.82 : 0.72), 8.0);
            BoxPtr num_b = layout_math_node(cr, node->num, child_sz, false, depth + 1);
            BoxPtr den_b = layout_math_node(cr, node->den, child_sz, false, depth + 1);

            double bar_t = std::max(1.0, font_size * 0.055);
            double gap = std::max(1.8, font_size * 0.11);
            double pad_x = font_size * 0.14;
            double content_w = std::max(num_b->width, den_b->width) + pad_x;

            double bar_y = -axis_h;
            double num_y = bar_y - bar_t * 0.5 - gap - num_b->descent;
            double den_y = bar_y + bar_t * 0.5 + gap + den_b->ascent;

            auto frac = make_box();
            add_child(frac, (content_w - num_b->width) * 0.5, num_y, num_b);
            add_child(frac, (content_w - den_b->width) * 0.5, den_y, den_b);

            if (node->draw_line) {
                auto line = make_box(LayoutBox::DrawKind::HLine);
                line->width = content_w;
                line->line_thickness = bar_t;
                line->ascent = bar_t * 0.5;
                line->descent = bar_t * 0.5;
                line->right_ext = content_w;
                add_child(frac, 0.0, bar_y, line);
            }
            frac->width = content_w;
            frac->right_ext = std::max(frac->right_ext, frac->width);

            if (node->has_parens) {
                double pad_v = font_size * 0.08;
                double dtop = -frac->ascent - pad_v;
                double dbot = frac->descent + pad_v;
                // Symmetrize around math axis
                double half = std::max(-dtop - axis_h, dbot + axis_h);
                dtop = -axis_h - half;
                dbot = -axis_h + half;
                BoxPtr lp = layout_delimiter_box("(", dtop, dbot, font_size);
                BoxPtr rp = layout_delimiter_box(")", dtop, dbot, font_size);
                auto wrap = make_box();
                add_child(wrap, 0.0, 0.0, lp);
                add_child(wrap, lp->width, 0.0, frac);
                add_child(wrap, lp->width + frac->width, 0.0, rp);
                wrap->width = lp->width + frac->width + rp->width;
                wrap->right_ext = std::max(wrap->right_ext, wrap->width);
                return wrap;
            }
            return frac;
        }

        case MathNode::Type::Sqrt: {
            BoxPtr inner = layout_math_node(cr, node->radicand, font_size, is_display, depth + 1);
            double bar_t = std::max(1.0, font_size * 0.055);
            double pad_top = std::max(2.0, font_size * 0.14);
            double pad_right = font_size * 0.10;
            double top_y = -(inner->ascent + pad_top);
            double bot_y = inner->descent + font_size * 0.06;
            double H = bot_y - top_y;
            double rad_w = std::clamp(H * 0.34, font_size * 0.42, font_size * 0.95);

            double extra_left = 0.0;
            BoxPtr idx_b;
            if (node->root_index) {
                double idx_sz = std::max(font_size * 0.56, 8.0);
                idx_b = layout_math_node(cr, node->root_index, idx_sz, false, depth + 1);
                extra_left = std::max(0.0, idx_b->width - rad_w * 0.32);
            }

            auto rad = make_box(LayoutBox::DrawKind::Radical);
            rad->rad_w = rad_w;
            rad->rad_top_y = top_y;
            rad->rad_bot_y = bot_y;
            rad->rad_bar_w = inner->width + pad_right;
            rad->line_thickness = bar_t;
            rad->width = rad_w + inner->width + pad_right;
            rad->ascent = std::max(0.0, -top_y + bar_t * 0.5);
            rad->descent = std::max(0.0, bot_y);
            rad->right_ext = rad->width;

            auto wrap = make_box();
            add_child(wrap, extra_left, 0.0, rad);
            add_child(wrap, extra_left + rad_w + font_size * 0.04, 0.0, inner);
            if (idx_b) {
                double idx_x = extra_left + rad_w * 0.35 - idx_b->width;
                double idx_y = bot_y - H * 0.52 - idx_b->descent;
                add_child(wrap, idx_x, idx_y, idx_b);
            }
            wrap->width = extra_left + rad->width + font_size * 0.04;
            wrap->right_ext = std::max(wrap->right_ext, wrap->width);
            return wrap;
        }

        case MathNode::Type::Scripts: {
            BoxPtr base_b = layout_math_node(cr, node->base, font_size, is_display, depth + 1);
            bool stack_default = false;
            bool is_op = node_is_large_op(node->base, stack_default);
            bool use_limits = false;
            if (node->limits_mode == LimitsMode::Limits) use_limits = true;
            else if (node->limits_mode == LimitsMode::NoLimits) use_limits = false;
            else if (is_op && is_display && stack_default) use_limits = true;

            double script_sz = std::max(font_size * (use_limits ? 0.62 : 0.65), 8.0);
            BoxPtr sub_b = node->sub ? layout_math_node(cr, node->sub, script_sz, false, depth + 1) : nullptr;
            BoxPtr sup_b = node->sup ? layout_math_node(cr, node->sup, script_sz, false, depth + 1) : nullptr;

            auto wrap = make_box();
            if (use_limits) {
                double w = base_b->width;
                if (sub_b) w = std::max(w, sub_b->width);
                if (sup_b) w = std::max(w, sup_b->width);
                double gap = font_size * 0.10;
                add_child(wrap, (w - base_b->width) * 0.5, 0.0, base_b);
                if (sup_b) {
                    double sy = -(base_b->ascent + gap + sup_b->descent);
                    add_child(wrap, (w - sup_b->width) * 0.5, sy, sup_b);
                }
                if (sub_b) {
                    double sy = base_b->descent + gap + sub_b->ascent;
                    add_child(wrap, (w - sub_b->width) * 0.5, sy, sub_b);
                }
                wrap->width = w;
                wrap->right_ext = std::max(wrap->right_ext, w);
                return wrap;
            }

            add_child(wrap, 0.0, 0.0, base_b);
            double sup_y = 0.0;
            double sub_y = 0.0;
            if (sup_b) {
                sup_y = -std::max(base_b->ascent * 0.58, font_size * 0.40);
                if (sup_y + sup_b->descent > -font_size * 0.14) {
                    sup_y = -font_size * 0.14 - sup_b->descent;
                }
            }
            if (sub_b) {
                sub_y = std::max(base_b->descent * 0.55 + font_size * 0.18, font_size * 0.22);
            }
            if (sup_b && sub_b) {
                double min_gap = font_size * 0.12;
                double gap = (sub_y - sub_b->ascent) - (sup_y + sup_b->descent);
                if (gap < min_gap) {
                    double push = (min_gap - gap) * 0.5;
                    sup_y -= push;
                    sub_y += push;
                }
            }
            double sx = base_b->width + font_size * 0.02;
            double max_sw = 0.0;
            if (sup_b) {
                add_child(wrap, sx, sup_y, sup_b);
                max_sw = std::max(max_sw, sup_b->width);
            }
            if (sub_b) {
                add_child(wrap, sx, sub_y, sub_b);
                max_sw = std::max(max_sw, sub_b->width);
            }
            wrap->width = sx + max_sw + font_size * 0.03;
            wrap->right_ext = std::max(wrap->right_ext, wrap->width);
            return wrap;
        }

        case MathNode::Type::Delimited: {
            BoxPtr inner_b = layout_math_node(cr, node->inner, font_size, is_display, depth + 1);
            double pad_v = font_size * 0.08;
            double top_y = -inner_b->ascent - pad_v;
            double bot_y = inner_b->descent + pad_v;
            double half = std::max({-top_y - axis_h, bot_y + axis_h, font_size * 0.55});
            top_y = -axis_h - half;
            bot_y = -axis_h + half;

            BoxPtr lb = layout_delimiter_box(node->left_delim, top_y, bot_y, font_size);
            BoxPtr rb = layout_delimiter_box(node->right_delim, top_y, bot_y, font_size);
            double gap = font_size * 0.04;

            auto wrap = make_box();
            double cx = 0.0;
            if (lb->width > 0.0) {
                add_child(wrap, cx, 0.0, lb);
                cx += lb->width + gap;
            }
            add_child(wrap, cx, 0.0, inner_b);
            cx += inner_b->width;
            if (rb->width > 0.0) {
                cx += gap;
                add_child(wrap, cx, 0.0, rb);
                cx += rb->width;
            }
            wrap->width = cx;
            wrap->right_ext = std::max(wrap->right_ext, cx);
            return wrap;
        }

        case MathNode::Type::Accent: {
            BoxPtr inner_b = layout_math_node(cr, node->inner, font_size, is_display, depth + 1);
            auto wrap = make_box();
            double w = std::max(inner_b->width, font_size * 0.35);
            double inner_dx = (w - inner_b->width) * 0.5;
            add_child(wrap, inner_dx, 0.0, inner_b);

            auto acc = make_box(LayoutBox::DrawKind::AccentDraw);
            acc->accent_kind = node->accent_kind;
            acc->accent_w = w;
            acc->width = w;
            acc->font_size = font_size;
            acc->line_thickness = std::max(1.0, font_size * 0.055);

            bool is_below = (node->accent_kind == AccentKind::Underline ||
                             node->accent_kind == AccentKind::Underbrace);
            if (is_below) {
                double acc_h = (node->accent_kind == AccentKind::Underbrace)
                    ? font_size * 0.24 : acc->line_thickness * 2.2;
                double ay = inner_b->descent + font_size * 0.10;
                acc->accent_y = ay;
                acc->ascent = 0.0;
                acc->descent = ay + acc_h;
            } else {
                double acc_h = (node->accent_kind == AccentKind::Overbrace)
                    ? font_size * 0.24 : font_size * 0.20;
                double ay = -(inner_b->ascent + font_size * 0.08);
                acc->accent_y = ay;
                acc->ascent = -ay + acc_h;
                acc->descent = 0.0;
            }
            acc->right_ext = w;
            add_child(wrap, 0.0, 0.0, acc);
            wrap->width = w;
            wrap->right_ext = std::max(wrap->right_ext, w);
            return wrap;
        }

        case MathNode::Type::Matrix: {
            size_t nrows = node->rows.size();
            size_t ncols = 0;
            for (const auto& r : node->rows) ncols = std::max(ncols, r.size());

            if (nrows == 0 || ncols == 0) {
                auto empty = make_box();
                empty->ascent = font_size * 0.7;
                return empty;
            }

            std::vector<std::vector<BoxPtr>> cells(nrows, std::vector<BoxPtr>(ncols));
            std::vector<double> col_w(ncols, 0.0);
            std::vector<double> row_asc(nrows, font_size * 0.65);
            std::vector<double> row_desc(nrows, font_size * 0.20);

            for (size_t r = 0; r < nrows; ++r) {
                for (size_t c = 0; c < node->rows[r].size(); ++c) {
                    cells[r][c] = layout_math_node(cr, node->rows[r][c], font_size, false, depth + 1);
                    col_w[c] = std::max(col_w[c], cells[r][c]->width);
                    row_asc[r] = std::max(row_asc[r], cells[r][c]->ascent);
                    row_desc[r] = std::max(row_desc[r], cells[r][c]->descent);
                }
            }

            double col_gap = font_size * 0.65;
            double row_gap = font_size * 0.35;
            double total_w = 0.0;
            for (size_t c = 0; c < ncols; ++c) {
                total_w += col_w[c];
                if (c + 1 < ncols) total_w += col_gap;
            }
            double total_h = 0.0;
            for (size_t r = 0; r < nrows; ++r) {
                total_h += row_asc[r] + row_desc[r];
                if (r + 1 < nrows) total_h += row_gap;
            }

            bool is_cases = (node->env_name == "cases");
            bool is_aligned = (node->env_name == "aligned" || node->env_name == "align");

            auto grid = make_box();
            double top = -axis_h - total_h * 0.5;
            double cur_y = top;
            for (size_t r = 0; r < nrows; ++r) {
                double base_y = cur_y + row_asc[r];
                double cur_x = 0.0;
                for (size_t c = 0; c < ncols; ++c) {
                    if (cells[r][c]) {
                        double cell_dx = cur_x;
                        if (is_cases || is_aligned) {
                            cell_dx = cur_x; // left-aligned
                        } else {
                            cell_dx = cur_x + (col_w[c] - cells[r][c]->width) * 0.5;
                        }
                        add_child(grid, cell_dx, base_y, cells[r][c]);
                    }
                    cur_x += col_w[c] + col_gap;
                }
                cur_y += row_asc[r] + row_desc[r] + row_gap;
            }
            grid->width = total_w;
            grid->right_ext = std::max(grid->right_ext, total_w);

            std::string ldel = ".";
            std::string rdel = ".";
            if (node->env_name == "pmatrix") { ldel = "("; rdel = ")"; }
            else if (node->env_name == "bmatrix") { ldel = "["; rdel = "]"; }
            else if (node->env_name == "Bmatrix") { ldel = "{"; rdel = "}"; }
            else if (node->env_name == "vmatrix") { ldel = "|"; rdel = "|"; }
            else if (node->env_name == "Vmatrix") { ldel = "||"; rdel = "||"; }
            else if (is_cases) { ldel = "{"; rdel = "."; }

            if (ldel == "." && rdel == ".") return grid;

            double pad_v = font_size * 0.12;
            double dtop = -grid->ascent - pad_v;
            double dbot = grid->descent + pad_v;
            double half = std::max(-dtop - axis_h, dbot + axis_h);
            dtop = -axis_h - half;
            dbot = -axis_h + half;

            BoxPtr lb = layout_delimiter_box(ldel, dtop, dbot, font_size);
            BoxPtr rb = layout_delimiter_box(rdel, dtop, dbot, font_size);
            double gap = font_size * 0.08;

            auto wrap = make_box();
            double cx = 0.0;
            if (lb->width > 0.0) {
                add_child(wrap, cx, 0.0, lb);
                cx += lb->width + gap;
            }
            add_child(wrap, cx, 0.0, grid);
            cx += grid->width;
            if (rb->width > 0.0) {
                cx += gap;
                add_child(wrap, cx, 0.0, rb);
                cx += rb->width;
            }
            wrap->width = cx;
            wrap->right_ext = std::max(wrap->right_ext, cx);
            return wrap;
        }

        case MathNode::Type::ColorGroup: {
            BoxPtr inner_b = layout_math_node(cr, node->inner, font_size, is_display, depth + 1);
            RGBA col;
            if (!parse_latex_color(node->color_str, col)) {
                return inner_b;
            }
            auto wrap = make_box(LayoutBox::DrawKind::ColorWrap);
            wrap->color = col;
            add_child(wrap, 0.0, 0.0, inner_b);
            wrap->width = inner_b->width;
            wrap->right_ext = inner_b->right_ext;
            return wrap;
        }
    }
    return make_box();
}

// ─── Layout Drawing ──────────────────────────────────────────────────────────

static void draw_delimiter_path(cairo_t* cr, const LayoutBox& b, double x, double y) {
    const std::string& d = b.delim_type;
    if (d == "." || d.empty()) return;

    double top = y + b.delim_top_y;
    double bot = y + b.delim_bot_y;
    double mid = (top + bot) * 0.5;
    double h = bot - top;
    double w = b.width;
    double lw = b.line_thickness;

    cairo_save(cr);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (d == "(") {
        double x_out = x + w * 0.80;
        double x_in = x + w * 0.22;
        cairo_move_to(cr, x_out, top);
        cairo_curve_to(cr, x_in, top + h * 0.25, x_in, bot - h * 0.25, x_out, bot);
        cairo_stroke(cr);
    } else if (d == ")") {
        double x_out = x + w * 0.20;
        double x_in = x + w * 0.78;
        cairo_move_to(cr, x_out, top);
        cairo_curve_to(cr, x_in, top + h * 0.25, x_in, bot - h * 0.25, x_out, bot);
        cairo_stroke(cr);
    } else if (d == "[") {
        double x_stem = x + w * 0.30;
        double x_tip = x + w * 0.82;
        cairo_move_to(cr, x_tip, top);
        cairo_line_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_line_to(cr, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "]") {
        double x_stem = x + w * 0.70;
        double x_tip = x + w * 0.18;
        cairo_move_to(cr, x_tip, top);
        cairo_line_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_line_to(cr, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "{") {
        double x_tip = x + w * 0.82;
        double x_mid = x + w * 0.48;
        double x_cusp = x + w * 0.14;
        cairo_move_to(cr, x_tip, top);
        cairo_curve_to(cr, x_mid, top, x_mid, mid - h * 0.08, x_cusp, mid);
        cairo_curve_to(cr, x_mid, mid + h * 0.08, x_mid, bot, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "}") {
        double x_tip = x + w * 0.18;
        double x_mid = x + w * 0.52;
        double x_cusp = x + w * 0.86;
        cairo_move_to(cr, x_tip, top);
        cairo_curve_to(cr, x_mid, top, x_mid, mid - h * 0.08, x_cusp, mid);
        cairo_curve_to(cr, x_mid, mid + h * 0.08, x_mid, bot, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "|") {
        double cx = x + w * 0.5;
        cairo_move_to(cr, cx, top);
        cairo_line_to(cr, cx, bot);
        cairo_stroke(cr);
    } else if (d == "||") {
        double x1 = x + w * 0.34;
        double x2 = x + w * 0.66;
        cairo_move_to(cr, x1, top);
        cairo_line_to(cr, x1, bot);
        cairo_move_to(cr, x2, top);
        cairo_line_to(cr, x2, bot);
        cairo_stroke(cr);
    } else if (d == "<") {
        double x_out = x + w * 0.82;
        double x_in = x + w * 0.18;
        cairo_move_to(cr, x_out, top);
        cairo_line_to(cr, x_in, mid);
        cairo_line_to(cr, x_out, bot);
        cairo_stroke(cr);
    } else if (d == ">") {
        double x_out = x + w * 0.18;
        double x_in = x + w * 0.82;
        cairo_move_to(cr, x_out, top);
        cairo_line_to(cr, x_in, mid);
        cairo_line_to(cr, x_out, bot);
        cairo_stroke(cr);
    } else if (d == "lfloor") {
        double x_stem = x + w * 0.30;
        double x_tip = x + w * 0.82;
        cairo_move_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_line_to(cr, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "rfloor") {
        double x_stem = x + w * 0.70;
        double x_tip = x + w * 0.18;
        cairo_move_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_line_to(cr, x_tip, bot);
        cairo_stroke(cr);
    } else if (d == "lceil") {
        double x_stem = x + w * 0.30;
        double x_tip = x + w * 0.82;
        cairo_move_to(cr, x_tip, top);
        cairo_line_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_stroke(cr);
    } else if (d == "rceil") {
        double x_stem = x + w * 0.70;
        double x_tip = x + w * 0.18;
        cairo_move_to(cr, x_tip, top);
        cairo_line_to(cr, x_stem, top);
        cairo_line_to(cr, x_stem, bot);
        cairo_stroke(cr);
    } else if (d == "/") {
        cairo_move_to(cr, x + w * 0.80, top);
        cairo_line_to(cr, x + w * 0.20, bot);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void draw_accent_path(cairo_t* cr, const LayoutBox& b, double x, double y) {
    double w = b.accent_w;
    double ay = y + b.accent_y;
    double lw = b.line_thickness;
    double fs = b.font_size;

    cairo_save(cr);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (b.accent_kind) {
        case AccentKind::Bar:
        case AccentKind::Overline:
        case AccentKind::Underline: {
            double pad = (b.accent_kind == AccentKind::Bar) ? w * 0.12 : 0.0;
            cairo_move_to(cr, x + pad, ay);
            cairo_line_to(cr, x + w - pad, ay);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::Hat:
        case AccentKind::WideHat: {
            double pad = (b.accent_kind == AccentKind::Hat) ? std::max(0.0, (w - fs * 0.36) * 0.5) : 0.0;
            double peak_h = fs * 0.14;
            cairo_move_to(cr, x + pad, ay);
            cairo_line_to(cr, x + w * 0.5, ay - peak_h);
            cairo_line_to(cr, x + w - pad, ay);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::Tilde:
        case AccentKind::WideTilde: {
            double pad = (b.accent_kind == AccentKind::Tilde) ? std::max(0.0, (w - fs * 0.42) * 0.5) : 0.0;
            double x0 = x + pad;
            double x1 = x + w - pad;
            double amp = fs * 0.10;
            cairo_move_to(cr, x0, ay - amp * 0.2);
            cairo_curve_to(cr,
                           x0 + (x1 - x0) * 0.33, ay - amp * 1.4,
                           x0 + (x1 - x0) * 0.67, ay + amp * 0.9,
                           x1, ay - amp * 0.5);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::Vec:
        case AccentKind::OverRightArrow: {
            double pad = (b.accent_kind == AccentKind::Vec) ? std::max(0.0, (w - fs * 0.52) * 0.5) : 0.0;
            double x0 = x + pad;
            double x1 = x + w - pad;
            double arrow_y = ay - fs * 0.06;
            double head = std::max(2.5, fs * 0.11);
            cairo_move_to(cr, x0, arrow_y);
            cairo_line_to(cr, x1, arrow_y);
            cairo_move_to(cr, x1 - head, arrow_y - head * 0.65);
            cairo_line_to(cr, x1, arrow_y);
            cairo_line_to(cr, x1 - head, arrow_y + head * 0.65);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::OverLeftArrow: {
            double x0 = x;
            double x1 = x + w;
            double arrow_y = ay - fs * 0.06;
            double head = std::max(2.5, fs * 0.11);
            cairo_move_to(cr, x0, arrow_y);
            cairo_line_to(cr, x1, arrow_y);
            cairo_move_to(cr, x0 + head, arrow_y - head * 0.65);
            cairo_line_to(cr, x0, arrow_y);
            cairo_line_to(cr, x0 + head, arrow_y + head * 0.65);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::Dot:
        case AccentKind::DDot:
        case AccentKind::DDDot: {
            int cnt = (b.accent_kind == AccentKind::Dot) ? 1 : (b.accent_kind == AccentKind::DDot ? 2 : 3);
            double r = std::max(1.2, fs * 0.055);
            double spacing = r * 3.2;
            double start_x = x + w * 0.5 - (cnt - 1) * spacing * 0.5;
            for (int i = 0; i < cnt; ++i) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, start_x + i * spacing, ay - r, r, 0.0, 2.0 * M_PI);
                cairo_fill(cr);
            }
            break;
        }
        case AccentKind::Overbrace: {
            double h = fs * 0.18;
            double xm = x + w * 0.5;
            cairo_move_to(cr, x, ay);
            cairo_curve_to(cr, x, ay - h * 0.7, xm - w * 0.1, ay - h * 0.4, xm, ay - h);
            cairo_curve_to(cr, xm + w * 0.1, ay - h * 0.4, x + w, ay - h * 0.7, x + w, ay);
            cairo_stroke(cr);
            break;
        }
        case AccentKind::Underbrace: {
            double h = fs * 0.18;
            double xm = x + w * 0.5;
            cairo_move_to(cr, x, ay);
            cairo_curve_to(cr, x, ay + h * 0.7, xm - w * 0.1, ay + h * 0.4, xm, ay + h);
            cairo_curve_to(cr, xm + w * 0.1, ay + h * 0.4, x + w, ay + h * 0.7, x + w, ay);
            cairo_stroke(cr);
            break;
        }
    }
    cairo_restore(cr);
}

static void draw_layout_box(cairo_t* cr, const LayoutBox& box, double x, double y) {
    switch (box.draw_kind) {
        case LayoutBox::DrawKind::Container: {
            for (const auto& ch : box.children) {
                if (ch.box) {
                    draw_layout_box(cr, *ch.box, x + ch.dx, y + ch.dy);
                }
            }
            break;
        }
        case LayoutBox::DrawKind::ColorWrap: {
            cairo_save(cr);
            cairo_set_source_rgba(cr, box.color.r, box.color.g, box.color.b, box.color.a);
            for (const auto& ch : box.children) {
                if (ch.box) {
                    draw_layout_box(cr, *ch.box, x + ch.dx, y + ch.dy);
                }
            }
            cairo_restore(cr);
            break;
        }
        case LayoutBox::DrawKind::Glyphs: {
            if (!box.text.empty()) {
                cairo_save(cr);
                cairo_select_font_face(cr, box.font_family.c_str(), box.slant, box.weight);
                cairo_set_font_size(cr, box.font_size);
                cairo_move_to(cr, x + box.glyph_dx, y);
                cairo_show_text(cr, box.text.c_str());
                cairo_restore(cr);
            }
            break;
        }
        case LayoutBox::DrawKind::HLine: {
            cairo_save(cr);
            cairo_set_line_width(cr, box.line_thickness);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
            cairo_move_to(cr, x, y);
            cairo_line_to(cr, x + box.width, y);
            cairo_stroke(cr);
            cairo_restore(cr);
            break;
        }
        case LayoutBox::DrawKind::Radical: {
            double top = y + box.rad_top_y;
            double bot = y + box.rad_bot_y;
            double H = bot - top;
            double rw = box.rad_w;
            cairo_save(cr);
            cairo_set_line_width(cr, box.line_thickness);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
            cairo_move_to(cr, x + rw * 0.04, bot - H * 0.35);
            cairo_line_to(cr, x + rw * 0.24, bot - H * 0.42);
            cairo_line_to(cr, x + rw * 0.48, bot);
            cairo_line_to(cr, x + rw, top);
            cairo_line_to(cr, x + rw + box.rad_bar_w, top);
            cairo_stroke(cr);
            cairo_restore(cr);
            break;
        }
        case LayoutBox::DrawKind::Delimiter: {
            draw_delimiter_path(cr, box, x, y);
            break;
        }
        case LayoutBox::DrawKind::AccentDraw: {
            draw_accent_path(cr, box, x, y);
            break;
        }
    }
}

// ─── Full Line Layout (Mixed Plain + Math Segments) ─────────────────────────

static BoxPtr layout_full_text(cairo_t* cr, const std::string& text, double font_size) {
    auto segs = split_latex_segments(text);
    auto line = make_box();
    double cx = 0.0;

    for (const auto& seg : segs) {
        if (seg.kind == TextSegment::Kind::Plain) {
            if (seg.content.empty()) continue;
            BoxPtr pb = layout_text_glyphs(cr, seg.content, MathStyle::PlainText, font_size);
            add_child(line, cx, 0.0, pb);
            cx += pb->width;
        } else {
            bool is_display = (seg.kind == TextSegment::Kind::DisplayMath);
            MathParser parser(seg.content);
            NodePtr ast = parser.parse();
            BoxPtr mb = layout_math_node(cr, ast, font_size, is_display);
            add_child(line, cx, 0.0, mb);
            cx += mb->width;
        }
    }
    line->width = cx;
    line->right_ext = std::max(line->right_ext, cx);
    return line;
}

MathMetrics measure_latex_text(cairo_t* cr, const std::string& text, double font_size) {
    BoxPtr box = layout_full_text(cr, text, font_size);
    MathMetrics m;
    double left = box->left_ext;
    double right = std::max(box->width, box->right_ext);
    m.x_bearing = left;
    m.y_bearing = -box->ascent;
    m.ascent = box->ascent;
    m.descent = box->descent;
    m.width = std::max(right - left, 10.0);
    m.height = std::max(box->ascent + box->descent, font_size * 0.75);
    if (box->ascent <= 0.0 && box->descent <= 0.0) {
        m.y_bearing = -font_size * 0.75;
    }
    return m;
}

void draw_latex_text(cairo_t* cr, const std::string& text, double x, double y, double font_size) {
    BoxPtr box = layout_full_text(cr, text, font_size);
    draw_layout_box(cr, *box, x, y);
}
