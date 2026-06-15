# W1 tests. Mirrors bolt's manual add_executable pattern: one TU per test,
# linked against ycpp::ycpp + GTest. We avoid gtest_discover_tests so the
# test list stays explicit + the CMake reload is fast.

function(ycpp_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            ycpp::ycpp
            GTest::gtest
            GTest::gtest_main
    )
    ycpp_apply_hardening(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ycpp_add_test(test_ycpp_varint)
ycpp_add_test(test_ycpp_reader_writer)
ycpp_add_test(test_ycpp_arena)
ycpp_add_test(test_ycpp_pool)
ycpp_add_test(test_ycpp_ring)
ycpp_add_test(test_ycpp_hashmap)

# W2 — types + decoder
ycpp_add_test(test_ycpp_id)
ycpp_add_test(test_ycpp_delete_set)
ycpp_add_test(test_ycpp_struct_store)
ycpp_add_test(test_ycpp_decode_v1)

# W3 — Doc + Y.Map LWW + state vector + encode_diff_v1 + two-doc convergence
ycpp_add_test(test_ycpp_state_vector)
ycpp_add_test(test_ycpp_doc)
ycpp_add_test(test_ycpp_convergence)

# W4 — Y.Array + Y.Text, RPC primitives (envelope / protocol / awareness)
ycpp_add_test(test_ycpp_yarray_text)
ycpp_add_test(test_ycpp_envelope)
ycpp_add_test(test_ycpp_protocol)
ycpp_add_test(test_ycpp_awareness)
