# Claude Working Directory

This directory contains all Claude Code-specific files, scripts, and documentation for the Cygnus Actor Framework project.

## Contents

### Scripts
- **`build.bat`** - Windows batch script for building the project
  - Automatically sets up Visual Studio environment (calls vcvarsall.bat)
  - Runs CMake build
  - Reports success/failure
  - Usage: `./claude/build.bat`

### Documentation (`planning/`)
Contains all planning documents, design notes, bug reports, and research:

- **`CLAUDE.md`** - Framework design principles and implementation notes
  - API design philosophy
  - Dependencies strategy
  - Threading model
  - Naming conventions
  - Implementation status

- **`bug_message_sender.md`** - Critical bug documentation
  - Message sender incorrectly set to target instead of source
  - Detailed analysis with debug output
  - Proposed solutions
  - **Status**: 🔴 CRITICAL - blocks reply-based communication

- **`build_setup_paths.md`** - Build system setup guide
  - Required Visual Studio paths for PATH environment variable
  - INCLUDE and LIB environment setup
  - Alternative: using Developer Command Prompt

- **`automatic_handler_registration.md`** - Research on automatic message handler detection
- **`automatic_message_handler_research.md`** - Additional handler registration research
- **`statis_reflections.md`** - Static reflection research notes

## Quick Start

### Building
```bash
# From project root:
./claude/build.bat
```

### Understanding the Framework
1. Start with main `CLAUDE.md` in project root for quick reference
2. Read `claude/planning/CLAUDE.md` for detailed design philosophy
3. Check `claude/planning/bug_message_sender.md` for current critical issues

## Directory Purpose

This directory serves as:
1. **Claude's workspace** - Scripts and tools for Claude Code to use
2. **Documentation hub** - All design docs, bugs, and research in one place
3. **Build utilities** - Scripts that make building easier
4. **Context preservation** - Keeps Claude's planning and notes organized

## Notes for Claude

When working on this project:
- All build scripts go in `claude/`
- All planning/design docs go in `claude/planning/`
- User's planning documents stay in `planning/` (project root)
- Update `CLAUDE.md` (root) with quick reference info
- Update `claude/planning/CLAUDE.md` with detailed design notes
- Document bugs in `claude/planning/bug_*.md` files
