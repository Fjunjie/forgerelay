# ForgeRelay 测试（GTest；单元 + 协议；集成测试 M2 起填充）。

set(FR_UNIT_TEST_SOURCES
    unit/test_error.cc
    unit/test_byteorder.cc
    unit/test_checked.cc
    unit/test_buffer.cc
    unit/test_crc32.cc
    unit/test_hex.cc
    unit/test_path.cc
    unit/test_fd.cc
)
set(FR_PROTOCOL_TEST_SOURCES
    protocol/test_frame.cc
)

foreach(_src IN LISTS FR_UNIT_TEST_SOURCES FR_PROTOCOL_TEST_SOURCES)
    get_filename_component(_name "${_src}" NAME_WE)
    add_executable(${_name} ${_src})
    target_link_libraries(${_name} PRIVATE frcore GTest::gtest_main)
    fr_apply_warnings(${_name} cxx)
    fr_apply_sanitizers(${_name})
    gtest_discover_tests(${_name}
        DISCOVERY_TIMEOUT 60
        EXTRA_ARGS --gtest_color=yes)
endforeach()
