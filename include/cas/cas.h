#ifndef CAS_H
#define CAS_H

// Cygnus Actor System - Main header
// Include this to get access to all framework components

#include "message_base.h"
#include "actor_ref.h"
#include "actor.h"
#include "stateful_actor.h"
#include "fast_actor.h"
#include "inline_actor.h"
#include "actor_registry.h"
#include "system.h"
#include "ask_message.h"
#include "timer.h"

// Include template implementations AFTER all headers are fully defined
// This resolves the circular dependency between actor_ref.h and actor.h
#include "actor_ref_impl.h"

// ZeroMQ relay actor (optional - requires CAS_ENABLE_ZEROMQ)
#include "zeromq_relay_actor.h"

// Main namespace: cas (Cygnus Actor System)
namespace cas {
    // All types available in namespace
}

#endif // CAS_H
