add_executable(nexus-edge
    edge_main.cpp
)
target_link_libraries(nexus-edge PRIVATE
    nexus_runtime
    nexus_warnings
    nexus_sanitizers
)

add_executable(nexus-client
    client_main.cpp
)
target_link_libraries(nexus-client PRIVATE
    nexus_runtime
    nexus_warnings
    nexus_sanitizers
)
