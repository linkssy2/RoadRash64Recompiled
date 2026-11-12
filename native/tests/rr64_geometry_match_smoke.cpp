#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "hle/rt64_rr64_geometry_match.h"

namespace {
    void require(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "RR64 geometry match failure: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }
}

int main() {
    using namespace RT64::RR64GeometryMatch;
    const std::array<float, 12> positions{
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    const std::array<uint32_t, 6> indices{10u, 11u, 12u, 10u, 12u, 13u};
    const GeometryView original{positions, indices, 10u};
    require(validGeometry(original) && compatibleGeometry(original, original),
        "a nonempty finite mesh matches itself");

    const auto relocatedPositions = positions;
    const std::array<uint32_t, 6> relocatedIndices{500u, 501u, 502u, 500u, 502u, 503u};
    const GeometryView relocated{relocatedPositions, relocatedIndices, 500u};
    require(compatibleGeometry(original, relocated) && compatibleGeometry(relocated, original),
        "buffer relocation and global index rebasing preserve local geometry");

    auto changedPositions = positions;
    changedPositions[6] = 4.0f;
    require(!compatibleGeometry(original, {changedPositions, indices, 10u}),
        "same-count streamed geometry with changed positions is rejected");
    require(compatibleTopology(original, {changedPositions, indices, 10u}),
        "explicitly identified animation may retain topology while positions change");
    changedPositions = positions;
    changedPositions[3] = 0.0f;
    changedPositions[9] = 1.0f;
    require(!compatibleGeometry(original, {changedPositions, indices, 10u}),
        "ordered vertex correspondence cannot silently change");

    const std::array<uint32_t, 6> changedTopology{10u, 11u, 13u, 11u, 12u, 13u};
    const std::array<uint32_t, 6> reorderedTriangles{10u, 12u, 13u, 10u, 11u, 12u};
    const std::array<uint32_t, 6> reversedWinding{10u, 12u, 11u, 10u, 13u, 12u};
    require(!compatibleGeometry(original, {positions, changedTopology, 10u}) &&
        !compatibleTopology(original, {positions, changedTopology, 10u}),
        "equal triangle counts cannot replace exact connectivity");
    require(!compatibleGeometry(original, {positions, reorderedTriangles, 10u}),
        "triangle order is part of exact draw correspondence");
    require(!compatibleGeometry(original, {positions, reversedWinding, 10u}),
        "different triangle winding is rejected");
    require(!compatibleGeometry(original, {positions, {indices.data(), 3u}, 10u}),
        "a changed draw count invalidates correspondence");

    const std::array<uint32_t, 6> belowRange{9u, 11u, 12u, 10u, 12u, 13u};
    const std::array<uint32_t, 6> aboveRange{10u, 11u, 14u, 10u, 12u, 13u};
    require(!validGeometry({positions, belowRange, 10u}) &&
        !validGeometry({positions, aboveRange, 10u}),
        "indices outside this transform's vertex range are rejected");
    require(!validGeometry({{positions.data(), 11u}, indices, 10u}) &&
        !validGeometry({positions, {indices.data(), 5u}, 10u}),
        "incomplete vertex or triangle triples are invalid");
    require(!validGeometry({{}, indices, 10u}) &&
        !validGeometry({positions, {}, 10u}) &&
        !compatibleGeometry({}, {}),
        "empty inputs never certify geometry");
    require(!validGeometry({{nullptr, 12u}, indices, 10u}) &&
        !validGeometry({positions, {nullptr, 6u}, 10u}),
        "null storage with a nonzero length is invalid");

    auto invalidPositions = positions;
    invalidPositions[4] = std::numeric_limits<float>::infinity();
    require(!validGeometry({invalidPositions, indices, 10u}), "infinite positions are invalid");
    invalidPositions[4] = -std::numeric_limits<float>::infinity();
    require(!validGeometry({invalidPositions, indices, 10u}), "negative infinity is invalid");
    invalidPositions[4] = std::numeric_limits<float>::quiet_NaN();
    require(!compatibleGeometry({invalidPositions, indices, 10u}, {invalidPositions, indices, 10u}) &&
        !compatibleTopology(original, {invalidPositions, indices, 10u}),
        "even identical NaN inputs cannot certify a mesh");
    auto signedZeroPositions = positions;
    signedZeroPositions[0] = -0.0f;
    require(compatibleGeometry(original, {signedZeroPositions, indices, 10u}),
        "signed zero represents the same finite local position");

    constexpr uint32_t maximumIndex = std::numeric_limits<uint32_t>::max();
    const std::array<uint32_t, 6> highIndices{
        maximumIndex - 3u, maximumIndex - 2u, maximumIndex - 1u,
        maximumIndex - 3u, maximumIndex - 1u, maximumIndex
    };
    require(compatibleGeometry(original, {positions, highIndices, maximumIndex - 3u}),
        "a valid relocated range may end at the maximum global index");
    require(!validGeometry({positions, highIndices, maximumIndex - 2u}),
        "a range that would overflow the global index domain is invalid");
    require(!validGeometry({positions, indices, maximumIndex}),
        "range arithmetic cannot wrap to accept low indices");

    std::cout << "RR64 geometry match smoke passed\n";
    return EXIT_SUCCESS;
}
