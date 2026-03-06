//===-- llvm-offload-binary.cpp - offload binary management utility -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tool takes several device object files and bundles them into a single
// binary image using a custom binary format. This is intended to be used to
// embed many device files into an application to create a fat binary. It also
// supports extracting these files from a known location.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/WithColor.h"

using namespace llvm;
using namespace llvm::object;

static cl::opt<bool> Help("h", cl::desc("Alias for -help"), cl::Hidden);

static cl::OptionCategory OffloadBinaryCategory("llvm-offload-binary options");

static cl::opt<std::string> OutputFile("o", cl::desc("Write output to <file>."),
                                       cl::value_desc("file"),
                                       cl::cat(OffloadBinaryCategory));

static cl::opt<std::string> InputFile(cl::Positional,
                                      cl::desc("Extract from <file>."),
                                      cl::value_desc("file"),
                                      cl::cat(OffloadBinaryCategory));

static cl::list<std::string>
    DeviceImages("image",
                 cl::desc("List of key and value arguments. Required keywords "
                          "are 'file' and 'triple'."),
                 cl::value_desc("<key>=<value>,..."),
                 cl::cat(OffloadBinaryCategory));

static cl::opt<bool>
    CreateArchive("archive",
                  cl::desc("Write extracted files to a static archive"),
                  cl::cat(OffloadBinaryCategory));

/// Path of the current binary.
static const char *PackagerExecutable;

// Get a map containing all the arguments for the image. Repeated arguments will
// be placed in a comma separated list.
static DenseMap<StringRef, StringRef> getImageArguments(StringRef Image,
                                                        StringSaver &Saver) {
  DenseMap<StringRef, StringRef> Args;
  for (StringRef Arg : llvm::split(Image, ",")) {
    auto [Key, Value] = Arg.split("=");
    auto [It, Inserted] = Args.try_emplace(Key, Value);
    if (!Inserted)
      It->second = Saver.save(It->second + "," + Value);
  }

  return Args;
}

static Error writeFile(StringRef Filename, StringRef Data) {
  Expected<std::unique_ptr<FileOutputBuffer>> OutputOrErr =
      FileOutputBuffer::create(Filename, Data.size());
  if (!OutputOrErr)
    return OutputOrErr.takeError();
  std::unique_ptr<FileOutputBuffer> Output = std::move(*OutputOrErr);
  llvm::copy(Data, Output->getBufferStart());
  if (Error E = Output->commit())
    return E;
  return Error::success();
}

// Check if this is an Intel SPIR-V target requiring nested extraction
static bool needsNestedExtraction(const OffloadBinary *Binary) {
  StringRef Triple = Binary->getTriple();
  return Triple.contains("spirv64-intel") || Triple.contains("spirv32-intel");
}

// Get the appropriate file extension for nested OffloadBinary
static StringRef getNestedImageExtension(StringRef ImageData) {
  // Check if image is an inner OffloadBinary
  if (identify_magic(ImageData) == file_magic::offload_binary) {
    MemoryBufferRef InnerBuffer(ImageData, "inner-offload-binary");
    auto InnerBinaries = OffloadBinary::create(InnerBuffer);
    if (!InnerBinaries || InnerBinaries->empty())
      return "bin"; // Fallback

    const OffloadBinary *InnerBinary = (*InnerBinaries)[0].get();
    ImageKind Kind = InnerBinary->getImageKind();

    // Return extension based on inner image kind
    return getImageKindName(Kind);
  }

  // Legacy format - assume SPIR-V
  return "spv";
}

// Extract image from inner OffloadBinary or legacy formats
static Error extractFromInnerOffloadBinary(StringRef ImageData,
                                           StringRef Filename) {
  // Check if image is an inner OffloadBinary (nested format)
  if (identify_magic(ImageData) == file_magic::offload_binary) {
    // Parse inner OffloadBinary
    MemoryBufferRef InnerBuffer(ImageData, "inner-offload-binary");
    auto InnerBinaries = OffloadBinary::create(InnerBuffer);
    if (!InnerBinaries)
      return InnerBinaries.takeError();

    if (InnerBinaries->size() != 1)
      return createStringError(inconvertibleErrorCode(),
                              "Expected single entry in inner OffloadBinary");

    const OffloadBinary *InnerBinary = (*InnerBinaries)[0].get();
    ImageKind Kind = InnerBinary->getImageKind();

    // Extract image data
    StringRef ExtractedData = InnerBinary->getImage();

    // Write image to file
    if (Error Err = writeFile(Filename, ExtractedData))
      return Err;

    // Display metadata from inner OffloadBinary
    const char *KindName = nullptr;
    if (Kind == object::IMG_SPIRV)
      KindName = "SPIR-V";
    else if (Kind == object::IMG_Object)
      KindName = "Native binary";
    else
      KindName = "Image";

    outs() << "Extracted " << KindName << ": " << Filename << "\n";
    outs() << "  Inner metadata:\n";
    for (auto [Key, Value] : InnerBinary->strings()) {
      outs() << "    " << Key << " = " << Value << "\n";
    }

    return Error::success();
  }

  // Legacy format: Raw SPIR-V or other formats
  // For now, just write the data as-is (could be raw SPIR-V or ELF-wrapped)
  if (Error Err = writeFile(Filename, ImageData))
    return Err;

  outs() << "Extracted: " << Filename << " (legacy format)\n";
  return Error::success();
}

