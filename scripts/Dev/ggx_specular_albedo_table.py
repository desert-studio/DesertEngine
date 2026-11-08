"""Prints Mesh/BRDF.glslh kGGXSpecularAlbedo: the GGX lobe's directional albedo E(N.V, roughness) for F0 = 1.

The integrand is Graphic/BRDFLut.cpp IntegrateBRDF's scale + bias at F0 = 1 (GGX importance sampling over a
Hammersley set, Smith-Schlick G with k = roughness^2 / 2), evaluated at the 16 x 16 texel centres (i + 0.5) / 16 of
u = N.V and v = roughness. Change the LUT's integrand and this script together, then paste the rows.
Usage: python scripts/Dev/ggx_specular_albedo_table.py
"""
import numpy as np

N = 16
SAMPLES = 16384


def hammersley(count):
    i = np.arange(count, dtype=np.uint64)
    b = i & 0xFFFFFFFF
    b = ((b << 16) | (b >> 16)) & 0xFFFFFFFF
    b = (((b & 0x55555555) << 1) | ((b & 0xAAAAAAAA) >> 1)) & 0xFFFFFFFF
    b = (((b & 0x33333333) << 2) | ((b & 0xCCCCCCCC) >> 2)) & 0xFFFFFFFF
    b = (((b & 0x0F0F0F0F) << 4) | ((b & 0xF0F0F0F0) >> 4)) & 0xFFFFFFFF
    b = (((b & 0x00FF00FF) << 8) | ((b & 0xFF00FF00) >> 8)) & 0xFFFFFFFF
    return i.astype(np.float64) / count, b.astype(np.float64) / 2.0**32


def main():
    u1, u2 = hammersley(SAMPLES)
    for j in range(N):
        roughness = (j + 0.5) / N
        a = roughness * roughness
        k = a / 2.0
        phi = 2.0 * np.pi * u1
        cos_t = np.sqrt((1.0 - u2) / (1.0 + (a * a - 1.0) * u2))
        sin_t = np.sqrt(np.maximum(0.0, 1.0 - cos_t * cos_t))
        H = np.stack([sin_t * np.cos(phi), sin_t * np.sin(phi), cos_t])
        row = []
        for x in range(N):
            nov = (x + 0.5) / N
            V = np.array([np.sqrt(max(0.0, 1.0 - nov * nov)), 0.0, nov])
            voh = V @ H
            L = 2.0 * voh * H - V[:, None]
            nol = L[2]
            valid = (nol > 0.0) & (voh > 0.0)
            G = (nol / (nol * (1.0 - k) + k)) * (nov / (nov * (1.0 - k) + k))
            weight = np.where(valid, G * voh / np.maximum(H[2] * nov, 1e-5), 0.0)
            row.append(weight.sum() / SAMPLES)
        values = ["%.4ff" % v for v in row]
        print("\t" + ", ".join(values[:8]) + ",")
        print("\t" + ", ".join(values[8:]) + ",")


if __name__ == "__main__":
    main()
