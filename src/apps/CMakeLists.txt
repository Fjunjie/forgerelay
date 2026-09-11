# 应用层（Application，§3.2）：forgerelayd 服务端 + frctl 客户端。

add_executable(forgerelayd forgerelayd/main.cpp)
target_link_libraries(forgerelayd PRIVATE frtransport)
fr_apply_warnings(forgerelayd cxx)
fr_apply_sanitizers(forgerelayd)

add_executable(frctl frctl/main.cpp)
target_link_libraries(frctl PRIVATE frclient)
fr_apply_warnings(frctl cxx)
fr_apply_sanitizers(frctl)
