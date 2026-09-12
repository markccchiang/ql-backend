# Configuration for the Sphinx build of the ql-backend documentation.
#
# The pages themselves are the Markdown files that used to sit in the
# repository root; myst_parser is what lets Sphinx read them unchanged, so a
# page stays readable on GitHub and in the built HTML without a second copy.

project = "Interactive QuantLib Service"
author = "Cheng-Chin Chiang"
copyright = "2026, Cheng-Chin Chiang"

extensions = [
    "myst_parser",
    "sphinxcontrib.mermaid",
    "sphinx.ext.mathjax",
]

# The maths pages are written with $...$ and $$...$$ and use ::: fences for
# their notes, which is how they are written in the frontend guide the section
# is shared with. MathJax renders them in the browser, so no LaTeX install is
# needed to build these docs.
myst_enable_extensions = [
    "dollarmath",
    "colon_fence",
    "deflist",
]

# DESIGN.md draws its figures as ```mermaid fences, which GitHub renders on
# its own. This is what makes Sphinx render the same fence rather than print
# it as a code block, so the diagram has one source and no exported image to
# fall out of step with it.
myst_fence_as_directive = ["mermaid"]

# The pages cross-link to each other by section anchor -- DESIGN.md carries a
# table of contents of its own, and INSTALL.md points at "Why sessions
# matter". Sphinx only resolves those if MyST has minted the anchors, and the
# deepest heading that is linked to is a level-3 (### 9.4); 4 leaves room.
myst_heading_anchors = 4

exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", "requirements.txt"]

html_theme = "sphinx_rtd_theme"
html_title = "Interactive QuantLib Service"

# A page links to source files as ../src/..., which is outside the doc tree:
# correct on GitHub, and not something Sphinx can resolve into the build.
# Warning about it on every build would train the reader to ignore warnings.
suppress_warnings = ["myst.xref_missing"]
