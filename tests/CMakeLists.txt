# Production-tree tests.
#
# No test framework is selected. "Test framework" remains an open foundation choice in
# docs/architecture.md, and selecting one is a dependency decision under ADR 0007 that no
# increment has yet justified. These are plain executables registered with CTest, matching
# what P1a did, and the choice can be revisited when a milestone actually needs the features
# a framework provides.

add_subdirectory(render)
