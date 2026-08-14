find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(morfphoto_demo main.cpp)
target_link_libraries(morfphoto_demo PRIVATE morfPhoto::morfPhoto Qt6::Core)

if(MSVC)
    target_compile_options(morfphoto_demo PRIVATE /utf-8)
endif()
