cmake_minimum_required(VERSION 3.10)
project(distributed_kv_store)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(proto)

include_directories(proto)

add_executable(replica server/server.cpp)
target_link_libraries(replica gRPC::grpc++ kv_proto)

add_executable(client client/client.cpp)
target_link_libraries(client gRPC::grpc++ kv_proto)
