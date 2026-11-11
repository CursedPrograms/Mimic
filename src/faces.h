// Mimic - finding faces (SCRFD, insightface's det_10g) and knowing who they are (ArcFace w600k_r50)
#pragma once
#include "image.h"
#include "ort.h"
#include <string>
#include <vector>

struct Face {
    float box[4] = {0, 0, 0, 0};   // x1, y1, x2, y2
    float score = 0;
    float kps[10] = {0};            // right eye, left eye, nose, right and left mouth corners (x, y)
    std::vector<float> emb, embNorm;   // ArcFace's 512 numbers, raw and unit length (empty until embed())
    float area() const { return (box[2] - box[0]) * (box[3] - box[1]); }
};

namespace Faces {
// every face, biggest first; minScore is SCRFD's confidence (FaceFusion's default 0.5)
bool detect(const Img& im, std::vector<Face>& out, std::string& err, float minScore = 0.5f, Ort::Say say = nullptr);
bool embed(const Img& im, Face& f, std::string& err, Ort::Say say = nullptr);
// how alike two faces are: cosine distance of their identities (0 = the same person, ~1 = strangers)
float distance(const Face& a, const Face& b);
}
