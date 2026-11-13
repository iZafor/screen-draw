#pragma once

#include <cairo.h>
#include <string>
#include <vector>

// ─── LaTeX Math Segment Splitting & Rendering ────────────────────────────────

struct TextSegment {
    enum class Kind {
        Plain,
        InlineMath,
        DisplayMath
    };
    Kind kind = Kind::Plain;
    std::string content;
};

struct MathMetrics {
    double width = 0.0;
    double height = 0.0;
    double x_bearing = 0.0;
    double y_bearing = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

// Splits `text` into plain-text and `$...$` / `$$...$$` math segments.
// If there are no valid math segments, returns a single Plain segment with the original text.
std::vector<TextSegment> split_latex_segments(const std::string& text);

// Returns true if `text` contains at least one complete `$...$` or `$$...$$` math segment.
bool contains_latex_math(const std::string& text);

// Measures a text string that may contain plain text and `$...$` / `$$...$$` LaTeX math.
MathMetrics measure_latex_text(cairo_t* cr, const std::string& text, double font_size);

// Renders a text string (plain text + `$...$` / `$$...$$` LaTeX math) with baseline at (x, y).
void draw_latex_text(cairo_t* cr, const std::string& text, double x, double y, double font_size);
