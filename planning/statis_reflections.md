Yes! C++26 Static Reflection Solves This Perfectly
P2996 "Reflection for C++26" was voted into C++26 in June 2025. It provides exactly what you need—compile-time introspection of function signatures that could automatically detect on_message() handlers.
How It Would Work
cpp#include <experimental/meta>

class my_actor : public actor {
void on_message(const ping_message& msg) { /* ... */ }
void on_message(const pong_message& msg) { /* ... */ }

    // Automatic registration at compile-time
    consteval {
        for (auto member : std::meta::members_of(^^my_actor)) {
            if (std::meta::identifier_of(member) == "on_message") {
                // Extract parameter type from function signature
                auto params = std::meta::parameters_of(member);
                auto msg_type = std::meta::type_of(params[0]);
                
                // Automatically register this handler
                register_handler_for_type(msg_type, member);
            }
        }
    }
};
Core Capabilities
Reflection operator ^^ - Gets compile-time metadata about any entity:
cppauto class_info = ^^MyClass;
auto func_info = ^^some_function;
Introspection functions - Query the metadata:
cppstd::meta::members_of(^^MyClass)     // All members
std::meta::type_of(member)           // Type of member
std::meta::identifier_of(member)     // Name as string
std::meta::parameters_of(function)   // Function parameters
Splice operator [: r :] - Convert metadata back to code:
cpptypename[: reflected_type :]         // Use as type
[: reflected_value :]                // Use as value
Code generation - define_class() creates new types:
cppstd::meta::define_class(^^NewClass, member_specs);
Complete Auto-Registration Example
cpp// Framework helper
template<typename Actor>
consteval void auto_register_handlers() {
for (auto member : std::meta::members_of(^^Actor)) {
if (!std::meta::is_member_function(member)) continue;
if (std::meta::identifier_of(member) != "on_message") continue;

        auto params = std::meta::parameters_of(member);
        if (params.size() != 1) continue;
        
        // Extract message type from const MessageType& parameter
        auto param_type = std::meta::type_of(params[0]);
        auto msg_type = std::meta::remove_cvref(param_type);
        
        // Register at compile-time
        Actor::template register_handler_impl<[:msg_type:]>(member);
    }
}

// Usage - ZERO user code needed
class my_actor : public actor<my_actor> {
void on_message(const ping& msg) { }
void on_message(const pong& msg) { }
// Handlers auto-detected and registered!
};
Availability
Now (experimental):

Bloomberg's Clang fork on Compiler Explorer
EDG's experimental implementation
Both incomplete but demonstrate most features

Production (C++26):

GCC/Clang/MSVC: 2026-2027 expected
Feature freeze: June 2025
Standard published: Early 2026

Limitations for Your Use Case
C++26 reflection can't inject function definitions yet. This means:

✅ Can detect which handlers exist
✅ Can extract parameter types
✅ Can auto-register handlers
❌ Can't synthesize signal implementations (Qt needs this)
❌ Can't inject entire method bodies

For synthesizing code: You'll need P3294 (Code Injection) targeting C++29.
Comparison to Current Approaches
cpp// Current: Manual registration
register_handler<ping_message>();
register_handler<pong_message>();

// C++20 concepts: Compile-time detection, still need type list
template<typename... Messages>
class actor_with_handlers : actor {
(try_register<Messages>(), ...);
};

// C++26 reflection: TRULY automatic
class my_actor : public actor {
// Just write handlers - framework auto-detects everything
};
Bottom Line
Static reflection in C++26 absolutely solves automatic handler detection. It can introspect your class, find all on_message() methods, extract their parameter types, and register them—all at compile-time with zero boilerplate.
The main limitation is code generation (defining new functions) which is deferred to C++29. For pure detection and registration, C++26 reflection is perfect. For replacing Qt's MOC entirely (which generates signal implementations), you'll need to wait for C++29 or use C++26 reflection with some helper macros.
For your actor framework: C++26 reflection is exactly what you need. Plan to adopt it when compilers ship support in 2026-2027.