# Yjs interop CLI — built when YCPP_BUILD_INTEROP=ON.

add_executable(ycpp_cli ycpp_cli.cpp)
target_link_libraries(ycpp_cli PRIVATE ycpp::ycpp)
ycpp_apply_hardening(ycpp_cli)
