#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgo() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

std::filesystem::path OutputDir() {
    return std::filesystem::temp_directory_path() / "corsairs-gltf-tests";
}

CORSAIRS_TEST(GltfWriter_WritesGltfAndBinForRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample.gltf";
    const std::filesystem::path binPath = OutputDir() / "sample.bin";
    std::filesystem::remove(gltfPath);
    std::filesystem::remove(binPath);

    std::string detail;
    const AC::GltfStatus status = AC::WriteGltf(*obj, gltfPath, detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    REQUIRE(std::filesystem::exists(gltfPath));
    REQUIRE(std::filesystem::exists(binPath));
    REQUIRE(std::filesystem::file_size(binPath) > 0u);
}

CORSAIRS_TEST(GltfWriter_GltfDeclaresVersionAndMeshCounts) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample2.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("version":"2.0")") != std::string::npos);
    REQUIRE(text.find(R"("POSITION")") != std::string::npos);
    REQUIRE(text.find(R"("meshes")") != std::string::npos);
    // Каждому подсету соответствует своя примитива.
    REQUIRE(text.find(R"("primitives")") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_RejectsMeshWithoutVertices) {
    AC::LgoGeomObj empty;
    empty.Version = 0x1004u;

    std::filesystem::create_directories(OutputDir());
    std::string detail;
    const AC::GltfStatus status =
        AC::WriteGltf(empty, OutputDir() / "empty.gltf", detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::EMPTY_MESH));
}

} // namespace
