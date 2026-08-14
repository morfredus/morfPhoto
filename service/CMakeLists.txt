# Demon de service morfPhoto : execute la bibliotheque en tache de fond.
# Le binaire s'appelle « morfphoto » (comme l'unite systemd et /opt/morfphoto).
add_executable(morfphoto main.cpp)

target_link_libraries(morfphoto PRIVATE morfPhoto::morfPhoto Qt6::Core)

if(MSVC)
    target_compile_options(morfphoto PRIVATE /utf-8)
endif()

install(TARGETS morfphoto RUNTIME DESTINATION bin)
