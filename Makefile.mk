# This file is under ruling of Alinode Cloud Project.

BUILDTYPE=Release

all: test

.PHONY: build-test test
build-test:
	mkdir -p build
	(cd build && cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=$(BUILDTYPE))
	cmake --build build

test: build-test
	(cd build && ctest -C Debug --output-on-failure)
