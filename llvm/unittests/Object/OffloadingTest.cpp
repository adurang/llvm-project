#include "llvm/Object/OffloadBinary.h"

#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"
#include <random>

using namespace llvm;
using namespace llvm::object;

TEST(OffloadingTest, checkOffloadingBinary) {
  // Create random data to fill the image.
  std::mt19937 Rng(std::random_device{}());
  std::uniform_int_distribution<uint64_t> SizeDist(0, 256);
  std::uniform_int_distribution<uint16_t> KindDist(0);
  std::uniform_int_distribution<uint16_t> BinaryDist(
      std::numeric_limits<uint8_t>::min(), std::numeric_limits<uint8_t>::max());
  std::uniform_int_distribution<int16_t> StringDist('!', '~');
  std::vector<uint8_t> Image(SizeDist(Rng));
  std::generate(Image.begin(), Image.end(), [&]() { return BinaryDist(Rng); });
  std::vector<std::pair<std::string, std::string>> Strings(SizeDist(Rng));
  for (auto &KeyAndValue : Strings) {
    std::string Key(SizeDist(Rng), '\0');
    std::string Value(SizeDist(Rng), '\0');

    std::generate(Key.begin(), Key.end(), [&]() { return StringDist(Rng); });
    std::generate(Value.begin(), Value.end(),
                  [&]() { return StringDist(Rng); });

    KeyAndValue = std::make_pair(Key, Value);
  }

  // Create the image.
  MapVector<StringRef, StringRef> StringData;
  for (auto &KeyAndValue : Strings)
    StringData[KeyAndValue.first] = KeyAndValue.second;
  std::unique_ptr<MemoryBuffer> ImageData = MemoryBuffer::getMemBuffer(
      {reinterpret_cast<char *>(Image.data()), Image.size()}, "", false);

  OffloadBinary::OffloadingImage Data;
  Data.TheImageKind = static_cast<ImageKind>(KindDist(Rng));
  Data.TheOffloadKind = static_cast<OffloadKind>(KindDist(Rng));
  Data.Flags = KindDist(Rng);
  Data.StringData = StringData;
  Data.Image = std::move(ImageData);

  auto BinaryBuffer =
      MemoryBuffer::getMemBufferCopy(OffloadBinary::write(Data));
  auto BinaryOrErr = OffloadBinary::create(*BinaryBuffer);
  if (!BinaryOrErr)
    FAIL();

  // Make sure we get the same data out.
  auto &Binaries = *BinaryOrErr;
  ASSERT_EQ(Binaries.size(), 1u);
  auto &Binary = *Binaries[0];
  ASSERT_EQ(Data.TheImageKind, Binary.getImageKind());
  ASSERT_EQ(Data.TheOffloadKind, Binary.getOffloadKind());
  ASSERT_EQ(Data.Flags, Binary.getFlags());

  for (auto &KeyAndValue : Strings)
    ASSERT_TRUE(StringData[KeyAndValue.first] ==
                Binary.getString(KeyAndValue.first));

  EXPECT_TRUE(Data.Image->getBuffer() == Binary.getImage());

  // Ensure the size and alignment of the data is correct.
  EXPECT_TRUE(Binary.getSize() % OffloadBinary::getAlignment() == 0);
  EXPECT_TRUE(Binary.getSize() == BinaryBuffer->getBuffer().size());
}

