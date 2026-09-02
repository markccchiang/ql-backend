# Building qlservice

Two routes. **Route A builds QuantLib from a submodule** and works unmodified
from a fresh clone; **Route B links an installed QuantLib** and is faster to
rebuild once you have one. They differ only in where QuantLib comes from — the
rest of the build is identical.

Route B is the default in `CMakeLists.txt`, because a QuantLib build is nearly a
thousand translation units and you do not want to repeat it. Route A is the one
to start with, because it cannot be misconfigured.

The targets are `qlservice` (the static library) and `ql-backend` (the daemon,
which shares its name with the CMake project). The build options below are
spelled `QLSERVICE_*`, after the library.

## Prerequisites

| Needs | Known good here | Notes |
| --- | --- | --- |
| CMake ≥ 3.16 | 4.2.3 | |
| A C++17 compiler | AppleClang 21.0.0 | C is also enabled, for uSockets |
| Boost ≥ 1.58, headers | 1.92.0 (Homebrew) | Required by QuantLib on **both** routes |
| Protobuf with a CMake package config | 34.0 (Homebrew) | Found in CONFIG mode; see below |
| `protoc` | 34.0 | Invoked by the build, not by hand |
| git | | Both routes use submodules |
| Python 3 with `venv` | 3.14 | Only for `test/`, not for the build |

On macOS with Homebrew: `brew install cmake boost protobuf`.

Protobuf is found with `find_package(Protobuf CONFIG REQUIRED)`. The CONFIG
mode is deliberate — the deprecated `FindProtobuf` module misses the abseil
dependencies that libprotobuf now carries. A protobuf installed without its
CMake package config will not work.

Vendoring QuantLib does **not** make the build self-contained: Boost is an
external dependency on both routes.

## Route A — build QuantLib from the submodule

```bash
git submodule update --init --recursive                        # uWebSockets + uSockets
git submodule update --init --checkout third_party/QuantLib    # QuantLib v1.43
cmake -S . -B build -DQLSERVICE_VENDOR_QUANTLIB=ON
cmake --build build -j
./build/ql-backend --port 9111
```

`CMakeLists.txt` forces `QL_ENABLE_SESSIONS=ON` and `QL_ENABLE_OPENMP=OFF`
before adding QuantLib's directory, so the two settings this service cannot be
correct without are build settings rather than properties of somebody's install
tree. That is the whole reason this route exists — see
[Why sessions matter](#why-sessions-matter).

Two notes:

- The second command is separate because the QuantLib submodule is marked
  `update = none` in `.gitmodules`. A plain `--init --recursive` skips it, so
  nobody on Route B pays for an 800 MB clone they will not use. `--checkout` is
  what overrides that.
- QuantLib's full history is around 800 MB. Adding `--depth 1` to the second
  command fetches the pinned commit alone and brings that down to about
  50 MB — 41 MB of working tree, 11 MB of git data:

  ```bash
  git submodule update --init --checkout --depth 1 third_party/QuantLib
  ```

  This works because the pin is a release tag rather than an arbitrary commit —
  git fetches it by SHA and checks it out directly. Nothing in the build needs
  the history.

Expect the first build to take a while — QuantLib is ~1000 translation units and
all of them are new.

## Route B — link an installed QuantLib

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local/quantlib-sessions
cmake --build build -j
./build/ql-backend --port 9111
```

`CMAKE_PREFIX_PATH` must name a QuantLib **built with `QL_ENABLE_SESSIONS`**.
Nothing in this build checks that, and the failure is silent — see below. If you
do not already have one:

```bash
git clone --depth 1 --branch v1.43 https://github.com/lballabio/QuantLib.git /tmp/ql
cmake -S /tmp/ql -B /tmp/ql-build \
      -DCMAKE_INSTALL_PREFIX=$HOME/.local/quantlib-sessions \
      -DQL_ENABLE_SESSIONS=ON \
      -DQL_ENABLE_OPENMP=OFF \
      -DQL_BUILD_EXAMPLES=OFF \
      -DQL_BUILD_TEST_SUITE=OFF
cmake --build /tmp/ql-build -j
cmake --install /tmp/ql-build
```

Install it to a **prefix of its own**, not over a general-purpose QuantLib.
`QL_ENABLE_SESSIONS` changes the behaviour of `Singleton<T>::instance()` for
every program that links the library, and other projects will not expect it.
Two prefixes side by side is the intended arrangement:

| Prefix | `QL_ENABLE_SESSIONS` | Use |
| --- | --- | --- |
| `~/.local` | undefined | Other projects |
| `~/.local/quantlib-sessions` | `1` | This one |

`QL_ENABLE_OPENMP=OFF` is not incidental either — workers are deliberately built
without OpenMP (DESIGN §2.2).

## Why sessions matter

`Singleton<T>::instance()` returns one process-wide static in a default
QuantLib, and a `thread_local` one under `QL_ENABLE_SESSIONS`. `Settings`,
which holds `evaluationDate()`, is one of those singletons.

The wrong QuantLib therefore compiles, links, and prices a single session
perfectly correctly. It only goes wrong with two sessions in one process, which
then silently share one evaluation date — plausible numbers, wrong date, no
error anywhere. DESIGN §2 is the argument in full.

`test/smoke_v2.py` is the check that catches it. Two of its sessions sit on
evaluation dates one day apart and must disagree by one day of theta; **prices
that agree exactly mean the wrong QuantLib is linked.**

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `QLSERVICE_VENDOR_QUANTLIB` | `OFF` | Build QuantLib from `third_party/QuantLib` instead of `find_package` |
| `QLSERVICE_WERROR` | `OFF` | `-Werror` on `qlservice`. The tree is warning-free on both routes |

Warnings are `-Wall -Wextra -Wpedantic -Wno-switch`, and only on this project's
own targets: generated protobuf code and uSockets build with `-w`, and a
vendored QuantLib's headers are marked as system includes.

## Things that will bite

- **`QuantLib_DIR` is cached.** Changing `CMAKE_PREFIX_PATH` on an existing
  build tree does not move an already-resolved QuantLib. Reconfigure with
  `cmake --fresh` or delete the tree.
- **An uninitialized `third_party/uWebSockets` is not an error.** The configure
  succeeds, `libqlservice.a` still builds, and only the `ql-backend` executable
  is skipped — with a message telling you to init the submodule. If you ran
  `cmake` before `git submodule update`, this is why there is no binary.
- **`--recursive` matters for uWebSockets**, which nests uSockets inside itself.
  Without it uSockets is empty and the executable is skipped as above.
- **`QLSERVICE_VENDOR_QUANTLIB=ON` with an empty submodule** stops the configure
  with a `FATAL_ERROR` naming the checkout command. That one is deliberate: a
  vendored build that silently fell back to `find_package` would defeat the
  point of the option.

## Verifying the build

There is no unit-test target. `test/smoke_v2.py` drives a running `ql-backend`
over a real WebSocket; [`test/README.md`](test/README.md) has the setup and how
to run it.

Run it at least once after any change to which QuantLib you are linking. Its
two-session check is the only thing in the repository that distinguishes a
*wrong* QuantLib from a missing one.
