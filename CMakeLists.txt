cmake_minimum_required(VERSION 3.15)
project(distributed_kv_store CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_subdirectory(proto)

include_directories(proto)

add_executable(replica server/server.cpp)
target_link_libraries(replica
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    kv_proto
)

add_executable(client client/client.cpp)
target_link_libraries(client
    PRIVATE
    gRPC::grpc++
    protobuf::libprotobuf
    kv_proto
)