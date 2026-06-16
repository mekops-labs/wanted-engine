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

# clang-tidy the compiled first-party sources (needs a build dir with compile_commands.json).
tidy:
    clang-tidy -p {{tidy_build_dir}} --warnings-as-errors='*' \
        $(find src platform/linux cmd -name '*.c')

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