static Error bundleImages() {
  SmallVector<char, 1024> BinaryData;
  raw_svector_ostream OS(BinaryData);
  for (StringRef Image : DeviceImages) {
    BumpPtrAllocator Alloc;
    StringSaver Saver(Alloc);
    DenseMap<StringRef, StringRef> Args = getImageArguments(Image, Saver);

    if (!Args.count("file"))
      return createStringError(inconvertibleErrorCode(),
                               "'file' is a required image arguments");

    // Permit using multiple instances of `file` in a single string.
    for (auto &File : llvm::split(Args["file"], ",")) {
      OffloadBinary::OffloadingImage ImageBinary{};

      llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> ObjectOrErr =
          llvm::MemoryBuffer::getFileOrSTDIN(File);
      if (std::error_code EC = ObjectOrErr.getError())
        return errorCodeToError(EC);

      // Clang uses the '.o' suffix for LTO bitcode.
      if (identify_magic((*ObjectOrErr)->getBuffer()) == file_magic::bitcode)
        ImageBinary.TheImageKind = object::IMG_Bitcode;
      else if (sys::path::has_extension(File))
        ImageBinary.TheImageKind =
            getImageKind(sys::path::extension(File).drop_front());
      else
        ImageBinary.TheImageKind = IMG_None;
      ImageBinary.Image = std::move(*ObjectOrErr);
      for (const auto &[Key, Value] : Args) {
        if (Key == "kind") {
          ImageBinary.TheOffloadKind = getOffloadKind(Value);
        } else if (Key != "file") {
          ImageBinary.StringData[Key] = Value;
        }
      }
      llvm::SmallString<0> Buffer = OffloadBinary::write(ImageBinary);
      if (Buffer.size() % OffloadBinary::getAlignment() != 0)
        return createStringError(inconvertibleErrorCode(),
                                 "Offload binary has invalid size alignment");
      OS << Buffer;
    }
  }

  if (Error E = writeFile(OutputFile,
                          StringRef(BinaryData.begin(), BinaryData.size())))
    return E;
  return Error::success();
}

