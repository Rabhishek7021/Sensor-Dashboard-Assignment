#!/usr/bin/env bash
# Doxygen input filter for the Markdown pages (README.md, TESTING.md,
# Project-Overview.md). Doxygen cannot resolve GitHub-style in-page links such
# as [Architecture](#3-architecture), so they are turned into plain text for
# the generated documentation only; the files themselves are not changed.
# Used by FILTER_PATTERNS in the Doxyfile.
sed -E 's/\[([^]]*)\]\(#[^)]*\)/\1/g' "$1"
