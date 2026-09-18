# Configuration for the Sphinx build of the ql-backend documentation.
#
# The pages themselves are the Markdown files that used to sit in the
# repository root; myst_parser is what lets Sphinx read them unchanged, so a
# page stays readable on GitHub and in the built HTML without a second copy.

project = "ql-backend"
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

# Translations, the same way the frontend guide does them. The English
# Markdown is the source; Traditional Chinese lives in gettext catalogues under
# locale/zh_TW/LC_MESSAGES/, one per page -- gettext_compact=False is what keeps
# them per page, so `sphinx-intl stat` says which page has fallen behind. An
# untranslated string falls back to English, which makes a partial translation
# honest rather than broken. doc/build.sh builds both languages.
locale_dirs = ["locale/"]
gettext_compact = False
gettext_uuid = True
language = "en"

html_theme = "sphinx_rtd_theme"
html_title = "ql-backend"

# The logo set lives in assets/logo/ at the repository root, shared with the
# README and with the protobuf and frontend repositories. The sidebar header
# takes the frontend's dark ground so the teal mark reads as it does in the
# app, rather than sitting on the theme's default blue.
html_logo = "../assets/logo/mark-dark.svg"
html_favicon = "../assets/logo/mark.svg"
html_theme_options = {"style_nav_header_background": "#1a1b1e"}

# The language switch under the search box, and the styles it needs.
templates_path = ["_templates"]
html_static_path = ["_static"]
html_css_files = ["custom.css"]

# A page links to source files as ../src/..., which is outside the doc tree:
# correct on GitHub, and not something Sphinx can resolve into the build.
# Warning about it on every build would train the reader to ignore warnings.
suppress_warnings = ["myst.xref_missing"]
