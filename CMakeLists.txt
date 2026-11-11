cmake_minimum_required(VERSION 3.20)
project(ai_usage VERSION 0.1.4 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

add_library(ai_usage_core STATIC src/core.c)
target_include_directories(ai_usage_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ai_usage_core PUBLIC PkgConfig::SQLITE3 m)

add_executable(ai-usage src/cli.c)
target_link_libraries(ai-usage PRIVATE ai_usage_core)

add_executable(ai-usage-gtk src/gtk.c)
target_link_libraries(ai-usage-gtk PRIVATE ai_usage_core PkgConfig::GTK4 PkgConfig::ADWAITA)

enable_testing()
add_executable(test-ai-usage tests/test_core.c)
target_link_libraries(test-ai-usage PRIVATE ai_usage_core)
add_test(NAME core_unit_tests COMMAND test-ai-usage)

install(TARGETS ai-usage ai-usage-gtk RUNTIME DESTINATION bin)
install(FILES assets/com.riccardo.AiUsage.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/icons/hicolor/512x512/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/icons/hicolor/256x256/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/pixmaps)
install(FILES assets/com.riccardo.AiUsage.desktop DESTINATION share/applications)
