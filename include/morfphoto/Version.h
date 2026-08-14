/*
 * morfPhoto
 * Copyright (C) 2026 morfredus
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once
#include <QString>

namespace morfphoto {

// Version, injectee par CMake depuis le fichier VERSION.
#ifndef MORFPHOTO_VERSION
#  define MORFPHOTO_VERSION "dev"
#endif

inline QString version() { return QStringLiteral(MORFPHOTO_VERSION); }

// Version du protocole HTTP/JSON expose. >>> A ADAPTER si l'API change. <<<
inline constexpr const char* kProtocol = "morfphoto/1";

} // namespace morfphoto
