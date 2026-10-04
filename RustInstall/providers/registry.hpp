// providers/registry.hpp : Provider 注册与查找（工厂）
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "provider.hpp"

namespace prov {

class Registry {
public:
    using Factory = std::function<std::unique_ptr<Provider>()>;

    static Registry& instance() {
        static Registry r;
        return r;
    }

    void add(const std::string& id, Factory f) { factories_[id] = std::move(f); }

    std::unique_ptr<Provider> create(const std::string& id) const {
        auto it = factories_.find(id);
        return it == factories_.end() ? nullptr : it->second();
    }

    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (auto& e : factories_) out.push_back(e.first);
        return out;
    }

private:
    Registry() = default;
    std::map<std::string, Factory> factories_;
};

} // namespace prov
