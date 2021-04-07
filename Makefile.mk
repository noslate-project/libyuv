# This file is under ruling of Alinode Cloud Project.

.PHONY: test
test:
	mkdir -p build
	(cd build && cmake .. -DBUILD_TESTING=ON)
	cmake --build build
	(cd build && ctest -C Debug --output-on-failure)
