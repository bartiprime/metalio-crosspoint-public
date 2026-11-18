// r163 A13: host stand-ins for the three ImageBlock symbols Page.cpp links against.
//
// ImageBlock.cpp cannot be compiled on the host -- it pulls FontCacheManager, HalDisplay and the
// board's driver/gpio.h -- which is why no existing host test includes it either. This test only
// builds text records, so the drawing paths are never reached; deserialize() is reproduced
// verbatim so an image element in a record still round-trips through the real Page serialiser.
#include <Epub/blocks/ImageBlock.h>
#include <MemReader.h>
#include <Page.h>
#include <Serialization.h>

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, const int16_t width,
                       const int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void ImageBlock::render(GfxRenderer&, const int, const int) {}

void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const {}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

template <typename R>
std::unique_ptr<ImageBlock> ImageBlock::deserialize(R& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
}

template std::unique_ptr<ImageBlock> ImageBlock::deserialize<HalFile>(HalFile&);
template std::unique_ptr<ImageBlock> ImageBlock::deserialize<MemReader>(MemReader&);
