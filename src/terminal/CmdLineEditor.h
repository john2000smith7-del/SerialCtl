#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace serialctl {
// Local CMD drafts never enter stdout/VT or the network until submitted.
class CmdLineEditor {
  public:
    enum class Key { Left, Right, Home, End, Backspace, Delete, Up, Down };
    static constexpr size_t MaximumLineLength = 8191;
    struct PastePlan {
        std::wstring submitted, tail;
        size_t cursor = 0;
    };
    // Prepare the complete insertion without mutating the draft or history.
    // The caller must validate the encoded batch before sending any bytes.
    bool PreparePaste(const std::wstring &paste, PastePlan &plan) const {
        std::wstring combined = text_;
        combined.insert(cursor_, paste);
        size_t length = 0;
        for (wchar_t c : combined) {
            if (c == L'\r' || c == L'\n') length = 0;
            else if ((c < 32 && c != L'\t') || c == 127 || ++length > MaximumLineLength)
                return false;
        }
        const auto end = combined.find_last_of(L"\r\n");
        PastePlan ready;
        if (end == std::wstring::npos) {
            ready.tail = std::move(combined);
            ready.cursor = cursor_ + paste.size();
        } else {
            ready.submitted = combined.substr(0, end + 1);
            ready.tail = combined.substr(end + 1);
            const size_t caret = cursor_ + paste.size();
            ready.cursor = caret > end ? std::min(ready.tail.size(), caret - end - 1) : 0;
        }
        plan = std::move(ready);
        return true;
    }
    void CommitPaste(const PastePlan &plan) {
        if (!plan.submitted.empty()) SubmittedBatch(plan.submitted);
        text_ = plan.tail;
        cursor_ = std::min(plan.cursor, text_.size());
    }
    const std::wstring &Text() const {
        return text_;
    }
    size_t Cursor() const {
        return cursor_;
    }
    bool Insert(const std::wstring &text) {
        if (text.size() > MaximumLineLength - text_.size())
            return false;
        text_.insert(cursor_, text);
        cursor_ += text.size();
        return true;
    }
    void Edit(Key key) {
        switch (key) {
        case Key::Left:
            if (cursor_) {
                --cursor_;
                if (cursor_ && Low(text_[cursor_]))
                    --cursor_;
            }
            break;
        case Key::Right:
            if (cursor_ < text_.size()) {
                if (High(text_[cursor_]) && cursor_ + 1 < text_.size())
                    ++cursor_;
                ++cursor_;
            }
            break;
        case Key::Home:
            cursor_ = 0;
            break;
        case Key::End:
            cursor_ = text_.size();
            break;
        case Key::Backspace:
            if (cursor_) {
                auto end = cursor_;
                Edit(Key::Left);
                text_.erase(cursor_, end - cursor_);
            }
            break;
        case Key::Delete:
            if (cursor_ < text_.size()) {
                auto begin = cursor_;
                Edit(Key::Right);
                text_.erase(begin, cursor_ - begin);
                cursor_ = begin;
            }
            break;
        case Key::Up:
            if (!history_.empty()) {
                if (historyIndex_ == history_.size())
                    draft_ = text_;
                if (historyIndex_)
                    --historyIndex_;
                text_ = history_[historyIndex_];
                cursor_ = text_.size();
            }
            break;
        case Key::Down:
            if (historyIndex_ < history_.size()) {
                ++historyIndex_;
                text_ = historyIndex_ == history_.size() ? draft_ : history_[historyIndex_];
                cursor_ = text_.size();
            }
            break;
        }
    }
    void Submitted() {
        Remember(text_);
        Clear();
    }
    void SubmittedBatch(const std::wstring &batch) {
        size_t start = 0;
        while (start < batch.size()) {
            auto end = batch.find_first_of(L"\r\n", start);
            if (end == std::wstring::npos)
                break; // An incomplete clipboard tail remains a draft.
            Remember(batch.substr(start, end - start));
            start = end + 1;
            if (batch[end] == L'\r' && start < batch.size() && batch[start] == L'\n')
                ++start;
        }
        Clear();
    }
    void Clear() {
        text_.clear();
        draft_.clear();
        cursor_ = 0;
        historyIndex_ = history_.size();
    }

  private:
    void Remember(const std::wstring &line) {
        if (!line.empty() && (history_.empty() || history_.back() != line)) {
            history_.push_back(line);
            if (history_.size() > 128)
                history_.erase(history_.begin());
        }
    }
    static bool High(wchar_t c) {
        return c >= 0xd800 && c <= 0xdbff;
    }
    static bool Low(wchar_t c) {
        return c >= 0xdc00 && c <= 0xdfff;
    }
    std::wstring text_, draft_;
    size_t cursor_ = 0, historyIndex_ = 0;
    std::vector<std::wstring> history_;
};
} // namespace serialctl
