#pragma once

#include <map>
#include <string>

#include "HalStorage.h"

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  bool success = true;
  // Empty series = a standalone. The index must keep it empty; nothing may
  // derive a series from the title.
  std::string series;
  // Calibre's series_index in TENTHS (15 == 1.5); 0 = unnumbered.
  uint16_t seriesTenths = 0;
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  struct SeriesInfo {
    std::string name;
    uint16_t tenths = 0;
    bool numbered = false;
  };

  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    return true;
  }

  // The form the library walk uses: title, author and the series in one read.
  bool loadMetadata(std::string& title, std::string& author, SeriesInfo& series) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    series.name = metadata.series;
    series.tenths = metadata.series.empty() ? 0 : metadata.seriesTenths;
    series.numbered = series.tenths != 0;
    return true;
  }
};
