#include "MathFont.h"

#include <cstring>

namespace mathml {
namespace {

constexpr size_t HEADER_PREFIX = 10;
constexpr size_t SECTION_DESCRIPTOR_BYTES = 8;
constexpr size_t BLOCK_DESCRIPTOR_BYTES = 13;
constexpr size_t RANGE_BYTES = 6;
constexpr size_t METRICS_BYTES = 6;
constexpr size_t SPARSE_VALUE_BYTES = 4;
constexpr uint8_t XOR_ROWS = 1;
constexpr uint8_t RAW_BLOCK = 2;

uint16_t read16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

int16_t readI16(const uint8_t* bytes) { return static_cast<int16_t>(read16(bytes)); }

uint32_t read24(const uint8_t* bytes) {
  return read16(bytes) | (uint32_t(bytes[2]) << 16);
}

uint32_t read32(const uint8_t* bytes) {
  return read24(bytes) | (uint32_t(bytes[3]) << 24);
}

// Find the last sorted record whose key is no greater than the requested key.
template <size_t Width, typename KeyReader>
const uint8_t* findFloor(std::span<const uint8_t> records, uint32_t key, KeyReader readKey) {
  size_t first = 0;
  size_t end = records.size() / Width;
  while (first < end) {
    const size_t middle = first + (end - first) / 2;
    if (readKey(records.data() + middle * Width) <= key) {
      first = middle + 1;
    } else {
      end = middle;
    }
  }
  return first == 0 ? nullptr : records.data() + (first - 1) * Width;
}

size_t stride(const MathFont::Metrics& metrics) { return (metrics.width + 7u) / 8; }
size_t bitmapSize(const MathFont::Metrics& metrics) { return stride(metrics) * metrics.height; }

}  // namespace

MathFont::MathFont(std::span<const uint8_t> asset) {
  static_assert(static_cast<size_t>(Constant::Count) == 57);
  constexpr size_t headerBytes = HEADER_PREFIX + static_cast<size_t>(Section::Count) * SECTION_DESCRIPTOR_BYTES;
  if (asset.size() < headerBytes || std::memcmp(asset.data(), "CIMF", 4) != 0 || asset[4] != 1 || asset[5] == 0 ||
      read16(asset.data() + 6) == 0 || read16(asset.data() + 8) == 0) {
    return;
  }
  const size_t payloadSize = asset.size() - headerBytes;
  size_t nextOffset = 0;
  for (size_t i = HEADER_PREFIX; i < headerBytes; i += SECTION_DESCRIPTOR_BYTES) {
    const uint32_t offset = read32(asset.data() + i);
    const uint32_t length = read32(asset.data() + i + 4);
    if (offset != nextOffset || length > payloadSize - nextOffset) return;
    nextOffset += length;
  }
  if (nextOffset != payloadSize) return;
  asset_ = asset;
  const uint16_t count = read16(asset.data() + 8);
  if (section(Section::Cmap).size() % RANGE_BYTES != 0 ||
      section(Section::Constants).size() != static_cast<size_t>(Constant::Count) * 2 ||
      section(Section::Extended).size() != (count + 7u) / 8) {
    asset_ = {};
    return;
  }
  strikePpem_ = asset[5];
  unitsPerEm_ = read16(asset.data() + 6);
  glyphCount_ = count;
}

std::span<const uint8_t> MathFont::section(Section id) const {
  if (!valid()) return {};
  constexpr size_t headerBytes = HEADER_PREFIX + static_cast<size_t>(Section::Count) * SECTION_DESCRIPTOR_BYTES;
  const uint8_t* entry = asset_.data() + HEADER_PREFIX + static_cast<size_t>(id) * SECTION_DESCRIPTOR_BYTES;
  return asset_.subspan(headerBytes + read32(entry), read32(entry + 4));
}

MathFont::Glyph MathFont::glyphForCodepoint(uint32_t codepoint) const {
  const auto* range = findFloor<RANGE_BYTES>(section(Section::Cmap), codepoint, read24);
  if (!range) return MissingGlyph;
  const uint32_t delta = codepoint - read24(range);
  const uint32_t glyph = read16(range + 3) + delta;
  return delta < range[5] && glyph < glyphCount_ ? static_cast<Glyph>(glyph) : MissingGlyph;
}

MathFont::Block MathFont::findBlock(Section id, Glyph glyph) const {
  if (glyph >= glyphCount_) return {};
  const auto bytes = section(id);
  if (bytes.size() < 2) return {};
  const size_t directoryBytes = size_t(read16(bytes.data())) * BLOCK_DESCRIPTOR_BYTES;
  if (directoryBytes > bytes.size() - 2) return {};
  const auto* descriptor = findFloor<BLOCK_DESCRIPTOR_BYTES>(bytes.subspan(2, directoryBytes), glyph, read16);
  if (!descriptor) return {};
  const uint16_t count = read16(descriptor + 2);
  if (count == 0 || count > MaxBlockRecords || (descriptor[12] == XOR_ROWS && id != Section::Bitmaps)) return {};
  return {descriptor, bytes.subspan(2 + directoryBytes), read16(descriptor), count};
}

std::span<const uint8_t> MathFont::readBlock(const Block& block) {
  if (!block.descriptor) return {};
  const size_t rawSize = read16(block.descriptor + 4);
  if (rawSize == 0 || rawSize > MaxRawBlockBytes) return {};
  if (loadedBlock_ == block.descriptor) return {raw_.data(), rawSize};

  const size_t storedSize = read16(block.descriptor + 6);
  const size_t offset = read32(block.descriptor + 8);
  const uint8_t encoding = block.descriptor[12];
  if (storedSize == 0 || offset > block.payload.size() || storedSize > block.payload.size() - offset ||
      encoding > RAW_BLOCK) {
    return {};
  }
  const uint8_t* data = block.payload.data() + offset;
  loadedBlock_ = nullptr;
  if (encoding == RAW_BLOCK) {
    if (storedSize != rawSize) return {};
    std::memcpy(raw_.data(), data, rawSize);
  } else {
    inflate_.init();
    inflate_.setSource(data, storedSize);
    if (!inflate_.readExact(raw_.data(), rawSize)) return {};
  }
  loadedBlock_ = block.descriptor;
  return {raw_.data(), rawSize};
}

std::optional<MathFont::Metrics> MathFont::metrics(Glyph glyph) {
  const Block block = findBlock(Section::Metrics, glyph);
  if (!block.descriptor || glyph - block.first >= block.count) return std::nullopt;
  const auto bytes = readBlock(block);
  if (bytes.size() != block.count * METRICS_BYTES) return std::nullopt;
  const uint8_t* value = bytes.data() + (glyph - block.first) * METRICS_BYTES;
  return Metrics{read16(value), static_cast<int8_t>(value[2]), static_cast<int8_t>(value[3]), value[4], value[5]};
}

std::optional<MathFont::Bitmap> MathFont::bitmap(Glyph glyph) {
  const Block block = findBlock(Section::Bitmaps, glyph);
  if (!block.descriptor || glyph - block.first >= block.count) return std::nullopt;
  size_t offset = 0;
  for (Glyph prior = block.first; prior < glyph; ++prior) {
    const auto metric = metrics(prior);
    if (!metric) return std::nullopt;
    offset += bitmapSize(*metric);
  }
  const auto metric = metrics(glyph);
  if (!metric) return std::nullopt;
  const size_t size = bitmapSize(*metric);
  if (size == 0) return Bitmap{*metric, {}};
  if (size > MaxBitmapBytes) return std::nullopt;
  const auto bytes = readBlock(block);
  if (offset > bytes.size() || size > bytes.size() - offset) return std::nullopt;
  std::memcpy(bitmap_.data(), bytes.data() + offset, size);
  if (block.descriptor[12] == XOR_ROWS) {
    const size_t rowStride = stride(*metric);
    for (size_t i = rowStride; i < size; ++i) bitmap_[i] ^= bitmap_[i - rowStride];
  }
  return Bitmap{*metric, {bitmap_.data(), size}};
}

int16_t MathFont::constant(Constant index) const {
  const auto bytes = section(Section::Constants);
  const size_t offset = static_cast<size_t>(index) * 2;
  return offset + 2 <= bytes.size() ? readI16(bytes.data() + offset) : 0;
}

std::optional<int16_t> MathFont::sparseValue(Section id, Glyph glyph) {
  const Block block = findBlock(id, glyph);
  const auto bytes = readBlock(block);
  if (bytes.size() != block.count * SPARSE_VALUE_BYTES) return std::nullopt;
  const auto* value = findFloor<SPARSE_VALUE_BYTES>(bytes, glyph, read16);
  if (!value || read16(value) != glyph) return std::nullopt;
  return readI16(value + 2);
}

int16_t MathFont::italicCorrection(Glyph glyph) { return sparseValue(Section::Italics, glyph).value_or(0); }

std::optional<int16_t> MathFont::topAccent(Glyph glyph) { return sparseValue(Section::Accents, glyph); }

bool MathFont::isExtendedShape(Glyph glyph) const {
  return glyph < glyphCount_ && (section(Section::Extended)[glyph / 8] & (1u << (glyph % 8))) != 0;
}

int16_t MathFont::mathKern(Glyph glyph, Corner corner, int16_t height) {
  if (static_cast<unsigned>(corner) >= 4) return 0;
  auto bytes = readBlock(findBlock(Section::Kern, glyph));
  while (bytes.size() >= 3) {
    const Glyph id = read16(bytes.data());
    const uint8_t corners = bytes[2];
    if (id > glyph || corners > 0x0F) return 0;
    bytes = bytes.subspan(3);
    for (uint8_t index = 0; index < 4; ++index) {
      if ((corners & (1u << index)) == 0) continue;
      if (bytes.empty()) return 0;
      const size_t heightCount = bytes[0];
      const size_t recordBytes = 1 + (2 * heightCount + 1) * 2;
      if (recordBytes > bytes.size()) return 0;
      if (id == glyph && index == static_cast<uint8_t>(corner)) {
        size_t kernIndex = 0;
        while (kernIndex < heightCount && height >= readI16(bytes.data() + 1 + 2 * kernIndex)) ++kernIndex;
        return readI16(bytes.data() + 1 + 2 * (heightCount + kernIndex));
      }
      bytes = bytes.subspan(recordBytes);
    }
    if (id == glyph) return 0;
  }
  return 0;
}

bool MathFont::construction(Glyph glyph, Direction direction, Construction& result) {
  const auto id = direction == Direction::Vertical ? Section::Vertical : Section::Horizontal;
  auto bytes = readBlock(findBlock(id, glyph));
  while (bytes.size() >= 4) {
    const Glyph base = read16(bytes.data());
    const uint8_t variantCount = bytes[2];
    const uint8_t partCount = bytes[3];
    const size_t variantBytes = variantCount * 4u;
    const size_t assemblyBytes = partCount ? 2 + partCount * 9u : 0;
    if (base > glyph || variantCount > result.variants.size() || partCount > result.parts.size() ||
        bytes.size() < 4 + variantBytes + assemblyBytes) {
      return false;
    }
    bytes = bytes.subspan(4);
    if (base != glyph) {
      bytes = bytes.subspan(variantBytes + assemblyBytes);
      continue;
    }
    for (uint8_t i = 0; i < variantCount; ++i) {
      const auto* record = bytes.data() + i * 4;
      const Glyph variant = read16(record);
      if (variant >= glyphCount_) return false;
      result.variants[i] = {variant, read16(record + 2)};
    }
    result.italicCorrection = 0;
    if (partCount) {
      const auto* assembly = bytes.data() + variantBytes;
      result.italicCorrection = readI16(assembly);
      for (uint8_t i = 0; i < partCount; ++i) {
        const auto* record = assembly + 2 + i * 9;
        const Glyph part = read16(record);
        if (part >= glyphCount_) return false;
        result.parts[i] = {part, read16(record + 2), read16(record + 4), read16(record + 6), (record[8] & 1) != 0};
      }
    }
    result.variantCount = variantCount;
    result.partCount = partCount;
    return true;
  }
  return false;
}

}  // namespace mathml
