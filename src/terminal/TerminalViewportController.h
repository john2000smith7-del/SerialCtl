#pragma once
#include "TerminalModel.h"
#include <memory>
namespace serialctl {
// UI-thread-owned draft projection. Device revisions, cursor edits and view
// changes invalidate it; repainting an unchanged draft does no history copying.
class TerminalViewportController {
public:
    const TerminalModel& Preview(const TerminalModel& source, const std::wstring& text, size_t cursor) {
        if (!preview_ || source_ != &source || revision_ != source.Revision() || text_ != text || cursor_ != cursor) {
            preview_ = std::make_unique<TerminalModel>(source.PreviewInput(text, cursor));
            source_ = &source; revision_ = source.Revision(); text_ = text; cursor_ = cursor;
        }
        return *preview_;
    }
    void Clear() { preview_.reset(); source_ = nullptr; text_.clear(); }
private:
    const TerminalModel* source_ = nullptr;
    std::uint64_t revision_ = 0;
    size_t cursor_ = 0;
    std::wstring text_;
    std::unique_ptr<TerminalModel> preview_;
};
} // namespace serialctl
