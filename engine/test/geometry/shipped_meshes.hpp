#pragma once

// The five meshes this game ships, re-authored against sol::assets::MeshBuilder.
//
// These are the regression net for Phase 9 stage B: the primitive vocabulary
// was ported out of tools/scripts/gen_assets.ps1, and the only assertion that
// proves a port is that the same recipe comes out as the same bytes. Every
// call below is a line-for-line transcription of the script's own.
//
// They live in the test and not in the engine on purpose. A station's geometry
// is game content, and engine code that knew what a station looked like would
// be the same wall stage A just took down.

#include "sol/assets/mesh_build.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace shipped {

using sol::assets::BuildPoint;
using sol::assets::BuildUv;
using sol::assets::MeshBuilder;

[[nodiscard]] inline sol::assets::MeshData buildCube()
{
    // The script writes this one out by hand, and it is exactly an untiled
    // unit box: same face order, same corner order, same 0..1 uvs.
    MeshBuilder builder;
    builder.addBox({0, 0, 0}, {1, 1, 1});
    return builder.build();
}

[[nodiscard]] inline sol::assets::MeshData buildStation()
{
    MeshBuilder builder;
    builder.addTorus(90, 12, 40, 12, 8);
    builder.addBox({0, 0, 0}, {44, 60, 44});     // hub
    builder.addBox({47, 0, 0}, {86, 6, 6});      // spokes
    builder.addBox({-47, 0, 0}, {86, 6, 6});
    builder.addBox({0, 0, 47}, {6, 6, 86});
    builder.addBox({0, 0, -47}, {6, 6, 86});
    builder.addBox({0, 44, 0}, {3, 28, 3});      // panel masts
    builder.addBox({0, -44, 0}, {3, 28, 3});
    builder.addBox({0, 62, 0}, {76, 1.5, 26});   // solar panels
    builder.addBox({0, -62, 0}, {76, 1.5, 26});
    return builder.build();
}

[[nodiscard]] inline sol::assets::MeshData buildShip()
{
    MeshBuilder builder;
    const BuildPoint nose{0, 0.3, -7};
    const BuildPoint front[4] = {{-2.6, -1.3, -1}, {2.6, -1.3, -1}, {2.0, 1.5, -1}, {-2.0, 1.5, -1}};
    const BuildPoint rear[4] = {{-3.5, -1.7, 5}, {3.5, -1.7, 5}, {2.6, 2.0, 5}, {-2.6, 2.0, 5}};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        builder.addFlatTriangle(nose, front[j], front[i], {0.5, 0}, {1, 1}, {0, 1});
        builder.addFlatTriangle(front[i], front[j], rear[j], {0, 0}, {1, 0}, {1, 1});
        builder.addFlatTriangle(front[i], rear[j], rear[i], {0, 0}, {1, 1}, {0, 1});
    }
    builder.addFlatTriangle(rear[0], rear[1], rear[2], {0, 0}, {1, 0}, {1, 1});
    builder.addFlatTriangle(rear[0], rear[2], rear[3], {0, 0}, {1, 1}, {0, 1});
    builder.addFlatTriangle({0, 1.7, 1}, {0, 3.8, 4.6}, {0, 2.0, 5}, {0, 0}, {0.5, 1}, {1, 0});
    builder.addFlatTriangle({0, 1.7, 1}, {0, 2.0, 5}, {0, 3.8, 4.6}, {0, 0}, {1, 0}, {0.5, 1});
    return builder.build();
}

[[nodiscard]] inline BuildPoint normalizePoint(BuildPoint p)
{
    const double length = std::sqrt((p.x * p.x) + (p.y * p.y) + (p.z * p.z));
    if (length <= 0.0) {
        return {0, 0, 0};
    }
    return {p.x / length, p.y / length, p.z / length};
}

