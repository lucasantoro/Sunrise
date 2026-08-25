.PHONY: host-test clean

host-test:
	cmake -S . -B build-host
	cmake --build build-host
	ctest --test-dir build-host --output-on-failure

clean:
	rm -rf build-host

