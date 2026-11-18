#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/TokenBoundary.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <MemReader.h>

const char* lookupHtmlEntity(const char*, size_t) { return nullptr; }

#include <BidiUtils.h>

bool isExplicitHyphen(uint32_t) { return false; }
bool isSoftHyphen(uint32_t) { return false; }

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

bool ImageDecoderFactory::isFormatSupported(const std::string& path) {
  return Epub::enableGifProbeForTest && path.ends_with(".gif");
}
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }

void ImageBlock::render(GfxRenderer&, int, int) {}
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const {}
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(HalFile&) { return false; }
// The real deserialiser is a template on the reader (ImageBlock.h:44-46), so the stub must be one
// too: a non-template definition here is a different function, and the link (or the compile, once
// the explicit instantiations below name it) fails. Page.cpp instantiates its own deserialisers for
// exactly these two readers, so both are supplied and nothing else may be.
template <typename R>
std::unique_ptr<ImageBlock> ImageBlock::deserialize(R&) {
  return nullptr;
}
template std::unique_ptr<ImageBlock> ImageBlock::deserialize<HalFile>(HalFile&);
template std::unique_ptr<ImageBlock> ImageBlock::deserialize<MemReader>(MemReader&);

