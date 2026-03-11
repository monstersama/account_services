# libutils（spinlock, spsc_queue）
add_library(utils STATIC utils/spinlock.cpp)
target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# libshm
add_library(shm STATIC shm/shm_common.cpp shm/shm_generic.cpp shm/shm_config_reader.cpp)
target_include_directories(shm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shm PRIVATE rt)

# liblog_shm
add_library(log_shm STATIC
    logging/log.cpp
    logging/log_format_registry.cpp
    logging/log_demo_format_func.cpp
    logging/default_single_log.cpp
)
target_include_directories(log_shm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(log_shm PRIVATE shm rt)

# 可执行文件
# shm 依赖 log_shm 中的 BaseCoreMgr::log_sys_err，需用 --start-group 解决循环依赖
set(SHM_LIBS -Wl,--start-group shm log_shm -Wl,--end-group rt)

add_executable(log_demo logging/log_demo.cpp base_core_mgr.cpp)
target_link_libraries(log_demo PRIVATE ${SHM_LIBS})

add_executable(log_reader logging/log_reader.cpp)
target_link_libraries(log_reader PRIVATE ${SHM_LIBS})

add_executable(log_benchmark logging/log_benchmark.cpp)
target_link_libraries(log_benchmark PRIVATE ${SHM_LIBS})

# shm_generic 功能测试
add_executable(shm_generic_test shm/shm_generic_test.cpp)
target_link_libraries(shm_generic_test PRIVATE ${SHM_LIBS})

# shm_generic 性能测试
add_executable(shm_generic_benchmark shm/shm_generic_benchmark.cpp)
target_link_libraries(shm_generic_benchmark PRIVATE ${SHM_LIBS})

# shm_seqlock 性能测试（单槽位SeqLock方案）
add_executable(shm_seqlock_test shm/shm_seqlock_test.cpp base_core_mgr.cpp)
target_link_libraries(shm_seqlock_test PRIVATE ${SHM_LIBS})
