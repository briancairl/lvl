"""Compiler options shared by first-party targets."""

# First-party code never throws (see CLAUDE.md, "No exceptions"): invariant violations abort through
# the LVL_ASSERT* macros instead. Third-party code keeps the toolchain defaults.
LVL_COPTS = [
    "-fno-exceptions",
    "-Wno-missing-field-initializers",
]
