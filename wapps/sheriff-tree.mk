# SHERIFF_TREE records the submodule's `git describe`, rewritten only when it
# changes. A submodule bump or a new tag moves no source mtime, yet changes
# the code or the version a Sheriff-derived wasm embeds.
# Needs SHERIFF_SRC and DIR set by the including Makefile.

SHERIFF_TREE := $(DIR).sheriff-tree

.PHONY: FORCE
FORCE:

$(SHERIFF_TREE): FORCE
	@t=$$(git -C $(SHERIFF_SRC) describe --tags --dirty --always 2>/dev/null); \
	    [ "$$t" = "$$(cat $@ 2>/dev/null)" ] || printf '%s\n' "$$t" > $@
