.PHONY: build 

ifeq ($(BUILD_TYPE),)
BUILD_TYPE := Release
endif

build:
	mkdir -p build/$(BUILD_TYPE)/doc_doxybook2
	test -e doc/src/content/reference || (cd doc/src/content && ln -s ../../../build/$(BUILD_TYPE)/doc_doxybook2 reference)
	(cd build/$(BUILD_TYPE) && conan build ../..)
	doxybook2 --input build/$(BUILD_TYPE)/doc_doxygen/xml --config-data '{"baseUrl":"/reference/","linkLowercase":true,"linkSuffix":"/"}' --output build/$(BUILD_TYPE)/doc_doxybook2

install:
	mkdir -p build/$(BUILD_TYPE)
	(cd build/$(BUILD_TYPE) && conan install ../.. -s build_type=$(BUILD_TYPE) --build=missing)

create:
	(cd build/$(BUILD_TYPE) && conan create ../..)
