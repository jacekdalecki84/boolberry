add_subdirectory(gtest)
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})

file(GLOB_RECURSE CORE_TESTS core_tests/*)
file(GLOB_RECURSE CRYPTO_TESTS crypto/*)
file(GLOB_RECURSE FUNC_TESTS functional_tests/*)
file(GLOB_RECURSE PERFORMANCE_TESTS performance_tests/*)
file(GLOB_RECURSE CORE_PROXY core_proxy/*)
file(GLOB_RECURSE UNIT_TESTS unit_tests/*)
file(GLOB_RECURSE EXCHANGE_TESTS exchange_tests/*)

source_group(core_tests FILES ${CORE_TESTS})
source_group(crypto_tests FILES ${CRYPTO_TESTS})
source_group(functional_tests FILES ${FUNC_TESTS})
source_group(performance_tests FILES ${PERFORMANCE_TESTS})
source_group(core_proxy FILES ${CORE_PROXY})
source_group(unit_tests FILES ${UNIT_TESTS})

# add_subdirectory(daemon_tests)

add_executable(coretests ${CORE_TESTS})
add_executable(crypto-tests ${CRYPTO_TESTS})
add_executable(difficulty-tests difficulty/difficulty.cpp)
add_executable(hash-tests hash/main.cpp)
add_executable(hash-target-tests hash-target.cpp)
add_executable(functional_tests ${FUNC_TESTS})
add_executable(performance_tests ${PERFORMANCE_TESTS})
add_executable(core_proxy ${CORE_PROXY})
add_executable(unit_tests ${UNIT_TESTS})
add_executable(net_load_tests_clt net_load_tests/clt.cpp)
add_executable(net_load_tests_srv net_load_tests/srv.cpp)
add_executable(exchange_test ${EXCHANGE_TESTS})

add_dependencies(coretests version)

target_link_libraries(core_proxy currency_core common crypto ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(coretests currency_core common crypto lmdb ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(difficulty-tests currency_core)
target_link_libraries(functional_tests zlibstatic currency_core wallet common crypto upnpc-static ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(hash-tests crypto)
target_link_libraries(hash-target-tests crypto currency_core)
target_link_libraries(performance_tests currency_core common crypto ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(unit_tests zlibstatic currency_core common wallet crypto gtest_main lmdb ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(net_load_tests_clt currency_core common crypto gtest_main ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(net_load_tests_srv currency_core common crypto gtest_main ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
target_link_libraries(exchange_test zlibstatic ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})

if(MSVC)
  add_definitions("/Zi")
endif()


if(NOT MSVC)
  set_property(TARGET gtest gtest_main unit_tests net_load_tests_clt net_load_tests_srv APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-undef -Wno-sign-compare")
  if(APPLE)
    set_property(TARGET gtest gtest_main APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-unused-private-field")
  endif()
endif()


add_custom_target(tests DEPENDS coretests difficulty hash performance_tests core_proxy unit_tests)
set_property(TARGET coretests crypto-tests functional_tests difficulty-tests gtest gtest_main hash-tests hash-target-tests performance_tests core_proxy unit_tests tests net_load_tests_clt net_load_tests_srv PROPERTY FOLDER "tests")

add_test(coretests coretests --generate-and-play-test-data)
add_test(crypto crypto-tests ${CMAKE_CURRENT_SOURCE_DIR}/crypto/tests.txt)
add_test(difficulty difficulty-tests ${CMAKE_CURRENT_SOURCE_DIR}/difficulty/data.txt)
foreach(hash IN ITEMS fast tree)
  add_test(hash-${hash} hash-tests ${hash} ${CMAKE_CURRENT_SOURCE_DIR}/hash/tests-${hash}.txt)
endforeach(hash)
add_test(hash-target hash-target-tests)
add_test(unit_tests unit_tests)
