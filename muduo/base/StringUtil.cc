#include "muduo/base/StringUtil.h"
#include <stdarg.h>

namespace muduo {

size_t stringFormatAppendImpl(std::string& dst, const char* format, va_list ap)
{
    size_t bufsz = 1024;
    int result = 0;
    va_list backup_ap; // see "man va_start"

    while (true) {
        char buffer[bufsz];

        va_copy(backup_ap, ap);
        result = vsnprintf(buffer, sizeof(buffer), format, backup_ap);
        va_end(backup_ap);

        if (result < static_cast<int>(bufsz)) {
            result = (result <= 0) ? 0 : (dst.append(buffer, result), result);
            break;
        }

        bufsz = result + 1;
    }

    return result;
}

size_t stringFormatAppend(std::string& dst, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t result = stringFormatAppendImpl(dst, format, ap);
    va_end(ap);
    return result;
}

size_t stringFormat(std::string& dst, const char* format, ...)
{
    dst.clear();
    va_list ap;
    va_start(ap, format);
    size_t result = stringFormatAppendImpl(dst, format, ap);
    va_end(ap);
    return result;
}

std::string stringFormat(const char* format, ...)
{
    std::string result;
    va_list ap;
    va_start(ap, format);
    stringFormatAppendImpl(result, format, ap);
    va_end(ap);
    return result;
}

} // namespace muduo