static std::unique_ptr<MemoryBuffer>
createMultiEntryBinary(size_t NumEntries,
                       SmallVectorImpl<std::string> &StringStorage) {
  // Reserve space to prevent reallocation which would invalidate StringRefs.
  // Each entry needs: "id", id_value, "arch", arch_value, image_content = 5
  // strings.
  StringStorage.reserve(NumEntries * 5);

  SmallVector<OffloadBinary::OffloadingImage> Images;

  for (size_t i = 0; i < NumEntries; ++i) {
    OffloadBinary::OffloadingImage Data;
    Data.TheImageKind = static_cast<ImageKind>(i % IMG_LAST);
    Data.TheOffloadKind = static_cast<OffloadKind>(i % OFK_LAST);

    MapVector<StringRef, StringRef> StringData;

    StringStorage.push_back("id");
    StringStorage.push_back(std::to_string(i));
    StringData[StringStorage[StringStorage.size() - 2]] =
        StringStorage[StringStorage.size() - 1];

    StringStorage.push_back("arch");
    StringStorage.push_back("gpu" + std::to_string(i));
    StringData[StringStorage[StringStorage.size() - 2]] =
        StringStorage[StringStorage.size() - 1];

    Data.StringData = StringData;

    // Make the last entry metadata-only (no image)
    if (i == NumEntries - 1) {
      Data.Flags = OIF_Metadata;
      Data.Image = MemoryBuffer::getMemBuffer("", "", false);
    } else {
      Data.Flags = i * 100;
      StringStorage.push_back("ImageData" + std::to_string(i));
      Data.Image = MemoryBuffer::getMemBuffer(StringStorage.back(), "", false);
    }

    Images.push_back(std::move(Data));
  }

  return MemoryBuffer::getMemBufferCopy(OffloadBinary::write(Images));
}

// Test multi-entry binaries and extraction without index (get all entries).
TEST(OffloadingTest, checkMultiEntryBinaryExtraction) {
  const size_t NumEntries = 5;
  SmallVector<std::string> StringStorage;
  auto BinaryBuffer = createMultiEntryBinary(NumEntries, StringStorage);

  // Test extracting all entries (no index).
  auto BinariesOrErr = OffloadBinary::create(*BinaryBuffer);
  ASSERT_THAT_EXPECTED(BinariesOrErr, Succeeded());

  auto &Binaries = *BinariesOrErr;
  ASSERT_EQ(Binaries.size(), NumEntries)
      << "Expected all entries when no index provided";

  // Verify each entry.
  for (size_t i = 0; i < NumEntries; ++i) {
    auto &Binary = *Binaries[i];
    EXPECT_EQ(Binary.getImageKind(), static_cast<ImageKind>(i % IMG_LAST));
    EXPECT_EQ(Binary.getOffloadKind(), static_cast<OffloadKind>(i % OFK_LAST));
    EXPECT_EQ(Binary.getIndex(), i);

    std::string ExpectedId = std::to_string(i);
    std::string ExpectedArch = "gpu" + std::to_string(i);
    EXPECT_EQ(Binary.getString("id"), ExpectedId);
    EXPECT_EQ(Binary.getString("arch"), ExpectedArch);

    // Last entry is metadata-only.
    if (i == NumEntries - 1) {
      EXPECT_EQ(Binary.getFlags(), OIF_Metadata);
      EXPECT_TRUE(Binary.getImage().empty());
    } else {
      EXPECT_EQ(Binary.getFlags(), i * 100);
      std::string ExpectedImage = "ImageData" + std::to_string(i);
      EXPECT_EQ(Binary.getImage(), ExpectedImage);
    }
  }

  // Ensure the size and alignment of the data is correct.
  EXPECT_TRUE(Binaries[0]->getSize() % OffloadBinary::getAlignment() == 0);
  EXPECT_TRUE(Binaries[0]->getSize() == BinaryBuffer->getBuffer().size());
}