// ⚑ THIS IS NOT FNV-1a, AND IT MUST NOT BE. The generator script writes the
// FNV recurrence, but PowerShell has no 32-bit integer arithmetic: `[uint32] *
// 16777619` promotes to DOUBLE, so every step past the first is the product
// rounded to 53 bits before it is masked back to 32. Measured, not guessed -
// `('-0.5257|0.8507|0.0000')` hashes to 2051818176 in PowerShell and 27768919
// in real FNV-1a, and the asteroid this game ships is built on the first
// number. A "corrected" hash here would silently re-author a shipped asset.
//
// ⚑ This recipe is now the PROVENANCE of assets/meshes/asteroid.forge, which is
// baked rather than parametric precisely so that this arithmetic never has to
// exist in engine code. Both are pinned to the same hashes below, so they are
// one asset with a net rather than two implementations - but if you ever want a
// DIFFERENT rock, this is what you edit, and then you re-bake.
[[nodiscard]] inline std::uint32_t powershellHashStep(std::uint32_t hash, char character)
{
    const auto xored = static_cast<double>(hash ^ static_cast<unsigned char>(character));
    const double product = xored * 16777619.0; // <= 7.2e16: past 2^53, and that is the point
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(product) & 0xFFFFFFFFll);
}

// Displacement keyed on the ROUNDED unit direction, and memoised on that key,
// so vertices shared by neighbouring triangles always agree and the hull stays
// closed. The memo is load-bearing: the lump term is computed from the
// unrounded direction, so first call wins for a whole key.
class RockRadius
{
public:
    [[nodiscard]] double operator()(BuildPoint dir)
    {
        char key[64];
        std::snprintf(key, sizeof(key), "%.4f|%.4f|%.4f", dir.x, dir.y, dir.z);
        const auto found = m_cache.find(key);
        if (found != m_cache.end()) {
            return found->second;
        }
        std::uint32_t hash = 2166136261u;
        for (const char* c = key; *c != '\0'; ++c) {
            hash = powershellHashStep(hash, *c);
        }
        const double jitter = static_cast<double>(hash % 10000) / 10000.0;
        const double lump = (0.13 * std::sin((3.1 * dir.x) + 1.7)) +
                            (0.11 * std::sin((2.6 * dir.y) + 0.4)) +
                            (0.09 * std::sin((3.7 * dir.z) + 2.3));
        const double radius = 0.80 + (0.17 * jitter) + lump;
        m_cache.emplace(key, radius);
        return radius;
    }

private:
    std::unordered_map<std::string, double> m_cache;
};

[[nodiscard]] inline sol::assets::MeshData buildAsteroid()
{
    const double phi = (1 + std::sqrt(5.0)) / 2;
    const BuildPoint icoVerts[12] = {{-1, phi, 0}, {1, phi, 0},  {-1, -phi, 0}, {1, -phi, 0},
                                     {0, -1, phi}, {0, 1, phi},  {0, -1, -phi}, {0, 1, -phi},
                                     {phi, 0, -1}, {phi, 0, 1},  {-phi, 0, -1}, {-phi, 0, 1}};
    const int icoFaces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11},
                                 {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                 {3, 9, 4},  {3, 4, 2},  {3, 2, 6},  {3, 6, 8},  {3, 8, 9},
                                 {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},  {9, 8, 1}};

    struct Tri
    {
        BuildPoint a;
        BuildPoint b;
        BuildPoint c;
    };
    std::vector<Tri> tris;
    for (const auto& face : icoFaces) {
        tris.push_back({normalizePoint(icoVerts[face[0]]), normalizePoint(icoVerts[face[1]]),
                        normalizePoint(icoVerts[face[2]])});
    }
    for (int step = 0; step < 2; ++step) {
        std::vector<Tri> next;
        next.reserve(tris.size() * 4);
        for (const Tri& tri : tris) {
            const BuildPoint ab = normalizePoint(
                {(tri.a.x + tri.b.x) / 2, (tri.a.y + tri.b.y) / 2, (tri.a.z + tri.b.z) / 2});
            const BuildPoint bc = normalizePoint(
                {(tri.b.x + tri.c.x) / 2, (tri.b.y + tri.c.y) / 2, (tri.b.z + tri.c.z) / 2});
            const BuildPoint ca = normalizePoint(
                {(tri.c.x + tri.a.x) / 2, (tri.c.y + tri.a.y) / 2, (tri.c.z + tri.a.z) / 2});
            next.push_back({tri.a, ab, ca});
            next.push_back({ab, tri.b, bc});
            next.push_back({ca, bc, tri.c});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }

    RockRadius rockRadius;
    MeshBuilder builder;
    for (const Tri& tri : tris) {
        BuildPoint corners[3];
        BuildUv uvs[3];
        const BuildPoint dirs[3] = {tri.a, tri.b, tri.c};
        for (int i = 0; i < 3; ++i) {
            const double radius = rockRadius(dirs[i]);
            corners[i] = {dirs[i].x * radius, dirs[i].y * radius, dirs[i].z * radius};
            uvs[i] = {(dirs[i].x * 0.5) + 0.5, (dirs[i].z * 0.5) + 0.5};
        }
        builder.addFlatTriangle(corners[0], corners[1], corners[2], uvs[0], uvs[1], uvs[2]);
    }
    return builder.build();
}

