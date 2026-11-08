#pragma once

#include <Logging.h>
#include <Memory.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "MathLayout.h"

namespace mathml {

// The chapter parser uses its own layout depth, which can omit skipped elements.
class NamespaceScope {
 public:
  void enter(const char** attributes) {
    ++depth_;
    if (!attributes || overflowDepth_ != 0) return;
    for (size_t i = 0; attributes[i]; i += 2) {
      const std::string_view name(attributes[i]);
      if (name != "xmlns" && !name.starts_with("xmlns:")) continue;
      const auto prefix = name == "xmlns" ? std::string_view() : name.substr(6);
      const bool isMath = std::string_view(attributes[i + 1]) == MATH_URI;
      if (isMath == isMathPrefix(prefix)) continue;

      // Allocate the namespace stack only for chapters containing MathML.
      if (!bindings_) bindings_ = makeUniqueNoThrow<Bindings>();
      if (!bindings_ || count_ == bindings_->size() || prefix.size() > PREFIX_BYTES) {
        LOG_ERR("MML", "MathML namespace bindings exceeded memory or size limit");
        overflowDepth_ = depth_;
        break;
      }
      auto& binding = (*bindings_)[count_++];
      binding.depth = depth_;
      binding.isMath = isMath;
      binding.length = static_cast<uint8_t>(prefix.size());
      prefix.copy(binding.prefix.data(), prefix.size());
    }
  }

  void leave() {
    if (depth_ == 0) return;
    while (count_ > 0 && (*bindings_)[count_ - 1].depth == depth_) --count_;
    if (overflowDepth_ == depth_) overflowDepth_ = 0;
    --depth_;
  }

  std::string_view localMathName(std::string_view qualified) const {
    if (overflowDepth_ != 0) return {};
    const auto colon = qualified.find(':');
    const auto prefix = colon == std::string_view::npos ? std::string_view() : qualified.substr(0, colon);
    if (!isMathPrefix(prefix)) return {};
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
  }

 private:
  static constexpr std::string_view MATH_URI = "http://www.w3.org/1998/Math/MathML";
  static constexpr size_t PREFIX_BYTES = 48;
  struct Binding {
    uint32_t depth;
    bool isMath;
    uint8_t length;
    std::array<char, PREFIX_BYTES> prefix;
  };
  using Bindings = std::array<Binding, 16>;
  std::unique_ptr<Bindings> bindings_;
  size_t count_ = 0;
  uint32_t depth_ = 0;
  uint32_t overflowDepth_ = 0;

  bool isMathPrefix(std::string_view prefix) const {
    for (size_t i = count_; i > 0; --i) {
      const auto& binding = (*bindings_)[i - 1];
      if (prefix == std::string_view(binding.prefix.data(), binding.length)) return binding.isMath;
    }
    return false;
  }
};

// A single formula's transient XML tree. All string views borrow from bytes_.
class Capture {
 public:
  void reset() {
    count_ = used_ = depth_ = 0;
    overflowed_ = false;
  }

  void start(std::string_view name, const char** attributes) {
    if (overflowed_) return;
    if (depth_ == stack_.size() || count_ == nodes_.size()) {
      overflowed_ = true;
      return;
    }
    Node& node = appendNode();
    node.name = copy(name.empty() ? std::string_view("#foreign") : name);
    const size_t attrStart = used_;
    if (attributes) {
      for (size_t i = 0; attributes[i] && !overflowed_; i += 2) {
        const std::string_view key(attributes[i]);
        if (key == "xmlns" || key.starts_with("xmlns:")) continue;
        copy(std::string_view(attributes[i], key.size() + 1));
        copy(std::string_view(attributes[i + 1], std::strlen(attributes[i + 1]) + 1));
      }
    }
    node.attributes = std::string_view(bytes_.data() + attrStart, used_ - attrStart);
    stack_[depth_++] = {static_cast<uint16_t>(count_ - 1), NO_NODE};
  }

  void characters(std::string_view text) {
    if (overflowed_ || text.empty()) return;
    auto& parent = stack_[depth_ - 1];
    Node* node = &nodes_[parent.index];
    const bool isToken = node->name == "mi" || node->name == "mo" || node->name == "mn" || node->name == "mtext" ||
                         node->name == "ms" || node->name == "annotation";
    if (!isToken && text.find_first_not_of(" \r\n\t") == std::string_view::npos) return;
    if (parent.lastChild != NO_NODE) {
      node = &nodes_[parent.lastChild];
      if (node->name != "#text") {
        if (count_ == nodes_.size()) {
          overflowed_ = true;
          return;
        }
        node = &appendNode();
        node->name = "#text";
      }
    }
    const auto copied = copy(text);
    if (overflowed_) return;
    if (node->text.empty()) {
      node->text = copied;
    } else {
      node->text = std::string_view(node->text.data(), node->text.size() + copied.size());
    }
  }

  void end() {
    if (!overflowed_) --depth_;
  }

  bool valid() const { return !overflowed_; }
  std::span<const Node> nodes() const { return {nodes_.data(), count_}; }
  const char* rootAttribute(std::string_view key) const {
    auto attributes = nodes_[0].attributes;
    while (!attributes.empty()) {
      const std::string_view name(attributes.data());
      attributes.remove_prefix(name.size() + 1);
      if (attributes.empty()) break;
      const std::string_view value(attributes.data());
      if (name == key) return value.data();
      attributes.remove_prefix(value.size() + 1);
    }
    return nullptr;
  }

 private:
  struct Frame {
    uint16_t index;
    uint16_t lastChild;
  };
  std::array<Node, 128> nodes_;
  std::array<char, 2048> bytes_;
  std::array<Frame, 24> stack_;
  size_t count_ = 0;
  size_t used_ = 0;
  size_t depth_ = 0;
  bool overflowed_ = false;

  std::string_view copy(std::string_view value) {
    if (value.size() > bytes_.size() - used_) {
      overflowed_ = true;
      return {};
    }
    value.copy(bytes_.data() + used_, value.size());
    const std::string_view result(bytes_.data() + used_, value.size());
    used_ += value.size();
    return result;
  }

  Node& appendNode() {
    const auto index = static_cast<uint16_t>(count_++);
    if (depth_ != 0) {
      auto& parent = stack_[depth_ - 1];
      if (parent.lastChild == NO_NODE) {
        nodes_[parent.index].firstChild = index;
      } else {
        nodes_[parent.lastChild].nextSibling = index;
      }
      parent.lastChild = index;
    }
    nodes_[index] = {};
    return nodes_[index];
  }
};

}  // namespace mathml
