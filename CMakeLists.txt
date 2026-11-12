cmake_minimum_required(VERSION 3.20)
project(ai_usage VERSION 0.2.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
find_package(Threads REQUIRED)

add_library(ai_usage_core STATIC src/core.c)
target_include_directories(ai_usage_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ai_usage_core PUBLIC PkgConfig::SQLITE3 m)

add_executable(ai-usage src/cli.c)
target_link_libraries(ai-usage PRIVATE ai_usage_core)

add_executable(ai-usage-tui src/tui.c)
target_link_libraries(ai-usage-tui PRIVATE ai_usage_core PkgConfig::NCURSESW Threads::Threads)

add_executable(ai-usage-gtk src/gtk.c)
target_link_libraries(ai-usage-gtk PRIVATE ai_usage_core PkgConfig::GTK4 PkgConfig::ADWAITA)

enable_testing()
add_executable(test-ai-usage tests/test_core.c)
target_link_libraries(test-ai-usage PRIVATE ai_usage_core)
add_test(NAME core_unit_tests COMMAND test-ai-usage)

add_executable(test-ai-usage-tui tests/test_tui.c)
target_link_libraries(test-ai-usage-tui PRIVATE util)
add_test(NAME tui_interaction_tests COMMAND test-ai-usage-tui $<TARGET_FILE:ai-usage-tui>)
set_tests_properties(tui_interaction_tests PROPERTIES TIMEOUT 25)
add_test(NAME tui_help COMMAND ai-usage-tui --help)
set_tests_properties(tui_help PROPERTIES PASS_REGULAR_EXPRESSION "Keys: 1-4/Tab")
add_test(NAME tui_invalid_days COMMAND ai-usage-tui --days -1)
add_test(NAME tui_invalid_provider COMMAND ai-usage-tui --provider unknown)
add_test(NAME tui_missing_refresh COMMAND ai-usage-tui --refresh)
add_test(NAME tui_redirected_output COMMAND ai-usage-tui)
set_tests_properties(tui_invalid_days tui_invalid_provider tui_missing_refresh
    tui_redirected_output PROPERTIES WILL_FAIL TRUE)

install(TARGETS ai-usage ai-usage-gtk ai-usage-tui RUNTIME DESTINATION bin)
install(FILES assets/com.riccardo.AiUsage.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/icons/hicolor/512x512/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/icons/hicolor/256x256/apps)
install(FILES assets/com.riccardo.AiUsage.png DESTINATION share/pixmaps)
set(desktop_install_code [=[
    set(AI_USAGE_EXECUTABLE "${CMAKE_INSTALL_PREFIX}/bin/ai-usage-gtk")
    configure_file(
        "@CMAKE_CURRENT_SOURCE_DIR@/assets/com.riccardo.AiUsage.desktop.in"
        "@CMAKE_CURRENT_BINARY_DIR@/com.riccardo.AiUsage.desktop"
        @ONLY
    )
]=])
string(CONFIGURE "${desktop_install_code}" desktop_install_code @ONLY)
install(CODE "${desktop_install_code}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.riccardo.AiUsage.desktop" DESTINATION share/applications)
