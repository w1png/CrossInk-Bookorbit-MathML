#include <Logging.h>
#include <Memory.h>

#include "Epub/Page.h"

bool TableCellLine::setInlineMath(const std::span<const InlineObjectPlacement> objects, const int textHeight,
                                  const int textBaseline) {
  const auto count = std::count_if(objects.begin(), objects.end(),
                                   [](const auto& object) { return object.kind == InlineObjectKind::Math; });
  if (count == 0) {
    math.reset();
    return true;
  }
  const InlineLineMetrics metrics = measureInlineLine(objects, textHeight, textBaseline, false);
  if (count > TableCellMath::MAX_OBJECTS || metrics.height <= 0 || metrics.height > UINT16_MAX ||
      metrics.baseline < 0 || metrics.baseline > metrics.height || metrics.textTop < 0 ||
      metrics.textTop > metrics.height) {
    LOG_ERR("PTB", "Invalid inline MathML cell geometry");
    return false;
  }

  // The sidecar survives layout retries and is allocated only for math lines.
  // The placement array has at most MAX_OBJECTS records and no growth copies.
  auto sidecar = makeUniqueNoThrow<TableCellMath>();
  if (!sidecar || !(sidecar->placements = makeUniqueNoThrow<TableCellMathPlacement[]>(count))) {
    LOG_ERR("PTB", "Could not allocate MathML cell placements");
    return false;
  }
  sidecar->metrics = metrics;
  for (const auto& object : objects) {
    if (object.kind == InlineObjectKind::Math) sidecar->placements[sidecar->count++].placement = object;
  }
  math = std::move(sidecar);
  return true;
}
