// ColorVision.h - daltonization matrices for the analytic post pass. Mode ordinals cross the managed/native
// ABI (DLSS_CV_* in RenderforgeNative.h, ColorVisionMode in src\DlssConfig.cs); append only.
//
// D = I + R * (I - S), applied to COLUMN-VECTOR linear RGB (v' = D * v), stored ROW-MAJOR.
//
// S = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, transcribed from the authors'
//     own table: https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html
// R = error redistribution: how the colour a deficient eye cannot separate is pushed onto channels it can.
//     protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" - `err2mod` verbatim in their
//         MATLAB (https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m), which applies it to
//         the protan error (`errorp`) only; report at
//         http://acorn.stanford.edu/psych221/projects/2005/ofidaner/colorblindness_project.htm.
//     deuteranopia: the SAME matrix, and that is a deliberate, sourced choice - the MATLAB does not correct
//         the deutan error at all, but the maintained Python port of the same method applies the single
//         err2mod to every deficiency type (daltonize/daltonize.py:125 in
//         https://github.com/joergdietrich/daltonize; the type argument only picks the simulate() matrix,
//         :120). The alternative was ixora's distinct deutan R (1 0.7 0 / 0 0 0 / 0 0.7 1); it was rejected
//         because it rests on the same unverified secondary table as the tritan row below.
//     tritanopia: the matrix table at
//         https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html, which
//         redistributes onto the channels a tritan can still discriminate. SECONDARY source, coefficients
//         NOT independently verified - the page cites Simon-Liedtke & Farup (JVCI 2016) as justification
//         for using a per-type matrix, it is not the published origin of these numbers. Weaker than the
//         citations above; used because err2mod would push the tritan error onto B, the very channel a
//         tritan cannot discriminate.
//
// Every row of every D sums to 1 within 1e-6 (residual = the rounding in the published S), so neutral
// grey and white are fixed points. probe/colour_vision_probe.cpp asserts that, and asserts D against
// probe/colour_vision_ref.py, which derives the same numbers from the same publications independently.
#pragma once

// Mirrored by DLSS_CV_* (RenderforgeNative.h) and ColorVisionMode (src\DlssConfig.cs).
enum { RF_CV_NONE = 0, RF_CV_DEUTERANOPIA = 1, RF_CV_PROTANOPIA = 2, RF_CV_TRITANOPIA = 3 };

struct CvMatrix { float m[9]; };   // row-major: m[row * 3 + col]

namespace CvDetail {

struct M3 { float m[9]; };

constexpr M3 kIdentity = { { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f } };

constexpr M3 kSimulate[3] = {
    { {  0.367322f,  0.860646f, -0.227968f,
         0.280085f,  0.672501f,  0.047413f,
        -0.011820f,  0.042940f,  0.968881f } },   // deuteranopia
    { {  0.152286f,  1.052583f, -0.204868f,
         0.114503f,  0.786281f,  0.099216f,
        -0.003882f, -0.048116f,  1.051998f } },   // protanopia
    { {  1.255528f, -0.076749f, -0.178779f,
        -0.078411f,  0.930809f,  0.147602f,
         0.004733f,  0.691367f,  0.303900f } },   // tritanopia
};

constexpr M3 kRedistribute[3] = {
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // deuteranopia (err2mod, daltonize.py:125)
    { { 0.0f, 0.0f, 0.0f,  0.7f, 1.0f, 0.0f,  0.7f, 0.0f, 1.0f } },   // protanopia   (err2mod, Fidaner conv_img.m)
    { { 1.0f, 0.0f, 0.7f,  0.0f, 1.0f, 0.7f,  0.0f, 0.0f, 0.0f } },   // tritanopia   (ixora.io, secondary)
};

constexpr float Dot(const M3& a, const M3& b, int r, int c)
{
    return a.m[r * 3 + 0] * b.m[0 * 3 + c] + a.m[r * 3 + 1] * b.m[1 * 3 + c] + a.m[r * 3 + 2] * b.m[2 * 3 + c];
}

}   // namespace CvDetail

// Correction matrix for `mode`. Anything outside 1..3 returns the identity, so a bad ordinal from the
// managed side is a visual no-op instead of garbage.
constexpr CvMatrix CvCorrection(int mode)
{
    CvMatrix d = { { 1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f } };
    if (mode < RF_CV_DEUTERANOPIA || mode > RF_CV_TRITANOPIA) return d;
    const CvDetail::M3& s = CvDetail::kSimulate[mode - 1];
    const CvDetail::M3& r = CvDetail::kRedistribute[mode - 1];
    CvDetail::M3 error = { {} };
    for (int i = 0; i < 9; ++i) error.m[i] = CvDetail::kIdentity.m[i] - s.m[i];
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            d.m[row * 3 + col] = CvDetail::kIdentity.m[row * 3 + col] + CvDetail::Dot(r, error, row, col);
    return d;
}
