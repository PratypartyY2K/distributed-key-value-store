cmake_minimum_required(VERSION 3.15)

# AL2023 uses MODULE mode for Protobuf
find_package(Protobuf REQUIRED)

# AL2023 DOES support CONFIG mode for gRPC
find_package(gRPC CONFIG REQUIRED)

# Proto files
set(PROTO_FILES kv.proto)

# Output directory for generated code
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

# protoc and grpc_cpp_plugin
# (work for MODULE mode Protobuf + CONFIG mode gRPC)
get_target_property(PROTOC_EXECUTABLE protobuf::protoc LOCATION)
get_target_property(GRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin LOCATION)

# Generate protobuf + gRPC files
foreach(proto ${PROTO_FILES})
    get_filename_component(proto_abs ${proto} ABSOLUTE)
    get_filename_component(proto_name ${proto} NAME_WE)

    # --- Protobuf C++ generation ---
    add_custom_command(
        OUTPUT
            ${GENERATED_DIR}/${proto_name}.pb.cc
            ${GENERATED_DIR}/${proto_name}.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --cpp_out=${GENERATED_DIR}
             -I ${CMAKE_CURRENT_SOURCE_DIR}
             ${proto_abs}
        DEPENDS ${proto_abs}
    )

    # --- gRPC C++ generation ---
    add_custom_command(
        OUTPUT
            ${GENERATED_DIR}/${proto_name}.grpc.pb.cc
            ${GENERATED_DIR}/${proto_name}.grpc.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --grpc_out=${GENERATED_DIR}
             --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
             -I ${CMAKE_CURRENT_SOURCE_DIR}
             ${proto_abs}
        DEPENDS ${proto_abs}
    )

    list(APPEND PROTO_SRCS
        ${GENERATED_DIR}/${proto_name}.pb.cc
        ${GENERATED_DIR}/${proto_name}.grpc.pb.cc
    )

    list(APPEND PROTO_HDRS
        ${GENERATED_DIR}/${proto_name}.pb.h
        ${GENERATED_DIR}/${proto_name}.grpc.pb.h
    )
endforeach()

# Build gRPC/protobuf library
add_library(kv_proto ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(kv_proto PUBLIC ${GENERATED_DIR})

target_link_libraries(kv_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

