#include "geocoin/utils/logging.hpp"
#include <cstdio>
#include <cstdarg>

namespace geocoin::utils {

// БЕЗ МЬЮТЕКСА! Как в Bitcoin - мьютекс только для файла, не для консоли!
int LogPrintStr(const std::string& str) {
    // Выводим в stdout БЕЗ мьютекса (как в Bitcoin util.cpp:217-219)
    size_t written = fwrite(str.data(), 1, str.size(), stdout);

    // КРИТИЧНО: Проверяем что все символы записались
    if (written != str.size()) {
        // Потеря логов - критическая ошибка!
        // Но продолжаем работу (не крашим)
        if (written == 0) {
            // Ничего не записалось - серьезная ошибка
            perror("fwrite to stdout failed");
        }
    }

    int ret = fflush(stdout);
    if (ret != 0) {
        perror("fflush stdout failed");
    }

    return (int)written;
}

} // namespace geocoin::utils
