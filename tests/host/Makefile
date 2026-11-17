# tests/host/Makefile — host-test build orchestration
.PHONY: helpers fixtures clean

helpers:
	$(MAKE) -C helpers

fixtures: helpers
	# Curated PE inputs live in ../fixtures/pe/ and are not regenerated here.
	# Generated test data belongs in .last/ rather than a second fixture pool.

clean:
	$(MAKE) -C helpers clean
	rm -rf .last