[[nodiscard]] inline sol::assets::MeshData buildCockpit()
{
    // Authored in SHIP space around the eye at kCockpitOffset, so the seat and
    // the frame around it cannot drift apart.
    const double ex = 0.0;
    const double ey = 0.8;
    const double ez = -5.0;
    MeshBuilder builder;

    const BuildPoint l0{ex - 0.98, ey - 0.30, ez - 1.00};
    const BuildPoint r0{ex + 0.98, ey - 0.30, ez - 1.00};
    const BuildPoint l1{ex - 0.80, ey + 0.58, ez - 0.86};
    const BuildPoint r1{ex + 0.80, ey + 0.58, ez - 0.86};
    const BuildPoint l2{ex - 0.66, ey + 0.66, ez + 0.34};
    const BuildPoint r2{ex + 0.66, ey + 0.66, ez + 0.34};
    const BuildPoint l3{ex - 0.92, ey - 0.34, ez + 0.34};
    const BuildPoint r3{ex + 0.92, ey - 0.34, ez + 0.34};

    const double tile = 2.6; // texture repeats per meter
    builder.addBeam(l0, l1, 0.075, 0.075, tile);
    builder.addBeam(r0, r1, 0.075, 0.075, tile);
    builder.addBeam(l1, r1, 0.070, 0.070, tile);
    builder.addBeam(l1, l2, 0.070, 0.070, tile);
    builder.addBeam(r1, r2, 0.070, 0.070, tile);
    builder.addBeam(l2, r2, 0.070, 0.070, tile);
    builder.addBeam(l0, l3, 0.100, 0.090, tile);
    builder.addBeam(r0, r3, 0.100, 0.090, tile);
    builder.addBeam(l3, l2, 0.070, 0.070, tile);
    builder.addBeam(r3, r2, 0.070, 0.070, tile);

    builder.addBox({ex, ey - 0.46, ez - 0.70}, {2.00, 0.28, 0.62}, tile);
    builder.addBox({ex - 0.92, ey - 0.24, ez - 0.35}, {0.22, 0.18, 0.90}, tile);
    builder.addBox({ex + 0.92, ey - 0.24, ez - 0.35}, {0.22, 0.18, 0.90}, tile);
    builder.addBox({ex, ey - 0.72, ez - 0.35}, {1.20, 0.08, 1.20}, tile);
    builder.addBox({ex, ey - 0.10, ez + 0.28}, {0.62, 0.70, 0.10}, tile);
    builder.addBox({ex, ey + 0.34, ez + 0.30}, {0.34, 0.22, 0.10}, tile);
    builder.addBox({ex, ey + 0.10, ez + 0.40}, {1.80, 1.00, 0.10}, tile);

    const BuildPoint nA{ex - 0.34, ey - 0.68, ez - 1.00};
    const BuildPoint nB{ex + 0.34, ey - 0.68, ez - 1.00};
    const BuildPoint nC{ex + 0.34, ey - 0.46, ez - 1.00};
    const BuildPoint nD{ex - 0.34, ey - 0.46, ez - 1.00};
    const BuildPoint nT{ex, ey - 0.54, ez - 1.95};
    builder.addFlatTriangle(nD, nC, nT, {0, 0}, {1, 0}, {0.5, 1});
    builder.addFlatTriangle(nB, nA, nT, {0, 0}, {1, 0}, {0.5, 1});
    builder.addFlatTriangle(nA, nD, nT, {0, 0}, {1, 0}, {0.5, 1});
    builder.addFlatTriangle(nC, nB, nT, {0, 0}, {1, 0}, {0.5, 1});
    builder.addFlatTriangle(nA, nB, nC, {0, 0}, {1, 0}, {1, 1});
    builder.addFlatTriangle(nA, nC, nD, {0, 0}, {1, 1}, {0, 1});
    return builder.build();
}

} // namespace shipped
