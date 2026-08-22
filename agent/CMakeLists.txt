# Helper privilegie de morfPhoto (Linux uniquement).
#
# Seul point d'entree root du service : monte un partage SMB en lecture seule sous
# /mnt/photos_<hostname>, et rien d'autre. morfDeploy l'installe setuid 4750,
# root:<compte-service>, sous /usr/lib/morfsystem/morfphoto/ (hors /opt). Il DOIT
# donc etre produit A COTE du binaire de service (dossier build service/), la ou
# morfDeploy le cherche (helper_binary de service.json).
add_executable(morfphoto-helper privileged-helper.cpp)
target_include_directories(morfphoto-helper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(morfphoto-helper PRIVATE Qt6::Core)
set_target_properties(morfphoto-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/service"
)

install(TARGETS morfphoto-helper RUNTIME DESTINATION bin)
