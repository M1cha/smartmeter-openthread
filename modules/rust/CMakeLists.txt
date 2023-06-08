set(features)
if (CONFIG_SMARTMETER_RUST_LOGGER)
set(features "logger")
endif()

set(target thumbv7m-none-eabi)

if (CONFIG_SMARTMETER_DEBUG_PROFILE)
set(lib_file "${CMAKE_CURRENT_BINARY_DIR}/target/${target}/debug/libsmartmeter.a")
set(profile "")
else()
set(lib_file "${CMAKE_CURRENT_BINARY_DIR}/target/${target}/release/libsmartmeter.a")
set(profile "--release")
endif()

set(include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
set(include_file "${include_dir}/smartmeter-rust.h")

add_custom_target(smartmeter_rust-build
	BYPRODUCTS "${lib_file}"
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	COMMAND cargo build -Z build-std=core,panic_abort -Z build-std-features=panic_immediate_abort --target "${target}" --no-default-features ${profile} --target-dir "${CMAKE_CURRENT_BINARY_DIR}/target"  --features "${features}"
	VERBATIM
	USES_TERMINAL
)

add_custom_target(smartmeter_rust-cbindgen
	BYPRODUCTS "${include_file}"
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	COMMAND cbindgen --config smartmeter/cbindgen.toml --crate smartmeter --output "${include_file}" -q
	VERBATIM
)

add_dependencies(zephyr_interface smartmeter_rust-cbindgen)
zephyr_include_directories("${include_dir}")

zephyr_library_import(smartmeter_rust "${lib_file}")
add_dependencies(smartmeter_rust smartmeter_rust-build)
