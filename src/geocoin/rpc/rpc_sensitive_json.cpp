#include "rpc_sensitive_json.hpp"

#include "geocoin/utils/secure_memory.hpp"

namespace geocoin::rpc::detail {

void cleanse_json_strings(json& value) noexcept {
    try {
        if (value.is_string()) {
            utils::cleanse_container(value.get_ref<std::string&>());
            return;
        }
        if (value.is_object()) {
            auto& object = value.get_ref<json::object_t&>();
            while (!object.empty()) {
                auto node = object.extract(object.begin());
                utils::cleanse_container(node.key());
                cleanse_json_strings(node.mapped());
            }
            return;
        }
        if (value.is_array()) {
            for (auto& item : value) {
                cleanse_json_strings(item);
            }
        }
    } catch (...) {
        // Secret cleanup is best-effort and must not throw from a destructor.
    }
}

ScopedJsonStringCleanser::~ScopedJsonStringCleanser() {
    cleanse_json_strings(value_);
}

} // namespace geocoin::rpc::detail
