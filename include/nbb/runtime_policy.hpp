#pragma once
#include <array>
#include <cstdint>

namespace nbb::runtime {
// Same explicit boundary and GOG distinction as WingFlight's runtime policy.
// Do not interpret a numeric version interval as support for unreleased builds.
struct Version {
    std::uint16_t major, minor, build;
    std::uint16_t store; // SKSE packed low nibble, NOT the PE file revision
    const char* name;
};
inline constexpr std::array<Version, 14> supported{{
    {1,5,97,0,"1.5.97"}, {1,6,317,0,"1.6.317"}, {1,6,318,0,"1.6.318"},
    {1,6,323,0,"1.6.323"}, {1,6,342,0,"1.6.342"}, {1,6,353,0,"1.6.353"},
    {1,6,629,0,"1.6.629"}, {1,6,640,0,"1.6.640"}, {1,6,659,1,"1.6.659"},
    {1,6,1130,0,"1.6.1130"}, {1,6,1170,0,"1.6.1170"}, {1,6,1179,1,"1.6.1179"},
    {1,7,99,0,"1.7.99"}, {1,7,104,0,"1.7.104"}
}};
constexpr const Version* selectSKSE(std::uint16_t major, std::uint16_t minor, std::uint16_t build, std::uint16_t store) {
    for (const auto& v : supported)
        if (v.major == major && v.minor == minor && v.build == build && v.store == store) return &v;
    return nullptr;
}
constexpr const Version* selectFile(std::uint16_t major, std::uint16_t minor, std::uint16_t build, std::uint16_t revision) {
    if (revision != 0) return nullptr;
    for (const auto& v : supported)
        if (v.major == major && v.minor == minor && v.build == build) return &v;
    return nullptr;
}
constexpr unsigned addressFormat(const Version& v) { return v.minor == 5 ? 1 : v.minor == 7 ? 5 : 2; }
constexpr std::uint32_t pack(const Version& v) {
    return (std::uint32_t(v.major) << 24) | (std::uint32_t(v.minor) << 16) | (std::uint32_t(v.build) << 4) | v.store;
}
// Flat Skyrim TESNPC/TESFile ABI used by the two chained capture callbacks.
// The pinned CommonLib headers keep these layouts and vtable slots identical
// for all supported SE/AE builds. Selection still checks each known runtime and
// its SKSE store marker; PE revision zero must not be confused with GOG store 1.
struct CaptureLayout {
    std::uint32_t npcBytes, fileBytes;
    std::uint32_t loadSlot, initSlot;
    std::uint64_t seVtableID, aeVtableID;
};
inline constexpr CaptureLayout flatCapture{0x268,0x4C8,0x06,0x13,241857,195816};
constexpr const CaptureLayout* captureLayout(const Version& v) {
    return selectSKSE(v.major,v.minor,v.build,v.store)?&flatCapture:nullptr;
}

}
