cmake_minimum_required(VERSION 3.20)
project(Flow++)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

add_executable(flow main.cpp
        websocket.h
        websocket.cpp
        order.cpp
        order.h
        experts.h)

target_link_libraries(flow PRIVATE
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        Threads::Threads
        CURL::libcurl
)

# --- GUI (flow_gui): Dear ImGui + GLFW + OpenGL. Only built when the deps
# are present, so the console build still works without a GUI. ---------------
find_package(PkgConfig QUIET)
find_package(OpenGL QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GLFW QUIET IMPORTED_TARGET glfw3)
endif()

if(GLFW_FOUND AND OpenGL_FOUND)
    set(IMGUI_SOURCES
            imgui/imgui.cpp
            imgui/imgui_draw.cpp
            imgui/imgui_tables.cpp
            imgui/imgui_widgets.cpp
            imgui/backends/imgui_impl_glfw.cpp
            imgui/backends/imgui_impl_opengl3.cpp
    )

    add_executable(flow_gui gui.cpp
            websocket.h
            websocket.cpp
            order.cpp
            order.h
            experts.h
            ${IMGUI_SOURCES})

    target_include_directories(flow_gui PRIVATE imgui imgui/backends)

    target_link_libraries(flow_gui PRIVATE
            Boost::system
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
            Threads::Threads
            CURL::libcurl
            PkgConfig::GLFW
            OpenGL::GL
            dl
    )
else()
    message(WARNING "GLFW/OpenGL not found; flow_gui will not be built.")
endif()