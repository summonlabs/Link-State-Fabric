cmake_minimum_required(VERSION 3.25)

project(LinkStateFabricConsumer LANGUAGES CXX)

# Independent downstream consumer. It must find an installed Link State Fabric
# package, compile, link, publish evidence, query the resulting state and run:
#
#   cmake -S validation/consumer -B build/consumer -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build build/consumer
#   ./build/consumer/lsf_consumer

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LinkStateFabric CONFIG REQUIRED)

add_executable(lsf_consumer main.cpp)
target_link_libraries(lsf_consumer PRIVATE SummonSoftwareLabs::LinkStateFabric)

if(MSVC)
  target_compile_options(lsf_consumer PRIVATE /W4 /WX /utf-8 /permissive-)
else()
  target_compile_options(lsf_consumer PRIVATE -Wall -Wextra -Werror)
endif()
