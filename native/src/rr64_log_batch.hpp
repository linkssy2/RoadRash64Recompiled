#pragma once
#include <string>
#include <string_view>
namespace rr64 {
class LogBatch {
  public:
    using Sink = void (*)(std::string_view);
    explicit LogBatch(Sink sink) : sink_(sink) {
        text_.reserve(32768);
    }
    ~LogBatch() {
        flush();
    }
    void append(std::string_view text) {
        if (text_.size() + text.size() > 65536)
            flush();
        if (text.size() > 65536)
            sink_(text);
        else
            text_.append(text);
    }
    void flush() {
        if (!text_.empty()) {
            sink_(text_);
            text_.clear();
        }
    }

  private:
    Sink sink_;
    std::string text_;
};
}
