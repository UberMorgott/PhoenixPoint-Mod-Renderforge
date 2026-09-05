"""Independent reference for the Renderforge colour-vision correction matrices.

D = I + R*(I - S), column-vector linear RGB (v' = D*v), printed row-major.

S = Machado, Oliveira & Fernandes 2009 simulation matrices at severity 1.0, transcribed from the
    authors' own table: https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html
R = error redistribution.
    protanopia: Fidaner, Lin & Ozguven 2005, "Analysis of Color Blindness" - `err2mod` verbatim in their
        MATLAB (https://github.com/joergdietrich/daltonize/blob/main/doc/conv_img.m), which applies it to
        the protan error only.
    deuteranopia: the SAME err2mod. Citation: daltonize/daltonize.py:125 in the maintained Python port
        (https://github.com/joergdietrich/daltonize), where the one err2mod is applied for every
        color_deficit value; the type only selects the simulate() matrix (:120).
    tritanopia: per-deficiency redistribution from the matrix table at
        https://ixora.io/projects/colorblindness/color-blindness-simulation-research.html - a SECONDARY
        source whose coefficients are not independently verified (its Simon-Liedtke & Farup 2016 citation
        justifies the per-type approach, it is not the source of these numbers).

Deliberately stdlib-only and written from the published numbers, not from the C++ header, so that
colour_vision_probe.cpp compares two independent derivations rather than one value against itself.
"""

MODES = ("Deuteranopia", "Protanopia", "Tritanopia")

SIMULATE = {
    "Deuteranopia": (( 0.367322,  0.860646, -0.227968),
                     ( 0.280085,  0.672501,  0.047413),
                     (-0.011820,  0.042940,  0.968881)),
    "Protanopia":   (( 0.152286,  1.052583, -0.204868),
                     ( 0.114503,  0.786281,  0.099216),
                     (-0.003882, -0.048116,  1.051998)),
    "Tritanopia":   (( 1.255528, -0.076749, -0.178779),
                     (-0.078411,  0.930809,  0.147602),
                     ( 0.004733,  0.691367,  0.303900)),
}

REDISTRIBUTE = {
    "Deuteranopia": ((0.0, 0.0, 0.0), (0.7, 1.0, 0.0), (0.7, 0.0, 1.0)),
    "Protanopia":   ((0.0, 0.0, 0.0), (0.7, 1.0, 0.0), (0.7, 0.0, 1.0)),
    "Tritanopia":   ((1.0, 0.0, 0.7), (0.0, 1.0, 0.7), (0.0, 0.0, 0.0)),
}

IDENTITY = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def correction(mode):
    s, r = SIMULATE[mode], REDISTRIBUTE[mode]
    error = [[IDENTITY[i][j] - s[i][j] for j in range(3)] for i in range(3)]
    return [[IDENTITY[i][j] + sum(r[i][k] * error[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


if __name__ == "__main__":
    for mode in MODES:
        d = correction(mode)
        print(mode)
        for row in d:
            print("    " + ", ".join("%.9ff" % v for v in row) + ",")
        print("    row sums: " + ", ".join("%.9f" % sum(row) for row in d))
