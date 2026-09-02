cmake_minimum_required(VERSION 3.21)

# =============================================================================
#  morfPhoto — squelette reutilisable de service morfSystem.
#
#  POUR CREER UN NOUVEAU SERVICE : le plus simple est le script de clonage
#      scripts/new-service.sh   morfmonservice MorfMonService   (Linux/macOS)
#      scripts/new-service.ps1  morfmonservice MorfMonService   (Windows)
#  qui copie ce projet en remplacant les noms. Sinon, remplacer a la main :
#      morfPhoto  (nom du projet / bibliotheque)
#      morfphoto         (namespace, dossier include, binaire, unite systemd)
#      MORFPHOTO         (prefixe de macro)
#
#  Ensuite : coder le metier dans src/ExampleModule.* (ou de nouveaux modules),
#  adapter la fabrique (src/ModuleFactory.cpp) et la liste de sources ci-dessous.
# =============================================================================

# Version pilotee par le fichier VERSION (convention morfSystem).
set(MORFPHOTO_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MORFPHOTO_VERSION_FILE}")
file(STRINGS "${MORFPHOTO_VERSION_FILE}" MORFPHOTO_VERSION_FROM_FILE LIMIT_COUNT 1)
string(STRIP "${MORFPHOTO_VERSION_FROM_FILE}" MORFPHOTO_VERSION_FROM_FILE)

project(morfPhoto VERSION "${MORFPHOTO_VERSION_FROM_FILE}" LANGUAGES CXX)

# --- Standard C++ ---------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Qt : generation automatique du moc (classes QObject) -----------------
set(CMAKE_AUTOMOC ON)

# Core + Network : serveur HTTP entrant, client HTTP sortant, JSON.
# Sql : base SQLite locale du domaine photo (source de verite des metadonnees).
# Concurrent : chaque passe d'indexation tourne sur un thread du pool.
find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent)

# --- Enregistrement des compilations (best-effort) ------------------------
# Chronometre le vrai travail des compilateurs et signale la duree a
# morfAnalytics (POST /api/monitor/activity), QUEL QUE SOIT le declencheur :
# `cmake --build` direct, `morf upgrade` ou un deploiement morfDeploy (un
# marqueur evite le double comptage cote morfDeploy). Sans URL configuree
# (machine hors parc), c'est un no-op silencieux. A appeler AVANT les cibles.
include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/morf/morfdeploy/record_compile.cmake")
morf_enable_compile_recording()

# --- Dependance morfBeacon (annonce de presence sur le LAN) ---------------
# morfBeacon est EMBARQUE (copie vendoree) dans third_party/morf/beacon, lie
# statiquement : le projet se compile sans dependre d'aucun depot externe, a
# l'identique sous Windows, Linux x64 et Raspberry Pi (ARM64).
# Ne pas editer third_party/ ; resynchroniser avec scripts/sync-morf.(sh|ps1).
if(NOT TARGET morfBeacon::morfBeacon)
    add_subdirectory(third_party/morf/beacon)
endif()

# --- Bibliotheque ---------------------------------------------------------
# >>> A ADAPTER : ajouter/retirer ici les sources de VOS modules metier. <<<
add_library(morfPhoto STATIC
    src/ModuleRegistry.cpp
    src/HttpServer.cpp
    src/Service.cpp
    src/Paths.cpp                         # resolution des emplacements (doctrine FILESYSTEM.md)
    src/ModuleFactory.cpp
    src/ExampleModule.cpp                 # <-- remplacer par votre metier
    # --- domaine photo (metier morfPhoto) ---
    src/PhotoRepository.cpp               # depot SQLite : source de verite des metadonnees
    src/ExifExtractor.cpp                 # extraction ExifTool (QProcess -stay_open)
    src/PhotoScanner.cpp                  # parcours FS + garde-fou de perimetre
    src/ContextFile.cpp                   # lecture/ecriture .morfphoto.json (contexte par dossier)
    src/Indexer.cpp                       # passe de reconciliation (verdict, garde passe-unique)
    src/PhotoModule.cpp                   # module metier : assemble tout le domaine
    src/SourceManager.cpp                 # sources SMB poussees (montage via helper privilegie)
    src/PhotoApi.cpp                      # routage /api/v1 (delegue par HttpServer)
    include/morfphoto/PhotoTypes.h
    include/morfphoto/PhotoSchema.h
    include/morfphoto/PhotoRepository.h
    include/morfphoto/ExifExtractor.h
    include/morfphoto/PhotoScanner.h
    include/morfphoto/ContextFile.h
    include/morfphoto/Indexer.h
    include/morfphoto/PhotoModule.h
    include/morfphoto/SourceManager.h
    include/morfphoto/PhotoApi.h
    include/morfphoto/IModule.h
    include/morfphoto/ModuleRegistry.h
    include/morfphoto/ServiceConfig.h
    include/morfphoto/ModuleFactory.h
    include/morfphoto/HttpServer.h
    include/morfphoto/Service.h
    include/morfphoto/ExampleModule.h  # <-- remplacer par votre metier
    include/morfphoto/Version.h
)
add_library(morfPhoto::morfPhoto ALIAS morfPhoto)

target_include_directories(morfPhoto PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(morfPhoto
    PUBLIC  Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent
    PRIVATE morfBeacon::morfBeacon)

# Numero de version accessible depuis le code.
target_compile_definitions(morfPhoto PUBLIC MORFPHOTO_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(morfPhoto PRIVATE /utf-8)
endif()

# --- Cible service (le demon autonome) ------------------------------------
add_subdirectory(service)

# Signale la duree de compile a morfAnalytics apres le lien du binaire de service.
morf_record_target_build(morfphoto "morfPhoto")

# --- Helper privilegie (Linux uniquement) ---------------------------------
# Seul point d'entree root : monte un partage SMB en lecture seule. Produit a
# cote du binaire de service, installe setuid par morfDeploy (voir agent/).
if(UNIX AND NOT APPLE)
    add_subdirectory(agent)
endif()

# --- Test de fumee headless (aucun reseau, aucun ExifTool) ----------------
#   cmake --preset mingw -DMORFPHOTO_BUILD_SMOKE=ON && ctest --preset mingw
# Verifie le domaine (scanner, mapping EXIF, cycle d'indexation) contre la
# bibliotheque, sans dependance externe.
option(MORFPHOTO_BUILD_SMOKE "Compiler le test de fumee du domaine photo" OFF)
if(MORFPHOTO_BUILD_SMOKE)
    enable_testing()
    add_executable(morfphoto_smoke test/smoke_main.cpp)
    target_link_libraries(morfphoto_smoke PRIVATE morfPhoto::morfPhoto)
    add_test(NAME smoke COMMAND morfphoto_smoke)
endif()

# --- Exemple (compile seulement en build autonome, pas via add_subdirectory) -
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_morfphoto_standalone ON)
else()
    set(_morfphoto_standalone OFF)
endif()
option(MORFPHOTO_BUILD_EXAMPLES "Compiler l'exemple de demonstration" ${_morfphoto_standalone})

if(MORFPHOTO_BUILD_EXAMPLES)
    add_subdirectory(examples/minimal)
endif()