// Test index-based extraction from multi-entry binary.
TEST(OffloadingTest, checkIndexBasedExtraction) {
  const size_t NumEntries = 5;
  SmallVector<std::string> StringStorage;
  auto BinaryBuffer = createMultiEntryBinary(NumEntries, StringStorage);

  // Test extracting specific indices.
  for (uint64_t i = 0; i < NumEntries; ++i) {
    auto BinariesOrErr = OffloadBinary::create(*BinaryBuffer, i);
    ASSERT_THAT_EXPECTED(BinariesOrErr, Succeeded());

    auto &Binaries = *BinariesOrErr;
    ASSERT_EQ(Binaries.size(), 1u) << "Expected single entry when using index";

    auto &Binary = *Binaries[0];
    EXPECT_EQ(Binary.getImageKind(), static_cast<ImageKind>(i % IMG_LAST));
    EXPECT_EQ(Binary.getOffloadKind(), static_cast<OffloadKind>(i % OFK_LAST));
    EXPECT_EQ(Binary.getIndex(), i);

    std::string ExpectedId = std::to_string(i);
    std::string ExpectedArch = "gpu" + std::to_string(i);
    EXPECT_EQ(Binary.getString("id"), ExpectedId);
    EXPECT_EQ(Binary.getString("arch"), ExpectedArch);

    // Last entry is metadata-only.
    if (i == NumEntries - 1) {
      EXPECT_EQ(Binary.getFlags(), OIF_Metadata);
      EXPECT_TRUE(Binary.getImage().empty());
    } else {
      EXPECT_EQ(Binary.getFlags(), i * 100);
      std::string ExpectedImage = "ImageData" + std::to_string(i);
      EXPECT_EQ(Binary.getImage(), ExpectedImage);
    }
  }

  // Test out-of-bounds index.
  auto OutOfBoundsOrErr = OffloadBinary::create(*BinaryBuffer, NumEntries + 10);
  EXPECT_THAT_EXPECTED(OutOfBoundsOrErr, Failed());
}

TEST(OffloadingTest, checkEdgeCases) {
  // Test with empty string data.
  {
    OffloadBinary::OffloadingImage Data;
    Data.TheImageKind = IMG_Object;
    Data.TheOffloadKind = OFK_OpenMP;
    Data.Flags = 0;
    Data.StringData = MapVector<StringRef, StringRef>(); // Empty

    std::string ImageContent = "TestImage";
    Data.Image = MemoryBuffer::getMemBuffer(ImageContent, "", false);

    auto BinaryBuffer =
        MemoryBuffer::getMemBufferCopy(OffloadBinary::write(Data));
    auto BinariesOrErr = OffloadBinary::create(*BinaryBuffer);
    ASSERT_THAT_EXPECTED(BinariesOrErr, Succeeded());

    auto &Binaries = *BinariesOrErr;
    ASSERT_EQ(Binaries.size(), 1u);
    EXPECT_TRUE(Binaries[0]->strings().empty());
    EXPECT_EQ(Binaries[0]->getImage(), ImageContent);
  }

  // Test with empty image data.
  {
    std::string Key = "test";
    std::string Value = "value";

    OffloadBinary::OffloadingImage Data;
    Data.TheImageKind = IMG_Object;
    Data.TheOffloadKind = OFK_SYCL;
    Data.Flags = 0;

    MapVector<StringRef, StringRef> StringData;
    StringData[Key] = Value;
    Data.StringData = StringData;

    Data.Image = MemoryBuffer::getMemBuffer("", "", false); // Empty image

    auto BinaryBuffer =
        MemoryBuffer::getMemBufferCopy(OffloadBinary::write(Data));
    auto BinariesOrErr = OffloadBinary::create(*BinaryBuffer);
    ASSERT_THAT_EXPECTED(BinariesOrErr, Succeeded());

    auto &Binaries = *BinariesOrErr;
    ASSERT_EQ(Binaries.size(), 1u);
    EXPECT_TRUE(Binaries[0]->getImage().empty());
    EXPECT_EQ(Binaries[0]->getString("test"), "value");
  }

  // Test with large string values.
  {
    std::string Key = "large_key";
    std::string LargeValue(4096, 'X'); // Large value
    std::string ImageContent = "Image";

    OffloadBinary::OffloadingImage Data;
    Data.TheImageKind = IMG_Bitcode;
    Data.TheOffloadKind = OFK_OpenMP;
    Data.Flags = 0;

    MapVector<StringRef, StringRef> StringData;
    StringData[Key] = LargeValue;
    Data.StringData = StringData;

    Data.Image = MemoryBuffer::getMemBuffer(ImageContent, "", false);

    auto BinaryBuffer =
        MemoryBuffer::getMemBufferCopy(OffloadBinary::write(Data));
    auto BinariesOrErr = OffloadBinary::create(*BinaryBuffer);
    ASSERT_THAT_EXPECTED(BinariesOrErr, Succeeded());

    auto &Binaries = *BinariesOrErr;
    ASSERT_EQ(Binaries.size(), 1u);
    EXPECT_EQ(Binaries[0]->getString("large_key"), LargeValue);
    EXPECT_EQ(Binaries[0]->getString("large_key").size(), 4096u);
  }
}

