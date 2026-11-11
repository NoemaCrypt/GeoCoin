#include "geocoin/node/node_status_utils.hpp"

#include <iostream>
#include <string>

using namespace geocoin;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_height_lag_without_underflow() {
    using node::detail::is_height_behind_by_more_than;

    check(!is_height_behind_by_more_than(2, 2, 144), "equal low heights are not behind");
    check(!is_height_behind_by_more_than(2, 100, 144), "small positive lag below threshold");
    check(!is_height_behind_by_more_than(2, 146, 144), "lag equal to threshold is not over threshold");
    check(is_height_behind_by_more_than(2, 147, 144), "lag greater than threshold is over threshold");
    check(!is_height_behind_by_more_than(200, 100, 144), "best peer below local height is not behind");
}

void test_sync_lag_reason() {
    const std::string reason = node::detail::make_sync_lag_reason(10, 20);
    check(reason.find("local height 10") != std::string::npos, "reason includes local height");
    check(reason.find("best peer height 20") != std::string::npos, "reason includes peer height");
}

} // namespace

int main() {
    test_height_lag_without_underflow();
    test_sync_lag_reason();

    if (failures == 0) {
        std::cout << "All node status utility tests passed.\n";
        return 0;
    }

    std::cerr << failures << " node status utility test(s) failed.\n";
    return 1;
}
