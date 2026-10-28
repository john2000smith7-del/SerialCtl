#pragma once
#include "Connection.h"
#include "json.hpp"
#include <map>
#include <memory>
#include <mutex>
namespace serialctl {
// GUI and gateway share the same registered transport and its bounded write FIFO.
// Registry ownership keeps a transport alive until in-flight calls have returned.
class SessionService {
  public:
    using Json = nlohmann::json;
    using InputObserver = std::function<void(const std::string &, const Bytes &, const std::string &)>;
    using DetailedInputObserver = std::function<void(const std::string &, const Bytes &, const std::string &, unsigned)>;
    void ObserveInput(InputObserver observer) {
        observer_ = [observer = std::move(observer)](const auto& id, const auto& bytes, const auto& source, unsigned) {
            if (observer) observer(id, bytes, source);
        };
    }
    void ObserveInputWithCodePage(DetailedInputObserver observer) {
        observer_ = std::move(observer);
    }
    void Register(Json resource, std::shared_ptr<IConnection> transport);
    void Unregister(const std::string &);
    std::vector<std::string> Ids();
    Json Resources();
    Json State(const std::string &);
    Json Input(const std::string &, const Bytes &, const std::string &source);
    static Bytes CmdBatch(const Bytes &); // completed command lines, never interactive VT bytes
  private:
    struct Entry {
        Json resource;
        std::shared_ptr<IConnection> transport;
        std::mutex input;
    };
    std::shared_ptr<Entry> Find(const std::string &);
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    DetailedInputObserver observer_;
};
} // namespace serialctl
