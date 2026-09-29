// The deprecated header must work on its own: included first and alone, it
// has to provide everything call_message.h does. (It once did not - both
// headers used the include guard CAS_ASK_MESSAGE_H, so call_message.h was
// skipped when reached through ask_message.h.)
#include "cas/ask_message.h"

#include <type_traits>

static_assert(std::is_base_of_v<std::runtime_error, cas::call_deadlock_error>,
              "cas/ask_message.h alone must declare the call types");
static_assert(std::is_class_v<cas::call_type_error>,
              "cas/ask_message.h alone must declare the call types");
