// Mimic - Inoculant: poison a saved picture against face swaps / face recognition, cure a picture
// on import. Runs Inoculant's own command line (CursedPrograms/Inoculant), hidden.
#pragma once
#include <string>

namespace Inoculant {

struct Poison {
    float strength = 4.0f;   // how hard the cloak pushes (1-16)
    int steps = 40;          // optimisation steps (5-200)
    float texture = 0.6f;    // hide the change in textured areas (0-1)
    float colour = 0.5f;     // how much it may shift colour (0-1)
    float robustness = 0.5f; // survive JPEG / resizing (0-1)
};

struct Cure {
    int jpeg = 75;           // re-encode quality (10-100)
    float blur = 0.0f;       // blur radius (0-5)
    int shrink = 100;        // shrink to % and back (10-100)
};

std::string cli();  // inoculant-cli's path, "" when Inoculant isn't installed

// in -> out with the cloak; `report` gets Inoculant's summary (steps, PSNR, face similarity)
bool protect(const std::string& in, const std::string& out, const Poison& p, std::string& report, std::string& err);

// inspect `in` (verdict), then wash it with the purifiers into `out`
bool cure(const std::string& in, const std::string& out, const Cure& c, std::string& verdict, std::string& err);

// a temporary file path with this extension (".png")
std::string tempPath(const char* ext);

}  // namespace Inoculant
