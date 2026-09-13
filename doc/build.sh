#!/bin/sh
# Builds the documentation in both languages out of one source tree:
#
#     doc/_build/html/          English
#     doc/_build/html/zh-tw/    Traditional Chinese (zh_TW)
#
# English stays at the root, and an untranslated string falls back to English,
# so the two builds are the same pages rather than two documents. Run it from
# the repository root. It uses $PYTHON when that is set, the .venv that
# doc/requirements.txt describes when there is one, and python3 otherwise.
set -e
if [ -z "$PYTHON" ]; then
    PYTHON=.venv/bin/python
    [ -x "$PYTHON" ] || PYTHON=python3
fi

"$PYTHON" -m sphinx -b html doc doc/_build/html "$@"
"$PYTHON" -m sphinx -b html -D language=zh_TW -D html_title="ql-backend 文件" \
    doc doc/_build/html/zh-tw "$@"
