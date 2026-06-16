# WANTED Engine — in-container command runner.
#

# First-party C/H sources (vendored deps and generated trees are excluded).
src_dirs := "src platform cmd include"
# clang-tidy reads flags per TU from this build's compile_commands.json.
tidy_build_dir := env_var_or_default("TIDY_BUILD_DIR", "build-clang")

# List available recipes.
default:
    @just --list

# All blocking lint checks.
lint: lint-format lint-shell

# Reject any formatting drift (clang-format config in .clang-format).
lint-format:
    find {{src_dirs}} \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror

# Reformat the tree in place (developer helper; not run in CI).
format-fix:
    find {{src_dirs}} \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i

# Lint shell scripts. error severity only for now; ratchet down over time.
lint-shell:
    find . -name '*.sh' -not -path './vendor/*' -not -path './third_party/*' -not -path './build*/*' -print0 \
        | xargs -0 shellcheck --severity=error

# Configure + build the clang build dir clang-tidy reads compile_commands.json
# from. Idempotent: a no-op when the dir is already built (CI build-clang
# artifact). Builds so generated headers (e.g. the supervisor config) exist.
tidy-build:
    mkdir -p {{tidy_build_dir}}
    cd {{tidy_build_dir}} && CC=clang cmake -GNinja .. && ninja

# clang-tidy the compiled first-party sources. The file list is taken from the
# compile DB so conditionally-built sources (e.g. ssocket.c without OpenSSL) are
# analysed only when actually compiled. --config-file pins the root config so
# clang-tidy never loads a vendored .clang-tidy from an included header's tree.
tidy: tidy-build
    clang-tidy -p {{tidy_build_dir}} --config-file=.clang-tidy --warnings-as-errors='*' \
        $(python3 -c "import json,os; print('\n'.join(sorted({os.path.relpath(e['file']) for e in json.load(open('{{tidy_build_dir}}/compile_commands.json')) if os.path.relpath(e['file']).startswith(('src/','platform/linux/','cmd/'))})))")

# cppcheck does its own parsing, so it covers every platform without a build.
cppcheck:
    cppcheck --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
        --inline-suppr --error-exitcode=1 \
        -I include -I src/include -I platform/include \
        src platform cmd

# gcc -fanalyzer: deep but slow/verbose — run out-of-band, not on every push.
analyze build_dir="build-analyze":
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja -DCMAKE_C_FLAGS="-fanalyzer" .. && ninja

# Pattern-based security scan (C/C++ ruleset).
security:
    semgrep --config p/c --error --quiet {{src_dirs}}

# Scan the build image definition and the working tree for CVEs and secrets.
scan-image:
    trivy config --severity HIGH,CRITICAL docker/Dockerfile
    trivy fs --severity HIGH,CRITICAL --scanners vuln,secret .

# Scan vendored submodule commits against the OSV database.
scan-deps:
    osv-scanner --recursive .