static Error unbundleImages() {
  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
      MemoryBuffer::getFileOrSTDIN(InputFile);
  if (std::error_code EC = BufferOrErr.getError())
    return createFileError(InputFile, EC);
  std::unique_ptr<MemoryBuffer> Buffer = std::move(*BufferOrErr);

  // This data can be misaligned if extracted from an archive.
  if (!isAddrAligned(Align(OffloadBinary::getAlignment()),
                     Buffer->getBufferStart()))
    Buffer = MemoryBuffer::getMemBufferCopy(Buffer->getBuffer(),
                                            Buffer->getBufferIdentifier());

  SmallVector<OffloadFile> Binaries;
  if (Error Err = extractOffloadBinaries(*Buffer, Binaries))
    return Err;

  // If no --image filter specified, extract all images including nested ones
  if (DeviceImages.empty()) {
    BumpPtrAllocator Alloc;
    StringSaver Saver(Alloc);
    uint64_t Idx = 0;

    for (const OffloadFile &File : Binaries) {
      const auto *Binary = File.getBinary();

      // Check if Intel SPIR-V target with nested OffloadBinary
      if (needsNestedExtraction(Binary)) {
        StringRef ImageData = Binary->getImage();

        // Check if image contains nested OffloadBinary
        if (identify_magic(ImageData) == file_magic::offload_binary) {
          // Parse inner OffloadBinary
          MemoryBufferRef InnerBuffer(ImageData, "inner-offload-binary");
          auto InnerBinaries = OffloadBinary::create(InnerBuffer);
          if (!InnerBinaries)
            return InnerBinaries.takeError();

          // Extract each inner image
          for (const auto &InnerBinary : *InnerBinaries) {
            StringRef Extension = getImageKindName(InnerBinary->getImageKind());
            StringRef Filename =
                Saver.save(sys::path::stem(InputFile) + "-" +
                          Binary->getTriple() + "-" + Binary->getArch() +
                          "." + std::to_string(Idx++) + "." + Extension);

            if (Error E = writeFile(Filename, InnerBinary->getImage()))
              return E;

            outs() << "Extracted: " << Filename << "\n";
            outs() << "  Inner metadata:\n";
            for (auto [Key, Value] : InnerBinary->strings()) {
              outs() << "    " << Key << " = " << Value << "\n";
            }
          }
        } else {
          // Legacy format or raw image
          StringRef Extension = getNestedImageExtension(ImageData);
          StringRef Filename =
              Saver.save(sys::path::stem(InputFile) + "-" + Binary->getTriple() +
                        "-" + Binary->getArch() + "." + std::to_string(Idx++) +
                        "." + Extension);
          if (Error E = writeFile(Filename, ImageData))
            return E;
          outs() << "Extracted: " << Filename << " (legacy format)\n";
        }
      } else {
        // Non-nested binary
        StringRef Filename =
            Saver.save(sys::path::stem(InputFile) + "-" + Binary->getTriple() +
                      "-" + Binary->getArch() + "." + std::to_string(Idx++) +
                      "." + getImageKindName(Binary->getImageKind()));
        if (Error E = writeFile(Filename, Binary->getImage()))
          return E;
        outs() << "Extracted: " << Filename << "\n";
      }
    }

    return Error::success();
  }

  // Try to extract each device image specified by the user from the input file.
  for (StringRef Image : DeviceImages) {
    BumpPtrAllocator Alloc;
    StringSaver Saver(Alloc);
    auto Args = getImageArguments(Image, Saver);

    SmallVector<const OffloadBinary *> Extracted;
    for (const OffloadFile &File : Binaries) {
      const auto *Binary = File.getBinary();
      // We handle the 'file' and 'kind' identifiers differently.
      bool Match = llvm::all_of(Args, [&](auto &Arg) {
        const auto [Key, Value] = Arg;
        if (Key == "file")
          return true;
        if (Key == "kind")
          return Binary->getOffloadKind() == getOffloadKind(Value);
        return Binary->getString(Key) == Value;
      });
      if (Match)
        Extracted.push_back(Binary);
    }

    if (Extracted.empty())
      continue;

    if (CreateArchive) {
      if (!Args.count("file"))
        return createStringError(inconvertibleErrorCode(),
                                 "Image must have a 'file' argument.");

      SmallVector<NewArchiveMember> Members;
      for (const OffloadBinary *Binary : Extracted)
        Members.emplace_back(MemoryBufferRef(
            Binary->getImage(),
            Binary->getMemoryBufferRef().getBufferIdentifier()));

      if (Error E = writeArchive(
              Args["file"], Members, SymtabWritingMode::NormalSymtab,
              Archive::getDefaultKind(), true, false, nullptr))
        return E;
    } else if (auto It = Args.find("file"); It != Args.end()) {
      if (Extracted.size() > 1)
        WithColor::warning(errs(), PackagerExecutable)
            << "Multiple inputs match to a single file, '" << It->second
            << "'\n";
      const OffloadBinary *Binary = Extracted.back();
      // Check if Intel SPIR-V target requiring nested extraction
      if (needsNestedExtraction(Binary)) {
        if (Error E = extractFromInnerOffloadBinary(Binary->getImage(),
                                                         It->second))
          return E;
      } else {
        if (Error E = writeFile(It->second, Binary->getImage()))
          return E;
      }
    } else {
      uint64_t Idx = 0;
      for (const OffloadBinary *Binary : Extracted) {
        // Check if Intel SPIR-V target requiring nested extraction
        if (needsNestedExtraction(Binary)) {
          // Determine extension from inner OffloadBinary
          StringRef Extension = getNestedImageExtension(Binary->getImage());
          StringRef Filename =
              Saver.save(sys::path::stem(InputFile) + "-" + Binary->getTriple() +
                         "-" + Binary->getArch() + "." + std::to_string(Idx++) +
                         "." + Extension);
          if (Error E = extractFromInnerOffloadBinary(Binary->getImage(),
                                                           Filename))
            return E;
        } else {
          StringRef Filename =
              Saver.save(sys::path::stem(InputFile) + "-" + Binary->getTriple() +
                         "-" + Binary->getArch() + "." + std::to_string(Idx++) +
                         "." + getImageKindName(Binary->getImageKind()));
          if (Error E = writeFile(Filename, Binary->getImage()))
            return E;
        }
      }
    }
  }

  return Error::success();
}

int main(int argc, const char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  cl::HideUnrelatedOptions(OffloadBinaryCategory);
  cl::ParseCommandLineOptions(
      argc, argv,
      "A utility for bundling several object files into a single binary.\n"
      "The output binary can then be embedded into the host section table\n"
      "to create a fatbinary containing offloading code.\n");

  if (sys::path::stem(argv[0]).ends_with("clang-offload-packager"))
    WithColor::warning(errs(), PackagerExecutable)
        << "'clang-offload-packager' is deprecated. Use 'llvm-offload-binary' "
           "instead.\n";

  if (Help || (OutputFile.empty() && InputFile.empty())) {
    cl::PrintHelpMessage();
    return EXIT_SUCCESS;
  }

  PackagerExecutable = argv[0];
  auto reportError = [argv](Error E) {
    logAllUnhandledErrors(std::move(E), WithColor::error(errs(), argv[0]));
    return EXIT_FAILURE;
  };

  if (!InputFile.empty() && !OutputFile.empty())
    return reportError(
        createStringError(inconvertibleErrorCode(),
                          "Packaging to an output file and extracting from an "
                          "input file are mutually exclusive."));

  if (!OutputFile.empty()) {
    if (Error Err = bundleImages())
      return reportError(std::move(Err));
  } else if (!InputFile.empty()) {
    if (Error Err = unbundleImages())
      return reportError(std::move(Err));
  }

  return EXIT_SUCCESS;
}
