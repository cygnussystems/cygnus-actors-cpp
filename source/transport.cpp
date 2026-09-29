#include "cas/detail/transport.h"

namespace cas::detail {

transport_error::transport_error(const std::string& message, int code)
    : std::runtime_error(message), m_code(code) {
}

int transport_error::code() const noexcept {
    return m_code;
}

} // namespace cas::detail
