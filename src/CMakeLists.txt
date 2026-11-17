add_library(nexus_protocol STATIC
    protocol/protocol.cpp
)
target_include_directories(nexus_protocol PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(nexus_protocol PRIVATE
    nexus_warnings
    nexus_sanitizers
)

add_library(nexus_security STATIC
    security/auth_rate_limiter.cpp
    security/secret_config.cpp
    security/secure_token.cpp
    security/tls_connection.cpp
    security/tls_context.cpp
    security/tls_error.cpp
)
target_include_directories(nexus_security PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(nexus_security PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_link_libraries(nexus_security PRIVATE
    nexus_warnings
    nexus_sanitizers
)

add_library(nexus_runtime STATIC
    runtime/authentication.cpp
    runtime/config.cpp
    runtime/heartbeat.cpp
    runtime/logging.cpp
    runtime/relay.cpp
    runtime/retry.cpp
    runtime/shutdown.cpp
    runtime/socket_utils.cpp
    runtime/tls_transport.cpp
    runtime/unique_socket.cpp
)
target_include_directories(nexus_runtime PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(nexus_runtime PUBLIC
    nexus_protocol
    nexus_security
)
target_link_libraries(nexus_runtime PRIVATE
    nexus_warnings
    nexus_sanitizers
)
