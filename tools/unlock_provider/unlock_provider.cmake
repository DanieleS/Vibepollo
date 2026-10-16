# Fork addition (Scirocco): the lock-screen credential provider that unlocks a locked session when
# an allowed client starts a stream. See src/platform/windows/session_unlock_contract.h.
#
# LogonUI loads it from System32's search path, where the toolchain's runtime DLLs don't exist, so
# it links everything statically and imports only Windows' own libraries.
add_library(vibepollo_unlock_provider SHARED
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/dll.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/provider.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/credential.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/cred_packing.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/pipe_server.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/lsa_secrets.cpp"
        "${CMAKE_SOURCE_DIR}/tools/unlock_provider/unlock_provider.def")
target_compile_features(vibepollo_unlock_provider PRIVATE cxx_std_17)
target_compile_definitions(vibepollo_unlock_provider PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)
target_include_directories(vibepollo_unlock_provider PRIVATE "${CMAKE_SOURCE_DIR}")
set_target_properties(vibepollo_unlock_provider PROPERTIES
        PREFIX ""
        OUTPUT_NAME "VibepolloUnlock"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
target_link_options(vibepollo_unlock_provider PRIVATE -static -s)
target_link_libraries(vibepollo_unlock_provider PRIVATE advapi32 secur32 ole32 uuid)
add_dependencies(sunshine vibepollo_unlock_provider)

install(TARGETS vibepollo_unlock_provider
        RUNTIME DESTINATION "tools" LIBRARY DESTINATION "tools" COMPONENT application)
