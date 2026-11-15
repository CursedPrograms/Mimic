// Mimic - batch: many pictures and videos with the same face and settings, one after another (the window and mimic-cli)
#pragma once
#include "mimic.h"
#include "video.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Batch {

bool isPicture(const std::string& path);
bool isTarget(const std::string& path);          // a picture or a video Mimic can do
// files and folders into the targets in them (folders: their pictures and videos, sorted; subfolders too when asked),
// each once, leaving out Mimic's own results (*_mimic.*)
std::vector<std::string> collect(const std::vector<std::string>& paths, bool subfolders);
// where a target's result goes: into outDir (or beside it when outDir is empty) as <name><suffix>.<ext> (videos: .mp4);
// kept under the same subfolder as it was under `root` (when the target came from a folder)
std::string outputFor(const std::string& target, const std::string& outDir, const std::string& suffix, const std::string& root = "");

enum State { WAITING, WORKING, DONE, SKIPPED, FAILED, STOPPED };
const char* stateName(int s);

struct Options {
    std::string outDir, suffix = "_mimic";
    bool skipDone = true;                        // leave alone what's already there
    int jpegQuality = 95;
    Video::Output video;
};
// one target: a picture swapped and saved, or a video rendered frame by frame (with its sound).
// progress(0..1, what it's doing); stop/skip end it early (a stopped video keeps what was done).
// faces: how many faces were done (pictures; videos: on the first frame that had any)
bool processOne(const std::string& in, const std::string& out, const Mimic::Source& src, const Mimic::Settings& st,
                const Mimic::Reference& ref, const Options& opt, const std::atomic<bool>& stop, const std::atomic<bool>& skip,
                const std::function<void(float, const std::string&)>& progress, int& faces, std::string& err);

}