TEST(OffloadingTest, checkNestedOffloadBinaryWithOptions) {
  // Test nested OffloadBinary structure for Intel SPIR-V format with
  // compile/link options.
  // This simulates the new containerization where:
  // - Inner OffloadBinary contains raw SPIR-V + metadata + options
  // - Outer OffloadBinary contains the inner OffloadBinary

  // Create mock SPIR-V data (magic bytes: 0x07230203)
  std::vector<uint8_t> SPIRVData = {0x07, 0x23, 0x02, 0x03, 0x01, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00};

  // Create inner OffloadBinary with SPIR-V
  OffloadBinary::OffloadingImage InnerImage;
  InnerImage.TheImageKind = IMG_SPIRV;
  InnerImage.TheOffloadKind = OFK_OpenMP;
  InnerImage.Flags = 0;

  // Add metadata to inner binary (migrated from ELF notes)
  MapVector<StringRef, StringRef> InnerStringData;
  std::string VersionKey = "version";
  std::string VersionValue = "1.0";
  std::string FormatKey = "format";
  std::string FormatValue = "spirv";
  std::string TripleKey = "triple";
  std::string TripleValue = "spirv64-intel";
  std::string ArchKey = "arch";
  std::string ArchValue = "unknown";
  std::string CompileOptsKey = "compile-opts";
  std::string CompileOptsValue = "-O2 -g";
  std::string LinkOptsKey = "link-opts";
  std::string LinkOptsValue = "-cl-fast-relaxed-math";

  InnerStringData[VersionKey] = VersionValue;
  InnerStringData[FormatKey] = FormatValue;
  InnerStringData[TripleKey] = TripleValue;
  InnerStringData[ArchKey] = ArchValue;
  InnerStringData[CompileOptsKey] = CompileOptsValue;
  InnerStringData[LinkOptsKey] = LinkOptsValue;
  InnerImage.StringData = InnerStringData;

  InnerImage.Image = MemoryBuffer::getMemBuffer(
      StringRef(reinterpret_cast<const char *>(SPIRVData.data()),
                SPIRVData.size()),
      "", false);

  // Serialize inner OffloadBinary
  SmallVector<OffloadBinary::OffloadingImage> InnerImages;
  InnerImages.push_back(std::move(InnerImage));
  SmallString<0> InnerBinaryData = OffloadBinary::write(InnerImages);

  // Create outer OffloadBinary containing the inner binary
  OffloadBinary::OffloadingImage OuterImage;
  OuterImage.TheImageKind = IMG_SPIRV;
  OuterImage.TheOffloadKind = OFK_OpenMP;
  OuterImage.Flags = 0;

  // Outer binary metadata for filtering/selection
  MapVector<StringRef, StringRef> OuterStringData;
  std::string OuterTripleKey = "triple";
  std::string OuterTripleValue = "spirv64-intel";
  std::string OuterArchKey = "arch";
  std::string OuterArchValue = "unknown";

  OuterStringData[OuterTripleKey] = OuterTripleValue;
  OuterStringData[OuterArchKey] = OuterArchValue;
  OuterImage.StringData = OuterStringData;

  OuterImage.Image = MemoryBuffer::getMemBufferCopy(InnerBinaryData);

  // Serialize outer OffloadBinary
  SmallVector<OffloadBinary::OffloadingImage> OuterImages;
  OuterImages.push_back(std::move(OuterImage));
  SmallString<0> OuterBinaryData = OffloadBinary::write(OuterImages);

  // Parse outer binary
  auto OuterBuffer = MemoryBuffer::getMemBufferCopy(OuterBinaryData);
  auto OuterParsed = OffloadBinary::create(*OuterBuffer);
  ASSERT_THAT_EXPECTED(OuterParsed, Succeeded());

  auto &OuterBinaries = *OuterParsed;
  ASSERT_EQ(OuterBinaries.size(), 1u);

  const auto &OuterBinary = *OuterBinaries[0];
  EXPECT_EQ(OuterBinary.getImageKind(), IMG_SPIRV);
  EXPECT_EQ(OuterBinary.getOffloadKind(), OFK_OpenMP);
  EXPECT_EQ(OuterBinary.getString("triple"), "spirv64-intel");
  EXPECT_EQ(OuterBinary.getString("arch"), "unknown");

  // Extract inner binary data
  StringRef InnerData = OuterBinary.getImage();

  // Verify inner data has OffloadBinary magic bytes (0x10FF10AD)
  ASSERT_GE(InnerData.size(), 4u);
  EXPECT_EQ(static_cast<uint8_t>(InnerData[0]), 0x10);
  EXPECT_EQ(static_cast<uint8_t>(InnerData[1]), 0xFF);
  EXPECT_EQ(static_cast<uint8_t>(InnerData[2]), 0x10);
  EXPECT_EQ(static_cast<uint8_t>(InnerData[3]), 0xAD);

  // Parse inner binary
  MemoryBufferRef InnerBufferRef(InnerData, "inner-offload-binary");
  auto InnerParsed = OffloadBinary::create(InnerBufferRef);
  ASSERT_THAT_EXPECTED(InnerParsed, Succeeded());

  auto &InnerBinaries = *InnerParsed;
  ASSERT_EQ(InnerBinaries.size(), 1u);

  // Verify inner binary metadata including compile/link options
  const auto &InnerBinary = *InnerBinaries[0];
  EXPECT_EQ(InnerBinary.getImageKind(), IMG_SPIRV);
  EXPECT_EQ(InnerBinary.getOffloadKind(), OFK_OpenMP);
  EXPECT_EQ(InnerBinary.getString("version"), "1.0");
  EXPECT_EQ(InnerBinary.getString("format"), "spirv");
  EXPECT_EQ(InnerBinary.getString("triple"), "spirv64-intel");
  EXPECT_EQ(InnerBinary.getString("arch"), "unknown");
  EXPECT_EQ(InnerBinary.getString("compile-opts"), "-O2 -g");
  EXPECT_EQ(InnerBinary.getString("link-opts"), "-cl-fast-relaxed-math");

  // Extract SPIR-V from inner binary
  StringRef ExtractedSPIRV = InnerBinary.getImage();

  // Verify SPIR-V magic bytes (0x07230203)
  ASSERT_GE(ExtractedSPIRV.size(), 4u);
  EXPECT_EQ(static_cast<uint8_t>(ExtractedSPIRV[0]), 0x07);
  EXPECT_EQ(static_cast<uint8_t>(ExtractedSPIRV[1]), 0x23);
  EXPECT_EQ(static_cast<uint8_t>(ExtractedSPIRV[2]), 0x02);
  EXPECT_EQ(static_cast<uint8_t>(ExtractedSPIRV[3]), 0x03);

  // Verify the extracted SPIR-V matches the original
  EXPECT_EQ(ExtractedSPIRV.size(), SPIRVData.size());
  EXPECT_TRUE(std::equal(ExtractedSPIRV.begin(), ExtractedSPIRV.end(),
                         SPIRVData.begin()));
}
