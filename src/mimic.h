// Mimic - the swap: one person's face onto the faces in a picture or a video frame, then the face
// restorers. The pipeline is insightface's, as FaceFusion runs it: SCRFD finds the faces, ArcFace says who the source
// is, inswapper (or HyperSwap) swaps on an aligned crop (pixel boost: several passes on a bigger crop), the result goes
// back through a feathered box mask (and an occlusion mask: hands, hair and glasses in front stay), then GFPGAN,
// CodeFormer or GPEN restore the detail.
#pragma once
#include "faces.h"
#include <string>
#include <vector>

namespace Mimic {

enum Swapper { SW_INSWAPPER, SW_HYPERSWAP, SW_COUNT };
enum Enhancer { EN_NONE, EN_GFPGAN, EN_CODEFORMER, EN_GPEN, EN_COUNT };
enum Which { W_ALL, W_LARGEST, W_LIKE_REFERENCE, W_COUNT };   // which faces in the target get swapped

const char* swapperName(int s);
const char* enhancerName(int e);
const char* whichName(int w);

struct Settings {
    int swapper = SW_INSWAPPER;
    int boost = 1;                 // pixel boost: the crop is boost x the model's size, run in boost x boost passes
    float weight = 0.5f;           // 0 more of the source, 1 more of the target's own identity (0.5 = neither)
    int enhancer = EN_GFPGAN;
    float enhanceBlend = 0.8f;     // how much of the restored face (FaceFusion: 80%)
    float codeformerFidelity = 0.5f;
    float maskBlur = 0.3f;         // the box mask's feathering
    int padding[4] = {0, 0, 0, 0}; // top, right, bottom, left, in % of the crop
    bool occlusion = false;        // XSeg: hands, hair, glasses in front of the face stay
    int which = W_ALL;
    float referenceDistance = 0.6f;   // W_LIKE_REFERENCE: faces this close to the reference face
    float minScore = 0.5f;         // the detector's confidence
};

// the person to become: their identity, averaged over every source picture (the biggest face in each)
struct Source {
    std::vector<Face> faces;
    std::vector<float> emb;        // averaged raw identity
    std::vector<float> embNorm;
    bool ready() const { return emb.size() == 512; }
};
bool addSource(Source& src, const Img& picture, std::string& err, Ort::Say say = nullptr);
void finishSource(Source& src);   // the average, after adding

// a face in the target to look for (W_LIKE_REFERENCE)
struct Reference { Face face; bool set = false; };

// swap the faces in a picture or frame, in place; returns how many were swapped (-1 on error)
int swapFrame(Img& frame, const Source& src, const Settings& st, const Reference& ref, std::string& err, Ort::Say say = nullptr);

// load what these settings need now (so the first frame isn't slow, and missing models are fetched up front)
bool prepare(const Settings& st, std::string& err, Ort::Say say = nullptr);

}  // namespace Mimic
