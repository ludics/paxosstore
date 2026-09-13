# Repo-level wrappers. Real Certain build is certain/CMakeLists.txt.
# PaxosKV has its own CMake under paxoskv/.
#
#   make clean        # remove leftover in-tree .o / old binaries (keeps certain/build)
#   make distclean    # also wipe CMake build trees

.PHONY: clean clean-legacy distclean

clean clean-legacy:
	$(MAKE) -C certain clean-legacy

distclean:
	$(MAKE) -C certain distclean
	rm -rf build
